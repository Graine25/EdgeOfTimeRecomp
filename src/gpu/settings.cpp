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

REXCVAR_DEFINE_BOOL(eot_vsync, true, "eot", "Present with vsync.");
REXCVAR_DEFINE_STRING(eot_pso_dir, "pso", "eot",
                      "Directory of pipeline capture CSVs: every *.csv in it is precached on "
                      "worker threads at boot, and render-thread misses of this run are appended "
                      "to a new pso_misses_<tag>_<time>.csv there (empty = off).");
REXCVAR_DEFINE_STRING(eot_pso_tag, "", "eot",
                      "Tester or area tag written into pipeline capture file names and rows.");
REXCVAR_DEFINE_BOOL(eot_pso_capture, true, "eot",
                    "Capture pipelines the render thread had to build to eot_pso_dir.");
REXCVAR_DEFINE_DOUBLE(eot_render_scale, 1.0, "eot",
                      "Internal render scale: EDRAM surfaces and resolve mirrors are allocated at "
                      "guest size x this (0.25..4); the present scales to the window as before.");
REXCVAR_DEFINE_BOOL(eot_resolve_copy, false, "eot",
                    "Perform same-format, 1:1, no-reorder resolves as texture copies instead of "
                    "full-screen sampling draws. Measured neutral on the GPU and slower to record "
                    "on D3D12/AMD (2026-09-03); off until re-tested on Vulkan.");
REXCVAR_DEFINE_BOOL(eot_const_range, true, "eot",
                    "Upload only the prefix of each 4 KB float constant file the bound shader "
                    "can address (from its constant table) instead of the whole file.");
REXCVAR_DEFINE_BOOL(eot_vertex_mirrors, true, "eot",
                    "Mirror static guest vertex buffers into persistent host buffers on their "
                    "second sighting instead of byte-swapping the drawn range per draw.");
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

REXCVAR_DEFINE_BOOL(eot_present_gamma, true, "eot",
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
std::string Settings::PsoDir() { return std::string(REXCVAR_GET(eot_pso_dir)); }
std::string Settings::PsoTag() { return std::string(REXCVAR_GET(eot_pso_tag)); }
bool Settings::PsoCapture() { return REXCVAR_GET(eot_pso_capture); }
i32 Settings::PerfFrames() { return REXCVAR_GET(eot_perf_frames); }
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
i32 Settings::DiagFrame() { return REXCVAR_GET(eot_diag_frame); }
i32 Settings::RenderDocFrame() { return REXCVAR_GET(eot_rdc_frame); }
std::string Settings::RenderDocDll() { return std::string(REXCVAR_GET(eot_rdc_dll)); }
std::string Settings::RenderDocPath() { return std::string(REXCVAR_GET(eot_rdc_path)); }
bool Settings::PresentGamma() { return REXCVAR_GET(eot_present_gamma); }
bool Settings::PresentFrameLog() { return REXCVAR_GET(eot_present_log); }

}
