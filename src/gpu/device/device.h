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

constexpr u32 kHostDescriptorReserve = 8192;
constexpr u32 kBindlessTextureCount = 65536 - kHostDescriptorReserve;
constexpr u32 kHostSamplerReserve = 256;
constexpr u32 kBindlessSamplerCount = 1024 - kHostSamplerReserve;

struct ResolveRegion {
  bool valid = false;
  i32 left = 0;
  i32 top = 0;
  i32 right = 0;
  i32 bottom = 0;
  i32 destX = 0;
  i32 destY = 0;
};

class Video {
public:
  static bool CreateHostDevice();

  static bool CreateSwapChain(rex::ui::Window *window);

  static plume::RenderDevice *HostDevice();

  static void Present(GuestTexture *front_buffer = nullptr);

  static void BeginGuestFrame();

  static void ClearBoundTargets(u32 device_va, u32 flags, u32 color_va, float z,
                                u32 stencil);

  static void ResolveRenderTarget(u32 device_va, u32 flags, u32 dest_texture_va,
                                  u32 dest_level,
                                  const ResolveRegion &region = {});

  static void LogResolveStats();

  struct RecordingList {
    plume::RenderCommandList *cmd = nullptr;
    plume::RenderFramebuffer *framebuffer = nullptr;
    u32 targetWidth = 0;
    u32 targetHeight = 0;
    GuestTexture *colorTarget = nullptr;
    GuestTexture *depthTarget = nullptr;
    GuestTexture *colorTargets[kMaxRenderTargets] = {};
    std::unique_lock<std::mutex> lock;

    explicit operator bool() const { return cmd != nullptr; }
  };
  static RecordingList AcquireRecordingList();

  static bool TakePendingDepthClear(const RecordingList &rec, float &z,
                                    u32 &stencil, bool &clear_depth);

  static void NoteReducedViewportDraw(const RecordingList &rec, u32 vp_w,
                                      u32 vp_h);

  static void RequestResize();

  static u32 OutputWidth();
  static u32 OutputHeight();

  static u32 BindTextureSRV(GuestTexture *tex);

  static void SetRenderTarget(u32 index, GuestTexture *surface);
  static void SetDepthStencil(GuestTexture *surface);

  static void SetVertexShader(GuestShader *shader);
  static void SetPixelShader(GuestShader *shader);
  static bool IsBusiestSurface(const GuestTexture *tex);

  static GuestShader *BoundVertexShader();

  class ScopedBoundShaders {
  public:
    ScopedBoundShaders(GuestShader *vertex, GuestShader *pixel);
    ~ScopedBoundShaders();
    ScopedBoundShaders(const ScopedBoundShaders &) = delete;
    ScopedBoundShaders &operator=(const ScopedBoundShaders &) = delete;

  private:
    GuestShader *previous_vertex_ = nullptr;
    GuestShader *previous_pixel_ = nullptr;
    bool was_active_ = false;
  };
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

  struct AttachmentSize {
    u32 width = 0;
    u32 height = 0;
  };
  static AttachmentSize BoundAttachmentSize();

  static GuestTexture *BoundDepthTexture();
  static GuestTexture *BoundColorTexture();

  static u32 AcquireTextureDescriptor(GuestTexture *tex);

  static u32 AcquireSamplerDescriptor(const plume::RenderSamplerDesc &desc);

  static void ReleaseTextureDescriptor(u32 index);

  static u32 CurrentFrameSlot();

  static void NoteAttachmentsDrawnLocked(GuestTexture *color,
                                         GuestTexture *depth);

  static u32 BoundStreamStride(u32 stream);

  struct BoundStreamInfo {
    u32 address = 0;
    u32 stride = 0;
    u32 size = 0;
  };
  static BoundStreamInfo BoundStream(u32 stream);

  static void SetImmediateStream(u32 address, u32 stride, u32 size);

  struct BoundIndexInfo {
    u32 address = 0;
    u32 size = 0;
    bool index32 = false;
  };
  static BoundIndexInfo BoundIndexBuffer();

  static plume::RenderPipelineLayout *GuestPipelineLayout();

  static plume::RenderDescriptorSet *GuestTextureSet();
  static plume::RenderDescriptorSet *GuestSamplerSet();

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

  std::unique_ptr<plume::RenderPipelineLayout> guest_pipeline_layout;
  std::unique_ptr<plume::RenderDescriptorSet> guest_texture_set;

  u32 next_texture_slot = 1;
  u32 next_sampler_slot = 1;
  std::vector<std::unique_ptr<plume::RenderSampler>> guest_samplers;
  std::vector<u32> free_texture_slots;
  std::unique_ptr<plume::RenderSampler> guest_default_sampler;
  std::unique_ptr<plume::RenderTexture> null_texture;
  bool null_texture_filled = false;
  std::unique_ptr<plume::RenderBuffer> null_fill_staging;
  std::unique_ptr<plume::RenderTextureView> null_texture_view;
  std::unique_ptr<plume::RenderDescriptorSet> guest_sampler_set;
  bool guest_layout_failed = false;

  std::unique_ptr<plume::RenderCommandList> command_lists[kNumFrames];
  std::unique_ptr<plume::RenderCommandFence> fences[kNumFrames];
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores[kNumFrames];
  bool command_list_submitted[kNumFrames] = {};

  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores;

  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;

  std::unique_ptr<plume::RenderShader> blit_vs;
  std::unique_ptr<plume::RenderShader> blit_ps;
  std::unique_ptr<plume::RenderShader> resolve_ps;
  std::unique_ptr<plume::RenderSampler> blit_sampler;
  std::unique_ptr<plume::RenderPipelineLayout> blit_layout;
  std::unique_ptr<plume::RenderPipeline> blit_pipeline;

  std::unordered_map<u32, std::unique_ptr<plume::RenderPipeline>>
      resolve_pipelines;
  std::vector<std::unique_ptr<plume::RenderDescriptorSet>>
      resolve_sets[kNumFrames];
  u32 resolve_set_used[kNumFrames] = {};

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

  bool command_list_open[kNumFrames] = {};

  struct PendingDestroy {
    u32 guest_va;
    ResourceType type;
  };
  std::vector<PendingDestroy> deferred_destroy[kNumFrames];

  std::atomic<bool> resize_requested{false};

  GuestTexture *render_targets[kMaxRenderTargets] = {};
  GuestTexture *depth_stencil = nullptr;
  u64 depth_clear_serial = 0;
  float depth_clear_z = 0.0f;
  u32 depth_clear_stencil = 0;
  bool depth_clear_depth_aspect = true;
  u32 depth_clear_width = 0;
  u32 depth_clear_height = 0;
  u32 depth_clear_edram = ~0u;

  u64 frame_serial = 1;

  GuestTexture *last_drawn_rt[kNumFrames] = {};
  GuestTexture *busiest_rt = nullptr;
  bool vsync_enabled = true;
  u32 immediate_address = 0;
  u32 immediate_stride = 0;
  u32 immediate_size = 0;
  std::unique_ptr<GuestTexture> scene_snapshot;
  u64 scene_snapshot_serial = 0;
  u32 scene_snapshot_draws = 0;
  u32 busiest_rt_draws = 0;
  u64 busiest_rt_serial = 0;
  u32 frame_draw_total = 0;
  u32 frame_surface_count = 0;
  u32 peak_frame_draws = 0;
  GuestTexture *last_front_src = nullptr;
  u32 front_resolves = 0;
  u64 front_resolve_serial = 0;
  GuestTexture *frame_surfaces[16] = {};
  GuestTexture *last_drawn_ds[kNumFrames] = {};

  bool draw_framebuffer_bound = false;
  plume::RenderFramebuffer *bound_framebuffer = nullptr;
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
