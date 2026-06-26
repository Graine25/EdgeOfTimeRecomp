#include "src/goliath_engine/gpu/renderer/video.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <rex/logging.h>

#include "src/goliath_engine/gpu/renderer/render_internal.h"
#include "src/goliath_engine/gpu/renderer/render_state.h"

using namespace plume;

namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

namespace eot::render {
namespace {

constexpr RenderFormat kBackbufferFormat = RenderFormat::R8G8B8A8_UNORM;
constexpr RenderFormat kDepthFormat = RenderFormat::D32_FLOAT;
constexpr uint32_t kMaxTextures = 8192;

std::unique_ptr<RenderInterface> g_interface;
std::unique_ptr<RenderDevice> g_device;
std::unique_ptr<RenderCommandQueue> g_queue;
std::unique_ptr<RenderCommandList> g_commandList;
std::unique_ptr<RenderCommandFence> g_fence;
std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphore;
std::unique_ptr<RenderCommandSemaphore> g_renderSemaphore;
std::unique_ptr<RenderSwapChain> g_swapChain;
std::vector<std::unique_ptr<RenderFramebuffer>> g_framebuffers;
std::unique_ptr<RenderTexture> g_depthTex;
std::unique_ptr<RenderTextureView> g_depthView;

RenderWindow g_window{};
bool g_initialized = false;
bool g_swapChainValid = false;
bool g_commandsInFlight = false;

std::atomic<uint32_t> g_clearColorBits{0x19334CFFu};
std::mutex g_presentMutex;

std::unique_ptr<RenderDescriptorSet> g_texSet;
std::unique_ptr<RenderDescriptorSet> g_tex3DSet;
std::unique_ptr<RenderDescriptorSet> g_texCubeSet;
std::unique_ptr<RenderDescriptorSet> g_tex1DSet;
std::unique_ptr<RenderDescriptorSet> g_sampSet;
std::unique_ptr<RenderSampler> g_sampler;
std::unique_ptr<RenderPipelineLayout> g_texturedLayout;
std::unique_ptr<RenderPipelineLayout> g_gameLayout;
uint32_t g_cbvVS = 0, g_cbvPS = 0, g_cbvShared = 0;
bool g_texReady = false;

RenderColor UnpackClearColor() {
  uint32_t bits = g_clearColorBits.load(std::memory_order_relaxed);
  float r = ((bits >> 24) & 0xFF) / 255.0f;
  float g = ((bits >> 16) & 0xFF) / 255.0f;
  float b = ((bits >> 8) & 0xFF) / 255.0f;
  float a = (bits & 0xFF) / 255.0f;
  return RenderColor(r, g, b, a);
}

void RebuildFramebuffers() {
  g_framebuffers.clear();
  const uint32_t w = g_swapChain->getWidth(), h = g_swapChain->getHeight();
  g_depthTex = g_device->createTexture(RenderTextureDesc::DepthTarget(w, h, kDepthFormat));
  g_depthView = g_depthTex->createTextureView(RenderTextureViewDesc::Texture2D(kDepthFormat));
  const uint32_t count = g_swapChain->getTextureCount();
  g_framebuffers.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const RenderTexture* color = g_swapChain->getTexture(i);
    RenderFramebufferDesc desc(&color, 1, g_depthTex.get());
    g_framebuffers[i] = g_device->createFramebuffer(desc);
  }
}

}

RenderDevice* Device() { return g_device.get(); }

bool EnsureTextureSystem() {
  if (g_texReady) return true;
  RenderDevice* dev = g_device.get();
  if (!dev) return false;

  RenderDescriptorSetBuilder texB;
  texB.begin();
  texB.addTexture(0, kMaxTextures);
  texB.end(true, kMaxTextures);
  g_texSet = texB.create(dev);

  RenderDescriptorSetBuilder sampB;
  sampB.begin();
  sampB.addSampler(0, 1);
  sampB.end();
  g_sampSet = sampB.create(dev);
  RenderSamplerDesc sd;
  sd.minFilter = RenderFilter::LINEAR;
  sd.magFilter = RenderFilter::LINEAR;
  sd.addressU = RenderTextureAddressMode::WRAP;
  sd.addressV = RenderTextureAddressMode::WRAP;
  sd.addressW = RenderTextureAddressMode::WRAP;
  g_sampler = dev->createSampler(sd);
  g_sampSet->setSampler(0, g_sampler.get());

  RenderPipelineLayoutBuilder lb;
  lb.begin(false, true);
  lb.addDescriptorSet(texB);
  lb.addDescriptorSet(sampB);
  lb.addPushConstant(0, 2, 4, RenderShaderStageFlag::PIXEL);
  lb.end();
  g_texturedLayout = lb.create(dev);

  RenderDescriptorSetBuilder t3dB, tcB, t1dB;
  t3dB.begin(); t3dB.addTexture(0, 256); t3dB.end(true, 256); g_tex3DSet = t3dB.create(dev);
  tcB.begin();  tcB.addTexture(0, 256);  tcB.end(true, 256);  g_texCubeSet = tcB.create(dev);
  t1dB.begin(); t1dB.addTexture(0, 256); t1dB.end(true, 256); g_tex1DSet = t1dB.create(dev);
  RenderPipelineLayoutBuilder gb;
  gb.begin(false, true);
  gb.addDescriptorSet(texB);
  gb.addDescriptorSet(t3dB);
  gb.addDescriptorSet(tcB);
  gb.addDescriptorSet(sampB);
  gb.addDescriptorSet(t1dB);
  g_cbvVS = gb.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  g_cbvPS = gb.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  g_cbvShared = gb.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  gb.addPushConstant(3, 4, 4, RenderShaderStageFlag::PIXEL);
  gb.end();
  g_gameLayout = gb.create(dev);

  g_texReady = g_texSet && g_sampSet && g_texturedLayout && g_gameLayout;
  REXGPU_INFO("texture system ready: debugLayout={} gameLayout={} (cbv VS={} PS={} shared={})",
              g_texturedLayout != nullptr, g_gameLayout != nullptr, g_cbvVS, g_cbvPS, g_cbvShared);
  return g_texReady;
}

RenderDescriptorSet* TextureSet() { return g_texSet.get(); }
RenderDescriptorSet* SamplerSet() { return g_sampSet.get(); }
RenderDescriptorSet* Tex3DSet() { return g_tex3DSet.get(); }
RenderDescriptorSet* TexCubeSet() { return g_texCubeSet.get(); }
RenderDescriptorSet* Tex1DSet() { return g_tex1DSet.get(); }
RenderPipelineLayout* TexturedPipelineLayout() { return g_texturedLayout.get(); }
RenderPipelineLayout* GameLayout() { return g_gameLayout.get(); }
void GameCbvIndices(uint32_t& vs, uint32_t& ps, uint32_t& shared) {
  vs = g_cbvVS;
  ps = g_cbvPS;
  shared = g_cbvShared;
}

}

bool Video::Init(void* nativeWindowHandle, uint32_t width, uint32_t height) {
  using namespace eot::render;
  if (g_initialized) return true;
  g_window = reinterpret_cast<RenderWindow>(nativeWindowHandle);
  s_viewportWidth = width;
  s_viewportHeight = height;

  g_interface = plume::CreateD3D12Interface();
  if (!g_interface) {
    REXGPU_ERROR("Video::Init: CreateD3D12Interface failed");
    return false;
  }
  g_device = g_interface->createDevice();
  if (!g_device) {
    REXGPU_ERROR("Video::Init: createDevice failed");
    g_interface.reset();
    return false;
  }
  REXGPU_INFO("Video::Init: device = {}", g_device->getDescription().name);

  g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
  g_commandList = g_queue->createCommandList();
  g_fence = g_device->createCommandFence();
  g_acquireSemaphore = g_device->createCommandSemaphore();
  g_renderSemaphore = g_device->createCommandSemaphore();

  RenderSwapChainDesc desc(g_window, kBackbufferFormat, 2);
  g_swapChain = g_queue->createSwapChain(desc);
  g_swapChainValid = !g_swapChain->needsResize();
  if (g_swapChainValid) RebuildFramebuffers();

  g_initialized = true;
  REXGPU_INFO("Video::Init: swapchain {}x{} valid={}", g_swapChain->getWidth(),
              g_swapChain->getHeight(), g_swapChainValid);
  return true;
}

bool Video::IsInitialized() { return eot::render::g_initialized; }

void Video::SetClearColor(float r, float g, float b, float a) {
  auto clamp8 = [](float v) -> uint32_t {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return static_cast<uint32_t>(v * 255.0f + 0.5f);
  };
  uint32_t bits = (clamp8(r) << 24) | (clamp8(g) << 16) | (clamp8(b) << 8) | clamp8(a);
  eot::render::g_clearColorBits.store(bits, std::memory_order_relaxed);
}

void Video::Present() {
  using namespace eot::render;
  if (!g_initialized) return;
  std::lock_guard<std::mutex> lock(g_presentMutex);

  if (!g_swapChainValid || g_swapChain->needsResize()) {
    if (g_commandsInFlight) {
      g_queue->waitForCommandFence(g_fence.get());
      g_commandsInFlight = false;
    }
    g_swapChainValid = g_swapChain->resize();
    if (!g_swapChainValid) return;
    RebuildFramebuffers();
  }

  uint32_t backBufferIndex = 0;
  if (!g_swapChain->acquireTexture(g_acquireSemaphore.get(), &backBufferIndex)) {
    g_swapChainValid = false;
    return;
  }

  RenderTexture* backBuffer = g_swapChain->getTexture(backBufferIndex);
  RenderFramebuffer* framebuffer = g_framebuffers[backBufferIndex].get();

  g_commandList->begin();
  const RenderTextureBarrier startBarriers[] = {
      RenderTextureBarrier(backBuffer, RenderTextureLayout::COLOR_WRITE),
      RenderTextureBarrier(g_depthTex.get(), RenderTextureLayout::DEPTH_WRITE),
  };
  g_commandList->barriers(RenderBarrierStage::GRAPHICS, startBarriers, 2);
  g_commandList->setFramebuffer(framebuffer);
  g_commandList->clearColor(0, UnpackClearColor());
  g_commandList->clearDepth();
  ReplayCapturedDraws(g_commandList.get(), g_swapChain->getWidth(), g_swapChain->getHeight());
  g_commandList->setFramebuffer(framebuffer);
  if (PresentMovieFrame(g_commandList.get(), g_swapChain->getWidth(), g_swapChain->getHeight()))
    g_commandList->setFramebuffer(framebuffer);
  g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(backBuffer, RenderTextureLayout::PRESENT));
  g_commandList->end();

  RenderCommandSemaphore* waitSemaphores[] = {g_acquireSemaphore.get()};
  RenderCommandSemaphore* signalSemaphores[] = {g_renderSemaphore.get()};
  const RenderCommandList* commandLists[] = {g_commandList.get()};
  g_queue->executeCommandLists(commandLists, 1, waitSemaphores, 1, signalSemaphores, 1,
                               g_fence.get());
  g_commandsInFlight = true;

  g_swapChainValid = g_swapChain->present(backBufferIndex, signalSemaphores, 1);

  g_queue->waitForCommandFence(g_fence.get());
  g_commandsInFlight = false;
}

void Video::WaitForGPU() {
  using namespace eot::render;
  if (!g_initialized) return;
  if (g_commandsInFlight) {
    g_queue->waitForCommandFence(g_fence.get());
    g_commandsInFlight = false;
  }
}

void Video::Shutdown() {
  using namespace eot::render;
  if (!g_initialized) return;
  if (g_commandsInFlight) {
    g_queue->waitForCommandFence(g_fence.get());
    g_commandsInFlight = false;
  }
  g_texSet.reset();
  g_tex3DSet.reset();
  g_texCubeSet.reset();
  g_tex1DSet.reset();
  g_sampSet.reset();
  g_sampler.reset();
  g_texturedLayout.reset();
  g_gameLayout.reset();
  g_texReady = false;
  g_framebuffers.clear();
  g_depthView.reset();
  g_depthTex.reset();
  g_swapChain.reset();
  g_renderSemaphore.reset();
  g_acquireSemaphore.reset();
  g_fence.reset();
  g_commandList.reset();
  g_queue.reset();
  g_device.reset();
  g_interface.reset();
  g_initialized = false;
}
