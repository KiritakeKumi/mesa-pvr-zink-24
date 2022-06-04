/*
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VN_FEEDBACK_H
#define VN_FEEDBACK_H

#include "vn_common.h"

struct vn_feedback_pool {
   /* single lock for simplicity though free_slots can use another */
   simple_mtx_t mutex;

   struct vn_device *device;
   const VkAllocationCallbacks *alloc;

   uint32_t size;
   uint32_t used;

   /* first entry is the active feedback buffer */
   struct list_head feedback_buffers;

   /* cache for returned feedback slots */
   struct list_head free_slots;
};

enum vn_feedback_type {
   VN_FEEDBACK_TYPE_FENCE,
   VN_FEEDBACK_TYPE_TIMELINE_SEMAPHORE,
   VN_FEEDBACK_TYPE_EVENT,
};

struct vn_feedback_slot {
   enum vn_feedback_type type;
   uint32_t offset;
   VkBuffer buffer;

   union {
      void *data;
      VkResult *status;
      uint64_t *counter;
   };

   struct list_head head;
};

VkResult
vn_feedback_pool_init(struct vn_device *dev,
                      struct vn_feedback_pool *pool,
                      uint32_t size,
                      const VkAllocationCallbacks *alloc);

void
vn_feedback_pool_fini(struct vn_feedback_pool *pool);

struct vn_feedback_slot *
vn_feedback_pool_alloc(struct vn_feedback_pool *pool,
                       enum vn_feedback_type type);

void
vn_feedback_pool_free(struct vn_feedback_pool *pool,
                      struct vn_feedback_slot *slot);

#endif /* VN_FEEDBACK_H */
