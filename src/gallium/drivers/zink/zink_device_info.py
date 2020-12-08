# Copyright © 2020 Hoe Hao Cheng
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
# 
# Authors:
#    Hoe Hao Cheng <haochengho12907@gmail.com>
# 

from mako.template import Template
from mako.lookup import TemplateLookup
from os import path
import re
import sys

# constructor: 
#     Extensions(name, alias="", required=False, properties=False, features=False, conditions=None, guard=False)
# The attributes:
#  - required: the generated code debug_prints "ZINK: {name} required!" and
#              returns NULL if the extension is unavailable.
#
#  - properties: enable the detection of extension properties in a physical
#                device in the generated code using vkGetPhysicalDeviceProperties2(),
#                and store the returned properties struct inside
#                `zink_device_info.{alias}_props`.
#                Example: the properties for `VK_EXT_transform_feedback`, is stored in
#                `VkPhysicalDeviceTransformFeedbackPropertiesEXT tf_props`.
#
#  - features: enable the getting extension features in a
#              device. Similar to `properties`, this stores the features
#              struct inside `zink_device_info.{alias}_feats`.
#
#  - conditions: criteria for enabling an extension. This is an array of strings,
#                where each string is a condition, and all conditions have to be true
#                for `zink_device_info.have_{name}` to be true.
#
#                The code generator will replace "$feats" and "$props" with the
#                respective variables, e.g. "$feats.nullDescriptor" becomes 
#                "info->rb2_feats.nullDescriptor" in the final code for VK_EXT_robustness2.
#
#                When empty or None, the extension is enabled when the extensions
#                given by vkEnumerateDeviceExtensionProperties() include the extension.
#
#  - guard: adds a #if defined(`extension_name`)/#endif guard around the code generated for this Extension.
def EXTENSIONS():
    return [
        Extension("VK_KHR_maintenance1",
            required=True),
        Extension("VK_KHR_external_memory"),
        Extension("VK_KHR_external_memory_fd"),
        Extension("VK_KHR_vulkan_memory_model"),
        Extension("VK_EXT_conditional_rendering",
            alias="cond_render", 
            features=True, 
            conditions=["$feats.conditionalRendering"]),
        Extension("VK_EXT_transform_feedback",
            alias="tf",
            properties=True,
            features=True,
            conditions=["$feats.transformFeedback"]),
        Extension("VK_EXT_index_type_uint8",
            alias="index_uint8",
            features=True,
            conditions=["$feats.indexTypeUint8"]),
        Extension("VK_EXT_robustness2",
            alias="rb2",
            properties=True,
            features=True,
            conditions=["$feats.nullDescriptor"]),
        Extension("VK_EXT_vertex_attribute_divisor",
            alias="vdiv", 
            properties=True,
            features=True,
            conditions=["$feats.vertexAttributeInstanceRateDivisor"]),
        Extension("VK_EXT_calibrated_timestamps"),
        Extension("VK_EXT_custom_border_color",
            alias="border_color",
            properties=True,
            features=True,
            conditions=["$feats.customBorderColors"]),
        Extension("VK_EXT_blend_operation_advanced",
            alias="blend",
            properties=True,
            # TODO: we can probably support non-premul here with some work?
            conditions=["$props.advancedBlendNonPremultipliedSrcColor", "$props.advancedBlendNonPremultipliedDstColor"]),
        Extension("VK_EXT_extended_dynamic_state",
            alias="dynamic_state",
            features=True,
            conditions=["$feats.extendedDynamicState"]),
        Extension("VK_EXT_pipeline_creation_cache_control",
            alias="pipeline_cache_control",
            features=True,
            conditions=["$feats.pipelineCreationCacheControl"]),
        Extension("VK_EXT_shader_stencil_export",
            alias="stencil_export"),
        Extension("VK_EXTX_portability_subset",
            alias="portability_subset_extx",
            properties=True,
            features=True,
            guard=True),
    ]

# constructor: Versions(device_version(major, minor, patch), struct_version(major, minor))
# The attributes:
#  - device_version: Vulkan version, as tuple, to use with VK_MAKE_VERSION(version_major, version_minor, version_patch)
#
#  - struct_version: Vulkan version, as tuple, to use with structures and macros
def VERSIONS():
    return [
        # VkPhysicalDeviceVulkan11Properties and VkPhysicalDeviceVulkan11Features is new from Vk 1.2, not Vk 1.1
        # https://www.khronos.org/registry/vulkan/specs/1.2-extensions/html/vkspec.html#_new_structures
        Version((1,2,0), (1,1)),
        Version((1,2,0), (1,2)),
    ]

# There exists some inconsistencies regarding the enum constants, fix them.
# This is basically generated_code.replace(key, value).
def REPLACEMENTS():
    return {
        "ROBUSTNESS2": "ROBUSTNESS_2"
    }


# This template provides helper functions for the other templates.
# Right now, the following functions are defined:
# - guard(ext) : surrounds the body with an if-def guard according to
#                `ext.extension_name()` if `ext.guard` is True.
include_template = """
<%def name="guard_(ext, body)">
%if ext.guard:
#ifdef ${ext.extension_name()}
%endif
   ${capture(body)|trim}
%if ext.guard:
#endif
%endif
</%def>

## This ugliness is here to prevent mako from adding tons of excessive whitespace
<%def name="guard(ext)">${capture(guard_, ext, body=caller.body).strip('\\r\\n')}</%def>
"""

header_code = """
<%namespace name="helpers" file="helpers"/>

#ifndef ZINK_DEVICE_INFO_H
#define ZINK_DEVICE_INFO_H

#include "util/u_memory.h"

#include <vulkan/vulkan.h>

#if defined(__APPLE__)
// Source of MVK_VERSION
// Source of VK_EXTX_PORTABILITY_SUBSET_EXTENSION_NAME
#include "MoltenVK/vk_mvk_moltenvk.h"
#endif

struct zink_screen;

struct zink_device_info {
   uint32_t device_version;

%for ext in extensions:
<%helpers:guard ext="${ext}">
   bool have_${ext.name_with_vendor()};
</%helpers:guard>
%endfor
%for version in versions:
   bool have_vulkan${version.struct()};
%endfor

   VkPhysicalDeviceFeatures2 feats;
%for version in versions:
   VkPhysicalDeviceVulkan${version.struct()}Features feats${version.struct()};
%endfor

   VkPhysicalDeviceProperties props;
%for version in versions:
   VkPhysicalDeviceVulkan${version.struct()}Properties props${version.struct()};
%endfor

   VkPhysicalDeviceMemoryProperties mem_props;

%for ext in extensions:
<%helpers:guard ext="${ext}">
%if ext.has_features:
   VkPhysicalDevice${ext.name_in_camel_case()}Features${ext.vendor()} ${ext.field("feats")};
%endif
%if ext.has_properties:
   VkPhysicalDevice${ext.name_in_camel_case()}Properties${ext.vendor()} ${ext.field("props")};
%endif
</%helpers:guard>
%endfor

    const char *extensions[${len(extensions)}];
    uint32_t num_extensions;
};

bool
zink_get_physical_device_info(struct zink_screen *screen);

#endif
"""


impl_code = """
<%namespace name="helpers" file="helpers"/>

#include "zink_device_info.h"
#include "zink_screen.h"

bool
zink_get_physical_device_info(struct zink_screen *screen) 
{
   struct zink_device_info *info = &screen->info;
%for ext in extensions:
<%helpers:guard ext="${ext}">
   bool support_${ext.name_with_vendor()} = false;
</%helpers:guard>
%endfor
   uint32_t num_extensions = 0;

   // get device API support
   vkGetPhysicalDeviceProperties(screen->pdev, &info->props);
   info->device_version = info->props.apiVersion;

   // get device memory properties
   vkGetPhysicalDeviceMemoryProperties(screen->pdev, &info->mem_props);

   // enumerate device supported extensions
   if (vkEnumerateDeviceExtensionProperties(screen->pdev, NULL, &num_extensions, NULL) == VK_SUCCESS) {
      if (num_extensions > 0) {
         VkExtensionProperties *extensions = MALLOC(sizeof(VkExtensionProperties) * num_extensions);
         if (!extensions) goto fail;
         vkEnumerateDeviceExtensionProperties(screen->pdev, NULL, &num_extensions, extensions);

         for (uint32_t i = 0; i < num_extensions; ++i) {
         %for ext in extensions:
         <%helpers:guard ext="${ext}">
            if (!strcmp(extensions[i].extensionName, "${ext.name}")) {
               support_${ext.name_with_vendor()} = true;
            }
         </%helpers:guard>
         %endfor
         }

         FREE(extensions);
      }
   }

   // get device features
   if (screen->vk_GetPhysicalDeviceFeatures2) {
      // check for device extension features
      info->feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

%for version in versions:
      if (${version.version()} <= info->device_version) {
         info->feats${version.struct()}.sType = ${version.stype("FEATURES")};
         info->feats${version.struct()}.pNext = info->feats.pNext;
         info->feats.pNext = &info->feats${version.struct()};
         info->have_vulkan${version.struct()} = true;
      }
%endfor

%for ext in extensions:
%if ext.has_features:
<%helpers:guard ext="${ext}">
      if (support_${ext.name_with_vendor()}) {
         info->${ext.field("feats")}.sType = ${ext.stype("FEATURES")};
         info->${ext.field("feats")}.pNext = info->feats.pNext;
         info->feats.pNext = &info->${ext.field("feats")};
      }
</%helpers:guard>
%endif
%endfor

      screen->vk_GetPhysicalDeviceFeatures2(screen->pdev, &info->feats);
   } else {
      vkGetPhysicalDeviceFeatures(screen->pdev, &info->feats.features);
   }

%for ext in extensions:
<%helpers:guard ext="${ext}">
%if ext.has_features == False:
   info->have_${ext.name_with_vendor()} = support_${ext.name_with_vendor()};
%endif
</%helpers:guard>
%endfor

   // check for device properties
   if (screen->vk_GetPhysicalDeviceProperties2) {
      VkPhysicalDeviceProperties2 props = {};
      props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

%for version in versions:
      if (${version.version()} <= info->device_version) {
         info->props${version.struct()}.sType = ${version.stype("PROPERTIES")};
         info->props${version.struct()}.pNext = props.pNext;
         props.pNext = &info->props${version.struct()};
      }
%endfor

%for ext in extensions:
%if ext.has_properties:
<%helpers:guard ext="${ext}">
      if (info->have_${ext.name_with_vendor()}) {
         info->${ext.field("props")}.sType = ${ext.stype("PROPERTIES")};
         info->${ext.field("props")}.pNext = props.pNext;
         props.pNext = &info->${ext.field("props")};
      }
</%helpers:guard>
%endif
%endfor

      // note: setting up local VkPhysicalDeviceProperties2.
      screen->vk_GetPhysicalDeviceProperties2(screen->pdev, &props);
   }

   // enable the extensions if they match the conditions given by ext.enable_conds 
   if (screen->vk_GetPhysicalDeviceProperties2) {
        %for ext in extensions:
<%helpers:guard ext="${ext}">
<%
    conditions = ""
    if ext.enable_conds:
        for cond in ext.enable_conds:
            cond = cond.replace("$feats", "info->" + ext.field("feats"))
            cond = cond.replace("$props", "info->" + ext.field("props"))
            conditions += "&& (" + cond + ")\\n"
    conditions = conditions.strip()
%>\
      info->have_${ext.name_with_vendor()} = support_${ext.name_with_vendor()}
         ${conditions};
</%helpers:guard>
        %endfor
   }

   // generate extension list
   num_extensions = 0;

%for ext in extensions:
<%helpers:guard ext="${ext}">
   if (info->have_${ext.name_with_vendor()}) {
       info->extensions[num_extensions++] = "${ext.name}";
%if ext.is_required:
   } else {
       debug_printf("ZINK: ${ext.name} required!\\n");
       goto fail;
%endif
   }
</%helpers:guard>
%endfor

   info->num_extensions = num_extensions;

   return true;

fail:
   return false;
}
"""

class Version:
    driver_version  : (1,0,0)
    struct_version  : (1,0)

    def __init__(self, version, struct):
        self.device_version = version
        self.struct_version = struct

    # e.g. "VM_MAKE_VERSION(1,2,0)"
    def version(self):
        return ("VK_MAKE_VERSION("
               + str(self.device_version[0])
               + ","
               + str(self.device_version[1])
               + ","
               + str(self.device_version[2])
               + ")")

    # e.g. "10"
    def struct(self):
        return (str(self.struct_version[0])+str(self.struct_version[1]))

    # the sType of the extension's struct
    # e.g. VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT
    # for VK_EXT_transform_feedback and struct="FEATURES"
    def stype(self, struct: str):
        return ("VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_"
                + str(self.struct_version[0]) + "_" + str(self.struct_version[1])
                + '_' + struct)

class Extension:
    name           : str   = None
    alias          : str   = None
    is_required    : bool  = False
    has_properties : bool  = False
    has_features   : bool  = False
    enable_conds   : [str] = None
    guard          : bool  = False

    def __init__(self, name, alias="", required=False, properties=False, features=False, conditions=None, guard=False):
        self.name = name
        self.alias = alias
        self.is_required = required
        self.has_properties = properties
        self.has_features = features
        self.enable_conds = conditions
        self.guard = guard

        if alias == "" and (properties == True or features == True):
            raise RuntimeError("alias must be available when properties and/or features are used")

    # e.g.: "VK_EXT_robustness2" -> "robustness2"
    def pure_name(self):
        return '_'.join(self.name.split('_')[2:])
    
    # e.g.: "VK_EXT_robustness2" -> "EXT_robustness2"
    def name_with_vendor(self):
        return self.name[3:]
    
    # e.g.: "VK_EXT_robustness2" -> "Robustness2"
    def name_in_camel_case(self):
        return "".join([x.title() for x in self.name.split('_')[2:]])
    
    # e.g.: "VK_EXT_robustness2" -> "VK_EXT_ROBUSTNESS2_EXTENSION_NAME"
    # do note that inconsistencies exist, i.e. we have
    # VK_EXT_ROBUSTNESS_2_EXTENSION_NAME defined in the headers, but then
    # we also have VK_KHR_MAINTENANCE1_EXTENSION_NAME
    def extension_name(self):
        return self.name.upper() + "_EXTENSION_NAME"

    # generate a C string literal for the extension
    def extension_name_literal(self):
        return '"' + self.name + '"'

    # get the field in zink_device_info that refers to the extension's
    # feature/properties struct
    # e.g. rb2_<suffix> for VK_EXT_robustness2
    def field(self, suffix: str):
        return self.alias + '_' + suffix

    # the sType of the extension's struct
    # e.g. VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT
    # for VK_EXT_transform_feedback and struct="FEATURES"
    def stype(self, struct: str):
        return ("VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_" 
                + self.pure_name().upper()
                + '_' + struct + '_' 
                + self.vendor())

    # e.g. EXT in VK_EXT_robustness2
    def vendor(self):
        return self.name.split('_')[1]


def replace_code(code: str, replacement: dict):
    for (k, v) in replacement.items():
        code = code.replace(k, v)
    
    return code


if __name__ == "__main__":
    try:
        header_path = sys.argv[1]
        impl_path = sys.argv[2]

        header_path = path.abspath(header_path)
        impl_path = path.abspath(impl_path)
    except:
        print("usage: %s <path to .h> <path to .c>" % sys.argv[0])
        exit(1)

    extensions = EXTENSIONS()
    versions = VERSIONS()
    replacement = REPLACEMENTS()

    lookup = TemplateLookup()
    lookup.put_string("helpers", include_template)

    with open(header_path, "w") as header_file:
        header = Template(header_code, lookup=lookup).render(extensions=extensions, versions=versions).strip()
        header = replace_code(header, replacement)
        print(header, file=header_file)

    with open(impl_path, "w") as impl_file:
        impl = Template(impl_code, lookup=lookup).render(extensions=extensions, versions=versions).strip()
        impl = replace_code(impl, replacement)
        print(impl, file=impl_file)
