/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * This file implements VkQueue
 */

#include "anv_private.h"

#include "xe/anv_queue.h"

#include "vk_common_entrypoints.h"

VkResult
anv_internal_queue_init(struct anv_device *device,
                        struct anv_queue *queue,
                        enum intel_engine_class engine_class)
{
   struct anv_physical_device *pdevice = device->physical;
   VkResult result;

   queue->device = device;
   queue->engine_class = engine_class;

   uint32_t family_index =
      anv_get_first_queue_index(pdevice, engine_class);
   struct anv_queue_family *queue_family =
      &device->physical->queue.families[family_index];
   queue->family = queue_family;
   queue->decoder = &device->decoder[family_index];

   result = device->kmd_backend->engine_create(
      device, queue, engine_class,
      VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR, false /* protected */);
   if (result != VK_SUCCESS) {
      vk_queue_finish(&queue->vk);
      return result;
   }

   /* Add a debug fence to wait on submissions if we're using the synchronized
    * submission feature or the shader-print feature.
    */
   if (INTEL_DEBUG(DEBUG_SYNC | DEBUG_SHADER_PRINT)) {
      result = vk_sync_create(&device->vk,
                              &pdevice->sync_syncobj_type,
                              0, 0, &queue->sync);
      if (result != VK_SUCCESS) {
         anv_queue_finish(queue);
         return result;
      }
   }

   return VK_SUCCESS;
}

void
anv_internal_queue_finish(struct anv_queue *queue)
{
   if (queue->init_submit) {
      anv_async_submit_wait(queue->init_submit);
      anv_async_submit_destroy(queue->init_submit);
   }

   if (queue->sync)
      vk_sync_destroy(&queue->device->vk, queue->sync);

   queue->device->kmd_backend->engine_destroy(queue->device, queue);
}

VkResult
anv_queue_init(struct anv_device *device, struct anv_queue *queue,
               const VkDeviceQueueCreateInfo *pCreateInfo,
               uint32_t index_in_family)
{
   struct anv_physical_device *pdevice = device->physical;
   assert(queue->vk.queue_family_index < pdevice->queue.family_count);
   struct anv_queue_family *queue_family =
      &device->physical->queue.families[pCreateInfo->queueFamilyIndex];
   VkResult result;

   result = vk_queue_init(&queue->vk, &device->vk, pCreateInfo,
                          index_in_family);
   if (result != VK_SUCCESS)
      return result;

   queue->vk.driver_submit = anv_queue_submit;
   queue->device = device;
   queue->family = queue_family;
   queue->engine_class = queue_family->engine_class;
   queue->decoder = &device->decoder[queue->vk.queue_family_index];

   const VkDeviceQueueGlobalPriorityCreateInfoKHR *priority =
      vk_find_struct_const(pCreateInfo->pNext,
                           DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR);

   result = device->kmd_backend->engine_create(
      device, queue, queue->engine_class,
      priority ? priority->globalPriority :
      VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,
      pCreateInfo->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT);
   if (result != VK_SUCCESS) {
      vk_queue_finish(&queue->vk);
      return result;
   }

   /* Add a debug fence to wait on submissions if we're using the synchronized
    * submission feature or the shader-print feature.
    */
   if (INTEL_DEBUG(DEBUG_SYNC | DEBUG_SHADER_PRINT)) {
      result = vk_sync_create(&device->vk,
                              &device->physical->sync_syncobj_type,
                              0, 0, &queue->sync);
      if (result != VK_SUCCESS) {
         anv_queue_finish(queue);
         return result;
      }
   }

   if (queue->engine_class == INTEL_ENGINE_CLASS_COPY ||
       queue->engine_class == INTEL_ENGINE_CLASS_COMPUTE) {
      result = vk_sync_create(&device->vk,
                              &device->physical->sync_syncobj_type,
                              0, 0, &queue->companion_sync);
      if (result != VK_SUCCESS) {
         anv_queue_finish(queue);
         return result;
      }
   }

   return VK_SUCCESS;
}

void
anv_queue_finish(struct anv_queue *queue)
{
   if (queue->init_submit) {
      anv_async_submit_wait(queue->init_submit);
      anv_async_submit_destroy(queue->init_submit);
   }
   if (queue->init_companion_submit) {
      anv_async_submit_wait(queue->init_companion_submit);
      anv_async_submit_destroy(queue->init_companion_submit);
   }

   if (queue->sync)
      vk_sync_destroy(&queue->device->vk, queue->sync);

   if (queue->companion_sync)
      vk_sync_destroy(&queue->device->vk, queue->companion_sync);

   queue->device->kmd_backend->engine_destroy(queue->device, queue);
   vk_queue_finish(&queue->vk);
}

VkResult
anv_QueueWaitIdle(VkQueue _queue)
{
   VK_FROM_HANDLE(anv_queue, queue, _queue);
   struct anv_device *device = queue->device;

   switch (device->info->kmd_type) {
   case INTEL_KMD_TYPE_XE:
      if (queue->vk.submit.mode != VK_QUEUE_SUBMIT_MODE_THREADED) {
         int ret = anv_xe_wait_exec_queue_idle(device, queue->exec_queue_id);

         if (ret == 0)
            return VK_SUCCESS;
         if (ret == -ECANCELED)
            return VK_ERROR_DEVICE_LOST;
         return vk_errorf(device, VK_ERROR_UNKNOWN, "anv_xe_wait_exec_queue_idle failed: %m");
      }
      FALLTHROUGH;
   case INTEL_KMD_TYPE_I915:
      return vk_common_QueueWaitIdle(_queue);
   default:
      unreachable("Missing");
   }

   return VK_SUCCESS;
}
