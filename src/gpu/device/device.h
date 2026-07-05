/**
 * @file    gpu/device/device.h
 * @brief   The Plume renderer device: creation and resource-creation entry
 *          points only.
 *
 * Narrow-slice port. re:Blue's Video class has ~60 static methods covering
 * present, draw, resolve, occlusion queries, bindless textures, MSAA resolve
 * and more, all sharing one VideoState. This version keeps only what
 * CreateTexture/CreateSurface/Release/AddRef/GetType need: a device to create
 * resources on. No swapchain, no command queue, no pipelines, no bindless
 * heap - those come with Present() and the draw hooks, which aren't ported
 * yet.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <memory>
#include <mutex>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/guest/resources.h"

namespace rex::ui {
class Window;
}

namespace eot::gpu {

class Video {
public:
  static bool CreateHostDevice();

  static plume::RenderDevice *HostDevice();

  static u32 BindTextureSRV(GuestTexture *tex);

  static void QueueResourceDestroy(u32 guest_va, ResourceType type);

  static plume::RenderSampleCounts CvarMSAASampleCount();
};

std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device,
                  const plume::RenderTextureDesc &desc, const char *tag);

bool CheckDeviceRemoved(const char *context);
bool DeviceIsLost();

struct VideoState {
  std::unique_ptr<plume::RenderInterface> render_iface;
  std::unique_ptr<plume::RenderDevice> device;
  std::mutex mutex;
  bool ready = false;
};

VideoState &state();

}
