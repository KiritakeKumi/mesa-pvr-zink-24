/*
 * Copyright © Microsoft Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dzn_private.h"

#include "vk_alloc.h"
#include "vk_debug_report.h"
#include "vk_format.h"
#include "vk_util.h"

dzn_cmd_pool::dzn_cmd_pool(dzn_device *device,
                           const VkCommandPoolCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator) :
                          flags(pCreateInfo->flags)
{
   vk_object_base_init(&device->vk, &base, VK_OBJECT_TYPE_COMMAND_POOL);
   alloc = pAllocator ? *pAllocator : device->vk.alloc;
}

dzn_cmd_pool::~dzn_cmd_pool()
{
   vk_object_base_finish(&base);
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_CreateCommandPool(VkDevice device,
                      const VkCommandPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkCommandPool *pCmdPool)
{
   return dzn_cmd_pool_factory::create(device,
                                       pCreateInfo,
                                       pAllocator,
                                       pCmdPool);
}

VKAPI_ATTR void VKAPI_CALL
dzn_DestroyCommandPool(VkDevice device,
                       VkCommandPool commandPool,
                       const VkAllocationCallbacks *pAllocator)
{
   dzn_cmd_pool_factory::destroy(device,
                                 commandPool,
                                 pAllocator);
}

dzn_batch::dzn_batch(dzn_cmd_buffer *cmd_buffer):
                    wait(wait_allocator(&cmd_buffer->pool->alloc)),
                    signal(signal_allocator(&cmd_buffer->pool->alloc))
{
   pool = cmd_buffer->pool;
   if (FAILED(cmd_buffer->device->dev->CreateCommandList(0, cmd_buffer->type,
                                                         cmd_buffer->alloc.Get(), NULL,
                                                         IID_PPV_ARGS(&cmdlist))))
      throw vk_error(cmd_buffer->device, VK_ERROR_OUT_OF_HOST_MEMORY);
}

dzn_batch::~dzn_batch()
{
}

const VkAllocationCallbacks *
dzn_batch::get_vk_allocator()
{
   return &pool->alloc;
}

dzn_batch *
dzn_batch::create(dzn_cmd_buffer *cmd_buffer)
{
   dzn_batch *batch = (dzn_batch *)
      vk_zalloc(&cmd_buffer->pool->alloc,
                sizeof(*batch), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (!batch)
      throw vk_error(cmd_buffer->device, VK_ERROR_OUT_OF_HOST_MEMORY);

   try {
      std::construct_at(batch, cmd_buffer);
   } catch (VkResult result) {
      vk_free(&cmd_buffer->pool->alloc, batch);
      throw result;
   }

   return batch;
}

void
dzn_batch::destroy(dzn_batch *batch, struct dzn_cmd_buffer *cmd_buffer)
{
   std::destroy_at(batch);
   vk_free(&cmd_buffer->pool->alloc, batch);
}

dzn_cmd_buffer::dzn_cmd_buffer(dzn_device *dev,
                               dzn_cmd_pool *cmd_pool,
                               VkCommandBufferLevel lvl,
                               const VkAllocationCallbacks *pAllocator) :
                              internal_bufs(bufs_allocator(pAllocator ? pAllocator : &cmd_pool->alloc)),
                              heaps(heaps_allocator(pAllocator ? pAllocator : &cmd_pool->alloc)),
                              batches(batches_allocator(pAllocator ? pAllocator : &cmd_pool->alloc))
{
   device = dev;
   level = lvl;
   pool = cmd_pool;

   VkResult result =
      vk_command_buffer_init(&vk, &device->vk);

   if (result != VK_SUCCESS)
      throw vk_error(device, result);

   struct d3d12_descriptor_pool *pool =
      d3d12_descriptor_pool_new(device->dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16);

   rtv_pool = std::unique_ptr<struct d3d12_descriptor_pool, d3d12_descriptor_pool_deleter>(pool);

   pool = d3d12_descriptor_pool_new(device->dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16);
   dsv_pool = std::unique_ptr<struct d3d12_descriptor_pool, d3d12_descriptor_pool_deleter>(pool);

   if (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      type = D3D12_COMMAND_LIST_TYPE_DIRECT;
   else
      type = D3D12_COMMAND_LIST_TYPE_BUNDLE;

   if (FAILED(device->dev->CreateCommandAllocator(type,
                                                  IID_PPV_ARGS(&alloc)))) {
      vk_command_buffer_finish(&vk);
      throw vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   try {
      open_batch();
   } catch (VkResult error) {
      vk_command_buffer_finish(&vk);
      throw error;
   }
}

dzn_cmd_buffer::~dzn_cmd_buffer()
{
   vk_command_buffer_finish(&vk);
}

void
dzn_cmd_buffer::close_batch()
{
   if (!batch.get())
      return;

   batch->cmdlist->Close();
   batches.push_back(dzn_object_unique_ptr<dzn_batch>(batch.release()));
   assert(batch.get() == NULL);
}

void
dzn_cmd_buffer::open_batch()
{
   batch = dzn_object_unique_ptr<dzn_batch>(dzn_batch::create(this));
}

dzn_batch *
dzn_cmd_buffer::get_batch(bool signal_event)
{
   if (batch.get()) {
      if (batch->signal.size() == 0 || signal_event)
         return batch.get();

      /* Close the current batch if there are event signaling pending. */
      close_batch();

      /* We need to make sure the current state is re-applied on the new
       * cmdlist, so mark things as dirty.
       */
      const dzn_graphics_pipeline * gfx_pipeline =
         reinterpret_cast<const dzn_graphics_pipeline *>(state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].pipeline);

      if (gfx_pipeline) {
         if (gfx_pipeline->vp.count)
            state.dirty |= DZN_CMD_DIRTY_VIEWPORTS;
         if (gfx_pipeline->scissor.count)
            state.dirty |= DZN_CMD_DIRTY_SCISSORS;

         state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].dirty |=
            DZN_CMD_BINDPOINT_DIRTY_PIPELINE;
      }

      if (state.ib.view.SizeInBytes)
         state.dirty |= DZN_CMD_DIRTY_IB;

      const dzn_pipeline *compute_pipeline =
         state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].pipeline;

      if (compute_pipeline) {
         state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].dirty |=
            DZN_CMD_BINDPOINT_DIRTY_PIPELINE;
      }

      state.pipeline = NULL;
   }

   open_batch();
   assert(batch.get());
   return batch.get();
}

void
dzn_cmd_buffer::reset()
{
   /* TODO: Return heaps to the command pool instead of freeing them */
   struct d3d12_descriptor_pool *new_rtv_pool =
      d3d12_descriptor_pool_new(device->dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16);
   struct d3d12_descriptor_pool *new_dsv_pool =
      d3d12_descriptor_pool_new(device->dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16);

   rtv_pool.reset(new_rtv_pool);
   dsv_pool.reset(new_dsv_pool);

   /* TODO: Return batches to the pool instead of freeing them. */
   batches.clear();
   batch.reset(NULL);

   /* Reset the state */
   memset(&state, 0, sizeof(state));

   vk_command_buffer_reset(&vk);
}

VkResult
dzn_cmd_pool::allocate_cmd_buffers(VkDevice device,
                                   const VkCommandBufferAllocateInfo *pAllocateInfo,
                                   VkCommandBuffer *pCommandBuffers)
{
   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      result = dzn_cmd_buffer_factory::create(device, this,
                                              pAllocateInfo->level,
                                              &alloc,
                                              &pCommandBuffers[i]);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      dzn_cmd_pool::free_cmd_buffers(device, i, pCommandBuffers);
      for (i = 0; i < pAllocateInfo->commandBufferCount; i++)
         pCommandBuffers[i] = VK_NULL_HANDLE;
   }

   return result;
}

void
dzn_cmd_pool::free_cmd_buffers(VkDevice device,
                               uint32_t commandBufferCount,
                               const VkCommandBuffer *pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++)
      dzn_cmd_buffer_factory::destroy(device, pCommandBuffers[i], &alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_AllocateCommandBuffers(VkDevice device,
                           const VkCommandBufferAllocateInfo *pAllocateInfo,
                           VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(dzn_cmd_pool, pool, pAllocateInfo->commandPool);

   return pool->allocate_cmd_buffers(device, pAllocateInfo, pCommandBuffers);
}

VKAPI_ATTR void VKAPI_CALL
dzn_FreeCommandBuffers(VkDevice device,
                       VkCommandPool commandPool,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(dzn_cmd_pool, pool, commandPool);

   pool->free_cmd_buffers(device, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                       VkCommandBufferResetFlags flags)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->reset();
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_BeginCommandBuffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   /* If this is the first vkBeginCommandBuffer, we must *initialize* the
    * command buffer's state. Otherwise, we must *reset* its state. In both
    * cases we reset it.
    *
    * From the Vulkan 1.0 spec:
    *
    *    If a command buffer is in the executable state and the command buffer
    *    was allocated from a command pool with the
    *    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT flag set, then
    *    vkBeginCommandBuffer implicitly resets the command buffer, behaving
    *    as if vkResetCommandBuffer had been called with
    *    VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT not set. It then puts
    *    the command buffer in the recording state.
    */
   cmd_buffer->reset();

   cmd_buffer->usage_flags = pBeginInfo->flags;

   /* VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT must be ignored for
    * primary level command buffers.
    *
    * From the Vulkan 1.0 spec:
    *
    *    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT specifies that a
    *    secondary command buffer is considered to be entirely inside a render
    *    pass. If this is a primary command buffer, then this bit is ignored.
    */
   if (cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      cmd_buffer->usage_flags &= ~VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;

   VkResult result = VK_SUCCESS;

#if 0
   if (cmd_buffer->usage_flags &
       VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
      assert(pBeginInfo->pInheritanceInfo);
      VK_FROM_HANDLE(dzn_render_pass, pass,
                      pBeginInfo->pInheritanceInfo->renderPass);
      struct dzn_subpass *subpass =
         &pass->subpasses[pBeginInfo->pInheritanceInfo->subpass];
      VK_FROM_HANDLE(dzn_framebuffer, framebuffer,
                      pBeginInfo->pInheritanceInfo->framebuffer);

      cmd_buffer->state.pass = pass;
      cmd_buffer->state.subpass = subpass;
   }
#endif

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->close_batch();

   return VK_SUCCESS;
}

D3D12_RESOURCE_STATES
dzn_get_states(VkImageLayout layout)
{
   switch (layout) {
   case VK_IMAGE_LAYOUT_PREINITIALIZED:
   case VK_IMAGE_LAYOUT_UNDEFINED:
   case VK_IMAGE_LAYOUT_GENERAL:
      /* YOLO! */
   case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      return D3D12_RESOURCE_STATE_COMMON;

   case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return D3D12_RESOURCE_STATE_COPY_DEST;

   case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return D3D12_RESOURCE_STATE_COPY_SOURCE;

   case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return D3D12_RESOURCE_STATE_RENDER_TARGET;

   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
   case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
      return D3D12_RESOURCE_STATE_DEPTH_WRITE;

   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
      return D3D12_RESOURCE_STATE_DEPTH_READ;

   case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

   default:
      unreachable("not implemented");
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdPipelineBarrier(VkCommandBuffer commandBuffer,
                       VkPipelineStageFlags srcStageMask,
                       VkPipelineStageFlags destStageMask,
                       VkBool32 byRegion,
                       uint32_t memoryBarrierCount,
                       const VkMemoryBarrier *pMemoryBarriers,
                       uint32_t bufferMemoryBarrierCount,
                       const VkBufferMemoryBarrier * pBufferMemoryBarriers,
                       uint32_t imageMemoryBarrierCount,
                       const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   dzn_batch *batch = cmd_buffer->get_batch();

   /* Global memory barriers can be emulated with NULL UAV/Aliasing barriers.
    * Scopes are not taken into account, but that's inherent to the current
    * D3D12 barrier API.
    */
   if (memoryBarrierCount) {
      D3D12_RESOURCE_BARRIER barriers[2];

      barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      barriers[0].UAV.pResource = NULL;
      barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
      barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      barriers[1].Aliasing.pResourceBefore = NULL;
      barriers[1].Aliasing.pResourceAfter = NULL;
      batch->cmdlist->ResourceBarrier(2, barriers);
   }

   for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
      VK_FROM_HANDLE(dzn_buffer, buf, pBufferMemoryBarriers[i].buffer);
      D3D12_RESOURCE_BARRIER barrier;

      /* UAV are used only for storage buffers, skip all other buffers. */
      if (!(buf->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
         continue;

      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      barrier.UAV.pResource = buf->res.Get();
      batch->cmdlist->ResourceBarrier(1, &barrier);
   }

   for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
      /* D3D12_RESOURCE_BARRIER_TYPE_TRANSITION */
      VK_FROM_HANDLE(dzn_image, image, pImageMemoryBarriers[i].image);
      const VkImageSubresourceRange *range =
         &pImageMemoryBarriers[i].subresourceRange;

      uint32_t base_layer, layer_count;
      if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
         base_layer = 0;
         layer_count = u_minify(image->vk.extent.depth, range->baseMipLevel);
      } else {
         base_layer = range->baseArrayLayer;
         layer_count = dzn_get_layerCount(image, range);
      }

      D3D12_RESOURCE_BARRIER barriers[2];
      /* We use placed resource's simple model, in which only one resource
       * pointing to a given heap is active at a given time. To make the
       * resource active we need to add an aliasing barrier.
       */
      barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
      barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      barriers[0].Aliasing.pResourceBefore = NULL;
      barriers[0].Aliasing.pResourceAfter = image->res.Get();
      barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

      barriers[1].Transition.pResource = image->res.Get();
      assert(base_layer == 0 && layer_count == 1);
      barriers[1].Transition.Subresource = 0; // YOLO

      if (pImageMemoryBarriers[i].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
          pImageMemoryBarriers[i].oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED)
         barriers[1].Transition.StateBefore = image->mem->initial_state;
      else
         barriers[1].Transition.StateBefore = dzn_get_states(pImageMemoryBarriers[i].oldLayout);

      barriers[1].Transition.StateAfter = dzn_get_states(pImageMemoryBarriers[i].newLayout);

      /* some layouts map to the states, and NOP-barriers are illegal */
      unsigned nbarriers = 1 + (barriers[1].Transition.StateBefore != barriers[1].Transition.StateAfter);
      batch->cmdlist->ResourceBarrier(nbarriers, barriers);
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdCopyBuffer2KHR(VkCommandBuffer commandBuffer,
                      const VkCopyBufferInfo2KHR* pCopyBufferInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_buffer, src_buffer, pCopyBufferInfo->srcBuffer);
   VK_FROM_HANDLE(dzn_buffer, dst_buffer, pCopyBufferInfo->dstBuffer);

   dzn_batch *batch = cmd_buffer->get_batch();
   ID3D12GraphicsCommandList *cmdlist = batch->cmdlist.Get();

   for (int i = 0; i < pCopyBufferInfo->regionCount; i++) {
      const VkBufferCopy2KHR *region = &pCopyBufferInfo->pRegions[i];

      cmdlist->CopyBufferRegion(dst_buffer->res.Get(), region->dstOffset,
                                src_buffer->res.Get(), region->srcOffset,
                                region->size);
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdCopyBufferToImage2KHR(VkCommandBuffer commandBuffer,
                             const VkCopyBufferToImageInfo2KHR *pCopyBufferToImageInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_buffer, src_buffer, pCopyBufferToImageInfo->srcBuffer);
   VK_FROM_HANDLE(dzn_image, dst_image, pCopyBufferToImageInfo->dstImage);

   D3D12_TEXTURE_COPY_LOCATION src_buf_loc = {
      .pResource = src_buffer->res.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = {
         .Footprint = {
            .Format = dzn_get_format(dst_image->vk.format),
         },
      },
   };

   D3D12_TEXTURE_COPY_LOCATION dst_img_loc = {
      .pResource = dst_image->res.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
   };

   ID3D12Device *dev = cmd_buffer->device->dev;

   dzn_batch *batch = cmd_buffer->get_batch();
   ID3D12GraphicsCommandList *cmdlist = batch->cmdlist.Get();

   for (int i = 0; i < pCopyBufferToImageInfo->regionCount; i++) {
      const VkBufferImageCopy2KHR *region = &pCopyBufferToImageInfo->pRegions[i];

      const uint32_t buffer_row_length =
         region->bufferRowLength ?
         region->bufferRowLength : region->imageExtent.width;

      const uint32_t buffer_image_height =
         region->bufferImageHeight ?
         region->bufferImageHeight : region->imageExtent.height;

      enum pipe_format pfmt = vk_format_to_pipe_format(dst_image->vk.format);
      uint32_t blksz = util_format_get_blocksize(pfmt);

      /* prepare source details */
      src_buf_loc.PlacedFootprint.Footprint.Depth = region->imageExtent.depth;
      src_buf_loc.PlacedFootprint.Footprint.Height = region->imageExtent.height;
      src_buf_loc.PlacedFootprint.Footprint.Width = region->imageExtent.width;
      src_buf_loc.PlacedFootprint.Footprint.RowPitch = blksz * buffer_row_length;

      uint32_t buffer_layer_stride =
         src_buf_loc.PlacedFootprint.Footprint.RowPitch *
         src_buf_loc.PlacedFootprint.Footprint.Height;

      D3D12_BOX src_box = {
         .left = 0,
         .top = 0,
         .front = 0,
         .right = region->imageExtent.width,
         .bottom = region->imageExtent.height,
         .back = region->imageExtent.depth,
      };

      for (uint32_t l = 0; l < region->imageSubresource.layerCount; l++) {
         dst_img_loc.SubresourceIndex =
            dzn_get_subresource_index(&dst_image->desc,
                                      region->imageSubresource.aspectMask,
                                      region->imageSubresource.mipLevel,
                                      region->imageSubresource.baseArrayLayer + l);
         src_buf_loc.PlacedFootprint.Offset =
            region->bufferOffset + (l * buffer_layer_stride);

         if (!(src_buf_loc.PlacedFootprint.Footprint.RowPitch & 255) &&
             !(src_buf_loc.PlacedFootprint.Offset & 511)) {
            /* RowPitch and Offset are properly aligned on 256 bytes, we can copy
             * the whole thing in one call.
             */
            cmdlist->CopyTextureRegion(&dst_img_loc, region->imageOffset.x,
                                       region->imageOffset.y, region->imageOffset.z,
                                       &src_buf_loc, &src_box);
         } else {
            /* Copy line-by-line if things are not properly aligned. */
            D3D12_TEXTURE_COPY_LOCATION src_buf_line_loc = src_buf_loc;

            src_buf_line_loc.PlacedFootprint.Footprint.Height = 1;
            src_buf_line_loc.PlacedFootprint.Footprint.Depth = 1;
            src_box.bottom = 1;
            src_box.back = 1;

            for (uint32_t z = 0; z < region->imageExtent.depth; z++) {
               for (uint32_t y = 0; y < region->imageExtent.height; y++) {
                  UINT64 tex_offset =
                     ((y * buffer_row_length) +
                      (z * buffer_image_height * buffer_row_length)) * blksz;
                  UINT64 offset = src_buf_loc.PlacedFootprint.Offset + tex_offset;

                  src_buf_line_loc.PlacedFootprint.Offset = offset & ~511ULL;
                  offset &= 511;
                  assert(!(offset % blksz));
                  src_box.left = offset / blksz;
                  src_box.right = src_box.left + region->imageExtent.width;
                  src_buf_line_loc.PlacedFootprint.Footprint.Width = src_box.right;
                  src_buf_line_loc.PlacedFootprint.Footprint.RowPitch =
                     ALIGN_POT(src_box.right * blksz, 256);
                  cmdlist->CopyTextureRegion(&dst_img_loc, region->imageOffset.x,
                                             region->imageOffset.y + y, region->imageOffset.z + z,
                                             &src_buf_line_loc, &src_box);
               }
            }
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdCopyImageToBuffer2KHR(VkCommandBuffer commandBuffer,
                             const VkCopyImageToBufferInfo2KHR *pCopyImageToBufferInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_image, src_image, pCopyImageToBufferInfo->srcImage);
   VK_FROM_HANDLE(dzn_buffer, dst_buffer, pCopyImageToBufferInfo->dstBuffer);

   D3D12_TEXTURE_COPY_LOCATION dst_buf_loc = {
      .pResource = dst_buffer->res.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
      .PlacedFootprint = {
         .Footprint = {
            .Format = dzn_get_format(src_image->vk.format),
         },
      },
   };

   D3D12_TEXTURE_COPY_LOCATION src_img_loc = {
      .pResource = src_image->res.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
   };

   ID3D12Device *dev = cmd_buffer->device->dev;
   dzn_batch *batch = cmd_buffer->get_batch();
   ID3D12GraphicsCommandList *cmdlist = batch->cmdlist.Get();

   for (int i = 0; i < pCopyImageToBufferInfo->regionCount; i++) {
      const VkBufferImageCopy2KHR *region = &pCopyImageToBufferInfo->pRegions[i];

      const uint32_t buffer_row_length =
         region->bufferRowLength ?
         region->bufferRowLength : region->imageExtent.width;

      const uint32_t buffer_image_height =
         region->bufferImageHeight ?
         region->bufferImageHeight : region->imageExtent.height;

      enum pipe_format pfmt = vk_format_to_pipe_format(src_image->vk.format);
      uint32_t blksz = util_format_get_blocksize(pfmt);

      /* prepare destination details */
      dst_buf_loc.PlacedFootprint.Footprint.Depth = region->imageExtent.depth;
      dst_buf_loc.PlacedFootprint.Footprint.Height = region->imageExtent.height;
      dst_buf_loc.PlacedFootprint.Footprint.Width = region->imageExtent.width;
      dst_buf_loc.PlacedFootprint.Footprint.RowPitch = buffer_row_length * blksz;

      uint32_t buffer_layer_stride =
         dst_buf_loc.PlacedFootprint.Footprint.RowPitch *
         dst_buf_loc.PlacedFootprint.Footprint.Height;

      D3D12_BOX src_box = {
         .left = (UINT)region->imageOffset.x,
         .top = (UINT)region->imageOffset.y,
         .front = (UINT)region->imageOffset.z,
         .right = (UINT)(region->imageOffset.x + region->imageExtent.width),
         .bottom = (UINT)(region->imageOffset.y + region->imageExtent.height),
         .back = (UINT)(region->imageOffset.z + region->imageExtent.depth),
      };

      for (uint32_t l = 0; l < MIN2(region->imageSubresource.layerCount, 1); l++) {
         src_img_loc.SubresourceIndex =
            dzn_get_subresource_index(&src_image->desc,
                                      region->imageSubresource.aspectMask,
                                      region->imageSubresource.mipLevel,
                                      region->imageSubresource.baseArrayLayer + l);
         dst_buf_loc.PlacedFootprint.Offset =
            region->bufferOffset + (l * buffer_layer_stride);


         if (!(dst_buf_loc.PlacedFootprint.Footprint.RowPitch & 255) &&
             !(dst_buf_loc.PlacedFootprint.Offset & 511)) {
            /* RowPitch and Offset are properly aligned on 256 bytes, we can copy
             * the whole thing in one call.
             */
            cmdlist->CopyTextureRegion(&dst_buf_loc, 0, 0, 0,
                                       &src_img_loc, &src_box);
         } else {
            /* Copy line-by-line if things are not properly aligned. */
            D3D12_TEXTURE_COPY_LOCATION dst_buf_line_loc = dst_buf_loc;

            dst_buf_line_loc.PlacedFootprint.Footprint.Height = 1;
            dst_buf_line_loc.PlacedFootprint.Footprint.Depth = 1;

            for (uint32_t z = 0; z < region->imageExtent.depth; z++) {
               src_box.front = region->imageOffset.z + z;
               src_box.back = region->imageOffset.z + z + 1;
               for (uint32_t y = 0; y < region->imageExtent.height; y++) {
                  UINT64 tex_offset =
                     ((y * buffer_row_length) +
                      (z * buffer_image_height * buffer_row_length)) * blksz;
                  UINT64 offset = dst_buf_loc.PlacedFootprint.Offset + tex_offset;

                  dst_buf_line_loc.PlacedFootprint.Offset = offset & ~511ULL;
                  offset &= 511;
                  assert(!(offset % blksz));

                  UINT dst_x = offset / blksz;

                  dst_buf_line_loc.PlacedFootprint.Footprint.Width =
                     dst_x + region->imageExtent.width;
                  dst_buf_line_loc.PlacedFootprint.Footprint.RowPitch =
                     ALIGN_POT(dst_buf_line_loc.PlacedFootprint.Footprint.Width * blksz, 256);

                  src_box.top = region->imageOffset.y + y;
                  src_box.bottom = region->imageOffset.y + y + 1;

                  cmdlist->CopyTextureRegion(&dst_buf_line_loc, dst_x, 0, 0,
                                             &src_img_loc, &src_box);
               }
            }
         }
      }
   }
}

static void
dzn_fill_image_copy_loc(const dzn_image *img,
                        const VkImageSubresourceLayers *subres,
                        uint32_t layer,
                        D3D12_TEXTURE_COPY_LOCATION *loc)
{
   loc->pResource = img->res.Get();
   if (img->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      assert((subres->baseArrayLayer + layer) == 0);
      assert(subres->mipLevel == 0);
      loc->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      loc->PlacedFootprint.Offset = 0;
      loc->PlacedFootprint.Footprint.Format = dzn_get_format(img->vk.format);
      loc->PlacedFootprint.Footprint.Width = img->vk.extent.width;
      loc->PlacedFootprint.Footprint.Height = img->vk.extent.height;
      loc->PlacedFootprint.Footprint.Depth = img->vk.extent.depth;
      loc->PlacedFootprint.Footprint.RowPitch = img->linear.row_stride;
   } else {
      loc->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      loc->SubresourceIndex =
         dzn_get_subresource_index(&img->desc,
                                   subres->aspectMask,
                                   subres->mipLevel,
                                   subres->baseArrayLayer + layer);
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdCopyImage2KHR(VkCommandBuffer commandBuffer,
                     const VkCopyImageInfo2KHR *pCopyImageInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_image, src, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(dzn_image, dst, pCopyImageInfo->dstImage);

   ID3D12Device *dev = cmd_buffer->device->dev;
   dzn_batch *batch = cmd_buffer->get_batch();
   ID3D12GraphicsCommandList *cmdlist = batch->cmdlist.Get();

   assert(src->vk.samples == dst->vk.samples);

   /* TODO: MS copies */
   assert(src->vk.samples == 1);

   for (int i = 0; i < pCopyImageInfo->regionCount; i++) {
      const VkImageCopy2KHR *region = &pCopyImageInfo->pRegions[i];
      VkImageSubresourceLayers src_subres = region->srcSubresource;
      VkImageSubresourceLayers dst_subres = region->dstSubresource;

      assert(src_subres.layerCount == dst_subres.layerCount);

      for (uint32_t l = 0; l < src_subres.layerCount; l++) {
         D3D12_TEXTURE_COPY_LOCATION dst_loc = { 0 }, src_loc = { 0 };

         dzn_fill_image_copy_loc(src, &src_subres, l, &src_loc);
         dzn_fill_image_copy_loc(dst, &dst_subres, l, &dst_loc);

         D3D12_BOX src_box = {
            .left = (UINT)MAX2(region->srcOffset.x, 0),
            .top = (UINT)MAX2(region->srcOffset.y, 0),
            .front = (UINT)MAX2(region->srcOffset.z, 0),
            .right = (UINT)region->srcOffset.x + region->extent.width,
            .bottom = (UINT)region->srcOffset.y + region->extent.height,
            .back = (UINT)region->srcOffset.z + region->extent.depth,
         };

         cmdlist->CopyTextureRegion(&dst_loc, region->dstOffset.x,
                                    region->dstOffset.y, region->dstOffset.z,
                                    &src_loc, &src_box);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdClearColorImage(VkCommandBuffer commandBuffer,
                       VkImage image,
                       VkImageLayout imageLayout,
                       const VkClearColorValue *pColor,
                       uint32_t rangeCount,
                       const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   dzn_batch *batch = cmd_buffer->get_batch();
   dzn_device *device = cmd_buffer->device;
   VK_FROM_HANDLE(dzn_image, img, image);
   D3D12_RENDER_TARGET_VIEW_DESC desc = {
      .Format = img->desc.Format,
   };

   switch (img->vk.image_type) {
   case VK_IMAGE_TYPE_1D:
      desc.ViewDimension =
         img->vk.array_layers > 1 ?
         D3D12_RTV_DIMENSION_TEXTURE1DARRAY : D3D12_RTV_DIMENSION_TEXTURE1D;
      break;
   case VK_IMAGE_TYPE_2D:
      if (img->vk.array_layers > 1) {
         desc.ViewDimension =
            img->vk.samples > 1 ?
            D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY :
            D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      } else {
         desc.ViewDimension =
            img->vk.samples > 1 ?
            D3D12_RTV_DIMENSION_TEXTURE2DMS :
            D3D12_RTV_DIMENSION_TEXTURE2D;
      }
      break;
   case VK_IMAGE_TYPE_3D:
      desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
      break;
   default: unreachable("Invalid image type\n");
   }

   for (uint32_t r = 0; r < rangeCount; r++) {
      const VkImageSubresourceRange *range = &pRanges[r];

      for (uint32_t l = 0; l < range->levelCount; l++) {
         switch (desc.ViewDimension) {
         case D3D12_RTV_DIMENSION_TEXTURE1D:
            desc.Texture1D.MipSlice = range->baseMipLevel + l;
            break;
         case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
            desc.Texture1DArray.MipSlice = range->baseMipLevel + l;
            desc.Texture1DArray.FirstArraySlice = range->baseArrayLayer;
            desc.Texture1DArray.ArraySize = range->layerCount;
            break;
         case D3D12_RTV_DIMENSION_TEXTURE2D:
            desc.Texture2D.MipSlice = range->baseMipLevel + l;
            if (range->aspectMask & VK_IMAGE_ASPECT_PLANE_1_BIT)
               desc.Texture2D.PlaneSlice = 1;
            else if (range->aspectMask & VK_IMAGE_ASPECT_PLANE_2_BIT)
               desc.Texture2D.PlaneSlice = 2;
            else
               desc.Texture2D.PlaneSlice = 0;
            break;
         case D3D12_RTV_DIMENSION_TEXTURE2DMS:
            break;
         case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
            desc.Texture2DArray.MipSlice = range->baseMipLevel + l;
            desc.Texture2DArray.FirstArraySlice = range->baseArrayLayer;
            desc.Texture2DArray.ArraySize = range->layerCount;
            if (range->aspectMask & VK_IMAGE_ASPECT_PLANE_1_BIT)
               desc.Texture2DArray.PlaneSlice = 1;
            else if (range->aspectMask & VK_IMAGE_ASPECT_PLANE_2_BIT)
               desc.Texture2DArray.PlaneSlice = 2;
            else
               desc.Texture2DArray.PlaneSlice = 0;
            break;
         case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
            desc.Texture2DMSArray.FirstArraySlice = range->baseArrayLayer;
            desc.Texture2DMSArray.ArraySize = range->layerCount;
            break;
         case D3D12_RTV_DIMENSION_TEXTURE3D:
            desc.Texture3D.MipSlice = range->baseMipLevel + l;
            desc.Texture3D.FirstWSlice = range->baseArrayLayer;
            desc.Texture3D.WSize = range->layerCount;
            break;
         }

         struct d3d12_descriptor_handle handle;
         d3d12_descriptor_pool_alloc_handle(cmd_buffer->rtv_pool.get(), &handle);
         device->dev->CreateRenderTargetView(img->res.Get(), &desc,
                                             handle.cpu_handle);
         batch->cmdlist->ClearRenderTargetView(handle.cpu_handle,
                                               pColor->float32, 0, NULL);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                             VkImage image,
                             VkImageLayout imageLayout,
                             const VkClearDepthStencilValue *pDepthStencil,
                             uint32_t rangeCount,
                             const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   dzn_batch *batch = cmd_buffer->get_batch();
   dzn_device *device = cmd_buffer->device;
   VK_FROM_HANDLE(dzn_image, img, image);
   D3D12_DEPTH_STENCIL_VIEW_DESC desc = {
      .Format = dzn_get_dsv_format(img->vk.format),
   };

   switch (img->vk.image_type) {
   case VK_IMAGE_TYPE_1D:
      desc.ViewDimension =
         img->vk.array_layers > 1 ?
         D3D12_DSV_DIMENSION_TEXTURE1DARRAY : D3D12_DSV_DIMENSION_TEXTURE1D;
      break;
   case VK_IMAGE_TYPE_2D:
      if (img->vk.array_layers > 1) {
         desc.ViewDimension =
            img->vk.samples > 1 ?
            D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY :
            D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
      } else {
         desc.ViewDimension =
            img->vk.samples > 1 ?
            D3D12_DSV_DIMENSION_TEXTURE2DMS :
            D3D12_DSV_DIMENSION_TEXTURE2D;
      }
      break;
   default:
      unreachable("Invalid image type\n");
   }

   for (uint32_t r = 0; r < rangeCount; r++) {
      const VkImageSubresourceRange *range = &pRanges[r];
      D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;

      for (uint32_t l = 0; l < range->levelCount; l++) {
         switch (desc.ViewDimension) {
         case D3D12_DSV_DIMENSION_TEXTURE1D:
            desc.Texture1D.MipSlice = range->baseMipLevel + l;
            break;
         case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
            desc.Texture1DArray.MipSlice = range->baseMipLevel + l;
            desc.Texture1DArray.FirstArraySlice = range->baseArrayLayer;
            desc.Texture1DArray.ArraySize = range->layerCount;
            break;
         case D3D12_DSV_DIMENSION_TEXTURE2D:
            desc.Texture2D.MipSlice = range->baseMipLevel + l;
            break;
         case D3D12_DSV_DIMENSION_TEXTURE2DMS:
            break;
         case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
            desc.Texture2DArray.MipSlice = range->baseMipLevel + l;
            desc.Texture2DArray.FirstArraySlice = range->baseArrayLayer;
            desc.Texture2DArray.ArraySize = range->layerCount;
            break;
         case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
            desc.Texture2DMSArray.FirstArraySlice = range->baseArrayLayer;
            desc.Texture2DMSArray.ArraySize = range->layerCount;
            break;
         }

         if (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
            flags |= D3D12_CLEAR_FLAG_DEPTH;
         if (range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
            flags |= D3D12_CLEAR_FLAG_STENCIL;

         struct d3d12_descriptor_handle handle;
         d3d12_descriptor_pool_alloc_handle(cmd_buffer->dsv_pool.get(), &handle);
         device->dev->CreateDepthStencilView(img->res.Get(), &desc,
                                             handle.cpu_handle);
         batch->cmdlist->ClearDepthStencilView(handle.cpu_handle, flags,
                                               pDepthStencil->depth, pDepthStencil->stencil,
                                               0, NULL);
      }
   }
}

void
dzn_cmd_buffer::clear_attachment(uint32_t idx,
                                 const VkClearValue *pClearValue,
                                 VkImageAspectFlags aspectMask,
                                 uint32_t rectCount,
                                 D3D12_RECT *rects)
{
   if (idx == VK_ATTACHMENT_UNUSED)
      return;

   dzn_image_view *view = state.framebuffer->attachments[idx];
   dzn_batch *batch = get_batch();

   if (vk_format_is_depth_or_stencil(view->vk_format)) {
      D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;

      if (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
         flags |= D3D12_CLEAR_FLAG_DEPTH;
      if (aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
         flags |= D3D12_CLEAR_FLAG_STENCIL;

      if (flags != 0)
         batch->cmdlist->ClearDepthStencilView(view->zs_handle.cpu_handle,
                                                flags,
                                                pClearValue->depthStencil.depth,
                                                pClearValue->depthStencil.stencil,
                                                rectCount, rects);
   } else if (aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
      batch->cmdlist->ClearRenderTargetView(view->rt_handle.cpu_handle,
                                             pClearValue->color.float32,
                                             rectCount, rects);
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdClearAttachments(VkCommandBuffer commandBuffer,
                        uint32_t attachmentCount,
                        const VkClearAttachment *pAttachments,
                        uint32_t rectCount,
                        const VkClearRect *pRects)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   struct dzn_render_pass *pass = cmd_buffer->state.pass;
   const struct dzn_subpass *subpass = &pass->subpasses[cmd_buffer->state.subpass];
   dzn_batch *batch = cmd_buffer->get_batch();

   auto rects_elems =
      dzn_transient_zalloc<D3D12_RECT>(rectCount, &cmd_buffer->device->vk.alloc);
   D3D12_RECT *rects = rects_elems.get();
   for (unsigned i = 0; i < rectCount; i++) {
      assert(pRects[i].baseArrayLayer == 0 && pRects[i].layerCount == 1);
      dzn_translate_rect(&rects[i], &pRects[i].rect);
   }

   for (unsigned i = 0; i < attachmentCount; i++) {
      uint32_t idx;
      if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)
         idx = subpass->colors[pAttachments[i].colorAttachment].idx;
      else
         idx = subpass->zs.idx;

      cmd_buffer->clear_attachment(idx, &pAttachments[i].clearValue,
                                   pAttachments[i].aspectMask,
                                   rectCount, rects);
   }
}

void
dzn_cmd_buffer::attachment_transition(const dzn_attachment_ref &att)
{
   dzn_batch *batch = get_batch();
   const dzn_image *image = state.framebuffer->attachments[att.idx]->image;

   if (att.before == att.during)
      return;

   D3D12_RESOURCE_BARRIER barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {
         .pResource = image->res.Get(),
         .Subresource = 0, // YOLO
         .StateBefore = att.before,
         .StateAfter = att.during,
      },
   };
   batch->cmdlist->ResourceBarrier(1, &barrier);
}

void
dzn_cmd_buffer::attachment_transition(const dzn_attachment &att)
{
   dzn_batch *batch = get_batch();
   const dzn_image *image = state.framebuffer->attachments[att.idx]->image;

   if (att.last == att.after)
      return;

   D3D12_RESOURCE_BARRIER barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition = {
         .pResource = image->res.Get(),
         .Subresource = 0, // YOLO
         .StateBefore = att.last,
         .StateAfter = att.after,
      },
   };
   batch->cmdlist->ResourceBarrier(1, &barrier);
}

void
dzn_cmd_buffer::resolve_attachment(uint32_t i)
{
   const struct dzn_subpass *subpass = &state.pass->subpasses[state.subpass];

   if (subpass->resolve[i].idx == VK_ATTACHMENT_UNUSED)
      return;

   dzn_batch *batch = get_batch();
   const dzn_framebuffer *framebuffer = state.framebuffer;
   struct dzn_image_view *src = framebuffer->attachments[subpass->colors[i].idx];
   struct dzn_image_view *dst = framebuffer->attachments[subpass->resolve[i].idx];
   D3D12_RESOURCE_BARRIER barriers[2];
   uint32_t barrier_count = 0;

   /* TODO: 2DArrays/3D */
   if (subpass->colors[i].during != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
      barriers[barrier_count++] = D3D12_RESOURCE_BARRIER {
         .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
         .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
         .Transition = {
            .pResource = src->image->res.Get(),
            .Subresource = 0,
            .StateBefore = subpass->colors[i].during,
            .StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
         },
      };
   }

   if (subpass->resolve[i].during != D3D12_RESOURCE_STATE_RESOLVE_DEST) {
      barriers[barrier_count++] = D3D12_RESOURCE_BARRIER {
         .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
         .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
         .Transition = {
            .pResource = dst->image->res.Get(),
            .Subresource = 0,
            .StateBefore = subpass->resolve[i].during,
            .StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST,
         },
      };
   }

   if (barrier_count)
      batch->cmdlist->ResourceBarrier(barrier_count, barriers);

   batch->cmdlist->ResolveSubresource(dst->image->res.Get(), 0,
                                      src->image->res.Get(), 0,
                                      dst->desc.Format);

   for (uint32_t b = 0; b < barrier_count; b++)
      std::swap(barriers[b].Transition.StateBefore, barriers[b].Transition.StateAfter);

   if (barrier_count)
      batch->cmdlist->ResourceBarrier(barrier_count, barriers);
}

void
dzn_cmd_buffer::begin_subpass()
{
   struct dzn_framebuffer *framebuffer = state.framebuffer;
   struct dzn_render_pass *pass = state.pass;
   const struct dzn_subpass *subpass = &pass->subpasses[state.subpass];
   dzn_batch *batch = get_batch();

   D3D12_CPU_DESCRIPTOR_HANDLE rt_handles[MAX_RTS] = { };
   D3D12_CPU_DESCRIPTOR_HANDLE zs_handle = { 0 };

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      if (subpass->colors[i].idx == VK_ATTACHMENT_UNUSED) continue;

      rt_handles[i] = framebuffer->attachments[subpass->colors[i].idx]->rt_handle.cpu_handle;
   }

   if (subpass->zs.idx != VK_ATTACHMENT_UNUSED) {
      zs_handle = framebuffer->attachments[subpass->zs.idx]->zs_handle.cpu_handle;
   }

   batch->cmdlist->OMSetRenderTargets(subpass->color_count,
                                      subpass->color_count ? rt_handles : NULL,
                                      FALSE, zs_handle.ptr ? &zs_handle : NULL);

   for (uint32_t i = 0; i < subpass->color_count; i++)
      attachment_transition(subpass->colors[i]);
   for (uint32_t i = 0; i < subpass->input_count; i++)
      attachment_transition(subpass->inputs[i]);
   if (subpass->zs.idx != VK_ATTACHMENT_UNUSED)
      attachment_transition(subpass->zs);
}

void
dzn_cmd_buffer::end_subpass()
{
   const dzn_subpass *subpass = &state.pass->subpasses[state.subpass];

   for (uint32_t i = 0; i < subpass->color_count; i++)
      resolve_attachment(i);
}

void
dzn_cmd_buffer::next_subpass()
{
   end_subpass();
   assert(state.subpass + 1 < state.pass->subpass_count);
   state.subpass++;
   begin_subpass();
}

void
dzn_cmd_buffer::begin_pass(const VkRenderPassBeginInfo *pRenderPassBeginInfo,
                           const VkSubpassBeginInfoKHR *pSubpassBeginInfo)
{
   VK_FROM_HANDLE(dzn_render_pass, pass, pRenderPassBeginInfo->renderPass);
   VK_FROM_HANDLE(dzn_framebuffer, framebuffer, pRenderPassBeginInfo->framebuffer);

   assert(pass->attachment_count == framebuffer->attachment_count);

   state.framebuffer = framebuffer;
   state.render_area = D3D12_RECT {
      .left = pRenderPassBeginInfo->renderArea.offset.x,
      .top = pRenderPassBeginInfo->renderArea.offset.y,
      .right = (LONG)(pRenderPassBeginInfo->renderArea.offset.x + pRenderPassBeginInfo->renderArea.extent.width),
      .bottom = (LONG)(pRenderPassBeginInfo->renderArea.offset.y + pRenderPassBeginInfo->renderArea.extent.height),
   };

   // The render area has an impact on the scissor state.
   state.dirty |= DZN_CMD_DIRTY_SCISSORS;
   state.pass = pass;
   state.subpass = 0;
   begin_subpass();

   uint32_t clear_count =
      MIN2(pRenderPassBeginInfo->clearValueCount, framebuffer->attachment_count);
   for (int i = 0; i < clear_count; ++i) {
      VkImageAspectFlags aspectMask =
         (pass->attachments[i].clear.color ? VK_IMAGE_ASPECT_COLOR_BIT : 0) |
         (pass->attachments[i].clear.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
         (pass->attachments[i].clear.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
      clear_attachment(i, &pRenderPassBeginInfo->pClearValues[i], aspectMask,
                       1, &state.render_area);
   }
}

void
dzn_cmd_buffer::end_pass(const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   end_subpass();

   for (uint32_t i = 0; i < state.pass->attachment_count; i++)
      attachment_transition(state.pass->attachments[i]);

   state.framebuffer = NULL;
   state.pass = NULL;
   state.subpass = 0;
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                        const VkRenderPassBeginInfo *pRenderPassBeginInfo,
                        const VkSubpassBeginInfoKHR *pSubpassBeginInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->begin_pass(pRenderPassBeginInfo, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                      const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->end_pass(pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdNextSubpass2(VkCommandBuffer commandBuffer,
                    const VkSubpassBeginInfo *pSubpassBeginInfo,
                    const VkSubpassEndInfo *pSubpassEndInfo)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->next_subpass();
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdBindPipeline(VkCommandBuffer commandBuffer,
                    VkPipelineBindPoint pipelineBindPoint,
                    VkPipeline _pipeline)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_pipeline, pipeline, _pipeline);

   cmd_buffer->state.bindpoint[pipelineBindPoint].pipeline = pipeline;
   cmd_buffer->state.bindpoint[pipelineBindPoint].dirty |= DZN_CMD_BINDPOINT_DIRTY_PIPELINE;
   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      const dzn_graphics_pipeline *gfx = (const dzn_graphics_pipeline *)pipeline;

      if (!gfx->vp.dynamic) {
         memcpy(cmd_buffer->state.viewports, gfx->vp.desc,
                gfx->vp.count * sizeof(cmd_buffer->state.viewports[0]));
         cmd_buffer->state.dirty |= DZN_CMD_DIRTY_VIEWPORTS;
      }

      if (!gfx->scissor.dynamic) {
         memcpy(cmd_buffer->state.scissors, gfx->scissor.desc,
                gfx->scissor.count * sizeof(cmd_buffer->state.scissors[0]));
         cmd_buffer->state.dirty |= DZN_CMD_DIRTY_SCISSORS;
      }

      for (uint32_t vb = 0; vb < gfx->vb.count; vb++)
         cmd_buffer->state.vb.views[vb].StrideInBytes = gfx->vb.strides[vb];

      if (gfx->vb.count > 0)
         BITSET_SET_RANGE(cmd_buffer->state.vb.dirty, 0, gfx->vb.count - 1);
   }
}

void
dzn_cmd_buffer::update_pipeline(uint32_t bindpoint)
{
   const dzn_pipeline *pipeline = state.bindpoint[bindpoint].pipeline;
   dzn_batch *batch = get_batch();

   if (!pipeline)
      return;

   if (state.bindpoint[bindpoint].dirty & DZN_CMD_BINDPOINT_DIRTY_PIPELINE) {
      if (bindpoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
         const dzn_graphics_pipeline *gfx =
            reinterpret_cast<const dzn_graphics_pipeline *>(pipeline);
         batch->cmdlist->SetGraphicsRootSignature(pipeline->layout->root.sig.Get());
         batch->cmdlist->IASetPrimitiveTopology(gfx->ia.topology);
      } else {
         batch->cmdlist->SetComputeRootSignature(pipeline->layout->root.sig.Get());
      }
   }

   if (state.pipeline != pipeline) {
      batch->cmdlist->SetPipelineState(pipeline->state.Get());
      state.pipeline = pipeline;
   }
}

void
dzn_cmd_buffer::update_heaps(uint32_t bindpoint)
{
   struct dzn_descriptor_state *desc_state = &state.bindpoint[bindpoint].desc_state;
   ID3D12DescriptorHeap **new_heaps = desc_state->heaps;
   const struct dzn_pipeline *pipeline = state.bindpoint[bindpoint].pipeline;
   dzn_batch *batch = get_batch();

   if (!(state.bindpoint[bindpoint].dirty & DZN_CMD_BINDPOINT_DIRTY_HEAPS))
      goto set_heaps;

   for (uint32_t type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        type <= D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; type++) {
      uint32_t desc_count = state.pipeline->layout->desc_count[type];
      if (!desc_count)
         continue;

      uint32_t dst_offset = 0;
      auto dst_heap =
         dzn_descriptor_heap(device, type, desc_count, true);

      for (uint32_t s = 0; s < MAX_SETS; s++) {
         const struct dzn_descriptor_set *set = desc_state->sets[s].set;
         if (!set) continue;

         uint32_t set_desc_count =
            type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ?
            set->layout->view_desc_count : set->layout->sampler_desc_count;
         if (!set_desc_count) continue;

         dst_heap.copy(dst_offset, set->heaps[type], 0, set_desc_count);
         dst_offset += set_desc_count;
      }

      new_heaps[type] = dst_heap;
      heaps.push_back(dst_heap);
   }

set_heaps:
   if (new_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] != state.heaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] ||
       new_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] != state.heaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER]) {
      ID3D12DescriptorHeap *desc_heaps[2];
      uint32_t num_desc_heaps = 0;
      if (new_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV])
         desc_heaps[num_desc_heaps++] = new_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
      if (new_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER])
         desc_heaps[num_desc_heaps++] = new_heaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER];
      batch->cmdlist->SetDescriptorHeaps(num_desc_heaps, desc_heaps);

      for (unsigned h = 0; h < ARRAY_SIZE(state.heaps); h++)
         state.heaps[h] = new_heaps[h];

      for (uint32_t r = 0; r < pipeline->layout->root.sets_param_count; r++) {
         D3D12_DESCRIPTOR_HEAP_TYPE type = pipeline->layout->root.type[r];
         D3D12_GPU_DESCRIPTOR_HANDLE handle = {
            .ptr = new_heaps[type]->GetGPUDescriptorHandleForHeapStart().ptr,
         };

         if (bindpoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
            batch->cmdlist->SetGraphicsRootDescriptorTable(r, handle);
         else
            batch->cmdlist->SetComputeRootDescriptorTable(r, handle);
      }
   }
}

void
dzn_cmd_buffer::update_sysvals(uint32_t bindpoint)
{
   if (!(state.bindpoint[bindpoint].dirty & DZN_CMD_BINDPOINT_DIRTY_SYSVALS))
      return;

   const struct dzn_pipeline *pipeline = state.bindpoint[bindpoint].pipeline;
   uint32_t sysval_cbv_param_idx = pipeline->layout->root.sysval_cbv_param_idx;
   dzn_batch *batch = get_batch();

   if (bindpoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      batch->cmdlist->SetGraphicsRoot32BitConstants(sysval_cbv_param_idx,
                                                    sizeof(state.sysvals.gfx) / 4,
                                                    &state.sysvals.gfx, 0);
   } else {
      batch->cmdlist->SetComputeRoot32BitConstants(sysval_cbv_param_idx,
                                                   sizeof(state.sysvals.compute) / 4,
                                                   &state.sysvals.compute, 0);
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                          VkPipelineBindPoint pipelineBindPoint,
                          VkPipelineLayout _layout,
                          uint32_t firstSet,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets,
                          uint32_t dynamicOffsetCount,
                          const uint32_t *pDynamicOffsets)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_pipeline_layout, layout, _layout);

   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      VK_FROM_HANDLE(dzn_descriptor_set, set, pDescriptorSets[i]);
      cmd_buffer->state.bindpoint[pipelineBindPoint].desc_state.sets[firstSet + i].set = set;
   }

   cmd_buffer->state.bindpoint[pipelineBindPoint].dirty |= DZN_CMD_BINDPOINT_DIRTY_HEAPS;
}

void
dzn_cmd_buffer::update_viewports()
{
   const dzn_graphics_pipeline *pipeline =
      reinterpret_cast<const dzn_graphics_pipeline *>(state.pipeline);
   dzn_batch *batch = get_batch();

   if (!(state.dirty & DZN_CMD_DIRTY_VIEWPORTS) ||
       !pipeline->vp.count)
      return;

   batch->cmdlist->RSSetViewports(pipeline->vp.count, state.viewports);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdSetViewport(VkCommandBuffer commandBuffer,
                   uint32_t firstViewport,
                   uint32_t viewportCount,
                   const VkViewport *pViewports)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   STATIC_ASSERT(MAX_VP <= DXIL_SPIRV_MAX_VIEWPORT);

   for (uint32_t i = 0; i < viewportCount; i++) {
      uint32_t vp = i + firstViewport;

      dzn_translate_viewport(&cmd_buffer->state.viewports[vp], &pViewports[i]);

      if (pViewports[i].minDepth > pViewports[i].maxDepth)
         cmd_buffer->state.sysvals.gfx.yz_flip_mask |= BITFIELD_BIT(vp + DXIL_SPIRV_Z_FLIP_SHIFT);
      else
         cmd_buffer->state.sysvals.gfx.yz_flip_mask &= ~BITFIELD_BIT(vp + DXIL_SPIRV_Z_FLIP_SHIFT);

      if (pViewports[i].height > 0)
         cmd_buffer->state.sysvals.gfx.yz_flip_mask |= BITFIELD_BIT(vp);
      else
         cmd_buffer->state.sysvals.gfx.yz_flip_mask &= ~BITFIELD_BIT(vp);
   }

   if (viewportCount) {
      cmd_buffer->state.dirty |= DZN_CMD_DIRTY_VIEWPORTS;
      cmd_buffer->state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].dirty |=
         DZN_CMD_BINDPOINT_DIRTY_SYSVALS;
   }
}

void
dzn_cmd_buffer::update_scissors()
{
   const dzn_graphics_pipeline *pipeline =
      reinterpret_cast<const dzn_graphics_pipeline *>(state.pipeline);
   dzn_batch *batch = get_batch();

   if (!(state.dirty & DZN_CMD_DIRTY_SCISSORS))
      return;

   if (!pipeline->scissor.count) {
      /* Apply a scissor delimiting the render area. */
      batch->cmdlist->RSSetScissorRects(1, &state.render_area);
      return;
   }

   D3D12_RECT scissors[MAX_SCISSOR];
   uint32_t scissor_count = pipeline->scissor.count;

   memcpy(scissors, state.scissors, sizeof(D3D12_RECT) * pipeline->scissor.count);
   for (uint32_t i = 0; i < pipeline->scissor.count; i++) {
      scissors[i].left = MAX2(scissors[i].left, state.render_area.left);
      scissors[i].top = MAX2(scissors[i].top, state.render_area.top);
      scissors[i].right = MIN2(scissors[i].right, state.render_area.right);
      scissors[i].bottom = MIN2(scissors[i].bottom, state.render_area.bottom);
   }

   batch->cmdlist->RSSetScissorRects(pipeline->scissor.count, scissors);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdSetScissor(VkCommandBuffer commandBuffer,
                  uint32_t firstScissor,
                  uint32_t scissorCount,
                  const VkRect2D *pScissors)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   for (uint32_t i = 0; i < scissorCount; i++)
      dzn_translate_rect(&cmd_buffer->state.scissors[i + firstScissor], &pScissors[i]);

   if (scissorCount)
      cmd_buffer->state.dirty |= DZN_CMD_DIRTY_SCISSORS;
}

void
dzn_cmd_buffer::update_vbviews()
{
   const dzn_graphics_pipeline *pipeline =
      reinterpret_cast<const dzn_graphics_pipeline *>(state.pipeline);
   dzn_batch *batch = get_batch();
   unsigned start, end;

   BITSET_FOREACH_RANGE(start, end, state.vb.dirty, MAX_VBS)
      batch->cmdlist->IASetVertexBuffers(start, end - start, state.vb.views);

   BITSET_CLEAR_RANGE(state.vb.dirty, 0, MAX_VBS);
}

void
dzn_cmd_buffer::update_ibview()
{
   if (!(state.dirty & DZN_CMD_DIRTY_IB))
      return;

   dzn_batch *batch = get_batch();

   batch->cmdlist->IASetIndexBuffer(&state.ib.view);
}

void
dzn_cmd_buffer::update_push_constants(uint32_t bindpoint)
{
   assert(bindpoint == VK_PIPELINE_BIND_POINT_GRAPHICS);

   dzn_batch *batch = get_batch();

   if (!(state.push_constant.stages & VK_SHADER_STAGE_ALL_GRAPHICS))
      return;

   uint32_t slot = state.pipeline->layout->root.push_constant_cbv_param_idx;
   uint32_t offset = state.push_constant.offset / 4;
   uint32_t end = ALIGN(state.push_constant.end, 4) / 4;

   batch->cmdlist->SetGraphicsRoot32BitConstants(slot, end - offset,
      state.push_constant.values + offset, offset);
   state.push_constant.stages = 0;
   state.push_constant.offset = 0;
   state.push_constant.end = 0;
}

void
dzn_CmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                     VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size,
                     const void *pValues)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   memcpy(((char *)cmd_buffer->state.push_constant.values) + offset, pValues, size);
   cmd_buffer->state.push_constant.stages |= stageFlags;

   uint32_t current_offset = cmd_buffer->state.push_constant.offset;
   uint32_t current_end = cmd_buffer->state.push_constant.end;
   uint32_t end = offset + size;
   if (current_end != 0) {
      offset = MIN2(current_offset, offset);
      end = MAX2(current_end, end);
   }
   cmd_buffer->state.push_constant.offset = offset;
   cmd_buffer->state.push_constant.end = end;
}

void
dzn_cmd_buffer::triangle_fan_create_index(uint32_t &vertex_count)
{
   uint8_t index_size = vertex_count <= 0xffff ? 2 : 4;
   uint32_t triangle_count = MAX2(vertex_count, 2) - 2;

   vertex_count = triangle_count * 3;
   if (!vertex_count)
      return;

   ID3D12Resource *index_buf =
      dzn_cmd_buffer::alloc_internal_buf(vertex_count * index_size,
                                         D3D12_HEAP_TYPE_UPLOAD,
                                         D3D12_RESOURCE_STATE_GENERIC_READ);
   void *cpu_ptr;
   index_buf->Map(0, NULL, &cpu_ptr);

   /* TODO: VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT */
   if (index_size == 2) {
      uint16_t *indices = (uint16_t *)cpu_ptr;
      for (uint32_t t = 0; t < triangle_count; t++) {
         indices[t * 3] = t + 1;
         indices[(t * 3) + 1] = t + 2;
         indices[(t * 3) + 2] = 0;
      }
      state.ib.view.Format = DXGI_FORMAT_R16_UINT;
   } else {
      uint32_t *indices = (uint32_t *)cpu_ptr;
      for (uint32_t t = 0; t < triangle_count; t++) {
         indices[t * 3] = t + 1;
         indices[(t * 3) + 1] = t + 2;
         indices[(t * 3) + 2] = 0;
      }
      state.ib.view.Format = DXGI_FORMAT_R32_UINT;
   }

   state.ib.view.SizeInBytes = vertex_count * index_size;
   state.ib.view.BufferLocation = index_buf->GetGPUVirtualAddress();
   state.dirty |= DZN_CMD_DIRTY_IB;
}

void
dzn_cmd_buffer::triangle_fan_rewrite_index(uint32_t &index_count,
                                           uint32_t &first_index)
{
   uint32_t triangle_count = MAX2(index_count, 2) - 2;

   index_count = triangle_count * 3;
   if (!index_count)
      return;

   /* New index is always 32bit to make the compute shader rewriting the
    * index simpler */
   ID3D12Resource *new_index_buf =
      dzn_cmd_buffer::alloc_internal_buf(index_count * 4,
                                         D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   D3D12_GPU_VIRTUAL_ADDRESS old_index_buf_gpu = state.ib.view.BufferLocation;

   auto index_type =
      dzn_meta_triangle_fan_rewrite_index::get_index_type(state.ib.view.Format);
   const dzn_meta_triangle_fan_rewrite_index *rewrite_index =
      device->triangle_fan[index_type].get();

   const dzn_pipeline *compute_pipeline =
      state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].pipeline;

   struct dzn_triangle_fan_rewrite_index_params params = {
      .first_index = first_index,
   };

   batch->cmdlist->SetComputeRootSignature(rewrite_index->root_sig.Get());
   batch->cmdlist->SetPipelineState(rewrite_index->pipeline_state.Get());
   batch->cmdlist->SetComputeRootUnorderedAccessView(0, new_index_buf->GetGPUVirtualAddress());
   batch->cmdlist->SetComputeRoot32BitConstants(1, sizeof(params) / 4,
                                                &params, 0);
   batch->cmdlist->SetComputeRootShaderResourceView(2, old_index_buf_gpu);
   batch->cmdlist->Dispatch(triangle_count, 1, 1);

   D3D12_RESOURCE_BARRIER post_barriers[] = {
      {
         .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
         .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          /* Transition the exec buffer to indirect arg so it can be
           * pass to ExecuteIndirect() as an argument buffer.
           */
         .Transition = {
            .pResource = new_index_buf,
            .Subresource = 0,
            .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            .StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER,
         },
      },
   };

   batch->cmdlist->ResourceBarrier(ARRAY_SIZE(post_barriers), post_barriers);

   /* We don't mess up with the driver state when executing our internal
    * compute shader, but we still change the D3D12 state, so let's mark
    * things dirty if needed.
    */
   state.pipeline = NULL;
   if (state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].pipeline) {
      state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].dirty |=
         DZN_CMD_BINDPOINT_DIRTY_PIPELINE;
   }

   state.ib.view.SizeInBytes = index_count * 4;
   state.ib.view.BufferLocation = new_index_buf->GetGPUVirtualAddress();
   state.ib.view.Format = DXGI_FORMAT_R32_UINT;
   state.dirty |= DZN_CMD_DIRTY_IB;
   first_index = 0;
}

void
dzn_cmd_buffer::prepare_draw(bool indexed)
{
   update_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS);
   update_heaps(VK_PIPELINE_BIND_POINT_GRAPHICS);
   update_sysvals(VK_PIPELINE_BIND_POINT_GRAPHICS);
   update_viewports();
   update_scissors();
   update_vbviews();
   update_push_constants(VK_PIPELINE_BIND_POINT_GRAPHICS);

   if (indexed)
      update_ibview();

   /* Reset the dirty states */
   state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].dirty = 0;
   state.dirty = 0;
}

void
dzn_cmd_buffer::draw(uint32_t vertex_count,
                     uint32_t instance_count,
                     uint32_t first_vertex,
                     uint32_t first_instance)
{
   const dzn_graphics_pipeline *pipeline =
      reinterpret_cast<const dzn_graphics_pipeline *>(state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].pipeline);
   dzn_batch *batch = get_batch();

   state.sysvals.gfx.first_vertex = first_vertex;
   state.sysvals.gfx.base_instance = first_instance;
   state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].dirty |=
      DZN_CMD_BINDPOINT_DIRTY_SYSVALS;

   if (pipeline->ia.triangle_fan) {
      D3D12_INDEX_BUFFER_VIEW ib_view = state.ib.view;

      triangle_fan_create_index(vertex_count);
      if (!vertex_count)
         return;

      state.sysvals.gfx.is_indexed_draw = true;
      prepare_draw(true);
      batch->cmdlist->DrawIndexedInstanced(vertex_count, instance_count, 0,
                                           first_vertex, first_instance);

      /* Restore the IB view if we modified it when lowering triangle fans. */
      if (ib_view.SizeInBytes > 0) {
         state.ib.view = ib_view;
         state.dirty |= DZN_CMD_DIRTY_IB;
      }
   } else {
      state.sysvals.gfx.is_indexed_draw = false;
      prepare_draw(false);
      batch->cmdlist->DrawInstanced(vertex_count, instance_count,
                                    first_vertex, first_instance);
   }
}

void
dzn_cmd_buffer::draw(uint32_t index_count,
                     uint32_t instance_count,
                     uint32_t first_index,
                     int32_t vertex_offset,
                     uint32_t first_instance)
{
   const dzn_graphics_pipeline *pipeline =
      reinterpret_cast<const dzn_graphics_pipeline *>(state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].pipeline);
   dzn_batch *batch = get_batch();

   state.sysvals.gfx.first_vertex = vertex_offset;
   state.sysvals.gfx.base_instance = first_instance;
   state.sysvals.gfx.is_indexed_draw = true;
   state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].dirty |=
      DZN_CMD_BINDPOINT_DIRTY_SYSVALS;

   D3D12_INDEX_BUFFER_VIEW ib_view = state.ib.view;

   if (pipeline->ia.triangle_fan) {
      triangle_fan_rewrite_index(index_count, first_index);
      if (!index_count)
         return;
   }

   prepare_draw(true);
   batch->cmdlist->DrawIndexedInstanced(index_count, instance_count, first_index,
                                        vertex_offset, first_instance);

   /* Restore the IB view if we modified it when lowering triangle fans. */
   if (pipeline->ia.triangle_fan && ib_view.SizeInBytes) {
      state.ib.view = ib_view;
      state.dirty |= DZN_CMD_DIRTY_IB;
   }
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdDraw(VkCommandBuffer commandBuffer,
            uint32_t vertexCount,
            uint32_t instanceCount,
            uint32_t firstVertex,
            uint32_t firstInstance)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->draw(vertexCount, instanceCount, firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                   uint32_t indexCount,
                   uint32_t instanceCount,
                   uint32_t firstIndex,
                   int32_t vertexOffset,
                   uint32_t firstInstance)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->draw(indexCount, instanceCount, firstIndex, vertexOffset,
                    firstInstance);
}

ID3D12Resource *
dzn_cmd_buffer::alloc_internal_buf(uint32_t size,
                                   D3D12_HEAP_TYPE heap_type,
                                   D3D12_RESOURCE_STATES init_state)
{
   ComPtr<ID3D12Resource> res;

   /* Align size on 64k (the default alignment) */
   size = ALIGN_POT(size, 64 * 1024);

   D3D12_HEAP_PROPERTIES hprops =
      device->dev->GetCustomHeapProperties(0, heap_type);
   D3D12_RESOURCE_DESC rdesc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
      .Width = size,
      .Height = 1,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = DXGI_FORMAT_UNKNOWN,
      .SampleDesc = { .Count = 1, .Quality = 0 },
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
   };

   HRESULT hres =
      device->dev->CreateCommittedResource(&hprops, D3D12_HEAP_FLAG_NONE, &rdesc,
                                           init_state,
                                           NULL, IID_PPV_ARGS(&res));
   assert(!FAILED(hres));

   internal_bufs.push_back(res);
   return res.Get();
}

uint32_t
dzn_cmd_buffer::triangle_fan_get_max_index_buf_size(bool indexed)
{
   dzn_graphics_pipeline *pipeline =
      reinterpret_cast<dzn_graphics_pipeline *>(state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].pipeline);

   if (!pipeline->ia.triangle_fan)
      return 0;

   uint32_t max_triangles;

   if (indexed) {
      uint32_t index_size = state.ib.view.Format == DXGI_FORMAT_R32_UINT ? 4 : 2;
      uint32_t max_indices = state.ib.view.SizeInBytes / index_size;

      max_triangles = MAX2(max_indices, 2) - 2;
   } else {
      uint32_t max_vertex = 0;
      for (uint32_t i = 0; i < pipeline->vb.count; i++) {
         max_vertex =
            MAX2(max_vertex,
                 state.vb.views[i].SizeInBytes / state.vb.views[i].StrideInBytes);
      }

      max_triangles = MAX2(max_vertex, 2) - 2;
   }

   return max_triangles * 3;
}

void
dzn_cmd_buffer::draw(dzn_buffer *draw_buf,
                     size_t draw_buf_offset,
                     uint32_t draw_count,
                     uint32_t draw_buf_stride,
                     bool indexed)
{
   dzn_graphics_pipeline *pipeline =
      reinterpret_cast<dzn_graphics_pipeline *>(state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].pipeline);
   bool triangle_fan = pipeline->ia.triangle_fan;
   dzn_batch *batch = get_batch();
   uint32_t min_draw_buf_stride =
      indexed ?
      sizeof(struct dzn_indirect_indexed_draw_params) :
      sizeof(struct dzn_indirect_draw_params);

   draw_buf_stride = draw_buf_stride ? draw_buf_stride : min_draw_buf_stride;
   assert(draw_buf_stride >= min_draw_buf_stride);
   assert((draw_buf_stride & 3) == 0);

   uint32_t sysvals_stride = ALIGN_POT(sizeof(state.sysvals.gfx), 256);
   uint32_t exec_buf_stride = 32;
   uint32_t triangle_fan_index_buf_stride =
      triangle_fan_get_max_index_buf_size(indexed) * sizeof(uint32_t);
   uint32_t triangle_fan_exec_buf_stride =
      sizeof(struct dzn_indirect_triangle_fan_rewrite_index_exec_params);
   ID3D12Resource *exec_buf =
      alloc_internal_buf(draw_count * exec_buf_stride,
                         D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   D3D12_GPU_VIRTUAL_ADDRESS draw_buf_gpu =
      draw_buf->res->GetGPUVirtualAddress() + draw_buf_offset;
   ID3D12Resource *triangle_fan_index_buf =
      triangle_fan_index_buf_stride ?
      alloc_internal_buf(draw_count * triangle_fan_index_buf_stride,
                         D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS) :
      NULL;
   ID3D12Resource *triangle_fan_exec_buf =
      triangle_fan_index_buf_stride ?
      alloc_internal_buf(draw_count * triangle_fan_exec_buf_stride,
                         D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS) :
      NULL;

   struct dzn_indirect_draw_triangle_fan_rewrite_params params = {
      .draw_buf_stride = draw_buf_stride,
      .triangle_fan_index_buf_stride = triangle_fan_index_buf_stride,
      .triangle_fan_index_buf_start =
         triangle_fan_index_buf ?
         triangle_fan_index_buf->GetGPUVirtualAddress() : 0,
   };
   uint32_t params_size =
      triangle_fan_index_buf_stride > 0 ?
      sizeof(struct dzn_indirect_draw_triangle_fan_rewrite_params) :
      sizeof(struct dzn_indirect_draw_rewrite_params);

   enum dzn_indirect_draw_type draw_type;

   if (indexed && triangle_fan_index_buf_stride > 0)
      draw_type = DZN_INDIRECT_INDEXED_DRAW_TRIANGLE_FAN;
   else if (!indexed && triangle_fan_index_buf_stride > 0)
      draw_type = DZN_INDIRECT_DRAW_TRIANGLE_FAN;
   else if (indexed)
      draw_type = DZN_INDIRECT_INDEXED_DRAW;
   else
      draw_type = DZN_INDIRECT_DRAW;

   dzn_meta_indirect_draw *indirect_draw =
      device->indirect_draws[draw_type].get();

   const dzn_pipeline *compute_pipeline =
      state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].pipeline;

   batch->cmdlist->SetComputeRootSignature(indirect_draw->root_sig.Get());
   batch->cmdlist->SetPipelineState(indirect_draw->pipeline_state.Get());
   batch->cmdlist->SetComputeRoot32BitConstants(0, params_size / 4, (const void *)&params, 0);
   batch->cmdlist->SetComputeRootShaderResourceView(1, draw_buf_gpu);
   batch->cmdlist->SetComputeRootUnorderedAccessView(2, exec_buf->GetGPUVirtualAddress());
   if (triangle_fan_exec_buf)
      batch->cmdlist->SetComputeRootUnorderedAccessView(3, triangle_fan_exec_buf->GetGPUVirtualAddress());

   batch->cmdlist->Dispatch(draw_count, 1, 1);

   D3D12_RESOURCE_BARRIER post_barriers[] = {
      {
         .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
         .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          /* Transition the exec buffer to indirect arg so it can be
           * pass to ExecuteIndirect() as an argument buffer.
           */
         .Transition = {
            .pResource = exec_buf,
            .Subresource = 0,
            .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            .StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
         },
      },
      {
         .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
         .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          /* Transition the exec buffer to indirect arg so it can be
           * pass to ExecuteIndirect() as an argument buffer.
           */
         .Transition = {
            .pResource = triangle_fan_exec_buf,
            .Subresource = 0,
            .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            .StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
         },
      },
   };

   uint32_t post_barrier_count = triangle_fan_exec_buf ? 2 : 1;

   batch->cmdlist->ResourceBarrier(post_barrier_count, post_barriers);

   D3D12_INDEX_BUFFER_VIEW ib_view = {};

   if (triangle_fan_exec_buf) {
      auto index_type =
         indexed ?
         dzn_meta_triangle_fan_rewrite_index::get_index_type(state.ib.view.Format) :
         dzn_meta_triangle_fan_rewrite_index::NO_INDEX;
      dzn_meta_triangle_fan_rewrite_index *rewrite_index =
         device->triangle_fan[index_type].get();

      struct dzn_triangle_fan_rewrite_index_params rewrite_index_params = {};

      batch->cmdlist->SetComputeRootSignature(rewrite_index->root_sig.Get());
      batch->cmdlist->SetPipelineState(rewrite_index->pipeline_state.Get());
      batch->cmdlist->SetComputeRootUnorderedAccessView(0, triangle_fan_index_buf->GetGPUVirtualAddress());
      batch->cmdlist->SetComputeRoot32BitConstants(1, sizeof(rewrite_index_params) / 4,
                                                   (const void *)&rewrite_index_params, 0);

      if (indexed)
         batch->cmdlist->SetComputeRootShaderResourceView(2, state.ib.view.BufferLocation);

      ID3D12CommandSignature *cmd_sig = rewrite_index->cmd_sig.Get();
      batch->cmdlist->ExecuteIndirect(cmd_sig,
                                      draw_count, triangle_fan_exec_buf,
                                      0, NULL, 0);

      D3D12_RESOURCE_BARRIER index_buf_barriers[] = {
         {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .Transition = {
               .pResource = triangle_fan_index_buf,
               .Subresource = 0,
               .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               .StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER,
            },
         },
      };

      batch->cmdlist->ResourceBarrier(ARRAY_SIZE(index_buf_barriers), index_buf_barriers);

      /* After our triangle-fan lowering the draw is indexed */
      indexed = true;
      ib_view = state.ib.view;
      state.ib.view.BufferLocation = triangle_fan_index_buf->GetGPUVirtualAddress();
      state.ib.view.SizeInBytes = triangle_fan_index_buf_stride;
      state.ib.view.Format = DXGI_FORMAT_R32_UINT;
      state.dirty |= DZN_CMD_DIRTY_IB;
   }

   /* We don't mess up with the driver state when executing our internal
    * compute shader, but we still change the D3D12 state, so let's mark
    * things dirty if needed.
    */
   state.pipeline = NULL;
   if (state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].pipeline) {
      state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].dirty |=
         DZN_CMD_BINDPOINT_DIRTY_PIPELINE;
   }

   state.sysvals.gfx.first_vertex = 0;
   state.sysvals.gfx.base_instance = 0;
   state.sysvals.gfx.is_indexed_draw = indexed;
   state.bindpoint[VK_PIPELINE_BIND_POINT_GRAPHICS].dirty |=
      DZN_CMD_BINDPOINT_DIRTY_SYSVALS;

   prepare_draw(indexed);

   /* Restore the old IB view if we modified it during the triangle fan lowering */
   if (ib_view.SizeInBytes) {
      state.ib.view = ib_view;
      state.dirty |= DZN_CMD_DIRTY_IB;
   }

   dzn_graphics_pipeline::indirect_cmd_sig_type cmd_sig_type =
      triangle_fan_index_buf_stride > 0 ?
      dzn_graphics_pipeline::INDIRECT_DRAW_TRIANGLE_FAN_CMD_SIG :
      indexed ?
      dzn_graphics_pipeline::INDIRECT_INDEXED_DRAW_CMD_SIG :
      dzn_graphics_pipeline::INDIRECT_DRAW_CMD_SIG;
   ID3D12CommandSignature *cmdsig =
      pipeline->get_indirect_cmd_sig(cmd_sig_type);

   batch->cmdlist->ExecuteIndirect(cmdsig,
                                   draw_count, exec_buf, 0, NULL, 0);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                    VkBuffer buffer,
                    VkDeviceSize offset,
                    uint32_t drawCount,
                    uint32_t stride)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_buffer, buf, buffer);

   cmd_buffer->draw(buf, offset, drawCount, stride, false);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                           VkBuffer buffer,
                           VkDeviceSize offset,
                           uint32_t drawCount,
                           uint32_t stride)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_buffer, buf, buffer);

   cmd_buffer->draw(buf, offset, drawCount, stride, true);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                         uint32_t firstBinding,
                         uint32_t bindingCount,
                         const VkBuffer *pBuffers,
                         const VkDeviceSize *pOffsets)
{
   if (!bindingCount)
      return;

   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   D3D12_VERTEX_BUFFER_VIEW *vbviews = cmd_buffer->state.vb.views;

   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(dzn_buffer, buf, pBuffers[i]);

      vbviews[firstBinding + i].BufferLocation = buf->res->GetGPUVirtualAddress() + pOffsets[i];
      vbviews[firstBinding + i].SizeInBytes = buf->size - pOffsets[i];
   }

   BITSET_SET_RANGE(cmd_buffer->state.vb.dirty, firstBinding,
                    firstBinding + bindingCount - 1);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                       VkBuffer buffer,
                       VkDeviceSize offset,
                       VkIndexType indexType)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_buffer, buf, buffer);

   cmd_buffer->state.ib.view.BufferLocation = buf->res->GetGPUVirtualAddress() + offset;
   cmd_buffer->state.ib.view.SizeInBytes = buf->size - offset;
   switch (indexType) {
   case VK_INDEX_TYPE_UINT16:
      cmd_buffer->state.ib.view.Format = DXGI_FORMAT_R16_UINT;
      break;
   case VK_INDEX_TYPE_UINT32:
      cmd_buffer->state.ib.view.Format = DXGI_FORMAT_R32_UINT;
      break;
   case VK_INDEX_TYPE_UINT8_EXT:
      cmd_buffer->state.ib.view.Format = DXGI_FORMAT_R8_UINT;
      break;
   default: unreachable("Invalid index type");
   }

   cmd_buffer->state.dirty |= DZN_CMD_DIRTY_IB;
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdResetEvent(VkCommandBuffer commandBuffer,
                  VkEvent _event,
                  VkPipelineStageFlags stageMask)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_event, event, _event);

   struct dzn_cmd_event_signal signal = {
      .event = event,
      .value = false,
   };

   dzn_batch *batch = cmd_buffer->get_batch(true);

   batch->signal.push_back(signal);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdSetEvent(VkCommandBuffer commandBuffer,
                VkEvent _event,
                VkPipelineStageFlags stageMask)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(dzn_event, event, _event);

   struct dzn_cmd_event_signal signal = {
      .event = event,
      .value = true,
   };

   dzn_batch *batch = cmd_buffer->get_batch(true);

   batch->signal.push_back(signal);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdWaitEvents(VkCommandBuffer commandBuffer,
                  uint32_t eventCount,
                  const VkEvent *pEvents,
                  VkPipelineStageFlags srcStageMask,
                  VkPipelineStageFlags dstStageMask,
                  uint32_t memoryBarrierCount,
                  const VkMemoryBarrier *pMemoryBarriers,
                  uint32_t bufferMemoryBarrierCount,
                  const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                  uint32_t imageMemoryBarrierCount,
                  const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   dzn_batch *batch = cmd_buffer->get_batch();

   for (uint32_t i = 0; i < eventCount; i++) {
      VK_FROM_HANDLE(dzn_event, event, pEvents[i]);

      batch->wait.push_back(event);
   }
}

void
dzn_cmd_buffer::prepare_dispatch()
{
   update_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE);
   update_heaps(VK_PIPELINE_BIND_POINT_COMPUTE);
   update_sysvals(VK_PIPELINE_BIND_POINT_COMPUTE);

   /* Reset the dirty states */
   state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].dirty = 0;
}

void
dzn_cmd_buffer::dispatch(uint32_t group_count_x,
                         uint32_t group_count_y,
                         uint32_t group_count_z)
{
   dzn_batch *batch = get_batch();

   state.sysvals.compute.group_count_x = group_count_x;
   state.sysvals.compute.group_count_y = group_count_y;
   state.sysvals.compute.group_count_z = group_count_z;
   state.bindpoint[VK_PIPELINE_BIND_POINT_COMPUTE].dirty |=
      DZN_CMD_BINDPOINT_DIRTY_SYSVALS;

   prepare_dispatch();
   batch->cmdlist->Dispatch(group_count_x, group_count_y, group_count_z);
}

VKAPI_ATTR void VKAPI_CALL
dzn_CmdDispatch(VkCommandBuffer commandBuffer,
                uint32_t groupCountX,
                uint32_t groupCountY,
                uint32_t groupCountZ)
{
   VK_FROM_HANDLE(dzn_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->dispatch(groupCountX, groupCountY, groupCountZ);
}
