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
#include <math.h>

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
   WSI_GET_CB(GetPhysicalDeviceProperties);
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

   VkPhysicalDeviceProperties properties;
   GetPhysicalDeviceProperties(pdevice, &properties);
   wsi->timestamp_period = properties.limits.timestampPeriod;

#define WSI_GET_CB(func) \
   wsi->func = (PFN_vk##func)proc_addr(pdevice, "vk" #func)
   WSI_GET_CB(AllocateMemory);
   WSI_GET_CB(AllocateCommandBuffers);
   WSI_GET_CB(BindBufferMemory);
   WSI_GET_CB(BindImageMemory);
   WSI_GET_CB(BeginCommandBuffer);
   WSI_GET_CB(CmdCopyImageToBuffer);
   WSI_GET_CB(CmdResetQueryPool);
   WSI_GET_CB(CmdWriteTimestamp);
   WSI_GET_CB(CreateBuffer);
   WSI_GET_CB(CreateCommandPool);
   WSI_GET_CB(CreateFence);
   WSI_GET_CB(CreateImage);
   WSI_GET_CB(CreateQueryPool);
   WSI_GET_CB(DestroyBuffer);
   WSI_GET_CB(DestroyCommandPool);
   WSI_GET_CB(DestroyFence);
   WSI_GET_CB(DestroyImage);
   WSI_GET_CB(DestroyQueryPool);
   WSI_GET_CB(EndCommandBuffer);
   WSI_GET_CB(FreeMemory);
   WSI_GET_CB(FreeCommandBuffers);
   WSI_GET_CB(GetBufferMemoryRequirements);
   WSI_GET_CB(GetImageDrmFormatModifierPropertiesEXT);
   WSI_GET_CB(GetImageMemoryRequirements);
   WSI_GET_CB(GetImageSubresourceLayout);
   if (!wsi->sw)
      WSI_GET_CB(GetMemoryFdKHR);
   WSI_GET_CB(GetPhysicalDeviceProperties);
   WSI_GET_CB(GetPhysicalDeviceFormatProperties);
   WSI_GET_CB(GetPhysicalDeviceFormatProperties2KHR);
   WSI_GET_CB(GetPhysicalDeviceImageFormatProperties2);
   WSI_GET_CB(GetPhysicalDeviceQueueFamilyProperties);
   WSI_GET_CB(GetQueryPoolResults);
   WSI_GET_CB(ResetFences);
   WSI_GET_CB(QueueSubmit);
   WSI_GET_CB(GetCalibratedTimestampsEXT);
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

bool
wsi_init_pthread_cond_monotonic(pthread_cond_t *cond)
{
   pthread_condattr_t condattr;
   bool ret = false;

   if (pthread_condattr_init(&condattr) != 0)
      goto fail_attr_init;

   if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0)
      goto fail_attr_set;

   if (pthread_cond_init(cond, &condattr) != 0)
      goto fail_cond_init;

   ret = true;

fail_cond_init:
fail_attr_set:
   pthread_condattr_destroy(&condattr);
fail_attr_init:
   return ret;
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
   chain->use_prime_blit = false;
   chain->timing_insert = 0;
   chain->timing_count = 0;

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

VkResult
wsi_image_init_timestamp(const struct wsi_swapchain *chain,
                         struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;
   /* Set up command buffer to get timestamp info */

   result = wsi->CreateQueryPool(
      chain->device,
      &(const VkQueryPoolCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 1,
            },
      NULL,
      &image->query_pool);

   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->AllocateCommandBuffers(
      chain->device,
      &(const VkCommandBufferAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = NULL,
            .commandPool = chain->cmd_pools[0],
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
            },
      &image->timestamp_buffer);
   if (result != VK_SUCCESS)
      goto fail;

   wsi->BeginCommandBuffer(
      image->timestamp_buffer,
      &(VkCommandBufferBeginInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = 0
            });

   wsi->CmdResetQueryPool(image->timestamp_buffer,
                          image->query_pool,
                          0, 1);

   wsi->CmdWriteTimestamp(image->timestamp_buffer,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          image->query_pool,
                          0);

   wsi->EndCommandBuffer(image->timestamp_buffer);

   return VK_SUCCESS;
fail:
   return result;
}

void
wsi_destroy_image(const struct wsi_swapchain *chain,
                  struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;

   if (image->prime.blit_cmd_buffers) {
      for (uint32_t i = 0; i < wsi->queue_family_count; i++) {
         wsi->FreeCommandBuffers(chain->device, chain->cmd_pools[i],
                                 1, &image->prime.blit_cmd_buffers[i]);
      }
      vk_free(&chain->alloc, image->prime.blit_cmd_buffers);
   }

   wsi->FreeMemory(chain->device, image->memory, &chain->alloc);
   wsi->DestroyImage(chain->device, image->image, &chain->alloc);
   wsi->FreeMemory(chain->device, image->prime.memory, &chain->alloc);
   wsi->DestroyBuffer(chain->device, image->prime.buffer, &chain->alloc);
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

static struct wsi_timing *
wsi_get_timing(struct wsi_swapchain *chain, uint32_t i)
{
   uint32_t j = WSI_TIMING_HISTORY + chain->timing_insert -
      chain->timing_count + i;

   if (j >= WSI_TIMING_HISTORY)
      j -= WSI_TIMING_HISTORY;
   return &chain->timing[j];
}

static struct wsi_timing *
wsi_next_timing(struct wsi_swapchain *chain, int image_index)
{
   uint32_t j = chain->timing_insert;
   ++chain->timing_insert;
   if (chain->timing_insert >= WSI_TIMING_HISTORY)
      chain->timing_insert = 0;
   if (chain->timing_count < WSI_TIMING_HISTORY)
      ++chain->timing_count;
   struct wsi_timing *timing = &chain->timing[j];
   memset(timing, '\0', sizeof (*timing));
   return timing;
}

void
wsi_present_complete(struct wsi_swapchain *swapchain,
                     struct wsi_image *image,
                     uint64_t ust,
                     uint64_t msc)
{
   const struct wsi_device *wsi = swapchain->wsi;
   struct wsi_timing *timing = image->timing;

   if (!timing)
      return;

   uint64_t render_timestamp;

   VkResult result = wsi->GetQueryPoolResults(
      swapchain->device, image->query_pool,
      0, 1, sizeof(render_timestamp), &render_timestamp,
      sizeof (uint64_t),
      VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
   if (result != VK_SUCCESS)
      return;

   static const VkCalibratedTimestampInfoEXT    timestampInfo[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
         .pNext = NULL,
         .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT,
      },
      {
         .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
         .pNext = NULL,
         .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
      },
   };

   uint64_t     timestamps[2];
   uint64_t     maxDeviation;

   result = wsi->GetCalibratedTimestampsEXT(swapchain->device,
                                            2,
                                            timestampInfo,
                                            timestamps,
                                            &maxDeviation);
   if (result != VK_SUCCESS)
      return;

   uint64_t current_gpu_timestamp = timestamps[0];
   uint64_t current_time = timestamps[1];

   VkRefreshCycleDurationGOOGLE display_timings;
   swapchain->get_refresh_cycle_duration(swapchain, &display_timings);

   uint64_t refresh_duration = display_timings.refreshDuration;

   /* When did drawing complete (in nsec) */

   int64_t since_render = (int64_t) floor ((double) (current_gpu_timestamp - render_timestamp) *
                                           (double) wsi->timestamp_period + 0.5);
   uint64_t render_time = current_time - since_render;

   if (render_time > ust)
      render_time = ust;

   uint64_t render_frames = (ust - render_time) / refresh_duration;

   uint64_t earliest_time = ust - render_frames * refresh_duration;

   /* Use the presentation mode to figure out when the image could have been
    * displayed. It couldn't have been displayed before the previous image, so
    * use that as a lower bound. If we're in FIFO mode, then it couldn't have
    * been displayed before one frame *after* the previous image
    */
   uint64_t possible_frame = swapchain->frame_ust;

   switch (swapchain->present_mode) {
   case VK_PRESENT_MODE_FIFO_KHR:
   case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
      possible_frame += refresh_duration;
      break;
   default:
      break;
   }
   if (earliest_time < possible_frame)
      earliest_time = possible_frame;

   if (earliest_time > ust)
      earliest_time = ust;

   timing->timing.actualPresentTime = ust;
   timing->timing.earliestPresentTime = earliest_time;
   timing->timing.presentMargin = earliest_time - render_time;
   timing->complete = true;

   swapchain->frame_msc = msc;
   swapchain->frame_ust = ust;
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
   const VkPresentTimesInfoGOOGLE *present_times_info =
      vk_find_struct_const(pPresentInfo->pNext, PRESENT_TIMES_INFO_GOOGLE);

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      VK_FROM_HANDLE(wsi_swapchain, swapchain, pPresentInfo->pSwapchains[i]);
      uint32_t image_index = pPresentInfo->pImageIndices[i];
      VkResult result;
      struct wsi_timing *timing = NULL;

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

      VkCommandBuffer submit_buffers[2];
      VkSubmitInfo submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .pNext = &mem_signal,
         .pCommandBuffers = submit_buffers,
         .commandBufferCount = 0
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

      if (swapchain->use_prime_blit) {
         /* If we are using prime blits, we need to perform the blit now.  The
          * command buffer is attached to the image.
          */
         mem_signal.memory = image->prime.memory;
         submit_buffers[submit_info.commandBufferCount++] =
            image->prime.blit_cmd_buffers[queue_family_index];
      }

      /* Set up GOOGLE_display_timing bits */
      if (present_times_info &&
          present_times_info->pTimes != NULL &&
          i < present_times_info->swapchainCount)
      {
         const VkPresentTimeGOOGLE *present_time =
            &present_times_info->pTimes[i];

         timing = wsi_next_timing(swapchain, pPresentInfo->pImageIndices[i]);
         timing->timing.presentID = present_time->presentID;
         timing->timing.desiredPresentTime = present_time->desiredPresentTime;
         timing->target_msc = 0;
         image->timing = timing;

         if (present_time->desiredPresentTime != 0)
         {
            int64_t delta_nsec = (int64_t) (present_time->desiredPresentTime -
                                            swapchain->frame_ust);

            /* Set the target msc only if it's no more than two seconds from
             * now, and not stale
             */
            if (0 <= delta_nsec && delta_nsec <= 2000000000ul) {
               VkRefreshCycleDurationGOOGLE refresh_timing;

               swapchain->get_refresh_cycle_duration(swapchain,
                                                     &refresh_timing);

               int64_t refresh = (int64_t) refresh_timing.refreshDuration;
               int64_t frames = (delta_nsec + refresh - 1) / refresh;
               timing->target_msc = swapchain->frame_msc + frames;
            }
         }

         submit_buffers[submit_info.commandBufferCount++] =
            image->timestamp_buffer;
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

VkResult
wsi_common_get_refresh_cycle_duration(
   const struct wsi_device *wsi,
   VkDevice device_h,
   VkSwapchainKHR _swapchain,
   VkRefreshCycleDurationGOOGLE *pDisplayTimingProperties)
{
   VK_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);

   if (!swapchain->get_refresh_cycle_duration)
      return VK_ERROR_EXTENSION_NOT_PRESENT;
   return swapchain->get_refresh_cycle_duration(swapchain,
                                                pDisplayTimingProperties);
}


VkResult
wsi_common_get_past_presentation_timing(
   const struct wsi_device *wsi,
   VkDevice device_h,
   VkSwapchainKHR _swapchain,
   uint32_t *count,
   VkPastPresentationTimingGOOGLE *timings)
{
   VK_FROM_HANDLE(wsi_swapchain, swapchain, _swapchain);
   uint32_t timing_count_requested = *count;
   uint32_t timing_count_available = 0;

   /* Count the number of completed entries, copy */
   for (uint32_t t = 0; t < swapchain->timing_count; t++) {
      struct wsi_timing *timing = wsi_get_timing(swapchain, t);

      if (timing->complete && !timing->consumed) {
         if (timings && timing_count_available < timing_count_requested) {
            timings[timing_count_available] = timing->timing;
            timing->consumed = true;
         }
         timing_count_available++;
      }
   }

   *count = timing_count_available;

   if (timing_count_available > timing_count_requested && timings != NULL)
      return VK_INCOMPLETE;

   return VK_SUCCESS;
}
