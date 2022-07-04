// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/fence.h"

#include <stddef.h>

#include "iree/base/tracing.h"

IREE_API_EXPORT iree_status_t iree_hal_fence_create(
    iree_host_size_t capacity, iree_allocator_t host_allocator,
    iree_hal_fence_t** out_fence) {
  IREE_ASSERT_ARGUMENT(out_fence);
  *out_fence = NULL;
  if (IREE_UNLIKELY(capacity >= UINT16_MAX)) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "capacity %" PRIhsz " is too large for fence storage", capacity);
  }

  IREE_TRACE_ZONE_BEGIN(z0);

  const iree_host_size_t semaphore_base = iree_host_align(
      sizeof(iree_hal_fence_t), iree_alignof(iree_hal_semaphore_t*));
  const iree_host_size_t value_base =
      iree_host_align(semaphore_base + capacity * sizeof(iree_hal_semaphore_t*),
                      iree_alignof(uint64_t));
  const iree_host_size_t total_size = value_base + capacity * sizeof(uint64_t);
  iree_hal_fence_t* fence = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, total_size, (void**)&fence));
  iree_atomic_ref_count_init(&fence->ref_count);
  fence->host_allocator = host_allocator;
  fence->capacity = (uint16_t)capacity;
  fence->count = 0;

  *out_fence = fence;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

// TODO(benvanik): actually join efficiently. Today we just create a fence that
// can hold the worst-case sum of all fence timepoints and then insert but it
// could be made much better. In most cases the joined fences have a near
// perfect overlap of semaphores and we are wasting memory.
IREE_API_EXPORT iree_status_t iree_hal_fence_join(
    iree_host_size_t fence_count, iree_hal_fence_t** fences,
    iree_allocator_t host_allocator, iree_hal_fence_t** out_fence) {
  IREE_ASSERT_ARGUMENT(out_fence);
  *out_fence = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);

  // Find the maximum required timepoint capacity.
  iree_host_size_t total_count = 0;
  for (iree_host_size_t i = 0; i < fence_count; ++i) {
    if (fences[i]) total_count += fences[i]->count;
  }

  // Empty list -> NULL.
  if (!total_count) {
    IREE_TRACE_ZONE_END(z0);
    return NULL;
  }

  // Create the fence with the maximum capacity.
  iree_hal_fence_t* fence = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_fence_create(total_count, host_allocator, &fence));

  // Insert all timepoints from all fences.
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < fence_count; ++i) {
    iree_hal_semaphore_list_t source_list =
        iree_hal_fence_semaphore_list(fences[i]);
    for (iree_host_size_t j = 0; j < source_list.count; ++j) {
      status = iree_hal_fence_insert(fence, source_list.semaphores[j],
                                     source_list.payload_values[j]);
      if (!iree_status_is_ok(status)) break;
    }
    if (!iree_status_is_ok(status)) break;
  }

  if (iree_status_is_ok(status)) {
    *out_fence = fence;
  } else {
    iree_hal_fence_release(fence);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

static void iree_hal_fence_destroy(iree_hal_fence_t* fence) {
  IREE_ASSERT_ARGUMENT(fence);
  IREE_ASSERT_REF_COUNT_ZERO(&fence->ref_count);
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_allocator_t host_allocator = fence->host_allocator;

  iree_hal_semaphore_list_t list = iree_hal_fence_semaphore_list(fence);
  for (iree_host_size_t i = 0; i < list.count; ++i) {
    iree_hal_semaphore_release(list.semaphores[i]);
  }
  iree_allocator_free(host_allocator, fence);

  IREE_TRACE_ZONE_END(z0);
}

IREE_API_EXPORT void iree_hal_fence_retain(iree_hal_fence_t* fence) {
  if (IREE_LIKELY(fence)) {
    iree_atomic_ref_count_inc(&fence->ref_count);
  }
}

IREE_API_EXPORT void iree_hal_fence_release(iree_hal_fence_t* fence) {
  if (IREE_LIKELY(fence) && iree_atomic_ref_count_dec(&fence->ref_count) == 1) {
    iree_hal_fence_destroy(fence);
  }
}

IREE_API_EXPORT iree_status_t iree_hal_fence_insert(
    iree_hal_fence_t* fence, iree_hal_semaphore_t* semaphore, uint64_t value) {
  IREE_ASSERT_ARGUMENT(fence);
  IREE_ASSERT_ARGUMENT(semaphore);
  iree_hal_semaphore_list_t list = iree_hal_fence_semaphore_list(fence);

  // Try to find an existing entry with the same semaphore.
  for (iree_host_size_t i = 0; i < list.count; ++i) {
    if (list.semaphores[i] == semaphore) {
      // Found existing; use max of both.
      list.payload_values[i] = iree_max(list.payload_values[i], value);
      return iree_ok_status();
    }
  }

  // Append to list if capacity remaining.
  if (list.count >= fence->capacity) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "fence unique semaphore capacity %u reached",
                            fence->capacity);
  }
  list.semaphores[list.count] = semaphore;
  iree_hal_semaphore_retain(semaphore);
  list.payload_values[list.count] = value;
  ++fence->count;
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t iree_hal_fence_signal(iree_hal_fence_t* fence) {
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_semaphore_list_t semaphore_list =
      iree_hal_fence_semaphore_list(fence);
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < semaphore_list.count; ++i) {
    status = iree_hal_semaphore_signal(semaphore_list.semaphores[i],
                                       semaphore_list.payload_values[i]);
    if (!iree_status_is_ok(status)) break;
  }

  IREE_TRACE_ZONE_END(z0);
  return status;
}

IREE_API_EXPORT void iree_hal_fence_fail(iree_hal_fence_t* fence,
                                         iree_status_t signal_status) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_APPEND_TEXT(
      z0, iree_status_code_string(iree_status_code(signal_status)));

  // This handles cases of empty lists by dropping signal_status if not
  // consumed. Otherwise it clones the signal_status for each semaphore except
  // the last, which in the common case of a single timepoint fence means no
  // expensive clones.
  iree_hal_semaphore_list_t semaphore_list =
      iree_hal_fence_semaphore_list(fence);
  for (iree_host_size_t i = 0; i < semaphore_list.count; ++i) {
    const bool is_last = i == semaphore_list.count - 1;
    iree_status_t semaphore_status;
    if (is_last) {
      // Can transfer ownership of the signal status.
      semaphore_status = signal_status;
      signal_status = iree_ok_status();
    } else {
      // Clone status for this particular signal.
      semaphore_status = iree_status_clone(signal_status);
    }
    iree_hal_semaphore_fail(semaphore_list.semaphores[i], semaphore_status);
  }
  iree_status_ignore(signal_status);

  IREE_TRACE_ZONE_END(z0);
}

IREE_API_EXPORT iree_hal_semaphore_list_t
iree_hal_fence_semaphore_list(iree_hal_fence_t* fence) {
  if (!fence) {
    return (iree_hal_semaphore_list_t){
        .count = 0,
        .semaphores = NULL,
        .payload_values = NULL,
    };
  }
  uint8_t* p = (uint8_t*)fence;
  const iree_host_size_t semaphore_base = iree_host_align(
      sizeof(iree_hal_fence_t), iree_alignof(iree_hal_semaphore_t*));
  const iree_host_size_t value_base = iree_host_align(
      semaphore_base + fence->capacity * sizeof(iree_hal_semaphore_t*),
      iree_alignof(uint64_t));
  return (iree_hal_semaphore_list_t){
      .count = fence->count,
      .semaphores = (iree_hal_semaphore_t**)(p + semaphore_base),
      .payload_values = (uint64_t*)(p + value_base),
  };
}
