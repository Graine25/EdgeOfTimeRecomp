#include "gpu/settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/cvar.h>

#include "core/logging.h"

REXCVAR_DEFINE_INT32(eot_trace_frames, 0, "EdgeOfTime/Debug",
                     "Log every hooked D3D call with its arguments for the "
                     "first N frames (Phase 0 call-stream trace).")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(eot_trace_start_frame, 0, "EdgeOfTime/Debug",
                     "Guest frame at which the eot_trace_frames call trace begins.")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_summary_frames, 0, "EdgeOfTime/Debug",
                     "Log a one-line per-frame D3D call summary for the first "
                     "N frames (0 = off).")
    .range(0, 1000000);

REXCVAR_DEFINE_INT32(eot_diag, 1, "EdgeOfTime/Debug",
                     "Renderer diagnostics verbosity: 0 quiet, 1 dropped draws "
                     "and unmapped resources (rate limited), 2 verbose.")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(eot_vsync, true, "EdgeOfTime/Video",
                    "Present in step with the display: no tearing, and the frame rate held to the "
                    "display's refresh rate (a lower eot_fps_limit still applies).");
REXCVAR_DEFINE_BOOL(eot_profiler, false, "EdgeOfTime/Debug",
                    "Start the Tracy profiler at boot so a viewer can attach. Zones are compiled "
                    "into every non-Release build (a Release one only with -DREEOT_PROFILING=ON) "
                    "and cost nothing until then.");
REXCVAR_DEFINE_INT32(eot_hitch_ms, 40, "EdgeOfTime/Debug",
                     "Log a [hitch] line with that frame's own CPU split for any presented "
                     "frame longer than this many milliseconds (0 = off).")
    .range(0, 1000);
REXCVAR_DEFINE_DOUBLE(eot_render_scale, 1.0, "EdgeOfTime/Video",
                      "Internal render scale: whatever eot_resolution settles on is multiplied by "
                      "this before the surfaces are allocated, so it is a supersample above 1 and "
                      "a cheaper frame below it, at every preset and in a window as well as "
                      "fullscreen. The present scales the result to the window as before.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_resolution, "1080p", "EdgeOfTime/Video",
                      "Internal render resolution in fullscreen: native (the guest's own 1120x632), "
                      "720p, 1080p, 1440p, 2160p (4K), or display (the display's own height, the "
                      "way Unleashed Recompiled and reblue size their frame). Whichever it is, "
                      "eot_render_scale multiplies it. The preset "
                      "is a target height the scale is derived from; the present always fits the "
                      "result to the window, so a 1440p internal image is blitted up to whatever "
                      "the display is. In a window the preset is not used: the render height "
                      "follows the display the window is on (1440p from 1440 rows up, 1080p on "
                      "1080, 720p below) and the picture stretches to the window. The installer "
                      "picks the preset the display suggests. Fidelity measurement "
                      "(tools/score_dense.py) needs native, which is what the guest and the Xenia "
                      "references render.")
    .allowed({"native", "720p", "1080p", "1440p", "2160p", "display"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_fullscreen_mode, "borderless", "EdgeOfTime/Video",
                      "borderless: the desktop's resolution, the internal render resampled to it "
                      "at present (a five-tap bicubic over every display pixel, 0.85 ms at 5K) "
                      "and composited by the desktop. exclusive: fullscreen switches the display "
                      "to the mode nearest the internal render, so the present is a plain copy "
                      "and the desktop compositor is out of the path; the picture is the "
                      "display's own scaler's.")
    .allowed({"borderless", "exclusive"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(eot_pip_scale, 50, "EdgeOfTime/Video",
                     "Resolution of a texture camera's record (the picture-in-picture window the "
                     "abilities open: a second scene with its own lights and post chain) as a "
                     "percentage of the render scale: 100, 66 or 50. The window ramps back to the "
                     "full scale over the last fifth of the frame's width, so one that grows into "
                     "the screen sharpens into place. Takes effect at boot.")
    .range(25, 100)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_aspect_ratio, "16:9", "EdgeOfTime/Video",
                      "Aspect ratio the game builds its projection for in fullscreen: auto follows "
                      "the window, the rest force a ratio. Wider than 16:9 shows more to the sides "
                      "rather than stretching, the HUD held in a centred 16:9 region. 16:10 "
                      "(MacWide) is the game's own 4:3 picture and screens with the sides of a "
                      "16:10 view, nothing moved vertically, the picture-in-picture screen alone "
                      "on its wide layout. In a window the picture always follows the window. The "
                      "installer picks the ratio nearest the display's.")
    .allowed({"auto", "4:3", "16:9", "16:10", "21:9", "32:9"});
REXCVAR_DEFINE_INT32(eot_fps_limit, 60, "EdgeOfTime/Video",
                     "Ceiling on presented frames per second (0 = unlimited). The installer suggests "
                     "the display's own refresh rate. The guest runs one frame per present, so this "
                     "paces the whole game, not just the display.")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(eot_fast_setters_verify, false, "EdgeOfTime/Debug",
                    "Run both the hook's setter and the XDK's on every call and log any "
                    "difference in what they wrote.");
REXCVAR_DEFINE_STRING(eot_host_aa, "off", "EdgeOfTime/Graphics",
                      "The port's own anti-aliasing, on top of whatever the game draws "
                      "(eot_msaa is the game's own):\n"
                      "  off  - the frame is presented as it was rendered.\n"
                      "  taa  - temporal: every 3D draw is offset by a sub-pixel amount "
                      "that changes each frame, and the scene is blended with the previous "
                      "frame's, reprojected by its motion vectors where the G-buffer wrote "
                      "them (eot_motion_vectors) and through the camera elsewhere, before "
                      "the game's own post chain reads it. Settles the shimmer on distant "
                      "geometry that multisampling leaves.\n"
                      "The ultra preset asks for taa.")
    .allowed({"off", "taa"});
REXCVAR_DEFINE_DOUBLE(eot_taa_feedback, 0.8, "EdgeOfTime/Graphics",
                      "How much of the previous frame the temporal anti-aliasing keeps when "
                      "nothing moves (0.3 crisp and quick, 0.9 smoother and softer); motion "
                      "without a vector lowers it towards zero on its own.")
    .range(0.3, 0.95);
REXCVAR_DEFINE_BOOL(eot_motion_vectors, false, "EdgeOfTime/Graphics",
                    "Per-object motion vectors for the temporal anti-aliasing: the G-buffer "
                    "draws run their vertex shader a second time on last frame's constants "
                    "and write where each pixel's surface was, so moving characters and "
                    "props converge like the scenery instead of being clamped every frame. "
                    "The ultra preset turns it on.");
REXCVAR_DEFINE_BOOL(eot_bloom, true, "EdgeOfTime/Graphics",
                    "Bloom (the HDR glow around bright light). Off keeps the tone curve.");
REXCVAR_DEFINE_BOOL(eot_depth_of_field, true, "EdgeOfTime/Graphics",
                    "Depth of field, including the bokeh variant cutscenes use.");
REXCVAR_DEFINE_BOOL(eot_scene_copy_refresh, true, "EdgeOfTime/Graphics",
                    "Re-take the scene-colour copy a distortion material reads (glass panels, "
                    "screens, heat haze) right before the draw, when the scene was drawn into "
                    "since the game took it. The game takes that copy once, before the "
                    "transparent effects, so on the console a pane of glass repaints away any "
                    "effect drawn before it. Off matches the console.");
REXCVAR_DEFINE_BOOL(eot_motion_blur, true, "EdgeOfTime/Graphics",
                    "Motion blur (object and camera).");
REXCVAR_DEFINE_BOOL(eot_color_grading, true, "EdgeOfTime/Graphics",
                    "The scene colour grade (3D LUT colorization). Off shows the ungraded image.");
REXCVAR_DEFINE_DOUBLE(eot_fov_scale, 1.0, "EdgeOfTime/Graphics",
                      "Field-of-view multiplier applied to every camera the game sets "
                      "(1.0 = the game's own; 1.2 shows more to the sides). Culling follows.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(eot_shadow_cascades, 0, "EdgeOfTime/Graphics",
                     "Shadow map cascades per camera: 0 = as the level asks, 1..4 (the engine's "
                     "maximum) split the shadow distance into that many maps.");
REXCVAR_DEFINE_DOUBLE(eot_shadow_distance_scale, 1.0, "EdgeOfTime/Graphics",
                      "Multiplier on the level's shadow distance (1 = as the level asks; "
                      "2 casts shadows twice as far at the same map resolution).");
REXCVAR_DEFINE_INT32(eot_debug_quality_level, -1, "EdgeOfTime/Debug",
                     "Reverse-engineering aid, named before the dword was understood: force the "
                     "game's own language id the pak loader matches against each package's list "
                     "(-1 = what the boot resolved; 1 English, 2 French, 3 Italian, 4 German, 5 "
                     "Spanish). The setting is eot_language.");
REXCVAR_DEFINE_INT32(eot_shadow_map_size, 0, "EdgeOfTime/Graphics",
                     "Shadow map size per cascade in texels: 0 = follow the render resolution "
                     "(the console's 1024 at the nearest whole multiple: 2048 at 1080p and "
                     "1440p, 3072 at 2160p), or 1024 / 2048 / 4096 / 8192 fixed (8192 is the "
                     "ultra preset's: a 16384-texel atlas, 1 GB). Always a multiple of "
                     "1024, since the game holds its cascades still on a 1024-texel grid. "
                     "Applies to shadow surfaces created after the change.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(eot_anisotropy, 16, "EdgeOfTime/Graphics",
                     "Anisotropic filtering: 0 = as the game asks per texture (the console "
                     "asks for none), 1 = off, 2/4/8/16 = at least that level on every "
                     "linearly filtered texture, trilinear with it. 16 is near-free on "
                     "modern GPUs and keeps floors and walls sharp at grazing angles.")
    .range(0, 16);
REXCVAR_DEFINE_INT32(eot_msaa, 0, "EdgeOfTime/Graphics",
                     "Multisampling on the full-frame scene surfaces: 0 = off, 2, 4 or 8 "
                     "samples, clamped to what the device supports. Every pass that draws "
                     "into the frame is multisampled and the resolves average the samples, "
                     "so geometry edges are anti-aliased before the post chain sees them. "
                     "Costs VRAM and bandwidth in proportion. Requires restart.")
    .range(0, 8)
    .validator([](std::string_view v) {
      int n = 0;
      const auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 0 || n == 2 || n == 4 || n == 8);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(eot_brightness, 0.0, "EdgeOfTime/Video",
                      "Display brightness offset applied at present (-0.25 .. 0.25 in the menu, "
                      "0 = off).");
REXCVAR_DEFINE_DOUBLE(eot_contrast, 1.0, "EdgeOfTime/Video",
                      "Display contrast applied at present (0.5 .. 1.5 in the menu, 1 = off).");
REXCVAR_DEFINE_DOUBLE(eot_saturation, 1.0, "EdgeOfTime/Video",
                      "Display saturation applied at present (0 = greyscale, 1 = off, 2 = vivid).");
REXCVAR_DEFINE_DOUBLE(eot_gamma, 1.0, "EdgeOfTime/Video",
                      "Display gamma applied at present before the console's own ramp "
                      "(0.5 .. 2.0, 1 = off).");
REXCVAR_DEFINE_INT32(eot_perf_frames, 600, "EdgeOfTime/Debug",
                     "Log a [perf] line every N presented frames: CPU ms per frame in draws, "
                     "resolves, texture uploads, shader links, pipeline builds and the present "
                     "phases, plus upload bytes and GPU ms per frame (0 = off).");

REXCVAR_DEFINE_INT32(eot_vram_report_seconds, 30, "EdgeOfTime/Debug",
                     "Seconds between [vram] memory reports: the process's video memory against the "
                     "budget the OS gives it, the port's census of what holds it (surface images, "
                     "resolve mirrors, textures, the recycle list, the temporal pass, the geometry "
                     "caches), the allocator's view by tag and shape, and the process's RAM "
                     "(0 = off). Going over the budget is logged whenever it happens, report or not.")
    .range(0, 3600);
REXCVAR_DEFINE_BOOL(eot_vram_csv, false, "EdgeOfTime/Debug",
                    "With each [vram] report, write logs/vram_<frame>_allocations.csv (every live "
                    "allocation: heap, kind, name with its shape, bytes) and "
                    "logs/vram_<frame>_surfaces.csv (every EDRAM surface image and the draws that "
                    "bound it), for a spreadsheet.");

REXCVAR_DEFINE_INT32(eot_diag_frame, 0, "EdgeOfTime/Debug",
                     "Guest frame whose draws are logged in detail and whose resolve "
                     "sources are dumped as logs/f<N>_r<K>.ppm (0 = off).")
    .range(0, 100000000);
REXCVAR_DEFINE_INT32(eot_diag_scene_from, 0, "EdgeOfTime/Debug",
                     "eot_diag_scene arms only from this guest frame on (0 = any).")
    .range(0, 100000000);
REXCVAR_DEFINE_BOOL(eot_diag_scene, false, "EdgeOfTime/Debug",
                    "Log the eot_diag_frame detail for the frame after the first frame of the "
                    "run with 400 or more draws, whatever its index.");
REXCVAR_DEFINE_INT32(eot_diag_hitch, 0, "EdgeOfTime/Debug",
                     "Log the eot_diag_frame detail for the frame after the first GPU frame at "
                     "or past this guest frame that the hitch reporter flags (40% over its "
                     "running average): the passes and transfers of a slow frame, once per "
                     "run (0 = off).");
REXCVAR_DEFINE_BOOL(eot_record, false, "EdgeOfTime/Debug",
                    "Record the view: while on, the one-frame GPU diagnostic runs every 120th "
                    "frame (draw lines without their textures) and [record] marks the start "
                    "and end, so tools/perf/record_report.py can sum what one view costs by "
                    "target, sample count and shader. Toggle it from the overlay.");
REXCVAR_DEFINE_BOOL(eot_diag_hitch_any, false, "EdgeOfTime/Debug",
                    "With eot_diag_hitch: arm on any flagged frame, not only those whose "
                    "largest categories are the twin transfers.");
REXCVAR_DEFINE_BOOL(eot_d3d12_debug, false, "EdgeOfTime/Debug",
                    "Enable the D3D12 debug layer before the device is created; its messages "
                    "are logged as [d3d12-debug]. Halves the frame rate.");
REXCVAR_DEFINE_BOOL(eot_diag_dump, false, "EdgeOfTime/Debug",
                    "Dump every resolve source of the diagnostic frame as a PPM (submits and stalls per dump).");

REXCVAR_DEFINE_INT32(eot_rdc_frame, 0, "EdgeOfTime/Debug",
                     "Guest frame to capture with the in-process RenderDoc API "
                     "(0 = off; needs eot_rdc_dll).")
    .range(0, 100000000);

REXCVAR_DEFINE_STRING(eot_rdc_dll,
                      "C:/Users/rieng/Documents/GitHub/renderdoc/x64/Development/renderdoc.dll",
                      "EdgeOfTime/Debug", "renderdoc.dll to load before the host device is created.");

REXCVAR_DEFINE_STRING(eot_rdc_path, "D:/reeot_caps/tmp/reeot", "EdgeOfTime/Debug",
                      "RenderDoc capture file path template.");

REXCVAR_DEFINE_INT32(eot_dump_every, 0, "EdgeOfTime/Debug",
                     "Write the presented back buffer as logs/frame_<N>.ppm every N "
                     "presented frames (0 = off).")
    .range(0, 1000000);
REXCVAR_DEFINE_INT32(eot_dump_burst, 1, "EdgeOfTime/Debug",
                     "How many consecutive frames each eot_dump_every dump writes, so "
                     "frame-to-frame flicker can be measured.")
    .range(1, 64);

REXCVAR_DEFINE_BOOL(eot_present_gamma, true, "EdgeOfTime/Video",
                    "Map the presented front buffer through the guest's display gamma "
                    "ramp (SetGammaRamp / SetPWLGamma), as the console's scan-out LUT does.");

REXCVAR_DEFINE_BOOL(eot_present_log, false, "EdgeOfTime/Debug",
                    "Write '[present] frame N t=<epoch_ms>' per present so "
                    "external screenshots align to renderer frames.");

namespace eot::gpu {

i32 Settings::TraceFrames() { return REXCVAR_GET(eot_trace_frames); }
i32 Settings::SummaryFrames() { return REXCVAR_GET(eot_summary_frames); }
i32 Settings::TraceStartFrame() { return REXCVAR_GET(eot_trace_start_frame); }
i32 Settings::DiagVerbosity() { return REXCVAR_GET(eot_diag); }
bool Settings::Vsync() { return REXCVAR_GET(eot_vsync); }
bool Settings::FastSettersVerify() { return REXCVAR_GET(eot_fast_setters_verify); }
f64 Settings::RenderScale() { return REXCVAR_GET(eot_render_scale); }
std::string Settings::Resolution() { return std::string(REXCVAR_GET(eot_resolution)); }
std::string Settings::FullscreenMode() { return std::string(REXCVAR_GET(eot_fullscreen_mode)); }
i32 Settings::PipScalePercent() { return REXCVAR_GET(eot_pip_scale); }
i32 Settings::FpsLimit() { return REXCVAR_GET(eot_fps_limit); }
std::string Settings::AspectRatio() { return std::string(REXCVAR_GET(eot_aspect_ratio)); }
i32 Settings::HitchMs() { return REXCVAR_GET(eot_hitch_ms); }
bool Settings::Profiler() { return REXCVAR_GET(eot_profiler); }
i32 Settings::PerfFrames() { return REXCVAR_GET(eot_perf_frames); }
i32 Settings::VramReportSeconds() { return REXCVAR_GET(eot_vram_report_seconds); }
bool Settings::VramCsv() { return REXCVAR_GET(eot_vram_csv); }
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
i32 Settings::DumpBurst() { return REXCVAR_GET(eot_dump_burst); }
namespace {
std::atomic<i32> g_diag_frame_armed{0};
}
i32 Settings::DiagFrame() {
  const i32 armed = g_diag_frame_armed.load(std::memory_order_relaxed);
  return armed > 0 ? armed : REXCVAR_GET(eot_diag_frame);
}
bool Settings::DiagScene() { return REXCVAR_GET(eot_diag_scene); }
i32 Settings::DiagSceneFrom() { return REXCVAR_GET(eot_diag_scene_from); }
bool Settings::DiagDump() { return REXCVAR_GET(eot_diag_dump); }
i32 Settings::DiagHitch() { return REXCVAR_GET(eot_diag_hitch); }
bool Settings::DiagHitchAny() { return REXCVAR_GET(eot_diag_hitch_any); }
bool Settings::Record() { return REXCVAR_GET(eot_record); }
bool Settings::D3D12Debug() { return REXCVAR_GET(eot_d3d12_debug); }
void Settings::ArmDiagFrame(i32 frame) { g_diag_frame_armed.store(frame, std::memory_order_relaxed); }
i32 Settings::RenderDocFrame() { return REXCVAR_GET(eot_rdc_frame); }
std::string Settings::RenderDocDll() { return std::string(REXCVAR_GET(eot_rdc_dll)); }
std::string Settings::RenderDocPath() { return std::string(REXCVAR_GET(eot_rdc_path)); }
bool Settings::PresentGamma() { return REXCVAR_GET(eot_present_gamma); }
bool Settings::Taa() { return REXCVAR_GET(eot_host_aa) == "taa"; }
bool Settings::MotionVectors() { return REXCVAR_GET(eot_motion_vectors); }
double Settings::TaaFeedback() { return REXCVAR_GET(eot_taa_feedback); }
bool Settings::Bloom() { return REXCVAR_GET(eot_bloom); }
bool Settings::DepthOfField() { return REXCVAR_GET(eot_depth_of_field); }
bool Settings::MotionBlur() { return REXCVAR_GET(eot_motion_blur); }
bool Settings::ColorGrading() { return REXCVAR_GET(eot_color_grading); }
bool Settings::SceneCopyRefresh() { return REXCVAR_GET(eot_scene_copy_refresh); }
double Settings::FovScale() { return REXCVAR_GET(eot_fov_scale); }
i32 Settings::Anisotropy() { return REXCVAR_GET(eot_anisotropy); }
i32 Settings::Msaa() { return REXCVAR_GET(eot_msaa); }
i32 Settings::QualityLevel() { return REXCVAR_GET(eot_debug_quality_level); }
i32 Settings::ShadowCascades() { return REXCVAR_GET(eot_shadow_cascades); }
i32 Settings::ShadowMapSize() { return REXCVAR_GET(eot_shadow_map_size); }
double Settings::ShadowDistanceScale() { return REXCVAR_GET(eot_shadow_distance_scale); }
double Settings::Brightness() { return REXCVAR_GET(eot_brightness); }
double Settings::Contrast() { return REXCVAR_GET(eot_contrast); }
double Settings::Saturation() { return REXCVAR_GET(eot_saturation); }
double Settings::Gamma() { return REXCVAR_GET(eot_gamma); }
bool Settings::PresentFrameLog() { return REXCVAR_GET(eot_present_log); }

namespace {

u32 g_auto_render_height = 1080;
u32 g_display_height = 0;

std::atomic<bool> g_fullscreen{true};
struct FullscreenWatch {
  FullscreenWatch() {
    rex::cvar::RegisterChangeCallback("fullscreen", [](std::string_view, std::string_view value) {
      g_fullscreen.store(value == "true" || value == "1", std::memory_order_relaxed);
    });
  }
} g_fullscreen_watch;

u32 PresetRows(std::string preset) {
  for (char &c : preset)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (preset == "4k" || preset == "uhd")
    return 2160;
  if (preset == "2k" || preset == "qhd")
    return 1440;
  if (preset == "fhd")
    return 1080;
  if (preset == "hd")
    return 720;
  if (!preset.empty() && preset.back() == 'p')
    preset.pop_back();
  if (preset.empty() || preset.find_first_not_of("0123456789") != std::string::npos)
    return 0;
  const u32 rows = static_cast<u32>(std::strtoul(preset.c_str(), nullptr, 10));
  return rows >= 360 && rows <= 4320 ? rows : 0;
}

f32 ComputeRenderScale() {
  const std::string preset = Settings::Resolution();
  u32 target_height = 0;
  if (!Settings::Fullscreen())
    target_height = g_auto_render_height;
  else if (preset == "display")
    target_height = g_display_height ? g_display_height : g_auto_render_height;
  else if (preset != "native") {
    target_height = PresetRows(preset);
    if (target_height == 0) {
      target_height = g_auto_render_height;
      EOT_WARN("[gpu] eot_resolution \"{}\" is not a preset (native, 720p, 1080p, 1440p, 2160p, display); "
               "rendering at the {} rows the display suggests",
               preset, target_height);
    }
  }
  const f64 base = target_height != 0
                       ? static_cast<f64>(target_height) / static_cast<f64>(kGuestRenderHeight)
                       : 1.0;
  const f64 scale = base * Settings::RenderScale();
  return static_cast<f32>(std::clamp(scale, 0.25, 5.0));
}

}

void SetAutoRenderHeight(u32 height) { g_auto_render_height = height; }
void SetDisplayHeight(u32 height) { g_display_height = height; }

bool Settings::Fullscreen() {
  static const bool initial = [] {
    const bool on = rex::cvar::Query<bool>("fullscreen");
    g_fullscreen.store(on, std::memory_order_relaxed);
    return on;
  }();
  (void)initial;
  return g_fullscreen.load(std::memory_order_relaxed);
}

std::string Settings::EffectiveAspectRatio() { return Fullscreen() ? AspectRatio() : std::string("auto"); }

f32 RenderScaleFactor() {
  static const f32 s = ComputeRenderScale();
  return s;
}

u32 InternalRenderWidth() { return ScaleDim(kGuestRenderWidth); }
u32 InternalRenderHeight() { return ScaleDim(kGuestRenderHeight); }

}
