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

#include <atomic>
#include <memory>
#include <mutex>
#include <rex/types.h>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/device/texture_upload.h"
#include "gpu/guest/resources.h"

namespace rex::ui {
class Window;
}

namespace eot::gpu {

constexpr u32 kNumFrames = 2;

class Video {
public:
  static bool CreateHostDevice();

  static bool CreateSwapChain(rex::ui::Window *window);

  static plume::RenderDevice *HostDevice();

  static void Present(GuestTexture *front_buffer = nullptr);

  static void BeginGuestFrame();

  static void RequestResize();

  static u32 OutputWidth();
  static u32 OutputHeight();

  static u32 BindTextureSRV(GuestTexture *tex);

  static void SetRenderTarget(u32 index, GuestTexture *surface);
  static void SetDepthStencil(GuestTexture *surface);

  static void SetVertexShader(GuestShader *shader);
  static void SetPixelShader(GuestShader *shader);
  static GuestShader *BoundVertexShader();
  static GuestShader *BoundPixelShader();

  static void SetStreamSource(u32 stream, GuestBuffer *buffer, u32 offset,
                              u32 stride);
  static void SetIndices(GuestBuffer *buffer);

  struct AttachmentFormats {
    plume::RenderFormat color = plume::RenderFormat::UNKNOWN;
    plume::RenderFormat depth = plume::RenderFormat::UNKNOWN;
    u32 sampleCount = 1;
  };
  static AttachmentFormats BoundAttachmentFormats();

  static u32 BoundStreamStride(u32 stream);

  enum class FramebufferBind {
    kBound,
    kNotReady,
    kNothingBound,
    kDepthOnly,
    kNoHostTexture,
    kCreateFailed,
  };

  static FramebufferBind BindDrawFramebuffer();

  static void NotifyTextureDestroyed(GuestTexture *dead);

  static void QueueResourceDestroy(u32 guest_va, ResourceType type);

  static plume::RenderSampleCounts CvarMSAASampleCount();
};

std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device,
                  const plume::RenderTextureDesc &desc, const char *tag);

bool CheckDeviceRemoved(const char *context);

void DrainValidationMessages();
bool DeviceIsLost();

struct VideoState {
  std::unique_ptr<plume::RenderInterface> render_iface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;

  std::unique_ptr<plume::RenderCommandList> command_lists[kNumFrames];
  std::unique_ptr<plume::RenderCommandFence> fences[kNumFrames];
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores[kNumFrames];
  bool command_list_submitted[kNumFrames] = {};

  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores;

  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;

  std::unique_ptr<plume::RenderShader> blit_vs;
  std::unique_ptr<plume::RenderShader> blit_ps;
  std::unique_ptr<plume::RenderSampler> blit_sampler;
  std::unique_ptr<plume::RenderPipelineLayout> blit_layout;
  std::unique_ptr<plume::RenderPipeline> blit_pipeline;
  std::unique_ptr<plume::RenderDescriptorSet> blit_descriptor_set[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderTextureView>>
      blit_view_graveyard[kNumFrames];

  StagingPool upload_staging[kNumFrames];

  std::atomic<u32> frame{0};
  u32 next_frame = 1 % kNumFrames;

  std::mutex mutex;
  bool ready = false;
  bool present_ready = false;

  bool frame_present_committed = false;

  struct PendingDestroy {
    u32 guest_va;
    ResourceType type;
  };
  std::vector<PendingDestroy> deferred_destroy[kNumFrames];

  std::atomic<bool> resize_requested{false};

  GuestTexture *render_targets[kMaxRenderTargets] = {};
  GuestTexture *depth_stencil = nullptr;

  bool draw_framebuffer_bound = false;
  GuestTexture *bound_fb_rt = nullptr;
  GuestTexture *bound_fb_ds = nullptr;

  std::unordered_set<GuestTexture *> framebuffer_owners;

  GuestShader *vertex_shader = nullptr;
  GuestShader *pixel_shader = nullptr;

  struct StreamSource {
    GuestBuffer *buffer = nullptr;
    u32 offset = 0;
    u32 stride = 0;

    u32 address() const { return buffer ? buffer->address + offset : 0; }
  };
  StreamSource streams[kMaxStreamSources];
  GuestBuffer *index_buffer = nullptr;
};

VideoState &state();

bool BuildFramebuffers(VideoState &s);
bool BuildPresentSemaphores(VideoState &s);

}
