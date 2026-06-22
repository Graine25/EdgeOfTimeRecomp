#include "video.h"

#include "src/goliath_engine/gpu/renderer/draw.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#include <rex/logging.h>

using namespace plume;

namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

namespace eot::gpu {
namespace {

constexpr RenderFormat kBackbufferFormat = RenderFormat::R8G8B8A8_UNORM;

std::unique_ptr<RenderInterface> g_interface;
std::unique_ptr<RenderDevice> g_device;
std::unique_ptr<RenderCommandQueue> g_queue;
std::unique_ptr<RenderCommandList> g_commandList;
std::unique_ptr<RenderCommandFence> g_fence;
std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphore;
std::unique_ptr<RenderCommandSemaphore> g_renderSemaphore;
std::unique_ptr<RenderSwapChain> g_swapChain;
std::vector<std::unique_ptr<RenderFramebuffer>> g_framebuffers;

RenderWindow g_window{};
bool g_initialized = false;
bool g_swapChainValid = false;
bool g_commandsInFlight = false;

std::atomic<uint32_t> g_clearColorBits{0x19334CFFu};

std::mutex g_presentMutex;

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
  const uint32_t count = g_swapChain->getTextureCount();
  g_framebuffers.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const RenderTexture* color = g_swapChain->getTexture(i);
    RenderFramebufferDesc desc(&color, 1);
    g_framebuffers[i] = g_device->createFramebuffer(desc);
  }
}

}

bool VideoInit(void* native_window_handle, uint32_t width, uint32_t height) {
  if (g_initialized) return true;
  g_window = reinterpret_cast<RenderWindow>(native_window_handle);

  g_interface = plume::CreateD3D12Interface();
  if (!g_interface) {
    REXGPU_ERROR("VideoInit: CreateD3D12Interface failed");
    return false;
  }
  g_device = g_interface->createDevice();
  if (!g_device) {
    REXGPU_ERROR("VideoInit: createDevice failed");
    g_interface.reset();
    return false;
  }
  REXGPU_INFO("VideoInit: device = {}", g_device->getDescription().name);

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
  REXGPU_INFO("VideoInit: swapchain {}x{} valid={}", g_swapChain->getWidth(),
              g_swapChain->getHeight(), g_swapChainValid);
  return true;
}

bool VideoIsInitialized() { return g_initialized; }

RenderDevice* Device() { return g_device.get(); }

void VideoSetClearColor(float r, float g, float b, float a) {
  auto clamp8 = [](float v) -> uint32_t {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return static_cast<uint32_t>(v * 255.0f + 0.5f);
  };
  uint32_t bits = (clamp8(r) << 24) | (clamp8(g) << 16) | (clamp8(b) << 8) | clamp8(a);
  g_clearColorBits.store(bits, std::memory_order_relaxed);
}

void VideoPresent() {
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
  g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(backBuffer, RenderTextureLayout::COLOR_WRITE));
  g_commandList->setFramebuffer(framebuffer);
  g_commandList->clearColor(0, UnpackClearColor());
  eot::gpu::ReplayCapturedDraws(g_commandList.get(), g_swapChain->getWidth(),
                                g_swapChain->getHeight());
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

void VideoShutdown() {
  if (!g_initialized) return;
  if (g_commandsInFlight) {
    g_queue->waitForCommandFence(g_fence.get());
    g_commandsInFlight = false;
  }
  g_framebuffers.clear();
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

}
