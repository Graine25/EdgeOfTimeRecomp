#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/quality.h"

REXCVAR_DEFINE_STRING(eot_quality_preset, "custom", "EdgeOfTime/Graphics", "Graphics quality preset")
    .allowed({"low", "medium", "high", "custom"});

namespace {

enum class Level { kLow, kMedium, kHigh, kCustom };

struct Gate {
  const char *name;
  const char *low, *medium, *high;
};

constexpr Gate kGates[] = {
    {"eot_msaa", "0", "4", "8"},
    {"eot_upscale", "bilinear", "bicubic", "lanczos"},
    {"eot_anisotropy", "0", "8", "16"},
    {"eot_shadow_map_size", "1024", "2048", "4096"},
};

Level Parse(std::string_view v) {
  if (v == "low")
    return Level::kLow;
  if (v == "medium")
    return Level::kMedium;
  if (v == "high")
    return Level::kHigh;
  return Level::kCustom;
}

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
std::atomic<bool> g_booted{false};

void Apply(Level level, std::string_view name) {
  g_applying = true;
  std::string line;
  for (const Gate &g : kGates) {
    const char *value = Value(g, level);
    const std::string current = rex::cvar::GetFlagByName(g.name);
    if (!Same(current, value) && !rex::cvar::SetFlagByName(g.name, value))
      EOT_WARN("[quality] preset {}: {} rejected {}", name, g.name, value);
    line += std::string(line.empty() ? "" : ", ") + g.name + " " + value;
  }
  g_applying = false;
  EOT_INFO("[quality] preset {}: {}", name, line);
}

struct Registrar {
  Registrar() {
    rex::cvar::RegisterChangeCallback("eot_quality_preset", [](std::string_view, std::string_view value) {
      if (g_applying)
        return;
      const Level level = Parse(value);
      if (level != Level::kCustom)
        Apply(level, value);
    });
    for (const Gate &g : kGates) {
      rex::cvar::RegisterChangeCallback(g.name, [gate = &g](std::string_view, std::string_view value) {
        EOT_DEBUG("[quality] {} -> {} (applying {}, booted {}, preset {})", gate->name, value, g_applying.load(),
                  g_booted.load(), REXCVAR_GET(eot_quality_preset));
        if (g_applying || !g_booted)
          return;
        const Level level = Parse(REXCVAR_GET(eot_quality_preset));
        if (level == Level::kCustom || Same(value, Value(*gate, level)))
          return;
        EOT_INFO("[quality] {} set to {} by hand; the preset is now custom", gate->name, value);
        rex::cvar::SetFlagByName("eot_quality_preset", "custom");
      });
    }
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
  const Level level = Parse(preset);
  if (level != Level::kCustom)
    Apply(level, preset);
  g_booted = true;
}

}
