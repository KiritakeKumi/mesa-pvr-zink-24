/*
 * Copyright © 2017 Intel Corporation
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

#include "wsi_common_private.h"
#include "util/macros.h"
#include "util/os_file.h"
#include "util/os_time.h"
#include "util/xmlconfig.h"
#include "vk_util.h"

#include <time.h>
#include <stdlib.h>
#include <stdio.h>

VkResult
wsi_device_init(struct wsi_device *wsi,
                VkPhysicalDevice pdevice,
                WSI_FN_GetPhysicalDeviceProcAddr proc_addr,
                const VkAllocationCallbacks *alloc,
                int display_fd,
                const struct driOptionCache *dri_options,
                bool sw_device)
{
   const char *present_mode;
   UNUSED VkResult result;

   memset(wsi, 0, sizeof(*wsi));

   wsi->instance_alloc = *alloc;
   wsi->pdevice = pdevice;
   wsi->sw = sw_device;
#define WSI_GET_CB(func) \
   PFN_vk##func func = (PFN_vk##func)proc_addr(pdevice, "vk" #func)
   WSI_GET_CB(GetPhysicalDeviceProperties2);
   WSI_GET_CB(GetPhysicalDeviceMemoryProperties);
   WSI_GET_CB(GetPhysicalDeviceQueueFamilyProperties);
#undef WSI_GET_CB

   wsi->pci_bus_info.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
   VkPhysicalDeviceProperties2 pdp2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &wsi->pci_bus_info,
   };
   GetPhysicalDeviceProperties2(pdevice, &pdp2);

   wsi->maxImageDimension2D = pdp2.properties.limits.maxImageDimension2D;
   wsi->override_present_mode = VK_PRESENT_MODE_MAX_ENUM_KHR;

   GetPhysicalDeviceMemoryProperties(pdevice, &wsi->memory_props);
   GetPhysicalDeviceQueueFamilyProperties(pdevice, &wsi->queue_family_count, NULL);

#define WSI_GET_CB(func) \
   wsi->func = (PFN_vk##func)proc_addr(pdevice, "vk" #func)
   WSI_GET_CB(AllocateMemory);
   WSI_GET_CB(AllocateCommandBuffers);
   WSI_GET_CB(BindBufferMemory);
   WSI_GET_CB(BindImageMemory);
   WSI_GET_CB(BeginCommandBuffer);
   WSI_GET_CB(CmdPipelineBarrier);
   WSI_GET_CB(CmdCopyImageToBuffer);
   WSI_GET_CB(CreateBuffer);
   WSI_GET_CB(CreateCommandPool);
   WSI_GET_CB(CreateFence);
   WSI_GET_CB(CreateImage);
   WSI_GET_CB(DestroyBuffer);
   WSI_GET_CB(DestroyCommandPool);
   WSI_GET_CB(DestroyFence);
   WSI_GET_CB(DestroyImage);
   WSI_GET_CB(EndCommandBuffer);
   WSI_GET_CB(FreeMemory);
   WSI_GET_CB(FreeCommandBuffers);
   WSI_GET_CB(GetBufferMemoryRequirements);
   WSI_GET_CB(GetImageDrmFormatModifierPropertiesEXT);
   WSI_GET_CB(GetImageMemoryRequirements);
   WSI_GET_CB(GetImageSubresourceLayout);
   if (!wsi->sw)
      WSI_GET_CB(GetMemoryFdKHR);
   WSI_GET_CB(GetPhysicalDeviceFormatProperties);
   WSI_GET_CB(GetPhysicalDeviceFormatProperties2KHR);
   WSI_GET_CB(GetPhysicalDeviceImageFormatProperties2);
   WSI_GET_CB(ResetFences);
   WSI_GET_CB(QueueSubmit);
   WSI_GET_CB(WaitForFences);
   WSI_GET_CB(MapMemory);
   WSI_GET_CB(UnmapMemory);
#undef WSI_GET_CB

#ifdef VK_USE_PLATFORM_XCB_KHR
   result = wsi_x11_init_wsi(wsi, alloc, dri_options);
   if (result != VK_SUCCESS)
      goto fail;
#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   result = wsi_wl_init_wsi(wsi, alloc, pdevice);
   if (result != VK_SUCCESS)
      goto fail;
#endif

#ifdef VK_USE_PLATFORM_WIN32_KHR
   result = wsi_win32_init_wsi(wsi, alloc, pdevice);
   if (result != VK_SUCCESS)
      goto fail;
#endif

#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   result = wsi_display_init_wsi(wsi, alloc, display_fd);
   if (result != VK_SUCCESS)
      goto fail;
#endif

   present_mode = getenv("MESA_VK_WSI_PRESENT_MODE");
   if (present_mode) {
      if (!strcmp(present_mode, "fifo")) {
         wsi->override_present_mode = VK_PRESENT_MODE_FIFO_KHR;
      } else if (!strcmp(present_mode, "relaxed")) {
          wsi->override_present_mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      } else if (!strcmp(present_mode, "mailbox")) {
         wsi->override_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
      } else if (!strcmp(present_mode, "immediate")) {
         wsi->override_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
      } else {
         fprintf(stderr, "Invalid MESA_VK_WSI_PRESENT_MODE value!\n");
      }
   }

   if (dri_options) {
      if (driCheckOption(dri_options, "adaptive_sync", DRI_BOOL))
         wsi->enable_adaptive_sync = driQueryOptionb(dri_options,
                                                     "adaptive_sync");

      if (driCheckOption(dri_options, "vk_wsi_force_bgra8_unorm_first",  DRI_BOOL)) {
         wsi->force_bgra8_unorm_first =
            driQueryOptionb(dri_options, "vk_wsi_force_bgra8_unorm_first");
      }
   }

   return VK_SUCCESS;
#if defined(VK_USE_PLATFORM_XCB_KHR) || \
   defined(VK_USE_PLATFORM_WAYLAND_KHR) || \
   defined(VK_USE_PLATFORM_WIN32_KHR) || \
   defined(VK_USE_PLATFORM_DISPLAY_KHR)
fail:
   wsi_device_finish(wsi, alloc);
   return result;
#endif
}

void
wsi_device_finish(struct wsi_device *wsi,
                  const VkAllocationCallbacks *alloc)
{
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   wsi_display_finish_wsi(wsi, alloc);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   wsi_wl_finish_wsi(wsi, alloc);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
   wsi_win32_finish_wsi(wsi, alloc);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   wsi_x11_finish_wsi(wsi, alloc);
#endif
}

VkResult
wsi_swapchain_init(const struct wsi_device *wsi,
                   struct wsi_swapchain *chain,
                   VkDevice device,
                   const VkSwapchainCreateInfoKHR *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator)
{
   VkResult result;

   memset(chain, 0, sizeof(*chain));

   vk_object_base_init(NULL, &chain->base, VK_OBJECT_TYPE_SWAPCHAIN_KHR);

   chain->wsi = wsi;
   chain->device = device;
   chain->alloc = *pAllocator;
   chain->use_buffer_blit = false;

   chain->cmd_pools =
      vk_zalloc(pAllocator, sizeof(VkCommandPool) * wsi->queue_family_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!chain->cmd_pools)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   for (uint32_t i = 0; i < wsi->queue_family_count; i++) {
      const VkCommandPoolCreateInfo cmd_pool_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .pNext = NULL,
         .flags = 0,
         .queueFamilyIndex = i,
      };
      result = wsi->CreateCommandPool(device, &cmd_pool_info, &chain->alloc,
                                      &chain->cmd_pools[i]);
      if (result != VK_SUCCESS)
         goto fail;
   }

   return VK_SUCCESS;

fail:
   wsi_swapchain_finish(chain);
   return result;
}

static bool
wsi_swapchain_is_present_mode_supported(struct wsi_device *wsi,
                                        const VkSwapchainCreateInfoKHR *pCreateInfo,
                                        VkPresentModeKHR mode)
{
      ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
      struct wsi_interface *iface = wsi->wsi[surface->platform];
      VkPresentModeKHR *present_modes;
      uint32_t present_mode_count;
      bool supported = false;
      VkResult result;

      result = iface->get_present_modes(surface, &present_mode_count, NULL);
      if (result != VK_SUCCESS)
         return supported;

      present_modes = malloc(present_mode_count * sizeof(*present_modes));
      if (!present_modes)
         return supported;

      result = iface->get_present_modes(surface, &present_mode_count,
                                        present_modes);
      if (result != VK_SUCCESS)
         goto fail;

      for (uint32_t i = 0; i < present_mode_count; i++) {
         if (present_modes[i] == mode) {
            supported = true;
            break;
         }
      }

fail:
      free(present_modes);
      return supported;
}

enum VkPresentModeKHR
wsi_swapchain_get_present_mode(struct wsi_device *wsi,
                               const VkSwapchainCreateInfoKHR *pCreateInfo)
{
   if (wsi->override_present_mode == VK_PRESENT_MODE_MAX_ENUM_KHR)
      return pCreateInfo->presentMode;

   if (!wsi_swapchain_is_present_mode_supported(wsi, pCreateInfo,
                                                wsi->override_present_mode)) {
      fprintf(stderr, "Unsupported MESA_VK_WSI_PRESENT_MODE value!\n");
      return pCreateInfo->presentMode;
   }

   return wsi->override_present_mode;
}

void
wsi_swapchain_finish(struct wsi_swapchain *chain)
{
   if (chain->fences) {
      for (unsigned i = 0; i < chain->image_count; i++)
         chain->wsi->DestroyFence(chain->device, chain->fences[i], &chain->alloc);

      vk_free(&chain->alloc, chain->fences);
   }

   for (uint32_t i = 0; i < chain->wsi->queue_family_count; i++) {
      chain->wsi->DestroyCommandPool(chain->device, chain->cmd_pools[i],
                                     &chain->alloc);
   }
   vk_free(&chain->alloc, chain->cmd_pools);

   vk_object_base_finish(&chain->base);
}

void
wsi_destroy_image(const struct wsi_swapchain *chain,
                  struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;

   if (image->buffer.blit_cmd_buffers) {
      for (uint32_t i = 0; i < wsi->queue_family_count; i++) {
         wsi->FreeCommandBuffers(chain->device, chain->cmd_pools[i],
                                 1, &image->buffer.blit_cmd_buffers[i]);
      }
      vk_free(&chain->alloc, image->buffer.blit_cmd_buffers);
   }

   wsi->FreeMemory(chain->device, image->memory, &chain->alloc);
   wsi->DestroyImage(chain->device, image->image, &chain->alloc);
   wsi->FreeMemory(chain->device, image->buffer.memory, &chain->alloc);
   wsi->DestroyBuffer(chain->device, image->buffer.buffer, &chain->alloc);
}

VkResult
wsi_common_get_surface_support(struct wsi_device *wsi_device,
                               uint32_t queueFamilyIndex,
                               VkSurfaceKHR _surface,
                               VkBool32* pSupported)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_support(surface, wsi_device,
                             queueFamilyIndex, pSupported);
}

VkResult
wsi_common_get_surface_capabilities(struct wsi_device *wsi_device,
                                    VkSurfaceKHR _surface,
                                    VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   VkSurfaceCapabilities2KHR caps2 = {
      .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
   };

   VkResult result = iface->get_capabilities2(surface, wsi_device, NULL, &caps2);

   if (result == VK_SUCCESS)
      *pSurfaceCapabilities = caps2.surfaceCapabilities;

   return result;
}

VkResult
wsi_common_get_surface_capabilities2(struct wsi_device *wsi_device,
                                     const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                     VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pSurfaceInfo->surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_capabilities2(surface, wsi_device, pSurfaceInfo->pNext,
                                   pSurfaceCapabilities);
}

VkResult
wsi_common_get_surface_capabilities2ext(
   struct wsi_device *wsi_device,
   VkSurfaceKHR _surface,
   VkSurfaceCapabilities2EXT *pSurfaceCapabilities)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   assert(pSurfaceCapabilities->sType ==
          VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT);

   struct wsi_surface_supported_counters counters = {
      .sType = VK_STRUCTURE_TYPE_WSI_SURFACE_SUPPORTED_COUNTERS_MESA,
      .pNext = pSurfaceCapabilities->pNext,
      .supported_surface_counters = 0,
   };

   VkSurfaceCapabilities2KHR caps2 = {
      .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
      .pNext = &counters,
   };

   VkResult result = iface->get_capabilities2(surface, wsi_device, NULL, &caps2);

   if (result == VK_SUCCESS) {
      VkSurfaceCapabilities2EXT *ext_caps = pSurfaceCapabilities;
      VkSurfaceCapabilitiesKHR khr_caps = caps2.surfaceCapabilities;

      ext_caps->minImageCount = khr_caps.minImageCount;
      ext_caps->maxImageCount = khr_caps.maxImageCount;
      ext_caps->currentExtent = khr_caps.currentExtent;
      ext_caps->minImageExtent = khr_caps.minImageExtent;
      ext_caps->maxImageExtent = khr_caps.maxImageExtent;
      ext_caps->maxImageArrayLayers = khr_caps.maxImageArrayLayers;
      ext_caps->supportedTransforms = khr_caps.supportedTransforms;
      ext_caps->currentTransform = khr_caps.currentTransform;
      ext_caps->supportedCompositeAlpha = khr_caps.supportedCompositeAlpha;
      ext_caps->supportedUsageFlags = khr_caps.supportedUsageFlags;
      ext_caps->supportedSurfaceCounters = counters.supported_surface_counters;
   }

   return result;
}

VkResult
wsi_common_get_surface_formats(struct wsi_device *wsi_device,
                               VkSurfaceKHR _surface,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormatKHR *pSurfaceFormats)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_formats(surface, wsi_device,
                             pSurfaceFormatCount, pSurfaceFormats);
}

VkResult
wsi_common_get_surface_formats2(struct wsi_device *wsi_device,
                                const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormat2KHR *pSurfaceFormats)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pSurfaceInfo->surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_formats2(surface, wsi_device, pSurfaceInfo->pNext,
                              pSurfaceFormatCount, pSurfaceFormats);
}

VkResult
wsi_common_get_surface_present_modes(struct wsi_device *wsi_device,
                                     VkSurfaceKHR _surface,
                                     uint32_t *pPresentModeCount,
                                     VkPresentModeKHR *pPresentModes)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_present_modes(surface, pPresentModeCount,
                                   pPresentModes);
}

VkResult
wsi_common_get_present_rectangles(struct wsi_device *wsi_device,
                                  VkSurfaceKHR _surface,
                                  uint32_t* pRectCount,
                                  VkRect2D* pRects)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, _surface);
   struct wsi_interface *iface = wsi_device->wsi[surface->platform];

   return iface->get_present_rectangles(surface, wsi_device,
                                        pRectCount, pRects);
}

VkResult
wsi_common_create_swapchain(struct wsi_device *wsi,
                            VkDevice device,
                            const VkSwapchainCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSwapchainKHR *pSwapchain)
{
   ICD_FROM_HANDLE(VkIcdSurfaceBase, surface, pCreateInfo->surface);
   struct wsi_interface *iface = wsi->wsi[surface->platform];
   struct wsi_swapchain *swapchain;

   VkResult result = iface->create_swapchain(surface, device, wsi,
                                             pCreateInfo, pAllocator,
                                             &swapchain);
   if (result != VK_SUCCESS)
      return result;

   swapchain->fences = vk_zalloc(pAllocator,
                                 sizeof (*swapchain->fences) * swapchain->image_count,
                                 sizeof (*swapchain->fences),
                                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!swapchain->fences) {
      swapchain->destroy(swapchain, pAllocator);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *pSwapchain = wsi_swapchain_to_handle(swapchain);

   return VK_SUCCESS;
}

void
wsi_common_destroy_swapchain(VkDevice device,
                             VkSwapchainKHR _swapchain,
                             const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   if (!swapchain)
      return;

   swapchain->destroy(swapchain, pAllocator);
}

VkResult
wsi_common_get_images(VkSwapchainKHR _swapchain,
                      uint32_t *pSwapchainImageCount,
                      VkImage *pSwapchainImages)
{
   VK_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   VK_OUTARRAY_MAKE_TYPED(VkImage, images, pSwapchainImages, pSwapchainImageCount);

   for (uint32_t i = 0; i < swapchain->image_count; i++) {
      vk_outarray_append_typed(VkImage, &images, image) {
         *image = swapchain->get_wsi_image(swapchain, i)->image;
      }
   }

   return vk_outarray_status(&images);
}

VkResult
wsi_common_acquire_next_image2(const struct wsi_device *wsi,
                               VkDevice device,
                               const VkAcquireNextImageInfoKHR *pAcquireInfo,
                               uint32_t *pImageIndex)
{
   VK_FROM_HANDLE(wsi_swapchain, swapchain, pAcquireInfo->swapchain);

   VkResult result = swapchain->acquire_next_image(swapchain, pAcquireInfo,
                                                   pImageIndex);
   if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
      return result;

   if (wsi->set_memory_ownership) {
      VkDeviceMemory mem = swapchain->get_wsi_image(swapchain, *pImageIndex)->memory;
      wsi->set_memory_ownership(swapchain->device, mem, true);
   }

   if (pAcquireInfo->semaphore != VK_NULL_HANDLE &&
       wsi->signal_semaphore_for_memory != NULL) {
      struct wsi_image *image =
         swapchain->get_wsi_image(swapchain, *pImageIndex);
      wsi->signal_semaphore_for_memory(device, pAcquireInfo->semaphore,
                                       image->memory);
   }

   if (pAcquireInfo->fence != VK_NULL_HANDLE &&
       wsi->signal_fence_for_memory != NULL) {
      struct wsi_image *image =
         swapchain->get_wsi_image(swapchain, *pImageIndex);
      wsi->signal_fence_for_memory(device, pAcquireInfo->fence,
                                   image->memory);
   }

   return result;
}

VkResult
wsi_common_queue_present(const struct wsi_device *wsi,
                         VkDevice device,
                         VkQueue queue,
                         int queue_family_index,
                         const VkPresentInfoKHR *pPresentInfo)
{
   VkResult final_result = VK_SUCCESS;

   const VkPresentRegionsKHR *regions =
      vk_find_struct_const(pPresentInfo->pNext, PRESENT_REGIONS_KHR);

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      VK_FROM_HANDLE(wsi_swapchain, swapchain, pPresentInfo->pSwapchains[i]);
      uint32_t image_index = pPresentInfo->pImageIndices[i];
      VkResult result;

      if (swapchain->fences[image_index] == VK_NULL_HANDLE) {
         const VkFenceCreateInfo fence_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
         };
         result = wsi->CreateFence(device, &fence_info,
                                   &swapchain->alloc,
                                   &swapchain->fences[image_index]);
         if (result != VK_SUCCESS)
            goto fail_present;
      } else {
         result =
            wsi->WaitForFences(device, 1, &swapchain->fences[image_index],
                               true, ~0ull);
         if (result != VK_SUCCESS)
            goto fail_present;

         result =
            wsi->ResetFences(device, 1, &swapchain->fences[image_index]);
         if (result != VK_SUCCESS)
            goto fail_present;
      }

      struct wsi_image *image =
         swapchain->get_wsi_image(swapchain, image_index);

      struct wsi_memory_signal_submit_info mem_signal = {
         .sType = VK_STRUCTURE_TYPE_WSI_MEMORY_SIGNAL_SUBMIT_INFO_MESA,
         .pNext = NULL,
         .memory = image->memory,
      };

      VkSubmitInfo submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = &mem_signal,
      };

      VkPipelineStageFlags *stage_flags = NULL;
      if (i == 0) {
         /* We only need/want to wait on semaphores once.  After that, we're
          * guaranteed ordering since it all happens on the same queue.
          */
         submit_info.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
         submit_info.pWaitSemaphores = pPresentInfo->pWaitSemaphores;

         /* Set up the pWaitDstStageMasks */
         stage_flags = vk_alloc(&swapchain->alloc,
                                sizeof(VkPipelineStageFlags) *
                                pPresentInfo->waitSemaphoreCount,
                                8,
                                VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
         if (!stage_flags) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail_present;
         }
         for (uint32_t s = 0; s < pPresentInfo->waitSemaphoreCount; s++)
            stage_flags[s] = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;

         submit_info.pWaitDstStageMask = stage_flags;
      }

      if (swapchain->use_buffer_blit) {
         /* If we are using buffer blits, we need to perform the blit now.  The
          * command buffer is attached to the image.
          */
         submit_info.commandBufferCount = 1;
         submit_info.pCommandBuffers =
            &image->buffer.blit_cmd_buffers[queue_family_index];
         mem_signal.memory = image->buffer.memory;
      }

      result = wsi->QueueSubmit(queue, 1, &submit_info, swapchain->fences[image_index]);
      vk_free(&swapchain->alloc, stage_flags);
      if (result != VK_SUCCESS)
         goto fail_present;

      const VkPresentRegionKHR *region = NULL;
      if (regions && regions->pRegions)
         region = &regions->pRegions[i];

      result = swapchain->queue_present(swapchain, image_index, region);
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
         goto fail_present;

      if (wsi->set_memory_ownership) {
         VkDeviceMemory mem = swapchain->get_wsi_image(swapchain, image_index)->memory;
         wsi->set_memory_ownership(swapchain->device, mem, false);
      }

   fail_present:
      if (pPresentInfo->pResults != NULL)
         pPresentInfo->pResults[i] = result;

      /* Let the final result be our first unsuccessful result */
      if (final_result == VK_SUCCESS)
         final_result = result;
   }

   return final_result;
}

uint64_t
wsi_common_get_current_time(void)
{
   return os_time_get_nano();
}

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

static uint32_t
vk_format_size(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return 4;
   default:
      unreachable("Unknown WSI Format");
   }
}

static uint32_t
select_memory_type(const struct wsi_device *wsi,
                   bool want_device_local,
                   uint32_t type_bits)
{
   assert(type_bits);

   bool all_local = true;
   for (uint32_t i = 0; i < wsi->memory_props.memoryTypeCount; i++) {
       const VkMemoryType type = wsi->memory_props.memoryTypes[i];
       bool local = type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

       if ((type_bits & (1 << i)) && local == want_device_local)
         return i;
       all_local &= local;
   }

   /* ignore want_device_local when all memory types are device-local */
   if (all_local) {
      assert(!want_device_local);
      return ffs(type_bits) - 1;
   }

   unreachable("No memory type found");
}

VkResult
wsi_create_buffer_image(const struct wsi_swapchain *chain,
                        const VkSwapchainCreateInfoKHR *pCreateInfo,
                        struct wsi_image *image,
                        unsigned stride_align,
                        unsigned size_align,
                        const void *buffer_create_extra,
                        const void *buffer_memory_extra)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   memset(image, 0, sizeof(*image));

   const uint32_t cpp = vk_format_size(pCreateInfo->imageFormat);
   const uint32_t linear_stride = align_u32(pCreateInfo->imageExtent.width * cpp,
                                            stride_align);

   uint32_t linear_size = linear_stride * pCreateInfo->imageExtent.height;
   linear_size = align_u32(linear_size, size_align);

   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = buffer_create_extra,
      .size = linear_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result = wsi->CreateBuffer(chain->device, &buffer_info,
                              &chain->alloc, &image->buffer.buffer);
   if (result != VK_SUCCESS)
      goto fail;

   VkMemoryRequirements reqs;
   wsi->GetBufferMemoryRequirements(chain->device, image->buffer.buffer, &reqs);
   assert(reqs.size <= linear_size);

   const VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = buffer_memory_extra,
      .image = VK_NULL_HANDLE,
      .buffer = image->buffer.buffer,
   };
   const VkMemoryAllocateInfo buffer_memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &buffer_memory_dedicated_info,
      .allocationSize = linear_size,
      .memoryTypeIndex = select_memory_type(wsi, false, reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &buffer_memory_info,
                                &chain->alloc, &image->buffer.memory);
   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->BindBufferMemory(chain->device, image->buffer.buffer,
                                  image->buffer.memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   const struct wsi_image_create_info image_wsi_info = {
      .sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA,
      .prime_blit_src = true,
   };
   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = pCreateInfo->imageFormat,
      .extent = {
         .width = pCreateInfo->imageExtent.width,
         .height = pCreateInfo->imageExtent.height,
         .depth = 1,
      },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = pCreateInfo->imageSharingMode,
      .queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount,
      .pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };

   /* only set prime_blit_src if this is a DMABUF handle */
   const VkExportMemoryAllocateInfo *export_memory_alloc_info =
      vk_find_struct_const(buffer_memory_extra, EXPORT_MEMORY_ALLOCATE_INFO);
   if (export_memory_alloc_info && (export_memory_alloc_info->handleTypes &
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
      image_info.pNext = &image_wsi_info;

   result = wsi->CreateImage(chain->device, &image_info,
                             &chain->alloc, &image->image);
   if (result != VK_SUCCESS)
      goto fail;

   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = NULL,
      .image = image->image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &memory_dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex = select_memory_type(wsi, true, reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->BindImageMemory(chain->device, image->image,
                                 image->memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   image->buffer.blit_cmd_buffers =
      vk_zalloc(&chain->alloc,
                sizeof(VkCommandBuffer) * wsi->queue_family_count, 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!image->buffer.blit_cmd_buffers) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   for (uint32_t i = 0; i < wsi->queue_family_count; i++) {
      const VkCommandBufferAllocateInfo cmd_buffer_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .pNext = NULL,
         .commandPool = chain->cmd_pools[i],
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      result = wsi->AllocateCommandBuffers(chain->device, &cmd_buffer_info,
                                           &image->buffer.blit_cmd_buffers[i]);
      if (result != VK_SUCCESS)
         goto fail;

      const VkCommandBufferBeginInfo begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      };
      wsi->BeginCommandBuffer(image->buffer.blit_cmd_buffers[i], &begin_info);

      VkImageMemoryBarrier img_mem_barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .pNext = NULL,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = image->image,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
      };
      wsi->CmdPipelineBarrier(image->buffer.blit_cmd_buffers[i],
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0,
                              0, NULL,
                              0, NULL,
                              1, &img_mem_barrier);

      struct VkBufferImageCopy buffer_image_copy = {
         .bufferOffset = 0,
         .bufferRowLength = linear_stride / vk_format_size(pCreateInfo->imageFormat),
         .bufferImageHeight = 0,
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .imageOffset = { .x = 0, .y = 0, .z = 0 },
         .imageExtent = {
            .width = pCreateInfo->imageExtent.width,
            .height = pCreateInfo->imageExtent.height,
            .depth = 1,
         },
      };
      wsi->CmdCopyImageToBuffer(image->buffer.blit_cmd_buffers[i],
                                image->image,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                image->buffer.buffer,
                                1, &buffer_image_copy);

      img_mem_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      img_mem_barrier.dstAccessMask = 0;
      img_mem_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      img_mem_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      wsi->CmdPipelineBarrier(image->buffer.blit_cmd_buffers[i],
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              0,
                              0, NULL,
                              0, NULL,
                              1, &img_mem_barrier);

      result = wsi->EndCommandBuffer(image->buffer.blit_cmd_buffers[i]);
      if (result != VK_SUCCESS)
         goto fail;
   }

   image->num_planes = 1;
   image->sizes[0] = linear_size;
   image->row_pitches[0] = linear_stride;
   image->offsets[0] = 0;

   return VK_SUCCESS;

fail:
   wsi_destroy_image(chain, image);

   return result;
}
