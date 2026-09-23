#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/quit_client.h"
#include "gamelogic/ui/menu_common.h"
#include "gamelogic/ui/hud_api.h"
#include "goliath/ui/name_crc.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_HUDOptionsScreen_BuildBar);         // (this r3)
REX_EXTERN(__imp__eot_HUDOptionsScreen_HandleInputEvent); // (this r3, event r4)
REX_EXTERN(__imp__eot_GameOptionsPopup_OnShow);           // (config r3, shown r4)
REX_EXTERN(__imp__eot_GameOptionsPopup_OnUpdate);         // (config r3, dt f1) -> wants to close
REX_EXTERN(__imp__eot_WindowComponent_Teardown);          // (component r3)
REX_EXTERN(__imp__eot_TCRWindow_OpeningEnter);            // (this r3): starts the grow tween
REX_EXTERN(__imp__eot_Input_IsPressed);                   // (pad mask r3, input r4) -> pressed this frame
REX_EXTERN(__imp__eot_Input_GetAxis);                     // (pad mask r3, input r4) -> f1
REX_EXTERN(__imp__eot_MultiValueControl_Create);   // (control r3, const uint32 handles[2] r4)
REX_EXTERN(__imp__eot_MultiValueControl_SetState);
REX_EXTERN(__imp__eot_MultiValueControl_SetLabel); // (control r3, string handle r4)
REX_EXTERN(__imp__eot_SliderControl_Create);       // (control r3)
REX_EXTERN(__imp__eot_SliderControl_Show);         // (control r3, shown r4)
REX_EXTERN(__imp__eot_SliderControl_SetState);     // (control r3, selected r4)
REX_EXTERN(__imp__eot_TextWnd_SetStringHandle);    // (const uint32 *window r3, string handle r4)
REX_EXTERN(__imp__eot_Wnd_SetX);                   // (const uint32 *window r3, x f1)
REX_EXTERN(__imp__eot_Wnd_SetColorBytes);
REX_EXTERN(__imp__eot_Audio_SetFxVolume);
REX_EXTERN(__imp__eot_Audio_SetMusicVolume);
REX_EXTERN(__imp__eot_Audio_SetVoiceVolume);
REX_EXTERN(__imp__eot_Subtitles_SetEnabled); // (shown r3)

REXCVAR_DEFINE_STRING(eot_button_glyphs, "auto", "EdgeOfTime/Input", "Which button prompts show")
    .allowed({"auto", "xbox", "switch", "keyboard"});
REXCVAR_DEFINE_BOOL(eot_spatial_audio, true, "EdgeOfTime/Audio", "Sounds fade with distance");
REXCVAR_DEFINE_BOOL(eot_battle_theme, true, "EdgeOfTime/Audio", "Play the battle music");

namespace {

using namespace eot::ui;

const PPCContext *g_ctx = nullptr;
uint8_t *g_base = nullptr;

struct CallScope {
  CallScope(const PPCContext &ctx, uint8_t *base) {
    g_ctx = &ctx;
    g_base = base;
  }
};

namespace guest {
constexpr uint32_t kFxVolume = 0x883C16FC;
constexpr uint32_t kMusicVolume = 0x883C1700;
constexpr uint32_t kVoiceVolume = 0x883C1704;
constexpr float kVolumeCeiling = 0.93f;
constexpr uint32_t kSubtitlesHider = 0x883C2194;
constexpr uint32_t kVibration = 0x883C1691;
constexpr uint32_t kInputApiPtr = 0x883CA224;
constexpr uint32_t kInputApiSetVibration = 60;
constexpr uint32_t kInvertY = 0x883CA0F5;
constexpr uint32_t kInvertX = 0x883CA0F6;
constexpr uint32_t kInvertYDefault = 0x883CA0F4;
}

double Number(std::string_view text) {
  double value = 0.0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

bool Truthy(std::string_view text) { return text == "true" || text == "1"; }

std::string BoolText(bool on) { return on ? "true" : "false"; }

std::string GetVolume(uint32_t addr) {
  const float have = std::min(std::bit_cast<float>(eot::mem::load<uint32_t>(addr)), guest::kVolumeCeiling);
  char text[16];
  std::snprintf(text, sizeof(text), "%.2f", have / guest::kVolumeCeiling);
  return text;
}

using GuestCall = void (*)(PPCContext &, uint8_t *);

bool SetVolume(GuestCall setter, std::string_view text) {
  if (!g_ctx)
    return false;
  const double volume = std::clamp(Number(text), 0.0, 1.0);
  PPCContext call = *g_ctx;
  call.f1.f64 = volume * guest::kVolumeCeiling;
  setter(call, g_base);
  return true;
}

std::string GetFxVolume() { return GetVolume(guest::kFxVolume); }
std::string GetMusicVolume() { return GetVolume(guest::kMusicVolume); }
std::string GetVoiceVolume() { return GetVolume(guest::kVoiceVolume); }
bool SetFxVolume(std::string_view v) {
  return SetVolume([](PPCContext &c, uint8_t *b) { __imp__eot_Audio_SetFxVolume(c, b); }, v);
}
bool SetMusicVolume(std::string_view v) {
  return SetVolume([](PPCContext &c, uint8_t *b) { __imp__eot_Audio_SetMusicVolume(c, b); }, v);
}
bool SetVoiceVolume(std::string_view v) {
  return SetVolume([](PPCContext &c, uint8_t *b) { __imp__eot_Audio_SetVoiceVolume(c, b); }, v);
}

std::string GetSubtitles() { return BoolText(eot::mem::load<uint32_t>(guest::kSubtitlesHider) == 0xFFFFFFFFu); }
bool SetSubtitles(std::string_view v) {
  if (!g_ctx)
    return false;
  PPCContext call = *g_ctx;
  call.r3.u32 = Truthy(v) ? 1 : 0;
  __imp__eot_Subtitles_SetEnabled(call, g_base);
  return true;
}

std::string GetVibration() { return BoolText(eot::mem::load<uint8_t>(guest::kVibration) != 0); }
bool SetVibration(std::string_view v) {
  if (!g_ctx)
    return false;
  const uint32_t on = Truthy(v) ? 1 : 0;
  eot::mem::store<uint8_t>(guest::kVibration, static_cast<uint8_t>(on));
  const uint32_t api = eot::mem::load<uint32_t>(guest::kInputApiPtr);
  if (api)
    hud::CallAt(*g_ctx, g_base, eot::mem::load<uint32_t>(api + guest::kInputApiSetVibration), on);
  return true;
}

std::string GetInvertY() { return BoolText(eot::mem::load<uint8_t>(guest::kInvertY) != 0); }
std::string GetInvertX() { return BoolText(eot::mem::load<uint8_t>(guest::kInvertX) != 0); }
bool SetInvertY(std::string_view v) {
  eot::mem::store<uint8_t>(guest::kInvertY, Truthy(v) ? 1 : 0);
  return true;
}
bool SetInvertX(std::string_view v) {
  eot::mem::store<uint8_t>(guest::kInvertX, Truthy(v) ? 1 : 0);
  return true;
}
std::string InvertYDefault() { return BoolText(eot::mem::load<uint8_t>(guest::kInvertYDefault) != 0); }

std::string GetKeyboardMouse() { return BoolText(rex::cvar::Query<bool>("mnk_mode")); }
bool SetKeyboardMouse(std::string_view v) {
  const std::string on = BoolText(Truthy(v));
  return rex::cvar::SetFlagByName("mnk_mode", on) && rex::cvar::SetFlagByName("mnk_mouse", on);
}

struct Accessor {
  std::string (*get)();
  bool (*set)(std::string_view value);
  const char *reset = nullptr;
  std::string (*reset_from)() = nullptr;
};

constexpr Accessor kFxVolume = {GetFxVolume, SetFxVolume, "1"};
constexpr Accessor kMusicVolume = {GetMusicVolume, SetMusicVolume, "1"};
constexpr Accessor kVoiceVolume = {GetVoiceVolume, SetVoiceVolume, "1"};
constexpr Accessor kSubtitles = {GetSubtitles, SetSubtitles, "true"};
constexpr Accessor kVibration = {GetVibration, SetVibration, "true"};
constexpr Accessor kInvertY = {GetInvertY, SetInvertY, nullptr, InvertYDefault};
constexpr Accessor kInvertX = {GetInvertX, SetInvertX, "false"};
constexpr Accessor kKeyboardMouse = {GetKeyboardMouse, SetKeyboardMouse, "false"};

struct Choice {
  const char *text;
  const char *value;
  const char *literal = nullptr;
};

enum class Format { kPlain, kPercent, kSignedPercent, kFrameRate };

struct Slider {
  double min = 0.0, max = 1.0, step = 0.1;
  Format format = Format::kPlain;
};

struct Setting {
  const char *label;
  const char *description;
  const char *cvar = nullptr;
  const Accessor *accessor = nullptr;
  std::span<const Choice> choices;
  Slider slider;
  bool numeric = false;
  bool restart = false;
  bool (*enabled)() = nullptr;
  const char *disabled_text = nullptr;
  std::span<const Choice> (*choices_of)() = nullptr;

  bool IsSlider() const { return choices.empty() && !choices_of; }
};

std::span<const Choice> Choices(const Setting &s) { return s.choices_of ? s.choices_of() : s.choices; }

std::string Value(const Setting &s) { return s.accessor ? s.accessor->get() : rex::cvar::GetFlagByName(s.cvar); }

bool SetValue(const Setting &s, std::string_view value) {
  return s.accessor ? s.accessor->set(value) : rex::cvar::SetFlagByName(s.cvar, value);
}

void ResetValue(const Setting &s) {
  if (!s.accessor) {
    rex::cvar::ResetToDefault(s.cvar);
    return;
  }
  if (s.accessor->reset_from)
    s.accessor->set(s.accessor->reset_from());
  else if (s.accessor->reset)
    s.accessor->set(s.accessor->reset);
}

const char *Name(const Setting &s) { return s.cvar ? s.cvar : s.label; }

struct Layout {
  const char *panel;
  const char *row_prefix;
  const char *scroll_up;
  const char *scroll_down;
  const char *info_title;
  const char *info_text;
  const char *info_value;
  const char *info_note;
  float box[4];
  float value_x, value_w;
  float arrow_gap;
};

struct Page {
  const char *title;
  const char *label;
  std::span<const Setting> settings;
  const Layout *layout;
  bool table = false;
};

constexpr Layout kWide = {"Reeot_OptionsPanel",
                          "Reeot_OptionsRow",
                          "Reeot_OptionsScrollUp",
                          "Reeot_OptionsScrollDown",
                          "Reeot_OptionsInfoTitle",
                          "Reeot_OptionsInfoText",
                          "Reeot_OptionsInfoValue",
                          "Reeot_OptionsInfoNote",
                          {0.06f, 0.20f, 0.50f, 0.60f},
                          0.670f,
                          0.170f,
                          0.008f};
constexpr Layout kNarrow = {"Reeot_ControlsPanel",
                            "Reeot_ControlsRow",
                            "Reeot_ControlsScrollUp",
                            "Reeot_ControlsScrollDown",
                            "Reeot_ControlsInfoTitle",
                            "Reeot_ControlsInfoText",
                            "Reeot_ControlsInfoValue",
                            "Reeot_ControlsInfoNote",
                            {0.06f, 0.20f, 0.34f, 0.60f},
                            0.660f,
                            0.260f,
                            0.004f};
const Layout kTable = {"Reeot_ModsPanel",
                           "Reeot_ModsRow",
                           "Reeot_ModsScrollUp",
                           "Reeot_ModsScrollDown",
                           "Reeot_ModsNoTitle",
                           "Reeot_ModsInfo",
                           "Reeot_ModsNoValue",
                           "Reeot_ModsNoNote",
                           {0.06f, 0.20f, 0.805f, 0.60f},
                           0.670f,
                           0.170f,
                           0.008f};
constexpr uint32_t kLayoutCount = 3;
extern const Layout kTable;
uint32_t LayoutIndex(const Layout *layout) { return layout == &kNarrow ? 1 : layout == &kTable ? 2 : 0; }

constexpr Choice kOnOff[] = {{"REEOT_VAL_OFF", "false"}, {"REEOT_VAL_ON", "true"}};
constexpr Choice kOnOffSwapped[] = {{"REEOT_VAL_ON", "false"}, {"REEOT_VAL_OFF", "true"}};
constexpr Choice kLanguages[] = {{"REEOT_VAL_AUTO", "auto"},   {"REEOT_VAL_ENGLISH", "en"}, {"REEOT_VAL_FRENCH", "fr"},
                                 {"REEOT_VAL_ITALIAN", "it"}, {"REEOT_VAL_GERMAN", "de"},  {"REEOT_VAL_SPANISH", "es"}};
std::vector<Choice> g_language_choices(std::begin(kLanguages), std::end(kLanguages));
std::vector<std::string> g_language_strings;
std::span<const Choice> LanguageChoices() { return g_language_choices; }

void RefreshLanguageChoices() {
  g_language_choices.assign(std::begin(kLanguages), std::end(kLanguages));
  g_language_strings.clear();
  char lines[1024] = {};
  if (!mods::Api().languages || mods::Api().languages(lines, sizeof(lines)) <= 0)
    return;
  std::vector<std::pair<std::string, std::string>> found;
  std::string_view rest(lines);
  while (!rest.empty()) {
    const size_t nl = rest.find('\n');
    const std::string_view line = rest.substr(0, nl);
    rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
    const size_t tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0)
      continue;
    found.emplace_back(std::string(line.substr(0, tab)), std::string(line.substr(tab + 1)));
  }
  g_language_strings.reserve(found.size() * 2);
  for (const auto &[tag, name] : found) {
    bool shipped = false;
    for (const Choice &c : kLanguages)
      shipped = shipped || tag == c.value;
    if (shipped)
      continue;
    g_language_strings.push_back(tag);
    g_language_strings.push_back(name.empty() ? tag : name);
  }
  for (size_t i = 0; i + 1 < g_language_strings.size(); i += 2)
    g_language_choices.push_back({nullptr, g_language_strings[i].c_str(), g_language_strings[i + 1].c_str()});
}
constexpr Choice kNormalInverted[] = {{"REEOT_VAL_NORMAL", "false"}, {"REEOT_VAL_INVERTED", "true"}};
constexpr Choice kResolution[] = {{"REEOT_VAL_NATIVE", "native"}, {"REEOT_VAL_720P", "720p"},
                                  {"REEOT_VAL_1080P", "1080p"},   {"REEOT_VAL_1440P", "1440p"},
                                  {"REEOT_VAL_2160P", "2160p"},   {"REEOT_VAL_DISPLAY", "display"}};
constexpr Choice kDisplayScale[] = {{"REEOT_VAL_50_PERCENT", "50"},   {"REEOT_VAL_66_PERCENT", "66"},
                                    {"REEOT_VAL_75_PERCENT", "75"},   {"REEOT_VAL_100_PERCENT", "100"},
                                    {"REEOT_VAL_150_PERCENT", "150"}, {"REEOT_VAL_200_PERCENT", "200"}};
constexpr Choice kFullscreenMode[] = {{"REEOT_VAL_BORDERLESS", "borderless"},
                                      {"REEOT_VAL_EXCLUSIVE", "exclusive"}};
constexpr Choice kPipScale[] = {{"REEOT_VAL_50_PERCENT", "50"}, {"REEOT_VAL_66_PERCENT", "66"},
                                {"REEOT_VAL_100_PERCENT", "100"}};
constexpr Choice kAspect[] = {{"REEOT_VAL_4_3", "4:3"},   {"REEOT_VAL_16_10", "16:10"}, {"REEOT_VAL_16_9", "16:9"},
                              {"REEOT_VAL_21_9", "21:9"}, {"REEOT_VAL_32_9", "32:9"}};
constexpr Choice kPreset[] = {{"REEOT_VAL_LOW", "low"}, {"REEOT_VAL_MEDIUM", "medium"}, {"REEOT_VAL_HIGH", "high"},
                              {"REEOT_VAL_ULTRA", "ultra"}, {"REEOT_VAL_CUSTOM", "custom"}};
constexpr Choice kMsaa[] = {{"REEOT_VAL_OFF", "0"}, {"REEOT_VAL_2X", "2"}, {"REEOT_VAL_4X", "4"}, {"REEOT_VAL_8X", "8"}};
constexpr Choice kAnisotropy[] = {{"REEOT_VAL_OFF", "0"}, {"REEOT_VAL_2X", "2"},   {"REEOT_VAL_4X", "4"},
                                  {"REEOT_VAL_8X", "8"},  {"REEOT_VAL_16X", "16"}};
constexpr Choice kShadowSize[] = {{"REEOT_VAL_AUTO", "0"},     {"REEOT_VAL_1024", "1024"},
                                  {"REEOT_VAL_2048", "2048"}, {"REEOT_VAL_4096", "4096"},
                                  {"REEOT_VAL_8192", "8192"}};
constexpr Choice kUpscale[] = {{"REEOT_VAL_BILINEAR", "bilinear"}, {"REEOT_VAL_BICUBIC", "bicubic"},
                               {"REEOT_VAL_LANCZOS", "lanczos"}};
constexpr Choice kGlyphs[] = {{"REEOT_VAL_AUTO", "auto"},
                              {"REEOT_VAL_XBOX", "xbox"},
                              {"REEOT_VAL_SWITCH", "switch"},
                              {"REEOT_VAL_KEYBOARD", "keyboard"}};

bool RenderScaleApplies() {
  return rex::cvar::Query<bool>("fullscreen") && rex::cvar::GetFlagByName("eot_resolution") == "native";
}
bool DisplayScaleApplies() {
  return rex::cvar::Query<bool>("fullscreen") && rex::cvar::GetFlagByName("eot_resolution") == "display";
}

bool FullscreenOn() { return rex::cvar::Query<bool>("fullscreen"); }

bool KeyboardMouseOn() { return rex::cvar::Query<bool>("mnk_mode"); }
bool MouseLookOn() { return KeyboardMouseOn() && FullscreenOn(); }

constexpr Setting kAudioSettings[] = {
    {.label = "REEOT_OPT_SFX_VOLUME", .description = "REEOT_DESC_SFX_VOLUME", .accessor = &kFxVolume,
     .slider = {0.0, 1.0, 0.05, Format::kPercent}},
    {.label = "REEOT_OPT_VOICE_VOLUME", .description = "REEOT_DESC_VOICE_VOLUME", .accessor = &kVoiceVolume,
     .slider = {0.0, 1.0, 0.05, Format::kPercent}},
    {.label = "REEOT_OPT_MUSIC_VOLUME", .description = "REEOT_DESC_MUSIC_VOLUME", .accessor = &kMusicVolume,
     .slider = {0.0, 1.0, 0.05, Format::kPercent}},
    {.label = "REEOT_OPT_SUBTITLES", .description = "REEOT_DESC_SUBTITLES", .accessor = &kSubtitles,
     .choices = kOnOff},
    {.label = "REEOT_OPT_SPATIAL_AUDIO", .description = "REEOT_DESC_SPATIAL_AUDIO", .cvar = "eot_spatial_audio",
     .choices = kOnOff},
    {.label = "REEOT_OPT_BATTLE_THEME", .description = "REEOT_DESC_BATTLE_THEME", .cvar = "eot_battle_theme",
     .choices = kOnOff},
    {.label = "REEOT_OPT_LANGUAGE", .description = "REEOT_DESC_LANGUAGE", .cvar = "eot_language",
     .restart = true, .choices_of = LanguageChoices},
};

constexpr Setting kVideoSettings[] = {
    {.label = "REEOT_OPT_FULLSCREEN", .description = "REEOT_DESC_FULLSCREEN", .cvar = "fullscreen",
     .choices = kOnOff},
    {.label = "REEOT_OPT_RESOLUTION", .description = "REEOT_DESC_RESOLUTION", .cvar = "eot_resolution",
     .choices = kResolution, .restart = true, .enabled = FullscreenOn, .disabled_text = "REEOT_VAL_STRETCH"},
    {.label = "REEOT_OPT_DISPLAY_SCALE", .description = "REEOT_DESC_DISPLAY_SCALE", .cvar = "eot_display_scale",
     .choices = kDisplayScale, .numeric = true, .restart = true, .enabled = DisplayScaleApplies},
    {.label = "REEOT_OPT_FULLSCREEN_MODE", .description = "REEOT_DESC_FULLSCREEN_MODE",
     .cvar = "eot_fullscreen_mode", .choices = kFullscreenMode, .restart = true, .enabled = FullscreenOn},
    {.label = "REEOT_OPT_PIP_SCALE", .description = "REEOT_DESC_PIP_SCALE", .cvar = "eot_pip_scale",
     .choices = kPipScale, .numeric = true, .restart = true},
    {.label = "REEOT_OPT_RENDER_SCALE", .description = "REEOT_DESC_RENDER_SCALE", .cvar = "eot_render_scale",
     .slider = {0.5, 2.0, 0.25, Format::kPercent}, .numeric = true, .restart = true, .enabled = RenderScaleApplies},
    {.label = "REEOT_OPT_ASPECT", .description = "REEOT_DESC_ASPECT", .cvar = "eot_aspect_ratio",
     .choices = kAspect, .enabled = FullscreenOn, .disabled_text = "REEOT_VAL_STRETCH"},
    {.label = "REEOT_OPT_FPS_LIMIT", .description = "REEOT_DESC_FPS_LIMIT", .cvar = "eot_fps_limit",
     .slider = {30.0, 240.0, 10.0, Format::kFrameRate}, .numeric = true},
    {.label = "REEOT_OPT_VSYNC", .description = "REEOT_DESC_VSYNC", .cvar = "eot_vsync", .choices = kOnOff},
    {.label = "REEOT_OPT_FPS_OVERLAY", .description = "REEOT_DESC_FPS_OVERLAY", .cvar = "show_fps_overlay",
     .choices = kOnOff},
    {.label = "REEOT_OPT_BRIGHTNESS", .description = "REEOT_DESC_BRIGHTNESS", .cvar = "eot_brightness",
     .slider = {-0.25, 0.25, 0.01, Format::kSignedPercent}, .numeric = true},
    {.label = "REEOT_OPT_CONTRAST", .description = "REEOT_DESC_CONTRAST", .cvar = "eot_contrast",
     .slider = {0.5, 1.5, 0.02, Format::kPercent}, .numeric = true},
    {.label = "REEOT_OPT_SATURATION", .description = "REEOT_DESC_SATURATION", .cvar = "eot_saturation",
     .slider = {0.0, 2.0, 0.1, Format::kPercent}, .numeric = true},
    {.label = "REEOT_OPT_GAMMA", .description = "REEOT_DESC_GAMMA", .cvar = "eot_gamma",
     .slider = {0.5, 2.0, 0.1, Format::kPercent}, .numeric = true},
};

constexpr Setting kGraphicsSettings[] = {
    {.label = "REEOT_OPT_PRESET", .description = "REEOT_DESC_PRESET", .cvar = "eot_quality_preset",
     .choices = kPreset},
    {.label = "REEOT_OPT_MSAA", .description = "REEOT_DESC_MSAA", .cvar = "eot_msaa", .choices = kMsaa,
     .numeric = true, .restart = true},
    {.label = "REEOT_OPT_TAA", .description = "REEOT_DESC_TAA", .cvar = "eot_taa", .choices = kOnOff},
    {.label = "REEOT_OPT_MOTION_VECTORS", .description = "REEOT_DESC_MOTION_VECTORS",
     .cvar = "eot_motion_vectors", .choices = kOnOff},
    {.label = "REEOT_OPT_ANISOTROPY", .description = "REEOT_DESC_ANISOTROPY", .cvar = "eot_anisotropy",
     .choices = kAnisotropy, .numeric = true},
    {.label = "REEOT_OPT_SHADOW_SIZE", .description = "REEOT_DESC_SHADOW_SIZE", .cvar = "eot_shadow_map_size",
     .choices = kShadowSize, .numeric = true, .restart = true},
    {.label = "REEOT_OPT_SHADOW_DISTANCE", .description = "REEOT_DESC_SHADOW_DISTANCE",
     .cvar = "eot_shadow_distance_scale", .slider = {0.5, 3.0, 0.25, Format::kPercent}, .numeric = true},
    {.label = "REEOT_OPT_UPSCALE", .description = "REEOT_DESC_UPSCALE", .cvar = "eot_upscale", .choices = kUpscale},
    {.label = "REEOT_OPT_FOV", .description = "REEOT_DESC_FOV", .cvar = "eot_fov_scale",
     .slider = {0.7, 1.5, 0.05, Format::kPercent}, .numeric = true, .restart = true},
    {.label = "REEOT_OPT_BLOOM", .description = "REEOT_DESC_BLOOM", .cvar = "eot_bloom", .choices = kOnOff},
    {.label = "REEOT_OPT_DOF", .description = "REEOT_DESC_DOF", .cvar = "eot_depth_of_field",
     .choices = kOnOffSwapped},
    {.label = "REEOT_OPT_MOTION_BLUR", .description = "REEOT_DESC_MOTION_BLUR", .cvar = "eot_motion_blur",
     .choices = kOnOff},
    {.label = "REEOT_OPT_RADIAL_BLUR", .description = "REEOT_DESC_RADIAL_BLUR", .cvar = "eot_radial_blur",
     .choices = kOnOff},
    {.label = "REEOT_OPT_COLOR_GRADING", .description = "REEOT_DESC_COLOR_GRADING", .cvar = "eot_color_grading",
     .choices = kOnOff},
};

constexpr Setting kGameSettings[] = {
    {.label = "REEOT_OPT_VIBRATION", .description = "REEOT_DESC_VIBRATION", .accessor = &kVibration,
     .choices = kOnOff},
    {.label = "REEOT_OPT_CAMERA_Y", .description = "REEOT_DESC_CAMERA_Y", .accessor = &kInvertY,
     .choices = kNormalInverted},
    {.label = "REEOT_OPT_CAMERA_X", .description = "REEOT_DESC_CAMERA_X", .accessor = &kInvertX,
     .choices = kNormalInverted},
    {.label = "REEOT_OPT_DEBUG_MODE", .description = "REEOT_DESC_DEBUG_MODE", .cvar = "eot_debug_mode",
     .choices = kOnOff, .restart = true},
    {.label = "REEOT_OPT_ACHIEVEMENT_TOASTS", .description = "REEOT_DESC_ACHIEVEMENT_TOASTS",
     .cvar = "eot_achievement_notifications", .choices = kOnOff},
};

constexpr Setting kControlsSettings[] = {
    {.label = "REEOT_OPT_GLYPHS", .description = "REEOT_DESC_GLYPHS", .cvar = "eot_button_glyphs",
     .choices = kGlyphs},
    {.label = "REEOT_OPT_MNK", .description = "REEOT_DESC_MNK", .accessor = &kKeyboardMouse, .choices = kOnOff},
    {.label = "REEOT_OPT_MOUSE_SENSITIVITY", .description = "REEOT_DESC_MOUSE_SENSITIVITY",
     .cvar = "mnk_sensitivity", .slider = {0.1, 1.9, 0.1, Format::kPlain}, .numeric = true,
     .enabled = MouseLookOn},
    {.label = "REEOT_OPT_BACKGROUND_INPUT", .description = "REEOT_DESC_BACKGROUND_INPUT",
     .cvar = "eot_background_input", .choices = kOnOff},
};

constexpr Page kAudioPage = {"REEOT_AUDIO_TITLE", "Audio", kAudioSettings, &kWide};
constexpr Page kVideoPage = {"REEOT_VIDEO_TITLE", "Video", kVideoSettings, &kWide};
constexpr Page kGraphicsPage = {"REEOT_GRAPHICS_TITLE", "Graphics", kGraphicsSettings, &kWide};
constexpr Page kGamePage = {"REEOT_GAME_TITLE", "Game", kGameSettings, &kWide};
constexpr Page kControlsPage = {"REEOT_CONTROLS_TITLE", "Controls", kControlsSettings, &kNarrow};

Page g_mods_page = {"REEOT_MODS_TITLE", "Mods", {}, &kTable, true};
std::vector<eot_mod_info> g_mods;
std::string g_remove_armed;
int32_t g_import_state = EOT_MODS_IDLE;
std::string g_mods_message;
bool g_mods_changed = false;
bool g_prompts_sent = false;

bool ImportIdle() { return g_import_state == EOT_MODS_IDLE; }

void RefreshMods() {
  g_mods.clear();
  const mods::Host &api = mods::Api();
  if (!api.count || !api.get)
    return;
  const int32_t count = api.count();
  for (int32_t i = 0; i < count; ++i) {
    eot_mod_info info;
    if (api.get(i, &info))
      g_mods.push_back(info);
  }
}

std::string Narrow(std::string_view utf8) {
  std::string out;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      const uint32_t code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
      out.push_back(code < 0x100 ? static_cast<char>(code) : '?');
      i += 2;
    } else {
      out.push_back('?');
      i += (c & 0xF0) == 0xF0 ? 4 : (c & 0xE0) == 0xE0 ? 3 : 1;
    }
  }
  return out;
}

constexpr uint32_t kRetailCount = 5;
constexpr uint32_t kAudioIndex = 0;
constexpr uint32_t kVideoIndex = 1;
constexpr uint32_t kGraphicsIndex = 2;
constexpr uint32_t kGameIndex = 3;
constexpr uint32_t kDifficultyIndex = 4;
constexpr uint32_t kControlsIndex = 5;
constexpr uint32_t kModsIndex = 6;
constexpr uint32_t kScreenCursorOff = 84;

bool ModsOnBar() { return rex::cvar::Query<bool>("eot_debug_mode"); }
uint32_t LastBarIndex() { return ModsOnBar() ? kModsIndex : kControlsIndex; }

const Page *PageAt(uint32_t cursor) {
  switch (cursor) {
  case kAudioIndex:
    return &kAudioPage;
  case kVideoIndex:
    return &kVideoPage;
  case kGraphicsIndex:
    return &kGraphicsPage;
  case kGameIndex:
    return &kGamePage;
  case kControlsIndex:
    return &kControlsPage;
  case kModsIndex:
    return ModsOnBar() ? &g_mods_page : nullptr;
  default:
    return nullptr;
  }
}

namespace cfg {
constexpr uint32_t kVtable = 0;
constexpr uint32_t kPriority = 4;
constexpr uint32_t kPadMask = 8;
constexpr uint32_t kInputDelay = 12;
constexpr uint32_t kOpenTime = 16;
constexpr uint32_t kCloseTime = 20;
constexpr uint32_t kBackdropAlpha = 24;
constexpr uint32_t kFlags = 28;
constexpr uint32_t kTitle = 32;
constexpr uint32_t kTitleCount = 6;
constexpr uint32_t kWindow = 56;
constexpr uint32_t kAuxWindow = 60;
constexpr uint32_t kAuxWindowCount = 3;
constexpr uint32_t kOne = 72;
constexpr uint32_t kPage = 76;
constexpr uint32_t kBody = 84;
constexpr uint32_t kResult = 88;
constexpr uint32_t kSize = 320;

constexpr uint8_t kFlagInputBlock = 0x04;
constexpr uint8_t kFlagOwnedByCaller = 0x20;

constexpr uint32_t kResultAcceptSave = 0;
constexpr uint32_t kResultAccept = 1;
constexpr uint32_t kResultCancel = 2;
constexpr uint32_t kResultNone = 3;

constexpr uint32_t kGameOptionsVtable = 0x880894DC;
constexpr uint32_t kYesNoVtable = 0x8808840C;
constexpr uint32_t kYesNoChoice = 100;
constexpr uint32_t kYesNoYes = 1;
}

constexpr uint32_t kComponentConfigOff = 36;

constexpr uint32_t kInputAccept = 9;
constexpr uint32_t kInputBack = 10;
constexpr uint32_t kInputReset = 18;
constexpr uint32_t kInputRemove = 19;
constexpr uint32_t kInputAxisX = 7;
constexpr uint32_t kInputAxisY = 8;
constexpr uint32_t kPadMask = 1;

constexpr double kRepeatDelay = 0.40;
constexpr double kRepeatInterval = 0.09;
constexpr double kAxisHeld = 0.5;

namespace ctl {
constexpr uint32_t kRoot = 0;
constexpr uint32_t kLabel = 4;
constexpr uint32_t kChoiceLeft = 8;
constexpr uint32_t kChoiceRight = 12;
constexpr uint32_t kChoiceValue = 16;
constexpr uint32_t kChoiceHandles = 20;
constexpr uint32_t kChoiceHandleSlots = 8;
constexpr uint32_t kChoiceCount = 52;
constexpr uint32_t kChoiceSize = 56;
constexpr uint32_t kSliderKnob = 20;
constexpr uint32_t kSliderKnobMin = 24;
constexpr uint32_t kSliderKnobMax = 28;
constexpr uint32_t kSliderSize = 40;
constexpr uint32_t kStateIdle = 0;
constexpr uint32_t kStateSelected = 1;
constexpr uint32_t kStateDisabled = 2;
}

constexpr uint8_t kShadeIdle[4] = {135, 135, 135, 255};
constexpr uint8_t kShadeSelected[4] = {230, 220, 220, 255};
constexpr uint8_t kShadeDisabled[4] = {135, 135, 135, 62};

constexpr uint32_t kRows = 6;
constexpr uint32_t kLayoutBlock = kRows * (ctl::kChoiceSize + ctl::kSliderSize);
constexpr uint32_t kBlockControls = 0;
constexpr uint32_t kBlockHandles = kBlockControls + kLayoutCount * kLayoutBlock;
constexpr uint32_t kBlockColour = kBlockHandles + 16;
constexpr uint32_t kBlockText = kBlockColour + 16;
constexpr uint32_t kBlockTextSize = 640;
constexpr uint32_t kBlockLine = kBlockText + kBlockTextSize;
constexpr uint32_t kBlockLineChars = 1024;
constexpr uint32_t kBlockSize = kBlockLine + kBlockLineChars * 2;

namespace tcr {
constexpr uint32_t kTweenStart = 76;
constexpr uint32_t kTweenEnd = 92;
constexpr uint32_t kCurrentSlotPtr = 0x883CA29C;
constexpr uint32_t kSlotTable = 0x88401058;
constexpr uint32_t kSlotSize = 28;
constexpr uint32_t kTitleTextCrc = 0xAE495FEE;
constexpr float kStartScale = 0.1f;
}

constexpr float kTextScale = 1.1f;
constexpr float kHeaderScale = 0.7f;
constexpr uint32_t kTextWndSetScale = 18;
constexpr uint32_t kTextWndGetXYScale = 67;
constexpr uint32_t kInfoTitleFits = 20;

constexpr float kTextBoxY = 0.03f;
constexpr float kTextBoxH = 0.50f;
constexpr float kArrowW = 0.031f;
constexpr float kArrowY = 0.07f, kArrowH = 0.42f;

uint32_t g_config = 0;
uint32_t g_restart_config = 0;
uint32_t g_block = 0;
uint32_t g_popup_id = 0;
bool g_popup_open = false;
bool g_restart_asked = false;

const Page *g_page = nullptr;
uint32_t g_cursor = 0;
uint32_t g_first = 0;
std::vector<std::string> g_opened_with;
double g_held = 0.0;
int g_held_dir = 0;

struct Windows {
  uint32_t panel = hud::kNoWindow;
  uint32_t row[kRows] = {};
  uint32_t name[kRows] = {};
  uint32_t creator[kRows] = {};
  uint32_t kind[kRows] = {};
  uint32_t state[kRows] = {};
  uint32_t header[4] = {};
  uint32_t scroll_up = hud::kNoWindow;
  uint32_t scroll_down = hud::kNoWindow;
  uint32_t info_title = hud::kNoWindow;
  uint32_t info_text = hud::kNoWindow;
  uint32_t info_value = hud::kNoWindow;
  uint32_t info_note = hud::kNoWindow;
  float info_title_scale = 0.0f;
  bool found = false;
  bool built = false;
};
Windows g_windows[kLayoutCount];
uint32_t g_title = hud::kNoWindow;

const Layout &CurrentLayout() { return *g_page->layout; }
Windows &CurrentWindows() { return g_windows[LayoutIndex(g_page->layout)]; }

float g_retail_tween[8] = {};
bool g_retail_tween_saved = false;

uint32_t g_video_label = 0;
uint32_t g_graphics_label = 0;
uint32_t g_mods_label = 0;

uint32_t ChoiceControl(uint32_t row) {
  return g_block + kBlockControls + LayoutIndex(g_page->layout) * kLayoutBlock + row * ctl::kChoiceSize;
}
uint32_t SliderControl(uint32_t row) {
  return g_block + kBlockControls + LayoutIndex(g_page->layout) * kLayoutBlock + kRows * ctl::kChoiceSize +
         row * ctl::kSliderSize;
}

bool Pressed(const PPCContext &ctx, uint8_t *base, uint32_t input) {
  PPCContext call = ctx;
  call.r3.u32 = kPadMask;
  call.r4.u32 = input;
  __imp__eot_Input_IsPressed(call, base);
  return (call.r3.u32 & 0xFF) != 0;
}

double Axis(const PPCContext &ctx, uint8_t *base, uint32_t input) {
  PPCContext call = ctx;
  call.r3.u32 = kPadMask;
  call.r4.u32 = input;
  __imp__eot_Input_GetAxis(call, base);
  return call.f1.f64;
}

uint32_t StringHandle(const PPCContext &ctx, uint8_t *base, const char *name) {
  const uint32_t handle = hud::FindString(ctx, base, NameCrc(name));
  if (!handle)
    EOT_WARN("[menu] no string named {}; is ReeotMenu.pkz mounted?", name);
  return handle;
}

void CallWithFloat(const PPCContext &ctx, uint8_t *base, uint32_t addr, uint32_t r3, float f1) {
  PPCFunc *fn = addr ? rex::runtime::ResolveIndirectFunction(addr) : nullptr;
  if (!fn)
    return;
  PPCContext call = ctx;
  call.r3.u32 = r3;
  call.f1.f64 = f1;
  fn(call, base);
}

void ScaleText(const PPCContext &ctx, uint8_t *base, uint32_t window, float factor) {
  if (window == hud::kNoWindow || !window)
    return;
  if (factor == 1.0f) {
    CallWithFloat(ctx, base, hud::Entry(kTextWndSetScale), window, 0.0f);
    return;
  }
  const uint32_t scratch = g_block + kBlockColour;
  eot::mem::store<uint32_t>(scratch, 0);
  eot::mem::store<uint32_t>(scratch + 4, 0);
  hud::Call(ctx, base, kTextWndGetXYScale, window, scratch, scratch + 4);
  const float style = std::bit_cast<float>(eot::mem::load<uint32_t>(scratch));
  if (style > 0.0f)
    CallWithFloat(ctx, base, hud::Entry(kTextWndSetScale), window, style * factor);
}

void SetRect(const PPCContext &ctx, uint8_t *base, uint32_t window, float x, float y, float w, float h) {
  if (window == hud::kNoWindow || !window)
    return;
  const uint32_t rect = g_block + kBlockColour;
  hud::Call(ctx, base, hud::kWndGetPos, window, rect);
  const float wanted[4] = {x, y, w, h};
  for (uint32_t i = 0; i < 4; ++i)
    if (wanted[i] >= 0.0f)
      eot::mem::store<uint32_t>(rect + i * 4, std::bit_cast<uint32_t>(wanted[i]));
  hud::Call(ctx, base, hud::kWndSetPos, window, rect);
}

void PlaceLabel(const PPCContext &ctx, uint8_t *base, uint32_t window) {
  SetRect(ctx, base, window, -1.0f, kTextBoxY, -1.0f, kTextBoxH);
}

void PlaceValue(const PPCContext &ctx, uint8_t *base, const Layout &layout, uint32_t value, uint32_t left,
                uint32_t right) {
  SetRect(ctx, base, value, layout.value_x, kTextBoxY, layout.value_w, kTextBoxH);
  SetRect(ctx, base, left, layout.value_x - kArrowW - layout.arrow_gap, kArrowY, kArrowW, kArrowH);
  SetRect(ctx, base, right, layout.value_x + layout.value_w + layout.arrow_gap, kArrowY, kArrowW, kArrowH);
}

void SetShade(const PPCContext &ctx, uint8_t *base, uint32_t window_ptr, const uint8_t rgba[4]) {
  const uint32_t colour = g_block + kBlockColour;
  for (uint32_t i = 0; i < 4; ++i)
    eot::mem::store<uint8_t>(colour + i, rgba[i]);
  PPCContext call = ctx;
  call.r3.u32 = window_ptr;
  call.r4.u32 = colour;
  __imp__eot_Wnd_SetColorBytes(call, base);
}

void ShadeWindow(const PPCContext &ctx, uint8_t *base, uint32_t window, const uint8_t rgba[4]) {
  if (window == hud::kNoWindow || !window)
    return;
  eot::mem::store<uint32_t>(g_block + kBlockHandles + 12, window);
  SetShade(ctx, base, g_block + kBlockHandles + 12, rgba);
}

void SetStringHandle(const PPCContext &ctx, uint8_t *base, uint32_t window, uint32_t string_handle) {
  if (window == hud::kNoWindow || !window || !string_handle)
    return;
  eot::mem::store<uint32_t>(g_block + kBlockHandles + 8, window);
  PPCContext call = ctx;
  call.r3.u32 = g_block + kBlockHandles + 8;
  call.r4.u32 = string_handle;
  __imp__eot_TextWnd_SetStringHandle(call, base);
}

void SetLine(const PPCContext &ctx, uint8_t *base, uint32_t window, const char *line) {
  if (window == hud::kNoWindow || !window)
    return;
  const uint32_t text = g_block + kBlockText;
  uint32_t n = 0;
  for (uint32_t i = 0; line[i] && n < kBlockTextSize - 2; ++i) {
    eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>(line[i]));
    if (line[i] == '%')
      eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>('%'));
  }
  eot::mem::store<uint8_t>(text + n, 0);
  hud::Call(ctx, base, hud::kTextWndSetString, window, text);
}

bool Enabled(const Setting &s) { return !s.enabled || s.enabled(); }

int CurrentChoice(const Setting &s) {
  const std::string value = Value(s);
  const std::span<const Choice> choices = Choices(s);
  for (size_t i = 0; i < choices.size(); ++i) {
    const bool same = s.numeric ? std::fabs(Number(choices[i].value) - Number(value)) < 1e-4
                                : value == choices[i].value;
    if (same)
      return static_cast<int>(i);
  }
  return -1;
}

int NearestChoice(const Setting &s) {
  if (!s.numeric)
    return 0;
  const double have = Number(Value(s));
  int best = 0;
  double best_gap = 1e300;
  const std::span<const Choice> choices = Choices(s);
  for (size_t i = 0; i < choices.size(); ++i) {
    const double gap = std::fabs(Number(choices[i].value) - have);
    if (gap < best_gap) {
      best_gap = gap;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void ShowChoiceValue(const PPCContext &ctx, uint8_t *base, uint32_t control, const Setting &s) {
  const int index = CurrentChoice(s);
  const Choice *choice = index >= 0 ? &Choices(s)[static_cast<size_t>(index)] : nullptr;
  const char *text = !Enabled(s) && s.disabled_text ? s.disabled_text : choice ? choice->text : nullptr;
  if (text) {
    PPCContext call = ctx;
    call.r3.u32 = control + ctl::kChoiceValue;
    call.r4.u32 = StringHandle(ctx, base, text);
    __imp__eot_TextWnd_SetStringHandle(call, base);
    return;
  }
  SetLine(ctx, base, eot::mem::load<uint32_t>(control + ctl::kChoiceValue),
          choice && choice->literal ? choice->literal : Value(s).c_str());
}

void BindChoiceRow(const PPCContext &ctx, uint8_t *base, uint32_t row, const Setting &s, bool selected) {
  const uint32_t control = ChoiceControl(row);
  PPCContext call = ctx;
  call.r3.u32 = control;
  call.r4.u32 = StringHandle(ctx, base, s.label);
  __imp__eot_MultiValueControl_SetLabel(call, base);
  const std::span<const Choice> choices = Choices(s);
  hud::Activate(ctx, base, eot::mem::load<uint32_t>(control + ctl::kChoiceLeft), choices.size() > 1);
  hud::Activate(ctx, base, eot::mem::load<uint32_t>(control + ctl::kChoiceRight), choices.size() > 1);
  const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(choices.size()), ctl::kChoiceHandleSlots);
  for (uint32_t i = 0; i < count; ++i)
    eot::mem::store<uint32_t>(control + ctl::kChoiceHandles + i * 4,
                              choices[i].text ? StringHandle(ctx, base, choices[i].text) : 0);
  eot::mem::store<uint32_t>(control + ctl::kChoiceCount, count);
  ShowChoiceValue(ctx, base, control, s);
  call = ctx;
  call.r3.u32 = control;
  call.r4.u32 = !Enabled(s) ? ctl::kStateDisabled : selected ? ctl::kStateSelected : ctl::kStateIdle;
  __imp__eot_MultiValueControl_SetState(call, base);
}

bool StepChoice(const Setting &s, int step) {
  const std::span<const Choice> choices = Choices(s);
  const int count = static_cast<int>(choices.size());
  int index = CurrentChoice(s);
  if (index < 0)
    index = NearestChoice(s) - (step > 0 ? 1 : 0);
  index = ((index + step) % count + count) % count;
  return SetValue(s, choices[static_cast<size_t>(index)].value);
}

int SliderStops(const Setting &s) {
  const int steps = static_cast<int>(std::lround((s.slider.max - s.slider.min) / s.slider.step));
  return steps + 1 + (s.slider.format == Format::kFrameRate ? 1 : 0);
}

bool IsUnlimitedStop(const Setting &s, int stop) {
  return s.slider.format == Format::kFrameRate && stop == SliderStops(s) - 1;
}

double StopValue(const Setting &s, int stop) {
  return IsUnlimitedStop(s, stop) ? 0.0 : s.slider.min + s.slider.step * stop;
}

int CurrentStop(const Setting &s) {
  const double have = Number(Value(s));
  const int stops = SliderStops(s);
  if (s.slider.format == Format::kFrameRate && have <= 0.0)
    return stops - 1;
  const int last_numbered = stops - 1 - (s.slider.format == Format::kFrameRate ? 1 : 0);
  const int stop = static_cast<int>(std::lround((have - s.slider.min) / s.slider.step));
  return std::clamp(stop, 0, last_numbered);
}

void FormatNumber(const Setting &s, double value, char *out, size_t size) {
  switch (s.slider.format) {
  case Format::kPercent:
    std::snprintf(out, size, "%d%%", static_cast<int>(std::lround(value * 100.0)));
    break;
  case Format::kSignedPercent:
    std::snprintf(out, size, "%+d%%", static_cast<int>(std::lround(value * 100.0)));
    break;
  case Format::kFrameRate:
    if (value <= 0.0)
      std::snprintf(out, size, "Unlimited");
    else
      std::snprintf(out, size, "%d", static_cast<int>(std::lround(value)));
    break;
  default:
    std::snprintf(out, size, "%g", value);
    break;
  }
}

void FormatCurrent(const Setting &s, char *out, size_t size) { FormatNumber(s, Number(Value(s)), out, size); }

void ShowSliderValue(const PPCContext &ctx, uint8_t *base, uint32_t control, const Setting &s) {
  const int stop = CurrentStop(s);
  const float t = static_cast<float>(stop) / static_cast<float>(std::max(1, SliderStops(s) - 1));
  const float lo = std::bit_cast<float>(eot::mem::load<uint32_t>(control + ctl::kSliderKnobMin));
  const float hi = std::bit_cast<float>(eot::mem::load<uint32_t>(control + ctl::kSliderKnobMax));
  PPCContext call = ctx;
  call.r3.u32 = control + ctl::kSliderKnob;
  call.f1.f64 = lo + (hi - lo) * t;
  __imp__eot_Wnd_SetX(call, base);
}

void BindSliderRow(const PPCContext &ctx, uint8_t *base, uint32_t row, const Setting &s, bool selected) {
  const uint32_t control = SliderControl(row);
  PPCContext call = ctx;
  call.r3.u32 = control + ctl::kLabel;
  call.r4.u32 = StringHandle(ctx, base, s.label);
  __imp__eot_TextWnd_SetStringHandle(call, base);
  ShowSliderValue(ctx, base, control, s);
  const bool on = Enabled(s);
  call = ctx;
  call.r3.u32 = control;
  call.r4.u32 = on && selected ? 1 : 0;
  __imp__eot_SliderControl_SetState(call, base);
  if (!on)
    for (uint32_t off = ctl::kRoot; off <= ctl::kSliderKnob; off += 4)
      SetShade(ctx, base, control + off, kShadeDisabled);
}

bool StepSlider(const Setting &s, int step) {
  const int stop = std::clamp(CurrentStop(s) + step, 0, SliderStops(s) - 1);
  if (stop == CurrentStop(s))
    return false;
  char value[32];
  std::snprintf(value, sizeof(value), "%g", StopValue(s, stop));
  return SetValue(s, value);
}

void ShowRowKind(const PPCContext &ctx, uint8_t *base, uint32_t row, bool slider) {
  const uint32_t choice_root = eot::mem::load<uint32_t>(ChoiceControl(row) + ctl::kRoot);
  hud::Call(ctx, base, slider ? hud::kWndRemoveFlags : hud::kWndAddFlags, choice_root, hud::kFlagActive);
  PPCContext call = ctx;
  call.r3.u32 = SliderControl(row);
  call.r4.u32 = slider ? 1 : 0;
  __imp__eot_SliderControl_Show(call, base);
}

uint32_t StringLength(const PPCContext &ctx, uint8_t *base, uint32_t string_handle) {
  if (!string_handle)
    return 0;
  constexpr uint32_t kStringTableResolveHandle = 0x821813A8;
  PPCFunc *fn = rex::runtime::ResolveIndirectFunction(kStringTableResolveHandle);
  if (!fn)
    return 0;
  const uint32_t line = g_block + kBlockLine;
  eot::mem::store<uint16_t>(line, 0);
  PPCContext call = ctx;
  call.r3.u32 = line;
  call.r4.u32 = string_handle;
  call.r5.u32 = 0;
  call.r6.u32 = 0;
  call.r7.u32 = 0;
  call.r8.u32 = 0;
  call.r9.u32 = 0;
  call.r10.u32 = 0;
  fn(call, base);
  uint32_t n = 0;
  while (n < kBlockLineChars - 1 && eot::mem::load<uint16_t>(line + n * 2))
    ++n;
  return n;
}

std::string ResolveText(const PPCContext &ctx, uint8_t *base, uint32_t string_handle) {
  std::string text;
  const uint32_t n = StringLength(ctx, base, string_handle);
  const uint32_t line = g_block + kBlockLine;
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t c = eot::mem::load<uint16_t>(line + i * 2);
    text.push_back(c < 0x100 ? static_cast<char>(c) : '?');
  }
  return text;
}

void FitInfoTitle(const PPCContext &ctx, uint8_t *base, uint32_t length) {
  Windows &w = CurrentWindows();
  if (w.info_title == hud::kNoWindow)
    return;
  if (w.info_title_scale <= 0.0f) {
    const uint32_t scratch = g_block + kBlockColour;
    eot::mem::store<uint32_t>(scratch, 0);
    eot::mem::store<uint32_t>(scratch + 4, 0);
    hud::Call(ctx, base, kTextWndGetXYScale, w.info_title, scratch, scratch + 4);
    w.info_title_scale = std::bit_cast<float>(eot::mem::load<uint32_t>(scratch));
    if (w.info_title_scale <= 0.0f)
      return;
  }
  const float factor =
      length > kInfoTitleFits ? static_cast<float>(kInfoTitleFits) / static_cast<float>(length) : 1.0f;
  CallWithFloat(ctx, base, hud::Entry(kTextWndSetScale), w.info_title,
                factor < 1.0f ? w.info_title_scale * factor : 0.0f);
}

void ShowInfo(const PPCContext &ctx, uint8_t *base, const Setting &s) {
  const Windows &w = CurrentWindows();
  const uint32_t label = StringHandle(ctx, base, s.label);
  FitInfoTitle(ctx, base, StringLength(ctx, base, label));
  SetStringHandle(ctx, base, w.info_title, label);
  SetStringHandle(ctx, base, w.info_text, StringHandle(ctx, base, s.description));
  hud::Activate(ctx, base, w.info_value, s.IsSlider());
  if (s.IsSlider()) {
    char now[32];
    FormatCurrent(s, now, sizeof(now));
    SetLine(ctx, base, w.info_value, now);
  }
  hud::Activate(ctx, base, w.info_note, s.restart);
}

void ShowModRows(const PPCContext &ctx, uint8_t *base) {
  const Windows &w = CurrentWindows();
  const uint32_t count = static_cast<uint32_t>(g_mods.size());
  static const char *const kKindStrings[] = {"REEOT_KIND_PACKAGE", "REEOT_KIND_REPLACEMENT", "REEOT_KIND_MODEL"};
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t index = g_first + row;
    const bool used = index < count;
    for (const uint32_t *column : {w.row, w.name, w.creator, w.kind, w.state})
      hud::Activate(ctx, base, column[row], used);
    if (!used)
      continue;
    const eot_mod_info &mod = g_mods[index];
    SetLine(ctx, base, w.name[row], Narrow(mod.name).c_str());
    SetLine(ctx, base, w.creator[row], Narrow(mod.creator).c_str());
    const uint32_t kind = mod.kind >= 0 && mod.kind < 3 ? static_cast<uint32_t>(mod.kind) : 1;
    SetStringHandle(ctx, base, w.kind[row], StringHandle(ctx, base, kKindStrings[kind]));
    SetStringHandle(ctx, base, w.state[row], StringHandle(ctx, base, mod.enabled ? "REEOT_VAL_ON" : "REEOT_VAL_OFF"));
    const uint8_t *shade = index == g_cursor ? kShadeSelected : mod.enabled ? kShadeIdle : kShadeDisabled;
    for (const uint32_t *column : {w.name, w.creator, w.kind, w.state})
      ShadeWindow(ctx, base, column[row], shade);
  }
  hud::Activate(ctx, base, w.scroll_up, g_first > 0);
  hud::Activate(ctx, base, w.scroll_down, g_first + kRows < count);
  std::string footer;
  if (g_import_state == EOT_MODS_CHOOSING || g_import_state == EOT_MODS_IMPORTING)
    footer = ResolveText(ctx, base, StringHandle(ctx, base, g_import_state == EOT_MODS_CHOOSING
                                                                ? "REEOT_MODS_CHOOSING"
                                                                : "REEOT_MODS_IMPORTING"));
  else if (!g_mods_message.empty())
    footer = Narrow(g_mods_message);
  else if (count == 0)
    footer = ResolveText(ctx, base, StringHandle(ctx, base, "REEOT_MODS_EMPTY"));
  else if (g_cursor < count && !g_remove_armed.empty() && g_remove_armed == g_mods[g_cursor].folder)
    footer = ResolveText(ctx, base, StringHandle(ctx, base, "REEOT_MODS_REMOVE_ARMED"));
  else if (g_cursor < count)
    footer = Narrow(g_mods[g_cursor].file) + ": " + Narrow(g_mods[g_cursor].status);
  if (g_mods_changed && g_mods_message.empty())
    footer += " " + ResolveText(ctx, base, StringHandle(ctx, base, "REEOT_DESC_RESTART"));
  SetLine(ctx, base, w.info_text, footer.c_str());
}

void ShowRows(const PPCContext &ctx, uint8_t *base) {
  if (g_page->table) {
    ShowModRows(ctx, base);
    return;
  }
  const Windows &w = CurrentWindows();
  const uint32_t count = static_cast<uint32_t>(g_page->settings.size());
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t index = g_first + row;
    const bool used = index < count;
    hud::Activate(ctx, base, w.row[row], used);
    if (!used)
      continue;
    const Setting &s = g_page->settings[index];
    ShowRowKind(ctx, base, row, s.IsSlider());
    if (s.IsSlider())
      BindSliderRow(ctx, base, row, s, index == g_cursor);
    else
      BindChoiceRow(ctx, base, row, s, index == g_cursor);
  }
  hud::Activate(ctx, base, w.scroll_up, g_first > 0);
  hud::Activate(ctx, base, w.scroll_down, g_first + kRows < count);
  ShowInfo(ctx, base, g_page->settings[g_cursor]);
}

void BuildRows(const PPCContext &ctx, uint8_t *base) {
  Windows &w = CurrentWindows();
  const Layout &layout = CurrentLayout();
  if (w.built)
    return;
  if (&layout == &kTable) {
    for (uint32_t row = 0; row < kRows; ++row)
      for (const uint32_t *column : {w.name, w.creator, w.kind, w.state})
        ScaleText(ctx, base, column[row], kTextScale);
    for (uint32_t i = 0; i < 4; ++i)
      ScaleText(ctx, base, w.header[i], kHeaderScale);
    w.built = true;
    EOT_INFO("[menu] the mods table's {} strips sized", kRows);
    return;
  }
  const uint32_t handles = g_block + kBlockHandles;
  eot::mem::store<uint32_t>(handles, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(handles + 4, 0xFFFFFFFFu);
  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t choice = ChoiceControl(row);
    for (uint32_t off = 0; off < ctl::kChoiceSize; off += 4)
      eot::mem::store<uint32_t>(choice + off, 0);
    PPCContext call = ctx;
    call.r3.u32 = choice;
    call.r4.u32 = handles;
    __imp__eot_MultiValueControl_Create(call, base);
    uint32_t root = eot::mem::load<uint32_t>(choice + ctl::kRoot);
    hud::Call(ctx, base, hud::kWndSetParent, root, w.row[row]);
    hud::Call(ctx, base, hud::kWndAddFlags, root, hud::kFlagActive);
    PlaceLabel(ctx, base, eot::mem::load<uint32_t>(choice + ctl::kLabel));
    PlaceValue(ctx, base, layout, eot::mem::load<uint32_t>(choice + ctl::kChoiceValue),
               eot::mem::load<uint32_t>(choice + ctl::kChoiceLeft), eot::mem::load<uint32_t>(choice + ctl::kChoiceRight));
    for (uint32_t off : {ctl::kLabel, ctl::kChoiceValue})
      ScaleText(ctx, base, eot::mem::load<uint32_t>(choice + off), kTextScale);

    const uint32_t slider = SliderControl(row);
    for (uint32_t off = 0; off < ctl::kSliderSize; off += 4)
      eot::mem::store<uint32_t>(slider + off, 0);
    call = ctx;
    call.r3.u32 = slider;
    __imp__eot_SliderControl_Create(call, base);
    root = eot::mem::load<uint32_t>(slider + ctl::kRoot);
    hud::Call(ctx, base, hud::kWndSetParent, root, w.row[row]);
    PlaceLabel(ctx, base, eot::mem::load<uint32_t>(slider + ctl::kLabel));
    ScaleText(ctx, base, eot::mem::load<uint32_t>(slider + ctl::kLabel), kTextScale);
  }
  w.built = true;
  EOT_INFO("[menu] {} settings rows built for the {} layout", kRows, &layout == &kNarrow ? "narrow" : "wide");
}

bool FindWindows(const PPCContext &ctx, uint8_t *base) {
  Windows &w = CurrentWindows();
  const Layout &layout = CurrentLayout();
  if (w.found)
    return true;
  w.panel = hud::Find(ctx, base, NameCrc(layout.panel));
  w.scroll_up = hud::Find(ctx, base, NameCrc(layout.scroll_up));
  w.scroll_down = hud::Find(ctx, base, NameCrc(layout.scroll_down));
  w.info_title = hud::Find(ctx, base, NameCrc(layout.info_title));
  w.info_text = hud::Find(ctx, base, NameCrc(layout.info_text));
  w.info_value = hud::Find(ctx, base, NameCrc(layout.info_value));
  w.info_note = hud::Find(ctx, base, NameCrc(layout.info_note));
  if (g_title == hud::kNoWindow)
    g_title = hud::Find(ctx, base, tcr::kTitleTextCrc);
  for (uint32_t i = 0; i < kRows; ++i) {
    char name[48];
    std::snprintf(name, sizeof(name), "%s%02u", layout.row_prefix, i);
    w.row[i] = hud::Find(ctx, base, NameCrc(name));
  }
  if (&layout == &kTable) {
    struct Column {
      const char *prefix;
      uint32_t *slots;
    };
    const Column columns[] = {{"Reeot_ModsName", w.name},
                              {"Reeot_ModsCreator", w.creator},
                              {"Reeot_ModsKind", w.kind},
                              {"Reeot_ModsState", w.state},
                              {"Reeot_ModsHead", w.header}};
    for (const Column &column : columns) {
      const uint32_t count = column.slots == w.header ? 4 : kRows;
      for (uint32_t i = 0; i < count; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "%s%02u", column.prefix, i);
        column.slots[i] = hud::Find(ctx, base, NameCrc(name));
      }
    }
  }
  w.found = w.panel != hud::kNoWindow;
  if (!w.found)
    EOT_WARN("[menu] {} is not there; is ReeotMenu.pkz mounted?", layout.panel);
  return w.found;
}

void DressWindows(const PPCContext &ctx, uint8_t *base) {
  const Windows &w = CurrentWindows();
  if (w.scroll_down == hud::kNoWindow)
    return;
  const float corners[8] = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  const uint32_t scratch = g_block + kBlockText;
  for (uint32_t i = 0; i < 8; ++i)
    eot::mem::store<uint32_t>(scratch + i * 4, std::bit_cast<uint32_t>(corners[i]));
  for (uint32_t wide = 0; wide <= 1; ++wide)
    hud::Call(ctx, base, hud::kWnd2DSetUVs, w.scroll_down, scratch, scratch + 8, scratch + 16, scratch + 24, wide);
}

void FillConfig(uint32_t c, uint32_t window, uint32_t title) {
  for (uint32_t off = 0; off < cfg::kSize; off += 4)
    eot::mem::store<uint32_t>(c + off, 0);
  eot::mem::store<uint32_t>(c + cfg::kVtable, cfg::kGameOptionsVtable);
  eot::mem::store<uint32_t>(c + cfg::kPriority, 0);
  eot::mem::store<uint32_t>(c + cfg::kPadMask, kPadMask);
  eot::mem::store<float>(c + cfg::kInputDelay, 0.0f);
  eot::mem::store<float>(c + cfg::kOpenTime, 0.25f);
  eot::mem::store<float>(c + cfg::kCloseTime, 0.15f);
  eot::mem::store<float>(c + cfg::kBackdropAlpha, 0.65f);
  eot::mem::store<uint8_t>(c + cfg::kFlags, cfg::kFlagOwnedByCaller | cfg::kFlagInputBlock);
  for (uint32_t i = 0; i < cfg::kTitleCount; ++i)
    eot::mem::store<uint32_t>(c + cfg::kTitle + i * 4, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kTitle, title);
  eot::mem::store<uint32_t>(c + cfg::kWindow, window);
  for (uint32_t i = 0; i < cfg::kAuxWindowCount; ++i)
    eot::mem::store<uint32_t>(c + cfg::kAuxWindow + i * 4, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kOne, 1);
  eot::mem::store<uint32_t>(c + cfg::kPage, 0);
  eot::mem::store<uint32_t>(c + cfg::kBody, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kResult, cfg::kResultNone);
}

void OpenPage(const PPCContext &ctx, uint8_t *base, const Page &page) {
  CallScope scope(ctx, base);
  g_page = &page;
  RefreshLanguageChoices();
  if (page.table) {
    g_mods_message.clear();
    g_remove_armed.clear();
    g_mods_changed = false;
    g_prompts_sent = false;
    g_import_state = mods::Api().import_state ? mods::Api().import_state(nullptr, 0) : EOT_MODS_IDLE;
    RefreshMods();
    if (!mods::Api().Bound())
      EOT_WARN("[menu] the host has no mods entry points; the Mods page can do nothing");
  }
  if (!g_config)
    g_config = AllocGuest(ctx, base, cfg::kSize);
  if (!g_block)
    g_block = AllocGuest(ctx, base, kBlockSize);
  if (!g_config || !g_block || !FindWindows(ctx, base)) {
    EOT_WARN("[menu] {} page: config {:#x} block {:#x}; not opening", page.label, g_config, g_block);
    g_page = nullptr;
    return;
  }
  BuildRows(ctx, base);
  DressWindows(ctx, base);

  g_cursor = 0;
  g_first = 0;
  g_held = 0.0;
  g_held_dir = 0;
  g_opened_with.clear();
  for (const Setting &s : page.settings)
    g_opened_with.push_back(Value(s));

  FillConfig(g_config, CurrentWindows().panel, StringHandle(ctx, base, page.title));
  PPCContext call = ctx;
  call.r3.u32 = g_config;
  __imp__eot_YesNoWindow_Open(call, base);
  g_popup_id = call.r3.u32;
  g_popup_open = g_popup_id != 0xFFFFFFFFu;
  EOT_INFO("[menu] {} page: {} {}, panel {:#x} -> pop-up id {:#x}", page.label,
           page.table ? g_mods.size() : page.settings.size(), page.table ? "mods" : "settings",
           CurrentWindows().panel, g_popup_id);
}

bool RestartDue() {
  if (g_page->table)
    return g_mods_changed;
  for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i)
    if (g_page->settings[i].restart && Value(g_page->settings[i]) != g_opened_with[i])
      return true;
  return false;
}

bool RetailChanged() {
  for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i)
    if (g_page->settings[i].accessor && Value(g_page->settings[i]) != g_opened_with[i])
      return true;
  return false;
}

void UndoRestartBound() {
  for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i) {
    const Setting &s = g_page->settings[i];
    if (s.restart && Value(s) != g_opened_with[i]) {
      SetValue(s, g_opened_with[i]);
      EOT_INFO("[menu] {} back to {} (no restart)", Name(s), g_opened_with[i]);
    }
  }
  rex::cvar::InvokeCommand("eot_save_settings", "");
}

void AskRestart(const PPCContext &ctx, uint8_t *base) {
  if (!g_restart_config)
    g_restart_config = AllocGuest(ctx, base, cfg::kSize);
  if (!g_restart_config)
    return;
  for (uint32_t off = 0; off < cfg::kSize; off += 4)
    eot::mem::store<uint32_t>(g_restart_config + off, 0);
  PPCContext call = ctx;
  call.r3.u32 = g_restart_config;
  __imp__eot_YesNoWindow_InitConfig(call, base);
  eot::mem::store<uint32_t>(g_restart_config + cfg::kVtable, cfg::kYesNoVtable);
  eot::mem::store<uint32_t>(g_restart_config + cfg::kTitle, StringHandle(ctx, base, "REEOT_RESTART_TITLE"));
  eot::mem::store<uint32_t>(g_restart_config + cfg::kBody, StringHandle(ctx, base, "REEOT_RESTART_BODY"));
  eot::mem::store<uint32_t>(g_restart_config + kYesNoCfgWindow, hud::Find(ctx, base, NameCrc("Reeot_LeaveWindow")));
  eot::mem::store<uint32_t>(g_restart_config + kYesNoCfgYes, hud::Find(ctx, base, NameCrc("Reeot_LeaveYes")));
  eot::mem::store<uint32_t>(g_restart_config + kYesNoCfgNo, hud::Find(ctx, base, NameCrc("Reeot_LeaveNo")));
  call = ctx;
  call.r3.u32 = g_restart_config;
  __imp__eot_YesNoWindow_Open(call, base);
  g_restart_asked = call.r3.u32 != 0xFFFFFFFFu;
  EOT_INFO("[menu] restart asked -> pop-up id {:#x}", call.r3.u32);
}

void Close(const PPCContext &ctx, uint8_t *base, bool accept) {
  bool restart = false;
  uint32_t result = cfg::kResultCancel;
  if (!accept) {
    for (size_t i = 0; i < g_page->settings.size() && i < g_opened_with.size(); ++i)
      if (Value(g_page->settings[i]) != g_opened_with[i])
        SetValue(g_page->settings[i], g_opened_with[i]);
  } else {
    restart = RestartDue();
    result = RetailChanged() ? cfg::kResultAcceptSave : cfg::kResultAccept;
    if (!rex::cvar::InvokeCommand("eot_save_settings", ""))
      EOT_WARN("[menu] eot_save_settings is not registered; the settings hold until exit");
  }
  eot::mem::store<uint32_t>(g_config + cfg::kResult, result);
  PlayCue(ctx, base, accept && !g_page->table ? kCueAccept : kCueBack);
  if (restart)
    AskRestart(ctx, base);
}

uint32_t RowCount() { return static_cast<uint32_t>(g_page->table ? g_mods.size() : g_page->settings.size()); }

void MoveCursor(const PPCContext &ctx, uint8_t *base, int step) {
  const int count = static_cast<int>(RowCount());
  const int next = static_cast<int>(g_cursor) + step;
  if (next < 0 || next >= count)
    return;
  g_cursor = static_cast<uint32_t>(next);
  if (g_cursor < g_first)
    g_first = g_cursor;
  else if (g_cursor >= g_first + kRows)
    g_first = g_cursor - kRows + 1;
  PlayCue(ctx, base, step > 0 ? kCueDown : kCueUp);
  ShowRows(ctx, base);
}

void ChangeValue(const PPCContext &ctx, uint8_t *base, int step) {
  const Setting &s = g_page->settings[g_cursor];
  if (!Enabled(s)) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  const bool moved = s.IsSlider() ? StepSlider(s, step) : StepChoice(s, step);
  if (!moved) {
    if (!s.IsSlider()) {
      EOT_WARN("[menu] {} refused the change", Name(s));
      PlayCue(ctx, base, kCueDenied);
    }
    return;
  }
  EOT_INFO("[menu] {} = {}", Name(s), Value(s));
  PlayCue(ctx, base, kCueChange);
  ShowRows(ctx, base);
}

void NoteModsResult(const PPCContext &ctx, uint8_t *base, bool ok, const char *message) {
  g_mods_message = message;
  g_mods_changed = g_mods_changed || ok;
  g_remove_armed.clear();
  PlayCue(ctx, base, ok ? kCueAccept : kCueDenied);
  EOT_INFO("[menu] mods: {}", g_mods_message);
  RefreshMods();
  if (g_cursor >= g_mods.size())
    g_cursor = g_mods.empty() ? 0 : static_cast<uint32_t>(g_mods.size()) - 1;
  if (g_first > g_cursor)
    g_first = g_cursor;
  ShowRows(ctx, base);
}

void ToggleMod(const PPCContext &ctx, uint8_t *base) {
  const mods::Host &api = mods::Api();
  if (g_cursor >= g_mods.size() || !ImportIdle() || !api.Bound()) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  const eot_mod_info &mod = g_mods[g_cursor];
  char message[512];
  const bool ok = api.set_enabled(mod.folder, mod.enabled ? 0 : 1, message, sizeof(message)) != 0;
  NoteModsResult(ctx, base, ok, message);
}

void RemoveMod(const PPCContext &ctx, uint8_t *base) {
  const mods::Host &api = mods::Api();
  if (g_cursor >= g_mods.size() || !ImportIdle() || !api.Bound()) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  const eot_mod_info &mod = g_mods[g_cursor];
  if (g_remove_armed != mod.folder) {
    g_remove_armed = mod.folder;
    g_mods_message.clear();
    PlayCue(ctx, base, kCueChange);
    ShowRows(ctx, base);
    return;
  }
  char message[512];
  const bool ok = api.remove(mod.folder, message, sizeof(message)) != 0;
  NoteModsResult(ctx, base, ok, message);
}

void AddMod(const PPCContext &ctx, uint8_t *base) {
  const mods::Host &api = mods::Api();
  if (!ImportIdle() || !api.Bound() || !api.add_begin()) {
    PlayCue(ctx, base, kCueDenied);
    return;
  }
  g_mods_message.clear();
  g_remove_armed.clear();
  g_import_state = EOT_MODS_CHOOSING;
  PlayCue(ctx, base, kCueAccept);
  EOT_INFO("[menu] mods: the file browser is up");
  ShowRows(ctx, base);
}

void PollImport(const PPCContext &ctx, uint8_t *base) {
  if (!mods::Api().import_state)
    return;
  char message[512];
  const int32_t state = mods::Api().import_state(message, sizeof(message));
  if (state == g_import_state)
    return;
  g_import_state = state;
  if (state == EOT_MODS_DONE || state == EOT_MODS_FAILED) {
    mods::Api().import_acknowledge();
    g_import_state = EOT_MODS_IDLE;
    NoteModsResult(ctx, base, state == EOT_MODS_DONE, message);
    return;
  }
  ShowRows(ctx, base);
}

void ShowTablePrompts(const PPCContext &ctx, uint8_t *base) {
  if (g_prompts_sent)
    return;
  g_prompts_sent = true;
  OverridePrompt(kPromptA, "REEOT_PROMPT_TOGGLE");
  OverridePrompt(kPromptX, "REEOT_PROMPT_REMOVE");
  OverridePrompt(kPromptY, "REEOT_PROMPT_ADD");
  SendPromptMask(ctx, base, 0, (1u << kPromptA) | (1u << kPromptBack) | (1u << kPromptX) | (1u << kPromptY),
                 g_block + kBlockColour);
}

void RestoreTablePrompts(const PPCContext &ctx, uint8_t *base) {
  if (!g_prompts_sent)
    return;
  g_prompts_sent = false;
  constexpr uint32_t kRetailPopupMask = 0x50020;
  SendPromptMask(ctx, base, 0, kRetailPopupMask, g_block + kBlockColour);
  RestorePrompt(kPromptA);
  RestorePrompt(kPromptX);
  RestorePrompt(kPromptY);
}

void PollHorizontal(const PPCContext &ctx, uint8_t *base, double dt) {
  const double axis = Axis(ctx, base, kInputAxisX);
  const int dir = axis > kAxisHeld ? 1 : axis < -kAxisHeld ? -1 : 0;
  if (Pressed(ctx, base, kInputAxisX)) {
    ChangeValue(ctx, base, axis > 0.0 ? 1 : -1);
    g_held = 0.0;
    g_held_dir = dir;
    return;
  }
  if (!dir || dir != g_held_dir || !g_page->settings[g_cursor].IsSlider()) {
    g_held = 0.0;
    g_held_dir = dir;
    return;
  }
  g_held += dt;
  if (g_held >= kRepeatDelay) {
    g_held -= kRepeatInterval;
    ChangeValue(ctx, base, dir);
  }
}

}

REX_HOOK_RAW(eot_HUDOptionsScreen_BuildBar) {
  g_video_label = StringHandle(ctx, base, "REEOT_VIDEO");
  g_graphics_label = StringHandle(ctx, base, "REEOT_GRAPHICS");
  g_mods_label = ModsOnBar() ? StringHandle(ctx, base, "REEOT_MODS") : 0;
  __imp__eot_HUDOptionsScreen_BuildBar(ctx, base);
}

void eot_OptionsBar_AddGraphics(PPCRegister &r6, PPCRegister &r31) {
  const uint32_t desc = r6.u32;
  if (!desc || eot::mem::load<uint32_t>(desc + kDescCount) != kRetailCount || !g_video_label || !g_graphics_label)
    return;
  eot::mem::store<uint32_t>(DescHandleAddr(desc, kVideoIndex), g_video_label);
  for (uint32_t i = kRetailCount; i > kGraphicsIndex; --i) {
    eot::mem::store<uint32_t>(DescHandleAddr(desc, i), eot::mem::load<uint32_t>(DescHandleAddr(desc, i - 1)));
    eot::mem::store<uint32_t>(DescPropAddr(desc, i), eot::mem::load<uint32_t>(DescPropAddr(desc, i - 1)));
  }
  eot::mem::store<uint32_t>(DescHandleAddr(desc, kGraphicsIndex), g_graphics_label);
  eot::mem::store<uint32_t>(DescPropAddr(desc, kGraphicsIndex), kPropDefault);
  eot::mem::store<uint32_t>(desc + kDescCount, kRetailCount + 1);
  const uint16_t mask = eot::mem::load<uint16_t>(desc + kDescSelectableMask);
  const uint16_t above = static_cast<uint16_t>((mask & ~((1u << kGraphicsIndex) - 1)) << 1);
  const uint16_t below = static_cast<uint16_t>(mask & ((1u << kGraphicsIndex) - 1));
  eot::mem::store<uint16_t>(desc + kDescSelectableMask, static_cast<uint16_t>(above | below | (1u << kGraphicsIndex)));
  eot::mem::store<uint32_t>(desc + kDescSelected, 0);
  if (r31.u32)
    eot::mem::store<uint32_t>(r31.u32 + kScreenCursorOff, 0);
  const int mods_slot = g_mods_label ? AppendEntry(desc, g_mods_label) : -1;
  EOT_INFO("[menu] options bar {:#x}: Video in slot {}, Graphics in slot {}{}", desc, kVideoIndex, kGraphicsIndex,
           mods_slot >= 0 ? std::format(", Mods in slot {}", mods_slot) : std::string());
}

void eot_OptionsBar_NavRightBound6(PPCRegister &r29, PPCCRRegister &cr6, PPCXERRegister &xer) {
  cr6.compare<uint32_t>(r29.u32, LastBarIndex(), xer);
}

REX_HOOK_RAW(eot_HUDOptionsScreen_HandleInputEvent) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  const uint32_t type = event ? eot::mem::load<uint32_t>(event + kEvtType) : 0;
  if (g_popup_open || g_restart_asked) {
    ConsumeEvent(event);
    return;
  }
  if (type == kEvtSelect && self) {
    const uint32_t cursor = eot::mem::load<uint32_t>(self + kScreenCursorOff);
    if (const Page *page = PageAt(cursor)) {
      OpenPage(ctx, base, *page);
      ConsumeEvent(event);
      return;
    }
    if (cursor == kDifficultyIndex) {
      eot::mem::store<uint32_t>(self + kScreenCursorOff, cursor - 1);
      __imp__eot_HUDOptionsScreen_HandleInputEvent(ctx, base);
      eot::mem::store<uint32_t>(self + kScreenCursorOff, cursor);
      return;
    }
  }
  __imp__eot_HUDOptionsScreen_HandleInputEvent(ctx, base);
}

REX_HOOK_RAW(eot_TCRWindow_OpeningEnter) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t slot_ptr = eot::mem::load<uint32_t>(tcr::kCurrentSlotPtr);
  const uint32_t slot = slot_ptr >= tcr::kSlotTable ? (slot_ptr - tcr::kSlotTable) / tcr::kSlotSize : 0xFFFFu;
  const bool ours = g_popup_open && g_page && slot == (g_popup_id & 0xFFFFu);
  if (self) {
    if (!g_retail_tween_saved) {
      for (uint32_t i = 0; i < 8; ++i)
        g_retail_tween[i] = std::bit_cast<float>(eot::mem::load<uint32_t>(self + tcr::kTweenStart + i * 4));
      g_retail_tween_saved = true;
    }
    float rects[8];
    if (ours) {
      const float *box = CurrentLayout().box;
      const float sw = box[2] * tcr::kStartScale, sh = box[3] * tcr::kStartScale;
      const float start[4] = {box[0] + (box[2] - sw) * 0.5f, box[1] + (box[3] - sh) * 0.5f, sw, sh};
      std::copy(start, start + 4, rects);
      std::copy(box, box + 4, rects + 4);
    } else {
      std::copy(g_retail_tween, g_retail_tween + 8, rects);
    }
    for (uint32_t i = 0; i < 8; ++i)
      eot::mem::store<uint32_t>(self + tcr::kTweenStart + i * 4, std::bit_cast<uint32_t>(rects[i]));
  }
  __imp__eot_TCRWindow_OpeningEnter(ctx, base);
}

REX_HOOK_RAW(eot_GameOptionsPopup_OnShow) {
  if (ctx.r3.u32 != g_config || !g_config) {
    __imp__eot_GameOptionsPopup_OnShow(ctx, base);
    return;
  }
  CallScope scope(ctx, base);
  const bool shown = (ctx.r4.u32 & 0xFF) != 0;
  ScaleText(ctx, base, g_title, shown ? kTextScale : 1.0f);
  if (shown && g_page)
    ShowRows(ctx, base);
}

REX_HOOK_RAW(eot_GameOptionsPopup_OnUpdate) {
  if (ctx.r3.u32 != g_config || !g_config || !g_page) {
    __imp__eot_GameOptionsPopup_OnUpdate(ctx, base);
    return;
  }
  CallScope scope(ctx, base);
  const double dt = ctx.f1.f64;
  ctx.r3.u32 = 0;
  if (g_page->table) {
    ShowTablePrompts(ctx, base);
    PollImport(ctx, base);
    if (Pressed(ctx, base, kInputAccept)) {
      ToggleMod(ctx, base);
    } else if (Pressed(ctx, base, kInputBack)) {
      Close(ctx, base, true);
      ctx.r3.u32 = 1;
    } else if (Pressed(ctx, base, kInputRemove)) {
      RemoveMod(ctx, base);
    } else if (Pressed(ctx, base, kInputReset)) {
      AddMod(ctx, base);
    } else if (Pressed(ctx, base, kInputAxisY)) {
      g_remove_armed.clear();
      MoveCursor(ctx, base, Axis(ctx, base, kInputAxisY) > 0.0 ? 1 : -1);
    }
    return;
  }
  if (Pressed(ctx, base, kInputAccept)) {
    Close(ctx, base, true);
    ctx.r3.u32 = 1;
  } else if (Pressed(ctx, base, kInputBack)) {
    Close(ctx, base, false);
    ctx.r3.u32 = 1;
  } else if (Pressed(ctx, base, kInputReset)) {
    for (const Setting &s : g_page->settings)
      ResetValue(s);
    PlayCue(ctx, base, kCueChange);
    ShowRows(ctx, base);
  } else if (Pressed(ctx, base, kInputAxisY)) {
    MoveCursor(ctx, base, Axis(ctx, base, kInputAxisY) > 0.0 ? 1 : -1);
  } else {
    PollHorizontal(ctx, base, dt);
  }
}

REX_HOOK_RAW(eot_WindowComponent_Teardown) {
  const uint32_t config = ctx.r3.u32 ? eot::mem::load<uint32_t>(ctx.r3.u32 + kComponentConfigOff) : 0;
  const uint32_t answer = config ? eot::mem::load<uint32_t>(config + cfg::kYesNoChoice) : 0;
  __imp__eot_WindowComponent_Teardown(ctx, base);
  CallScope scope(ctx, base);
  if (config && config == g_config) {
    g_popup_open = false;
    RestoreTablePrompts(ctx, base);
    EOT_INFO("[menu] {} page closed (id {:#x})", g_page ? g_page->label : "?", g_popup_id);
  } else if (config && config == g_restart_config && g_restart_asked) {
    g_restart_asked = false;
    EOT_INFO("[menu] restart {}", answer == cfg::kYesNoYes ? "accepted" : "declined");
    if (answer == cfg::kYesNoYes)
      eot::RestartProcessFromModule();
    else if (g_page)
      UndoRestartBound();
  }
}

REX_EXTERN(__imp__eot_PauseMenu_GetMainMenuBarInfo);
REX_EXTERN(__imp__eot_PauseMenu_HandleMainMenuSelectOption);
REX_EXTERN(__imp__eot_PauseMenu_HandleMessage);
REX_EXTERN(__imp__eot_PauseMenu_EnterOpening);

namespace {

using namespace eot::ui;

constexpr uint32_t kSelectedIndexOff = 40;
constexpr uint32_t kPauseRetailCount = 7;
constexpr uint32_t kQuitGameIndex = 6;
constexpr uint32_t kExitIndex = 7;
constexpr YesNoLayout kPauseYesNo{76, 152, 160, 184, 80};

bool g_confirm_pending = false;

}

void eot_PauseMenu_NavRightBoundFexit(PPCRegister &r31, PPCCRRegister &cr6, PPCXERRegister &xer) {
  cr6.compare<int32_t>(r31.s32, static_cast<int32_t>(kPauseRetailCount + 1), xer);
}

REX_HOOK_RAW(eot_PauseMenu_GetMainMenuBarInfo) {
  const uint32_t desc = ctx.r4.u32;
  __imp__eot_PauseMenu_GetMainMenuBarInfo(ctx, base);
  const uint32_t count = desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0;
  int slot = -1;
  if (count == kPauseRetailCount) {
    eot::mem::store<uint32_t>(DescHandleAddr(desc, kQuitGameIndex), kHandleExitToMenu);
    slot = AppendEntry(desc, kHandleExitGame);
  }
  EOT_INFO("[menu] pause bar {:#x}: count {} -> {}, Exit Game slot {}", desc, count,
           desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0u, slot);
}

REX_HOOK_RAW(eot_PauseMenu_HandleMainMenuSelectOption) {
  const uint32_t self = ctx.r3.u32;
  if (self && eot::mem::load<uint32_t>(self + kSelectedIndexOff) == kExitIndex) {
    if (!g_confirm_pending) {
      OpenExitConfirm(ctx, base, self, kPauseYesNo);
      g_confirm_pending = true;
    }
    return;
  }
  __imp__eot_PauseMenu_HandleMainMenuSelectOption(ctx, base);
}

REX_HOOK_RAW(eot_PauseMenu_HandleMessage) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kPauseYesNo))
    g_confirm_pending = false;
  __imp__eot_PauseMenu_HandleMessage(ctx, base);
}

REX_HOOK_RAW(eot_PauseMenu_EnterOpening) {
  g_confirm_pending = false;
  __imp__eot_PauseMenu_EnterOpening(ctx, base);
}

REX_EXTERN(__imp__eot_HUDSavegameSelect_HandleInputEvent); // (this r3, event r4)
REX_EXTERN(__imp__eot_HUDSavegameSelect_HandleMessage);    // (this r3, message r4, payload r5) -> handled

namespace {

using namespace eot::ui;

constexpr YesNoLayout kSaveYesNo{68, 144, 152, 172, 72};

}

REX_HOOK_RAW(eot_HUDSavegameSelect_HandleInputEvent) {
  const uint32_t self = ctx.r3.u32, event = ctx.r4.u32;
  if (self && event && eot::mem::load<uint32_t>(event + kEvtType) == kEvtBack && !g_confirm_pending) {
    ConsumeEvent(event);
    const uint32_t handle = OpenConfirm(ctx, base, self, kSaveYesNo, kHandleLeaveTitle, kHandleLeaveBody,
                                        NameCrc("Reeot_LeaveWindow"), NameCrc("Reeot_LeaveYes"), NameCrc("Reeot_LeaveNo"));
    g_confirm_pending = handle != 0xFFFFFFFFu;
    EOT_INFO("[menu] save selection: B, leave-the-game window {:#x}", handle);
    return;
  }
  __imp__eot_HUDSavegameSelect_HandleInputEvent(ctx, base);
}

REX_HOOK_RAW(eot_HUDSavegameSelect_HandleMessage) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kSaveYesNo)) {
    g_confirm_pending = false;
    EOT_INFO("[menu] save selection: leave-the-game window answered");
    ctx.r3.u32 = 1;
    return;
  }
  __imp__eot_HUDSavegameSelect_HandleMessage(ctx, base);
}

REX_EXTERN(__imp__eot_HUDSavegameSelect_Construct); // (this r3, ...)

namespace {

constexpr uint32_t kPromptTable = 0x883C9BC8; // uint32 nameCRC[32], filled by 0x8827C3E8
constexpr uint32_t kPromptCount = 32;

struct Rewrite {
  uint32_t id;
  uint32_t retail;
  const char *replacement;
};

constexpr Rewrite kRewrites[] = {
    {22, 0x3A79CC4E, "REEOT_PROMPT_EXIT_GAME"},
};

void ApplyRewrites() {
  for (const Rewrite &r : kRewrites) {
    if (r.id >= kPromptCount)
      continue;
    const uint32_t slot = kPromptTable + r.id * 4;
    if (eot::mem::load<uint32_t>(slot) != r.retail)
      continue;
    const uint32_t crc = eot::ui::NameCrc(r.replacement);
    eot::mem::store<uint32_t>(slot, crc);
    EOT_INFO("[menu] button prompt {} -> {} (crc {:#010x})", r.id, r.replacement, crc);
  }
}

uint32_t g_prompt_held[kPromptCount] = {};

}

REX_HOOK_RAW(eot_HUDSavegameSelect_Construct) {
  __imp__eot_HUDSavegameSelect_Construct(ctx, base);
  ApplyRewrites();
}

namespace eot::ui {

void OverridePrompt(uint32_t id, const char *name) {
  if (id >= kPromptCount)
    return;
  const uint32_t slot = kPromptTable + id * 4;
  if (!g_prompt_held[id])
    g_prompt_held[id] = eot::mem::load<uint32_t>(slot);
  eot::mem::store<uint32_t>(slot, eot::ui::NameCrc(name));
}

void RestorePrompt(uint32_t id) {
  if (id >= kPromptCount || !g_prompt_held[id])
    return;
  eot::mem::store<uint32_t>(kPromptTable + id * 4, g_prompt_held[id]);
  g_prompt_held[id] = 0;
}

void SendPromptMask(const PPCContext &ctx, uint8_t *base, uint32_t zone, uint32_t mask, uint32_t scratch) {
  constexpr uint32_t kApiLogicPtr = 0x883CA22C;
  constexpr uint32_t kHudsDataPtr = 0x883CA288;
  constexpr uint32_t kHelperOffset = 0xA4;
  constexpr uint32_t kSetMaskMessage = 0xDCDC2E9F;
  const uint32_t api = eot::mem::load<uint32_t>(kApiLogicPtr);
  const uint32_t huds = eot::mem::load<uint32_t>(kHudsDataPtr);
  if (!api || !huds || !scratch)
    return;
  const uint32_t helper = eot::mem::load<uint32_t>(huds + kHelperOffset);
  eot::mem::store<uint32_t>(scratch, zone);
  eot::mem::store<uint32_t>(scratch + 4, mask);
  hud::CallAt(ctx, base, eot::mem::load<uint32_t>(api), helper, 0, kSetMaskMessage, scratch);
}

}
