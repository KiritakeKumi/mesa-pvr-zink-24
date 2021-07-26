/*
 * Copyright © 2021 Intel Corporation
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

#include "vk_log.h"
#include "vk_debug_utils.h"
#include "vk_debug_report.h"

#include "vk_command_buffer.h"
#include "vk_queue.h"
#include "vk_device.h"
#include "vk_physical_device.h"

#include "log.h"

VkDebugUtilsObjectNameInfoEXT*
__vk_log_objs(int object_count, ...)
{
   if (object_count == 0)
      return NULL;

   VkDebugUtilsObjectNameInfoEXT *objects =
      malloc(object_count * sizeof(VkDebugUtilsObjectNameInfoEXT));

   va_list object_list;
   va_start(object_list, object_count);

   for (int i = 0; i < object_count; i++) {
      struct vk_object_base *base =
         va_arg(object_list, struct vk_object_base *);

      objects[i] = (VkDebugUtilsObjectNameInfoEXT) {
         .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
         .pNext = NULL,
         .objectType = base->type,
         .objectHandle = (uint64_t)(uintptr_t)base,
         .pObjectName = base->object_name,
      };
   }

   va_end(object_list);

   return objects;
}

void
__vk_log_impl(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT types,
              int object_count,
              void *objects_or_instance,
              const char *file,
              int line,
              const char *format,
              ...)
{
   struct vk_instance *instance = NULL;
   VkDebugUtilsObjectNameInfoEXT *objects = NULL;
   if (object_count == 0) {
      instance = objects_or_instance;
   } else {
      objects = objects_or_instance;
      struct vk_object_base *base =
         vk_object_base_from_u64_handle(objects[0].objectHandle,
                                        objects[0].objectType);
      instance = base->device->physical->instance;
   }

   va_list va;
   char *message = NULL;

   va_start(va, format);
   vasprintf(&message, format, va);
   va_end(va);

   char *message_idname = NULL;
   asprintf(&message_idname, "%s:%d", file, line);

   #if DEBUG
   switch (severity) {
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
      mesa_logd("%s: %s", message_idname, message);
      break;
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
      mesa_logi("%s: %s", message_idname, message);
      break;
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
      if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
         mesa_logw("%s: PERF: %s", message_idname, message);
      else
         mesa_logw("%s: %s", message_idname, message);
      break;
   case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
      mesa_loge("%s: %s", message_idname, message);
      break;
   default:
      unreachable("Invalid debug message severity");
      break;
   }
   #endif

   if (!instance) {
      free(message);
      free(message_idname);
      return;
   }

   /* If VK_EXT_debug_utils messengers have been set up, form the
    * message */
   if (!list_is_empty(&instance->debug_utils.callbacks)) {
      VkDebugUtilsMessengerCallbackDataEXT cbData = { 0 };
      cbData.sType =
         VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
      cbData.pMessageIdName = message_idname;
      cbData.messageIdNumber = 0;
      cbData.pMessage = message;

      for (int i = 0; i < object_count; i++) {
         struct vk_object_base *base =
            vk_object_base_from_u64_handle(objects[i].objectHandle,
                                           objects[i].objectType);
         switch (base->type) {
         case VK_OBJECT_TYPE_COMMAND_BUFFER: {
            struct vk_command_buffer *cmd_buffer =
               (struct vk_command_buffer *)base;
            if (cmd_buffer->labels.size > 0) {
               cbData.cmdBufLabelCount =
                  util_dynarray_num_elements(&cmd_buffer->labels,
                                             VkDebugUtilsLabelEXT);
               cbData.pCmdBufLabels = cmd_buffer->labels.data;
            }
            break;
         }

         case VK_OBJECT_TYPE_QUEUE: {
            struct vk_queue *queue =
               (struct vk_queue *) base;
            if (queue->labels.size > 0) {
               cbData.queueLabelCount = util_dynarray_num_elements(
                  &queue->labels, VkDebugUtilsLabelEXT);
               cbData.pQueueLabels = queue->labels.data;
            }
            break;
         }
         default:
            break;
         }
      }
      cbData.objectCount = object_count;
      cbData.pObjects = objects;

      vk_debug_utils(instance, severity, types, &cbData);

      if (object_count > 0)
         free(objects);
   }

   /* If VK_EXT_debug_report callbacks also have been set up, forward
    * the message there as well */
   if (!list_is_empty(&instance->debug_report.callbacks)) {
      VkDebugReportFlagsEXT flags = 0;

      switch (severity) {
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
         flags |= VK_DEBUG_REPORT_DEBUG_BIT_EXT;
         break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
         flags |= VK_DEBUG_REPORT_INFORMATION_BIT_EXT;
         break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
         flags |= VK_DEBUG_REPORT_WARNING_BIT_EXT;
         break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
         flags |= VK_DEBUG_REPORT_ERROR_BIT_EXT;
         break;
      default:
         unreachable("Invalid debug message severity");
         break;
      }

      if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) &&
          (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT))
         flags |= VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;

      /* VK_EXT_debug_report-provided callback accepts only one object
       * related to the message. Since they are given to us in
       * decreasing order of importance, we're forwarding the first
       * one. */
      struct vk_object_base *base = NULL;
      if (object_count > 0) {
         base = vk_object_base_from_u64_handle(objects[0].objectHandle,
                                               objects[0].objectType);
      }

      vk_debug_report(instance, flags, base, 0,
                      0, message_idname, message);
   }

   free(message);
   free(message_idname);
}
