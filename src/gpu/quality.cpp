#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/quality.h"

REXCVAR_DEFINE_STRING(eot_quality_preset, "custom", "EdgeOfTime/Graphics",
                      "Quality preset. low: no anti-aliasing, no sharpening, bilinear upscale, "
                      "the game's own texture filtering, 1024 shadow maps. medium: 2x MSAA or "
                      "FXAA (eot_aa_method), sharpening 0.5, bicubic upscale, 8x anisotropic, "
                      "2048 shadow maps. high: 4x MSAA or FXAA high, sharpening 0.5, Lanczos "
                      "upscale, 16x anisotropic, 4096 shadow maps. Choosing one writes those "
                      "values into eot_msaa (restart), eot_aa, eot_sharpen, eot_upscale, "
                      "eot_anisotropy and eot_shadow_map_size; changing any of them afterwards "
                      "makes the preset custom, which is also the default and touches nothing.")
    .allowed({"low", "medium", "high", "custom"});
REXCVAR_DEFINE_STRING(eot_aa_method, "fxaa", "EdgeOfTime/Graphics",
                      "Which anti-aliasing the quality preset uses: msaa multisamples the scene "
                      "(eot_msaa 2x at medium, 4x at high; needs a restart) with eot_aa off, fxaa "
                      "leaves the scene single-sampled and filters the finished frame (eot_aa fxaa "
                      "at medium, fxaa_high at high). Only read while a preset other than custom "
                      "is selected.")
    .allowed({"msaa", "fxaa"});

namespace {

enum class Level { kLow, kMedium, kHigh, kCustom };
enum class Method { kMsaa, kFxaa };

struct Gate {
  const char *name;
  const char *low, *medium, *high;
};

constexpr Gate kAaGates[2][2] = {
{{"eot_msaa", "0", "2", "4"}, {"eot_aa", "off", "off", "off"}},
{{"eot_msaa", "0", "0", "0"}, {"eot_aa", "off", "fxaa", "fxaa_high"}},
};

constexpr Gate kGates[] = {
    {"eot_sharpen", "0", "0.5", "0.5"},
    {"eot_upscale", "bilinear", "bicubic", "lanczos"},
    {"eot_anisotropy", "0", "8", "16"},
    {"eot_shadow_map_size", "1024", "2048", "4096"},
};

Level ParseLevel(std::string_view v) {
  if (v == "low")
    return Level::kLow;
  if (v == "medium")
    return Level::kMedium;
  if (v == "high")
    return Level::kHigh;
  return Level::kCustom;
}

Method ParseMethod(std::string_view v) { return v == "msaa" ? Method::kMsaa : Method::kFxaa; }

const Gate (&AaGates(Method method))[2] { return kAaGates[method == Method::kMsaa ? 0 : 1]; }

const char *Value(const Gate &g, Level level) {
  switch (level) {
  case Level::kLow:
    return g.low;
  case Level::kMedium:
    return g.medium;
  case Level::kHigh:
    return g.high;
  default:
    return nullptr;
  }
}

const char *Expected(std::string_view name, Level level, Method method) {
  for (const Gate &g : AaGates(method))
    if (name == g.name)
      return Value(g, level);
  for (const Gate &g : kGates)
    if (name == g.name)
      return Value(g, level);
  return nullptr;
}

bool Same(std::string_view a, std::string_view b) {
  const std::string sa(a), sb(b);
  char *ea = nullptr, *eb = nullptr;
  const double da = std::strtod(sa.c_str(), &ea);
  const double db = std::strtod(sb.c_str(), &eb);
  const bool na = ea != sa.c_str() && *ea == '\0';
  const bool nb = eb != sb.c_str() && *eb == '\0';
  if (na && nb)
    return da == db;
  return sa == sb;
}

std::atomic<bool> g_applying{false};

void ApplyGate(const Gate &g, Level level, std::string_view preset, std::string &line) {
  const char *value = Value(g, level);
  const std::string current = rex::cvar::GetFlagByName(g.name);
  if (!Same(current, value) && !rex::cvar::SetFlagByName(g.name, value))
    EOT_WARN("[quality] preset {}: {} rejected {}", preset, g.name, value);
  line += std::string(line.empty() ? "" : ", ") + g.name + " " + value;
}

void Apply(Level level, Method method, std::string_view preset) {
  g_applying = true;
  std::string line;
  for (const Gate &g : AaGates(method))
    ApplyGate(g, level, preset, line);
  for (const Gate &g : kGates)
    ApplyGate(g, level, preset, line);
  g_applying = false;
  EOT_INFO("[quality] preset {} ({}): {}", preset, method == Method::kMsaa ? "msaa" : "fxaa", line);
}

Level CurrentLevel() { return ParseLevel(REXCVAR_GET(eot_quality_preset)); }
Method CurrentMethod() { return ParseMethod(REXCVAR_GET(eot_aa_method)); }

struct Registrar {
  Registrar() {
    rex::cvar::RegisterChangeCallback("eot_quality_preset", [](std::string_view, std::string_view value) {
      if (g_applying)
        return;
      const Level level = ParseLevel(value);
      if (level != Level::kCustom)
        Apply(level, CurrentMethod(), value);
    });
    rex::cvar::RegisterChangeCallback("eot_aa_method", [](std::string_view, std::string_view value) {
      if (g_applying)
        return;
      const Level level = CurrentLevel();
      if (level != Level::kCustom)
        Apply(level, ParseMethod(value), REXCVAR_GET(eot_quality_preset));
    });
    const auto by_hand = [](std::string_view name, std::string_view value) {
      if (g_applying || !rex::cvar::IsFinalized())
        return;
      const Level level = CurrentLevel();
      if (level == Level::kCustom)
        return;
      const char *expected = Expected(name, level, CurrentMethod());
      if (!expected || Same(value, expected))
        return;
      EOT_INFO("[quality] {} set to {} by hand; the preset is now custom", name, value);
      rex::cvar::SetFlagByName("eot_quality_preset", "custom");
    };
    for (const Gate &g : kAaGates[0])
      rex::cvar::RegisterChangeCallback(g.name, by_hand);
    for (const Gate &g : kGates)
      rex::cvar::RegisterChangeCallback(g.name, by_hand);
  }
} g_registrar;

}

namespace eot::gpu {

void ApplyQualityPresetAtBoot() {
  static bool done = false;
  if (done)
    return;
  done = true;
  const std::string preset(REXCVAR_GET(eot_quality_preset));
  const Level level = ParseLevel(preset);
  if (level != Level::kCustom)
    Apply(level, CurrentMethod(), preset);
}

}
