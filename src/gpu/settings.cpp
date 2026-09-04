#include "gpu/settings.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(eot_trace_frames, 0, "eot", "Frames of D3D call tracing")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(eot_trace_start_frame, 0, "eot", "Frame the trace starts")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_summary_frames, 600, "eot", "Frames of call summaries")
    .range(0, 1000000);

REXCVAR_DEFINE_INT32(eot_diag, 1, "eot", "Renderer log verbosity")
    .range(0, 2);

REXCVAR_DEFINE_BOOL(eot_vsync, true, "eot", "Sync frames to display");
REXCVAR_DEFINE_STRING(eot_pso_dir, "pso", "eot", "Pipeline capture folder");
REXCVAR_DEFINE_STRING(eot_pso_tag, "", "eot", "Tag for capture files");
REXCVAR_DEFINE_BOOL(eot_pso_capture, true, "eot", "Capture missed pipelines");
REXCVAR_DEFINE_BOOL(eot_pso_compiled_in, true, "eot", "Precache built-in pipelines");
REXCVAR_DEFINE_BOOL(eot_pso_predict, true, "eot", "Predict model pipelines");
REXCVAR_DEFINE_BOOL(eot_pso_predict_all, false, "eot", "Predict every material combo");
REXCVAR_DEFINE_INT32(eot_pso_predict_fallback, 0, "eot", "Predictor matching level")
    .range(0, 2);
REXCVAR_DEFINE_INT32(eot_pso_gate_ms, 250, "eot", "Max wait for pipelines")
    .range(0, 5000);
REXCVAR_DEFINE_INT32(eot_pso_threads, 0, "eot", "Pipeline worker threads")
    .range(0, 16);
REXCVAR_DEFINE_DOUBLE(eot_render_scale, 1.0, "eot", "Internal render scale");
REXCVAR_DEFINE_BOOL(eot_resolve_copy, false, "eot", "Resolve with texture copies");
REXCVAR_DEFINE_BOOL(eot_const_range, true, "eot", "Upload only used constants");
REXCVAR_DEFINE_BOOL(eot_vertex_mirrors, true, "eot", "Mirror static vertex buffers");
REXCVAR_DEFINE_INT32(eot_perf_frames, 0, "eot", "Perf log every N frames");

REXCVAR_DEFINE_INT32(eot_diag_frame, 0, "eot", "Frame to log in detail")
    .range(0, 100000000);

REXCVAR_DEFINE_INT32(eot_rdc_frame, 0, "eot", "Frame to capture in RenderDoc")
    .range(0, 100000000);

REXCVAR_DEFINE_STRING(eot_rdc_dll,
                      "C:/Users/rieng/Documents/GitHub/renderdoc/x64/Development/renderdoc.dll",
                      "eot", "Path to renderdoc.dll");

REXCVAR_DEFINE_STRING(eot_rdc_path, "D:/reeot_caps/tmp/reeot", "eot", "RenderDoc capture path");

REXCVAR_DEFINE_INT32(eot_dump_every, 0, "eot", "Save a frame every N")
    .range(0, 1000000);

REXCVAR_DEFINE_BOOL(eot_present_gamma, true, "eot", "Use the console gamma ramp");

REXCVAR_DEFINE_BOOL(eot_present_log, false, "eot", "Log each present time");

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
std::string Settings::PsoDir() { return std::string(REXCVAR_GET(eot_pso_dir)); }
std::string Settings::PsoTag() { return std::string(REXCVAR_GET(eot_pso_tag)); }
bool Settings::PsoCapture() { return REXCVAR_GET(eot_pso_capture); }
bool Settings::PsoCompiledIn() { return REXCVAR_GET(eot_pso_compiled_in); }
bool Settings::PsoPredict() { return REXCVAR_GET(eot_pso_predict); }
bool Settings::PsoPredictAll() { return REXCVAR_GET(eot_pso_predict_all); }
i32 Settings::PsoPredictFallback() { return REXCVAR_GET(eot_pso_predict_fallback); }
i32 Settings::PsoGateMs() { return REXCVAR_GET(eot_pso_gate_ms); }
i32 Settings::PsoThreads() { return REXCVAR_GET(eot_pso_threads); }
i32 Settings::PerfFrames() { return REXCVAR_GET(eot_perf_frames); }
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
i32 Settings::DiagFrame() { return REXCVAR_GET(eot_diag_frame); }
i32 Settings::RenderDocFrame() { return REXCVAR_GET(eot_rdc_frame); }
std::string Settings::RenderDocDll() { return std::string(REXCVAR_GET(eot_rdc_dll)); }
std::string Settings::RenderDocPath() { return std::string(REXCVAR_GET(eot_rdc_path)); }
bool Settings::PresentGamma() { return REXCVAR_GET(eot_present_gamma); }
bool Settings::PresentFrameLog() { return REXCVAR_GET(eot_present_log); }

}
