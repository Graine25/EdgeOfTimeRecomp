#include "gpu/settings.h"

#include <charconv>
#include <system_error>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(eot_trace_frames, 0, "EdgeOfTime/Debug", "Frames of D3D call tracing")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(eot_trace_start_frame, 0, "EdgeOfTime/Debug", "Frame the trace starts")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_summary_frames, 0, "EdgeOfTime/Debug", "Frames of call summaries")
    .range(0, 1000000);

REXCVAR_DEFINE_INT32(eot_diag, 1, "EdgeOfTime/Debug", "Renderer log verbosity")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(eot_vsync, true, "EdgeOfTime/Video", "Sync frames to display");
REXCVAR_DEFINE_BOOL(eot_profiler, false, "EdgeOfTime/Debug", "Start Tracy at boot");
REXCVAR_DEFINE_BOOL(eot_committed_textures, false, "EdgeOfTime/Config", "Own allocation per surface");
REXCVAR_DEFINE_INT32(eot_hitch_ms, 0, "EdgeOfTime/Debug", "Log frames slower than this")
    .range(0, 1000);
REXCVAR_DEFINE_DOUBLE(eot_render_scale, 1.0, "EdgeOfTime/Video", "Internal render scale");
REXCVAR_DEFINE_STRING(eot_resolution, "1080p", "EdgeOfTime/Video", "Internal render resolution")
    .allowed({"native", "720p", "1080p", "1440p", "2160p"});
REXCVAR_DEFINE_STRING(eot_aspect_ratio, "16:9", "EdgeOfTime/Video", "Fullscreen aspect ratio")
    .allowed({"auto", "4:3", "16:9", "16:10", "21:9", "32:9"});
REXCVAR_DEFINE_INT32(eot_fps_limit, 60, "EdgeOfTime/Video", "Frame rate cap")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(eot_resolve_copy, false, "EdgeOfTime/Config", "Resolve with texture copies");
REXCVAR_DEFINE_BOOL(eot_const_range, true, "EdgeOfTime/Config", "Upload only used constants");
REXCVAR_DEFINE_BOOL(eot_vertex_mirrors, true, "EdgeOfTime/Config", "Mirror static vertex buffers");
REXCVAR_DEFINE_BOOL(eot_bloom, true, "EdgeOfTime/Graphics", "Glow around bright lights");
REXCVAR_DEFINE_BOOL(eot_depth_of_field, true, "EdgeOfTime/Graphics", "Depth of field blur");
REXCVAR_DEFINE_BOOL(eot_motion_blur, true, "EdgeOfTime/Graphics", "Motion blur on or off");
REXCVAR_DEFINE_BOOL(eot_radial_blur, true, "EdgeOfTime/Graphics", "Radial blur streaks");
REXCVAR_DEFINE_BOOL(eot_heat_effects, true, "EdgeOfTime/Graphics", "Heat vision and haze");
REXCVAR_DEFINE_BOOL(eot_film_grain, true, "EdgeOfTime/Graphics", "Film grain overlay");
REXCVAR_DEFINE_BOOL(eot_halo, true, "EdgeOfTime/Graphics", "Halo light bleed");
REXCVAR_DEFINE_BOOL(eot_color_grading, true, "EdgeOfTime/Graphics", "Scene color grading");
REXCVAR_DEFINE_DOUBLE(eot_fov_scale, 1.0, "EdgeOfTime/Graphics", "Field of view scale");
REXCVAR_DEFINE_INT32(eot_shadow_cascades, 0, "EdgeOfTime/Graphics", "Shadow cascades per camera");
REXCVAR_DEFINE_DOUBLE(eot_shadow_distance_scale, 1.0, "EdgeOfTime/Graphics", "Shadow distance multiplier");
REXCVAR_DEFINE_INT32(eot_debug_quality_level, -1, "EdgeOfTime/Debug", "Force the pak language id");
REXCVAR_DEFINE_INT32(eot_shadow_map_size, 0, "EdgeOfTime/Graphics", "Shadow map resolution");
REXCVAR_DEFINE_INT32(eot_anisotropy, 16, "EdgeOfTime/Graphics", "Anisotropic filtering level")
    .range(0, 16);
REXCVAR_DEFINE_INT32(eot_msaa, 0, "EdgeOfTime/Graphics", "MSAA sample count")
    .range(0, 8)
    .validator([](std::string_view v) {
      int n = 0;
      const auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 0 || n == 2 || n == 4 || n == 8);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(eot_brightness, 0.0, "EdgeOfTime/Video", "Screen brightness offset");
REXCVAR_DEFINE_DOUBLE(eot_contrast, 1.0, "EdgeOfTime/Video", "Screen contrast amount");
REXCVAR_DEFINE_DOUBLE(eot_saturation, 1.0, "EdgeOfTime/Video", "Screen color saturation");
REXCVAR_DEFINE_DOUBLE(eot_gamma, 1.0, "EdgeOfTime/Video", "Screen gamma curve");
REXCVAR_DEFINE_INT32(eot_perf_frames, 600, "EdgeOfTime/Debug", "Perf log every N frames");

REXCVAR_DEFINE_INT32(eot_diag_frame, 0, "EdgeOfTime/Debug", "Frame to log in detail")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_rdc_frame, 0, "EdgeOfTime/Debug", "Frame to capture in RenderDoc")
    .range(0, 100000000);

REXCVAR_DEFINE_STRING(eot_rdc_dll,
                      "C:/Users/rieng/Documents/GitHub/renderdoc/x64/Development/renderdoc.dll",
                      "EdgeOfTime/Debug", "Path to renderdoc.dll");

REXCVAR_DEFINE_STRING(eot_rdc_path, "D:/reeot_caps/tmp/reeot", "EdgeOfTime/Debug", "RenderDoc capture path");

REXCVAR_DEFINE_BOOL(eot_texture_replace, true, "EdgeOfTime/Config", "Replace textures from folder");
REXCVAR_DEFINE_BOOL(eot_dump_textures, false, "EdgeOfTime/Debug", "Dump every texture seen");
REXCVAR_DEFINE_INT32(eot_dump_every, 0, "EdgeOfTime/Debug", "Save a frame every N")
    .range(0, 1000000);

REXCVAR_DEFINE_BOOL(eot_present_gamma, true, "EdgeOfTime/Video", "Use the console gamma ramp");

REXCVAR_DEFINE_BOOL(eot_present_log, false, "EdgeOfTime/Debug", "Log each present time");

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
i32 Settings::HitchMs() { return REXCVAR_GET(eot_hitch_ms); }
bool Settings::CommittedTextures() { return REXCVAR_GET(eot_committed_textures); }
bool Settings::Profiler() { return REXCVAR_GET(eot_profiler); }
i32 Settings::PerfFrames() { return REXCVAR_GET(eot_perf_frames); }
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
bool Settings::TextureReplace() { return REXCVAR_GET(eot_texture_replace); }
bool Settings::DumpTextures() { return REXCVAR_GET(eot_dump_textures); }
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

f32 ComputeRenderScale() {
  const std::string preset = Settings::Resolution();
  u32 target_height = 0;
  if (preset == "720p")
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

f32 RenderScaleFactor() {
  static const f32 s = ComputeRenderScale();
  return s;
}

u32 InternalRenderWidth() { return ScaleDim(kGuestRenderWidth); }
u32 InternalRenderHeight() { return ScaleDim(kGuestRenderHeight); }

}
