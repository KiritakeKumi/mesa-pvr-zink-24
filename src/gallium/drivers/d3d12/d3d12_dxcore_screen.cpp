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

#include "d3d12_screen.h"

#include "util/debug.h"
#include "util/u_memory.h"
#include "util/u_dl.h"

#include <directx/dxcore.h>
#include <dxguids/dxguids.h>

static IDXCoreAdapterFactory *
get_dxcore_factory()
{
   typedef HRESULT(WINAPI *PFN_CREATE_DXCORE_ADAPTER_FACTORY)(REFIID riid, void **ppFactory);
   PFN_CREATE_DXCORE_ADAPTER_FACTORY DXCoreCreateAdapterFactory;

   util_dl_library *dxcore_mod = util_dl_open(UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT);
   if (!dxcore_mod) {
      debug_printf("D3D12: failed to load DXCore.DLL\n");
      return NULL;
   }

   DXCoreCreateAdapterFactory = (PFN_CREATE_DXCORE_ADAPTER_FACTORY)util_dl_get_proc_address(dxcore_mod, "DXCoreCreateAdapterFactory");
   if (!DXCoreCreateAdapterFactory) {
      debug_printf("D3D12: failed to load DXCoreCreateAdapterFactory from DXCore.DLL\n");
      return NULL;
   }

   IDXCoreAdapterFactory *factory = NULL;
   HRESULT hr = DXCoreCreateAdapterFactory(IID_IDXCoreAdapterFactory, (void **)&factory);
   if (FAILED(hr)) {
      debug_printf("D3D12: DXCoreCreateAdapterFactory failed: %08x\n", hr);
      return NULL;
   }

   return factory;
}

static IDXCoreAdapter *
choose_dxcore_adapter(IDXCoreAdapterFactory *factory, LUID *adapter)
{
   IDXCoreAdapter *ret;
   if (adapter) {
      if (SUCCEEDED(factory->GetAdapterByLuid(*adapter, &ret)))
         return ret;
      debug_printf("D3D12: requested adapter missing, falling back to auto-detection...\n");
   }

   // The first adapter is the default
   IDXCoreAdapterList *list = nullptr;
   if (SUCCEEDED(factory->CreateAdapterList(1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, &list))) {
      if (list->GetAdapterCount() > 0 && SUCCEEDED(list->GetAdapter(0, &ret)))
         return ret;
   }

   return NULL;
}

static const char *
dxcore_get_name(struct d3d12_screen *screen)
{
   d3d12_dxcore_screen *dxcore_screen = static_cast<d3d12_dxcore_screen *>(screen);
   static char buf[1000];
   if (dxcore_screen->description[0] == '\0')
      return "D3D12 (Unknown)";

   snprintf(buf, sizeof(buf), "D3D12 (%s)", dxcore_screen->description);
   return buf;
}

static IUnknown *
dxcore_get_adapter(struct d3d12_screen *screen)
{
   return static_cast<d3d12_dxcore_screen *>(screen)->adapter;
}

struct d3d12_screen *
d3d12_create_dxcore_screen(LUID *adapter_luid)
{
   d3d12_dxcore_screen *screen = CALLOC_STRUCT(d3d12_dxcore_screen);
   if (!screen)
      return nullptr;

   screen->factory = get_dxcore_factory();
   if (!screen->factory) {
      FREE(screen);
      return nullptr;
   }

   screen->adapter = choose_dxcore_adapter(screen->factory, adapter_luid);
   if (!screen->adapter) {
      debug_printf("D3D12: no suitable adapter\n");
      FREE(screen);
      return nullptr;
   }

   DXCoreHardwareID hardware_ids = {};
   if (FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::HardwareID, &hardware_ids)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, &screen->adapter_desc.dedicated_video_memory)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::DedicatedSystemMemory, &screen->adapter_desc.dedicated_system_memory)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::SharedSystemMemory, &screen->adapter_desc.shared_system_memory)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::DriverDescription,
                                           sizeof(screen->description),
                                           screen->description))) {
      debug_printf("D3D12: failed to retrieve adapter description\n");
      FREE(screen);
      return nullptr;
   }

   screen->get_name = dxcore_get_name;
   screen->get_adapter = dxcore_get_adapter;

   return screen;
}
