#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_GLAPIGraphics_WebAdd);
REX_EXTERN(__imp__eot_GLAPIGraphics_WebSetColor); // (web r3, float4 RGBA r4)
REX_EXTERN(__imp__eot_GLAPIGraphics_WebGetColor); // (float4 out r3, web r4)
REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);

REXCVAR_DEFINE_STRING(eot_web_color, "", "EdgeOfTime/Graphics", "Web color, hex or name");

namespace {

constexpr uint32_t kNoWeb = 0xFFFFFFFFu;

struct Colour {
  float r = 1.0f, g = 1.0f, b = 1.0f;
};

std::mutex g_colour_mutex;
Colour g_colour;
std::atomic<bool> g_colour_set{false};
std::atomic<bool> g_colour_stale{true};
std::atomic<bool> g_callback{false};
uint32_t g_scratch = 0;

bool ParseColour(const std::string &text, Colour &out) {
  struct Named {
    const char *name;
    uint32_t rgb;
  };
  static const Named kNamed[] = {{"orange", 0xFF8000}, {"red", 0xFF2010},   {"yellow", 0xFFE000}, {"green", 0x30FF40},
                                 {"cyan", 0x20E0FF},   {"blue", 0x2050FF},  {"purple", 0xA030FF}, {"pink", 0xFF60C0},
                                 {"white", 0xFFFFFF},  {"black", 0x000000}};
  std::string t;
  for (char c : text)
    if (c != ' ' && c != '#')
      t.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  if (t.empty())
    return false;
  uint32_t rgb = 0;
  bool found = false;
  for (const Named &n : kNamed)
    if (t == n.name) {
      rgb = n.rgb;
      found = true;
    }
  if (!found) {
    if (t.size() != 6 || t.find_first_not_of("0123456789abcdef") != std::string::npos) {
      EOT_WARN("[web] eot_web_color '{}' is neither RRGGBB nor a colour name; the game's colour stays", text);
      return false;
    }
    rgb = static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 16));
  }
  out.r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
  out.g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
  out.b = static_cast<float>(rgb & 0xFF) / 255.0f;
  return true;
}

void Refresh() {
  if (!g_callback.exchange(true)) {
    rex::cvar::RegisterChangeCallback("eot_web_color", [](std::string_view, std::string_view) {
      g_colour_stale.store(true, std::memory_order_release);
    });
  }
  if (!g_colour_stale.exchange(false))
    return;
  Colour c;
  const std::string text = REXCVAR_GET(eot_web_color);
  const bool set = ParseColour(text, c);
  {
    std::lock_guard lock(g_colour_mutex);
    g_colour = c;
  }
  g_colour_set.store(set, std::memory_order_release);
  if (set)
    EOT_INFO("[web] webs coloured {} ({:.2f} {:.2f} {:.2f})", text, c.r, c.g, c.b);
}

void Recolour(uint32_t float4) {
  Colour c;
  {
    std::lock_guard lock(g_colour_mutex);
    c = g_colour;
  }
  eot::mem::store<float>(float4 + 0, c.r);
  eot::mem::store<float>(float4 + 4, c.g);
  eot::mem::store<float>(float4 + 8, c.b);
}

uint32_t Scratch(const PPCContext &ctx, uint8_t *base) {
  if (!g_scratch) {
    PPCContext call = ctx;
    call.r3.u32 = 16;
    call.r4.u32 = 16;
    call.r5.u32 = 0xFFFFFFFFu;
    call.r6.u32 = 0;
    __imp__eot_MMMemoryMgr_Alloc(call, base);
    g_scratch = call.r3.u32;
  }
  return g_scratch;
}

}

REX_HOOK_RAW(eot_GLAPIGraphics_WebSetColor) {
  Refresh();
  if (g_colour_set.load(std::memory_order_acquire) && ctx.r3.u32 != kNoWeb && ctx.r4.u32)
    Recolour(ctx.r4.u32);
  __imp__eot_GLAPIGraphics_WebSetColor(ctx, base);
}

REX_HOOK_RAW(eot_GLAPIGraphics_WebAdd) {
  Refresh();
  __imp__eot_GLAPIGraphics_WebAdd(ctx, base);
  const uint32_t web = ctx.r3.u32;
  if (web == kNoWeb || !g_colour_set.load(std::memory_order_acquire))
    return;
  const uint32_t scratch = Scratch(ctx, base);
  if (!scratch)
    return;
  PPCContext get = ctx;
  get.r3.u32 = scratch;
  get.r4.u32 = web;
  __imp__eot_GLAPIGraphics_WebGetColor(get, base);
  Recolour(scratch);
  PPCContext set = ctx;
  set.r3.u32 = web;
  set.r4.u32 = scratch;
  __imp__eot_GLAPIGraphics_WebSetColor(set, base);
  ctx.r3.u32 = web;
}
