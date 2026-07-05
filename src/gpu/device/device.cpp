/**
 * @file    gpu/device/device.cpp
 * @brief   The Plume renderer device: creation and resource-creation entry
 *          points only (see device.h for what's deliberately not here yet).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device/device.h"

#include <atomic>
#include <mutex>

#include <plume_d3d12.h>
#include <rex/runtime.h>

#include "core/logging.h"
#include "gpu/device/host_resource_heap.h"
#include "platform/native_window.h"

namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

namespace eot::gpu {

namespace {

std::atomic<bool> g_device_lost{false};

void DestroyResourceNow(u32 guest_va, ResourceType type) {
  auto *memory = REX_KERNEL_MEMORY();
  void *host = memory->TranslateVirtual<void *>(guest_va);
  switch (type) {
  case ResourceType::Texture:
  case ResourceType::VolumeTexture:
  case ResourceType::RenderTarget:
  case ResourceType::DepthStencil:
    HostResourceHeap::Free(static_cast<GuestTexture *>(host));
    break;
  default:
    break;
  }
}

}

VideoState &state() {
  static VideoState s;
  return s;
}

bool Video::CreateHostDevice(rex::ui::Window *window) {
  if (!window) {
    EOT_ERROR("Video::CreateHostDevice called with null window");
    return false;
  }
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.ready) {
    return true;
  }

  plume::RenderWindow render_window{};
  if (!eot::platform::GetNativeRenderWindow(window, render_window)) {
    return false;
  }

  s.render_iface = plume::CreateD3D12Interface();
  if (!s.render_iface) {
    EOT_ERROR("Plume CreateD3D12Interface failed");
    return false;
  }
  s.device = s.render_iface->createDevice();
  if (!s.device) {
    EOT_ERROR("Plume RenderInterface::createDevice failed");
    return false;
  }

  s.ready = true;
  EOT_INFO("Video::CreateHostDevice: device created ({})",
           s.device->getDescription().name);
  return true;
}

plume::RenderDevice *Video::HostDevice() { return state().device.get(); }

u32 Video::BindTextureSRV(GuestTexture *) {
  return kInvalidDescriptorIndex;
}

void Video::QueueResourceDestroy(u32 guest_va, ResourceType type) {
  DestroyResourceNow(guest_va, type);
}

plume::RenderSampleCounts Video::CvarMSAASampleCount() {
  return plume::RenderSampleCount::COUNT_1;
}

std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device,
                  const plume::RenderTextureDesc &desc, const char *tag) {
  if (!device) {
    EOT_ERROR("CreateHostTexture({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  auto texture = device->createTexture(desc);
  if (!texture ||
      static_cast<plume::D3D12Texture *>(texture.get())->d3d == nullptr) {
    EOT_ERROR(
        "CreateHostTexture({}) failed: backend resource null ({}x{} fmt={})",
        tag ? tag : "?", desc.width, desc.height,
        static_cast<u32>(desc.format));
    CheckDeviceRemoved(tag ? tag : "texture");
    return nullptr;
  }
  return texture;
}

bool CheckDeviceRemoved(const char *context) {
  if (g_device_lost.load(std::memory_order_acquire))
    return true;

  auto *dev = static_cast<plume::D3D12Device *>(Video::HostDevice());
  if (!dev || !dev->d3d)
    return false;
  const long hr = dev->d3d->GetDeviceRemovedReason();
  if (hr == 0)
    return false;

  if (!g_device_lost.exchange(true, std::memory_order_acq_rel)) {
    EOT_ERROR("Device removed ({}): hr=0x{:08X}", context ? context : "?",
              static_cast<u32>(hr));
  }
  return true;
}

bool DeviceIsLost() { return g_device_lost.load(std::memory_order_acquire); }

}
