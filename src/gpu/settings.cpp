#include "gpu/settings.h"

#include <atomic>
#include <charconv>
#include <string_view>
#include <system_error>

#include <rex/cvar.h>

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

REXCVAR_DEFINE_BOOL(eot_vsync, true, "EdgeOfTime/Video", "Present with vsync.");
REXCVAR_DEFINE_BOOL(eot_profiler, false, "EdgeOfTime/Debug",
                    "Start the Tracy profiler at boot so a viewer can attach. Zones are compiled "
                    "into every non-Release build (a Release one only with -DREEOT_PROFILING=ON) "
                    "and cost nothing until then.");
REXCVAR_DEFINE_BOOL(eot_committed_textures, false, "EdgeOfTime/Config",
                    "Give every EDRAM surface, resolve mirror and guest texture its own "
                    "dedicated allocation. The game resizes render targets every frame in "
                    "some scenes, and a dedicated allocation costs about a millisecond each; "
                    "suballocating from shared heaps is far cheaper.");
REXCVAR_DEFINE_INT32(eot_hitch_ms, 0, "EdgeOfTime/Debug",
                     "Log a [hitch] line with that frame's own CPU split for any presented "
                     "frame longer than this many milliseconds (0 = off).")
    .range(0, 1000);
REXCVAR_DEFINE_DOUBLE(eot_render_scale, 1.0, "EdgeOfTime/Video",
                      "Internal render scale: EDRAM surfaces and resolve mirrors are allocated at "
                      "guest size x this (0.25..4); the present scales to the window as before. "
                      "Used when eot_resolution is native.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_resolution, "1080p", "EdgeOfTime/Video",
                      "Internal render resolution in fullscreen: native (the guest's own 1120x632, "
                      "scaled by eot_render_scale), 720p, 1080p, 1440p or 2160p (4K). The preset "
                      "is a target height the scale is derived from; the present always fits the "
                      "result to the window, so a 1440p internal image is blitted up to whatever "
                      "the display is. In a window the preset is not used: the render height "
                      "follows the display the window is on (1440p from 1440 rows up, 1080p on "
                      "1080, 720p below) and the picture stretches to the window. The installer "
                      "picks the preset the display suggests. Fidelity measurement "
                      "(tools/score_dense.py) needs native, which is what the guest and the Xenia "
                      "references render.")
    .allowed({"native", "720p", "1080p", "1440p", "2160p"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(eot_aspect_ratio, "16:9", "EdgeOfTime/Video",
                      "Aspect ratio the game builds its projection for in fullscreen: auto follows "
                      "the window, the rest force a ratio. Wider than 16:9 shows more to the sides "
                      "rather than stretching; the HUD is authored for 16:9 and is not corrected "
                      "yet. In a window the picture always follows the window. The installer picks "
                      "the ratio nearest the display's.")
    .allowed({"auto", "4:3", "16:9", "16:10", "21:9", "32:9"});
REXCVAR_DEFINE_INT32(eot_fps_limit, 60, "EdgeOfTime/Video",
                     "Ceiling on presented frames per second (0 = unlimited). The installer suggests "
                     "the display's own refresh rate. The guest runs one frame per present, so this "
                     "paces the whole game, not just the display.")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(eot_resolve_copy, true, "EdgeOfTime/Config",
                    "Perform same-format, no-reorder resolves as texture copies instead of "
                    "full-screen sampling draws.");
REXCVAR_DEFINE_BOOL(eot_stencil_twin, true, "EdgeOfTime/Config",
                    "Derive a multisampled depth surface's single-sample twin with its stencil "
                    "plane too (a pixel shader writing SV_StencilRef; D3D12), so the passes that "
                    "mark and test stencil without writing depth -- the deferred light volumes -- "
                    "draw single-sample.");
REXCVAR_DEFINE_BOOL(eot_geometry_vram, true, "EdgeOfTime/Config",
                    "Keep the persistent index and vertex caches in VRAM (copied from their "
                    "upload chunks at the end of the frame that admits them).");
REXCVAR_DEFINE_BOOL(eot_render_thread, true, "EdgeOfTime/Config",
                    "Record and submit the host frame on a render thread; the game thread "
                    "captures each draw and waits only at the frame's present.");
REXCVAR_DEFINE_INT32(eot_fast_setters, 7, "EdgeOfTime/Config",
                     "Bits: 1 SetTexture, 2 SetStreamSource, 4 SetIndices done in the hook "
                     "instead of by the recompiled XDK function (the same device stores).");
REXCVAR_DEFINE_BOOL(eot_fast_setters_verify, false, "EdgeOfTime/Debug",
                    "Run both the hook's setter and the XDK's on every call and log any "
                    "difference in what they wrote.");
REXCVAR_DEFINE_BOOL(eot_shadow_atlas_direct, true, "EdgeOfTime/Config",
                    "Draw the shadow tile straight into its atlas region instead of copying it "
                    "there after the pass (predicted from the previous frame).");
REXCVAR_DEFINE_BOOL(eot_resolve_transfer, true, "EdgeOfTime/Config",
                    "A whole-surface resolve hands the surface's single-sample image to the "
                    "destination texture instead of copying it (the surface copies it back "
                    "only if drawn over before its next clear).");
REXCVAR_DEFINE_BOOL(eot_resolve_hw, true, "EdgeOfTime/Config",
                    "Resolve a multisampled scene surface into its single-sample twin with the "
                    "hardware resolve (ResolveSubresource) instead of a shader that reads every "
                    "sample.");
REXCVAR_DEFINE_BOOL(eot_const_range, true, "EdgeOfTime/Config",
                    "Upload only the prefix of each 4 KB float constant file the bound shader "
                    "can address (from its constant table) instead of the whole file.");
REXCVAR_DEFINE_BOOL(eot_vertex_mirrors, true, "EdgeOfTime/Config",
                    "Mirror stable guest vertex ranges into persistent host buffers on their "
                    "second sighting instead of byte-swapping the drawn range per draw.");
REXCVAR_DEFINE_BOOL(eot_bloom, true, "EdgeOfTime/Graphics",
                    "Bloom (the HDR glow around bright light). Off keeps the tone curve.");
REXCVAR_DEFINE_BOOL(eot_depth_of_field, true, "EdgeOfTime/Graphics",
                    "Depth of field, including the bokeh variant cutscenes use.");
REXCVAR_DEFINE_BOOL(eot_motion_blur, true, "EdgeOfTime/Graphics",
                    "Motion blur (object and camera).");
REXCVAR_DEFINE_BOOL(eot_radial_blur, true, "EdgeOfTime/Graphics",
                    "Radial blur (the speed streaks of free falls and dashes).");
REXCVAR_DEFINE_BOOL(eot_heat_effects, true, "EdgeOfTime/Graphics",
                    "Heat vision and heat haze distortion.");
REXCVAR_DEFINE_BOOL(eot_film_grain, true, "EdgeOfTime/Graphics", "Film grain overlay.");
REXCVAR_DEFINE_BOOL(eot_halo, true, "EdgeOfTime/Graphics", "Halo (light bleed) effect.");
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
                     "Reverse-engineering aid: force the content quality level the pak loader "
                     "matches against each resource's level list (-1 = the game's own, 1 on the "
                     "console; the paks carry 1,2,3,4,5,8,9). Not a user option until the levels "
                     "are understood.");
REXCVAR_DEFINE_INT32(eot_shadow_map_size, 0, "EdgeOfTime/Graphics",
                     "Shadow map size per cascade in texels: 0 = follow the render resolution "
                     "(the console's 1024 scaled like the frame, 1750 at 1080p), or 1024 / 2048 / "
                     "4096 fixed. Applies to shadow surfaces created after the change.")
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
                      "Display brightness offset applied at present (-0.5 .. 0.5, 0 = off).");
REXCVAR_DEFINE_DOUBLE(eot_contrast, 1.0, "EdgeOfTime/Video",
                      "Display contrast applied at present (0.5 .. 2.0, 1 = off).");
REXCVAR_DEFINE_DOUBLE(eot_saturation, 1.0, "EdgeOfTime/Video",
                      "Display saturation applied at present (0 = greyscale, 1 = off, 2 = vivid).");
REXCVAR_DEFINE_DOUBLE(eot_gamma, 1.0, "EdgeOfTime/Video",
                      "Display gamma applied at present before the console's own ramp "
                      "(0.5 .. 2.0, 1 = off).");
REXCVAR_DEFINE_INT32(eot_perf_frames, 600, "EdgeOfTime/Debug",
                     "Log a [perf] line every N presented frames: CPU ms per frame in draws, "
                     "resolves, texture uploads, shader links, pipeline builds and the present "
                     "phases, plus upload bytes and GPU ms per frame (0 = off).");

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
REXCVAR_DEFINE_BOOL(eot_diag_scissor, false, "EdgeOfTime/Debug",
                    "Experiment: scissor every multisampled draw to one pixel (per-draw GPU cost only).");
REXCVAR_DEFINE_BOOL(eot_diag_skipdraw, false, "EdgeOfTime/Debug",
                    "Experiment: record no guest draws (black frame; the wall time is the guest's own).");
REXCVAR_DEFINE_BOOL(eot_diag_novtx, false, "EdgeOfTime/Debug",
                    "Experiment: multisampled indexed draws issue three indices only.");
REXCVAR_DEFINE_BOOL(eot_diag_extrapso, false, "EdgeOfTime/Debug",
                    "Experiment: an extra pipeline switch and one-pixel draw before every multisampled draw.");
REXCVAR_DEFINE_BOOL(eot_diag_nopso, false, "EdgeOfTime/Debug",
                    "Experiment: multisampled draws keep the bound pipeline (garbage output).");
REXCVAR_DEFINE_BOOL(eot_diag_noconst, false, "EdgeOfTime/Debug",
                    "Experiment: multisampled draws keep the bound constants (garbage output).");
REXCVAR_DEFINE_BOOL(eot_diag_rectclear, false, "EdgeOfTime/Debug",
                    "Experiment: clear the shadow tile through a covering rect.");
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
bool Settings::VertexMirrors() { return REXCVAR_GET(eot_vertex_mirrors); }
bool Settings::ConstRange() { return REXCVAR_GET(eot_const_range); }
bool Settings::ResolveCopy() { return REXCVAR_GET(eot_resolve_copy); }
bool Settings::ResolveHardware() { return REXCVAR_GET(eot_resolve_hw); }
bool Settings::ResolveTransfer() { return REXCVAR_GET(eot_resolve_transfer); }
bool Settings::ShadowAtlasDirect() { return REXCVAR_GET(eot_shadow_atlas_direct); }
bool Settings::GeometryVram() { return REXCVAR_GET(eot_geometry_vram); }
bool Settings::RenderThread() { return REXCVAR_GET(eot_render_thread); }
i32 Settings::FastSetters() { return REXCVAR_GET(eot_fast_setters); }
bool Settings::FastSettersVerify() { return REXCVAR_GET(eot_fast_setters_verify); }
bool Settings::StencilTwin() { return REXCVAR_GET(eot_stencil_twin); }
f64 Settings::RenderScale() { return REXCVAR_GET(eot_render_scale); }
std::string Settings::Resolution() { return std::string(REXCVAR_GET(eot_resolution)); }
i32 Settings::FpsLimit() { return REXCVAR_GET(eot_fps_limit); }
std::string Settings::AspectRatio() { return std::string(REXCVAR_GET(eot_aspect_ratio)); }
i32 Settings::HitchMs() { return REXCVAR_GET(eot_hitch_ms); }
bool Settings::CommittedTextures() { return REXCVAR_GET(eot_committed_textures); }
bool Settings::Profiler() { return REXCVAR_GET(eot_profiler); }
i32 Settings::PerfFrames() { return REXCVAR_GET(eot_perf_frames); }
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
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
bool Settings::DiagScissor() { return REXCVAR_GET(eot_diag_scissor); }
bool Settings::DiagRectClear() { return REXCVAR_GET(eot_diag_rectclear); }
bool Settings::DiagNoPso() { return REXCVAR_GET(eot_diag_nopso); }
bool Settings::DiagExtraPso() { return REXCVAR_GET(eot_diag_extrapso); }
bool Settings::DiagNoVtx() { return REXCVAR_GET(eot_diag_novtx); }
bool Settings::DiagSkipDraw() { return REXCVAR_GET(eot_diag_skipdraw); }
bool Settings::DiagNoConst() { return REXCVAR_GET(eot_diag_noconst); }
void Settings::ArmDiagFrame(i32 frame) { g_diag_frame_armed.store(frame, std::memory_order_relaxed); }
i32 Settings::RenderDocFrame() { return REXCVAR_GET(eot_rdc_frame); }
std::string Settings::RenderDocDll() { return std::string(REXCVAR_GET(eot_rdc_dll)); }
std::string Settings::RenderDocPath() { return std::string(REXCVAR_GET(eot_rdc_path)); }
bool Settings::PresentGamma() { return REXCVAR_GET(eot_present_gamma); }
bool Settings::Bloom() { return REXCVAR_GET(eot_bloom); }
bool Settings::DepthOfField() { return REXCVAR_GET(eot_depth_of_field); }
bool Settings::MotionBlur() { return REXCVAR_GET(eot_motion_blur); }
bool Settings::RadialBlur() { return REXCVAR_GET(eot_radial_blur); }
bool Settings::HeatEffects() { return REXCVAR_GET(eot_heat_effects); }
bool Settings::FilmGrain() { return REXCVAR_GET(eot_film_grain); }
bool Settings::Halo() { return REXCVAR_GET(eot_halo); }
bool Settings::ColorGrading() { return REXCVAR_GET(eot_color_grading); }
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

std::atomic<bool> g_fullscreen{true};
struct FullscreenWatch {
  FullscreenWatch() {
    rex::cvar::RegisterChangeCallback("fullscreen", [](std::string_view, std::string_view value) {
      g_fullscreen.store(value == "true" || value == "1", std::memory_order_relaxed);
    });
  }
} g_fullscreen_watch;

f32 ComputeRenderScale() {
  const std::string preset = Settings::Resolution();
  u32 target_height = 0;
  if (!Settings::Fullscreen())
    target_height = g_auto_render_height;
  else if (preset == "720p")
    target_height = 720;
  else if (preset == "1080p")
    target_height = 1080;
  else if (preset == "1440p")
    target_height = 1440;
  else if (preset == "2160p")
    target_height = 2160;
  const f64 scale = target_height != 0
                        ? static_cast<f64>(target_height) / static_cast<f64>(kGuestRenderHeight)
                        : Settings::RenderScale();
  return static_cast<f32>(std::clamp(scale, 0.25, 4.0));
}

}

void SetAutoRenderHeight(u32 height) { g_auto_render_height = height; }

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
