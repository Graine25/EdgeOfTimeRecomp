#include "gpu/device/rdc_capture.h"

#include <atomic>

#include <windows.h>

#include <rex/cvar.h>
#include <renderdoc_app.h>

#include "core/logging.h"
#include "core/settings.h"

REXCVAR_DEFINE_INT32(eot_rdc_capture_frame, -1, kCvarGroup,
                   "Trigger a RenderDoc capture on this frame index.");

namespace eot::gpu {

namespace {

RENDERDOC_API_1_4_0 *g_api = nullptr;
bool g_looked_up = false;
std::atomic<u32> g_frame{0};

RENDERDOC_API_1_4_0 *Api() {
  if (g_looked_up)
    return g_api;
  g_looked_up = true;

  HMODULE module = GetModuleHandleA("renderdoc.dll");
  if (!module)
    return nullptr;
  auto get_api =
      reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
  if (!get_api)
    return nullptr;
  if (get_api(eRENDERDOC_API_Version_1_4_0, reinterpret_cast<void **>(&g_api)) != 1)
    g_api = nullptr;
  if (g_api)
    EOT_INFO("[rdc] RenderDoc in-application API available");
  return g_api;
}

}

void TriggerCaptureNow() {
  if (auto *api = Api()) {
    api->TriggerCapture();
    EOT_INFO("[rdc] capture triggered on demand");
  }
}

void NotePresentForCapture() {
  static std::atomic<u32> presents{0};
  const u32 n = presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % 100 == 0)
    EOT_INFO("[present] frame {}", n);

  const int wanted = REXCVAR_GET(eot_rdc_capture_frame);
  if (wanted < 0)
    return;
  auto *api = Api();
  if (!api)
    return;

  const u32 frame = g_frame.fetch_add(1, std::memory_order_relaxed);
  if (frame != static_cast<u32>(wanted))
    return;

  api->TriggerCapture();
  EOT_INFO("[rdc] capture triggered on frame {}", frame);
}

}
