/*
 * Copyright © 2022 Google LLC
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

/* Initializes a BO sub-allocator using refcounts on BOs.
 */
void
tu_bo_suballocator_init(struct tu_suballocator *suballoc,
                        struct tu_device *dev,
                        uint32_t default_size, uint32_t flags)
{
   suballoc->dev = dev;
   suballoc->default_size = default_size;
   suballoc->flags = flags;
   suballoc->bo = NULL;
}

void
tu_bo_suballocator_finish(struct tu_suballocator *suballoc)
{
   if (suballoc->bo)
      tu_bo_finish(suballoc->dev, suballoc->bo);
}

VkResult
tu_suballoc_bo_alloc(struct tu_suballoc_bo *suballoc_bo,
                     struct tu_suballocator *suballoc,
                     uint32_t size, uint32_t align)
{
   struct tu_bo *bo = suballoc->bo;
   if (bo) {
      uint32_t offset = ALIGN(suballoc->next_offset, align);
      if (offset < bo->size && offset + size < bo->size) {
         suballoc_bo->bo = tu_bo_get_ref(bo);
         suballoc_bo->iova = bo->iova + offset;
         suballoc_bo->size = size;

         suballoc->next_offset = offset + size;
         return VK_SUCCESS;
      } else {
         tu_bo_finish(suballoc->dev, bo);
         suballoc->bo = NULL;
      }
   }

   VkResult result = tu_bo_init_new(suballoc->dev, &suballoc->bo,
                                    MAX2(size, suballoc->default_size),
                                    suballoc->flags);
   if (result != VK_SUCCESS)
      return result;

   result = tu_bo_map(suballoc->dev, suballoc->bo);
   if (result != VK_SUCCESS) {
      tu_bo_finish(suballoc->dev, suballoc->bo);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   suballoc_bo->bo = tu_bo_get_ref(suballoc->bo);
   suballoc_bo->iova = suballoc_bo->bo->iova;
   suballoc_bo->size = size;
   suballoc->next_offset = size;

   return VK_SUCCESS;
}

void
tu_suballoc_bo_free(struct tu_device *dev, struct tu_suballoc_bo *bo)
{
   if (bo)
      tu_bo_finish(dev, bo->bo);
}

void *
tu_suballoc_bo_map(struct tu_suballoc_bo *bo)
{
   return bo->bo->map + (bo->iova - bo->bo->iova);
}
