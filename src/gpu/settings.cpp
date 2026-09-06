#include "gpu/settings.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(eot_trace_frames, 0, "eot",
                     "Log every hooked D3D call with its arguments for the "
                     "first N frames (Phase 0 call-stream trace).")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(eot_trace_start_frame, 0, "eot",
                     "Guest frame at which the eot_trace_frames call trace begins.")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_summary_frames, 600, "eot",
                     "Log a one-line per-frame D3D call summary for the first "
                     "N frames.")
    .range(0, 1000000);

REXCVAR_DEFINE_INT32(eot_diag, 1, "eot",
                     "Renderer diagnostics verbosity: 0 quiet, 1 dropped draws "
                     "and unmapped resources (rate limited), 2 verbose.")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(eot_vsync, true, "EdgeOfTime/Video", "Present with vsync.");
REXCVAR_DEFINE_STRING(eot_pso_dir, "pso", "eot",
                      "Directory of pipeline capture CSVs: every *.csv in it is precached on "
                      "worker threads at boot, and render-thread misses of this run are appended "
                      "to a new pso_misses_<tag>_<time>.csv there (empty = off).");
REXCVAR_DEFINE_STRING(eot_pso_tag, "", "eot",
                      "Tester or area tag written into pipeline capture file names and rows.");
REXCVAR_DEFINE_BOOL(eot_pso_capture, true, "eot",
                    "Capture pipelines the render thread had to build to eot_pso_dir.");
REXCVAR_DEFINE_BOOL(eot_pso_compiled_in, true, "eot",
                    "Precache the compiled-in pipeline table at boot (false to measure the "
                    "load-time predictor alone).");
REXCVAR_DEFINE_BOOL(eot_pso_predict, true, "eot",
                    "Predict and prebuild a model's pipelines from its materials when the "
                    "guest streams it (ModelResource_LoadGeometry).");
REXCVAR_DEFINE_BOOL(eot_pso_predict_all, false, "eot",
                    "Predictor experiment: cross every material of a model with every descriptor "
                    "stride instead of only the (material, stride) pairs descriptors reference.");
REXCVAR_DEFINE_INT32(eot_pso_predict_fallback, 0, "eot",
                     "Predictor template matching: 0 exact (technique, pass, material class) only, "
                     "1 also the technique with the class for any pass, 2 also the technique alone.")
    .range(0, 2);
REXCVAR_DEFINE_INT32(eot_pso_gate_ms, 250, "eot",
                     "Longest a model load waits for its predicted pipelines before the model "
                     "is published (0 = never wait).")
    .range(0, 5000);
REXCVAR_DEFINE_BOOL(eot_profiler, false, "eot",
                    "Start the Tracy profiler at boot so a viewer can attach. Zones are "
                    "compiled into every non-Release build and cost nothing until then.");
REXCVAR_DEFINE_BOOL(eot_committed_textures, false, "eot",
                    "Give every EDRAM surface, resolve mirror and guest texture its own "
                    "dedicated allocation. The game resizes render targets every frame in "
                    "some scenes, and a dedicated allocation costs about a millisecond each; "
                    "suballocating from shared heaps is far cheaper.");
REXCVAR_DEFINE_INT32(eot_hitch_ms, 0, "eot",
                     "Log a [hitch] line with that frame's own CPU split for any presented "
                     "frame longer than this many milliseconds (0 = off).")
    .range(0, 1000);
REXCVAR_DEFINE_INT32(eot_pso_threads, 0, "eot",
                     "Pipeline worker threads (0 = hardware threads - 2, clamped to 1..8).")
    .range(0, 16);
REXCVAR_DEFINE_DOUBLE(eot_render_scale, 1.0, "EdgeOfTime/Video",
                      "Internal render scale: EDRAM surfaces and resolve mirrors are allocated at "
                      "guest size x this (0.25..4); the present scales to the window as before. "
                      "Used when eot_resolution is native.");
REXCVAR_DEFINE_STRING(eot_resolution, "1080p", "EdgeOfTime/Video",
                      "Internal render resolution: native (the guest's own 1120x632, scaled by "
                      "eot_render_scale), 720p, 1080p or 1440p. The preset is a target height the "
                      "scale is derived from; the present always fits the result to the window, so "
                      "a 1440p internal image is blitted up to whatever the display is. Fidelity "
                      "measurement (tools/score_dense.py) needs native, which is what the guest and "
                      "the Xenia references render.")
    .allowed({"native", "720p", "1080p", "1440p"});
REXCVAR_DEFINE_STRING(eot_aspect_ratio, "16:9", "EdgeOfTime/Video",
                      "Aspect ratio the game builds its projection for: auto follows the window, "
                      "the rest force a ratio. Wider than 16:9 shows more to the sides rather than "
                      "stretching; the HUD is authored for 16:9 and is not corrected yet.")
    .allowed({"auto", "4:3", "16:9", "16:10", "21:9", "32:9"});
REXCVAR_DEFINE_INT32(eot_fps_limit, 60, "EdgeOfTime/Video",
                     "Ceiling on presented frames per second (0 = unlimited; 30/60/90/120 are the "
                     "menu presets). The guest runs one frame per present, so this paces the whole "
                     "game, not just the display.")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(eot_resolve_copy, false, "eot",
                    "Perform same-format, 1:1, no-reorder resolves as texture copies instead of "
                    "full-screen sampling draws. Measured neutral on the GPU and slower to record "
                    "on D3D12/AMD (2026-09-03); off until re-tested on Vulkan.");
REXCVAR_DEFINE_BOOL(eot_const_range, true, "eot",
                    "Upload only the prefix of each 4 KB float constant file the bound shader "
                    "can address (from its constant table) instead of the whole file.");
REXCVAR_DEFINE_BOOL(eot_vertex_mirrors, true, "eot",
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
                      "(1.0 = the game's own; 1.2 shows more to the sides). Culling follows.");
REXCVAR_DEFINE_INT32(eot_shadow_cascades, 0, "EdgeOfTime/Graphics",
                     "Shadow map cascades per camera: 0 = as the level asks, 1..4 (the engine's "
                     "maximum) split the shadow distance into that many maps.");
REXCVAR_DEFINE_DOUBLE(eot_shadow_distance_scale, 1.0, "EdgeOfTime/Graphics",
                      "Multiplier on the level's shadow distance (1 = as the level asks; "
                      "2 casts shadows twice as far at the same map resolution).");
REXCVAR_DEFINE_INT32(eot_debug_quality_level, -1, "eot",
                     "Reverse-engineering aid: force the content quality level the pak loader "
                     "matches against each resource's level list (-1 = the game's own, 1 on the "
                     "console; the paks carry 1,2,3,4,5,8,9). Not a user option until the levels "
                     "are understood.");
REXCVAR_DEFINE_INT32(eot_shadow_map_size, 0, "EdgeOfTime/Graphics",
                     "Shadow map size per cascade in texels: 0 = follow the render resolution "
                     "(the console's 1024 scaled like the frame, 1750 at 1080p), or 1024 / 2048 / "
                     "4096 fixed. Applies to shadow surfaces created after the change.");
REXCVAR_DEFINE_INT32(eot_anisotropy, 0, "EdgeOfTime/Graphics",
                     "Anisotropic filtering: 0 = as the game asks per texture, 1 = off, "
                     "2/4/8/16 = at least that level on every filtered texture.");
REXCVAR_DEFINE_DOUBLE(eot_brightness, 0.0, "EdgeOfTime/Video",
                      "Display brightness offset applied at present (-0.5 .. 0.5, 0 = off).");
REXCVAR_DEFINE_DOUBLE(eot_contrast, 1.0, "EdgeOfTime/Video",
                      "Display contrast applied at present (0.5 .. 2.0, 1 = off).");
REXCVAR_DEFINE_DOUBLE(eot_saturation, 1.0, "EdgeOfTime/Video",
                      "Display saturation applied at present (0 = greyscale, 1 = off, 2 = vivid).");
REXCVAR_DEFINE_DOUBLE(eot_gamma, 1.0, "EdgeOfTime/Video",
                      "Display gamma applied at present before the console's own ramp "
                      "(0.5 .. 2.0, 1 = off).");
REXCVAR_DEFINE_INT32(eot_perf_frames, 0, "eot",
                     "Log a [perf] line every N presented frames: CPU ms per frame in draws, "
                     "resolves, texture uploads, shader links, pipeline builds and the present "
                     "phases, plus upload bytes (0 = off).");

REXCVAR_DEFINE_INT32(eot_diag_frame, 0, "eot",
                     "Guest frame whose draws are logged in detail and whose resolve "
                     "sources are dumped as logs/f<N>_r<K>.ppm (0 = off).")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_rdc_frame, 0, "eot",
                     "Guest frame to capture with the in-process RenderDoc API "
                     "(0 = off; needs eot_rdc_dll).")
    .range(0, 100000000);

REXCVAR_DEFINE_STRING(eot_rdc_dll,
                      "C:/Users/rieng/Documents/GitHub/renderdoc/x64/Development/renderdoc.dll",
                      "eot", "renderdoc.dll to load before the host device is created.");

REXCVAR_DEFINE_STRING(eot_rdc_path, "D:/reeot_caps/tmp/reeot", "eot",
                      "RenderDoc capture file path template.");

REXCVAR_DEFINE_INT32(eot_dump_every, 0, "eot",
                     "Write the presented back buffer as logs/frame_<N>.ppm every N "
                     "presented frames (0 = off).")
    .range(0, 1000000);

REXCVAR_DEFINE_BOOL(eot_present_gamma, true, "EdgeOfTime/Video",
                    "Map the presented front buffer through the guest's display gamma "
                    "ramp (SetGammaRamp / SetPWLGamma), as the console's scan-out LUT does.");

REXCVAR_DEFINE_BOOL(eot_present_log, false, "eot",
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
f64 Settings::RenderScale() { return REXCVAR_GET(eot_render_scale); }
std::string Settings::Resolution() { return std::string(REXCVAR_GET(eot_resolution)); }
i32 Settings::FpsLimit() { return REXCVAR_GET(eot_fps_limit); }
std::string Settings::AspectRatio() { return std::string(REXCVAR_GET(eot_aspect_ratio)); }
std::string Settings::PsoDir() { return std::string(REXCVAR_GET(eot_pso_dir)); }
std::string Settings::PsoTag() { return std::string(REXCVAR_GET(eot_pso_tag)); }
bool Settings::PsoCapture() { return REXCVAR_GET(eot_pso_capture); }
bool Settings::PsoCompiledIn() { return REXCVAR_GET(eot_pso_compiled_in); }
bool Settings::PsoPredict() { return REXCVAR_GET(eot_pso_predict); }
bool Settings::PsoPredictAll() { return REXCVAR_GET(eot_pso_predict_all); }
i32 Settings::PsoPredictFallback() { return REXCVAR_GET(eot_pso_predict_fallback); }
i32 Settings::PsoGateMs() { return REXCVAR_GET(eot_pso_gate_ms); }
i32 Settings::PsoThreads() { return REXCVAR_GET(eot_pso_threads); }
i32 Settings::HitchMs() { return REXCVAR_GET(eot_hitch_ms); }
bool Settings::CommittedTextures() { return REXCVAR_GET(eot_committed_textures); }
bool Settings::Profiler() { return REXCVAR_GET(eot_profiler); }
i32 Settings::PerfFrames() { return REXCVAR_GET(eot_perf_frames); }
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
i32 Settings::DiagFrame() { return REXCVAR_GET(eot_diag_frame); }
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

f32 ComputeRenderScale() {
  const std::string preset = Settings::Resolution();
  u32 target_height = 0;
  if (preset == "720p")
    target_height = 720;
  else if (preset == "1080p")
    target_height = 1080;
  else if (preset == "1440p")
    target_height = 1440;
  const f64 scale = target_height != 0
                        ? static_cast<f64>(target_height) / static_cast<f64>(kGuestRenderHeight)
                        : Settings::RenderScale();
  return static_cast<f32>(std::clamp(scale, 0.25, 4.0));
}

}

f32 RenderScaleFactor() {
  static const f32 s = ComputeRenderScale();
  return s;
}

u32 InternalRenderWidth() { return ScaleDim(kGuestRenderWidth); }
u32 InternalRenderHeight() { return ScaleDim(kGuestRenderHeight); }

}
