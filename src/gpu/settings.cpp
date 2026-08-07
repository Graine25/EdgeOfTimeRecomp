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
i32 Settings::DumpEvery() { return REXCVAR_GET(eot_dump_every); }
i32 Settings::DiagFrame() { return REXCVAR_GET(eot_diag_frame); }
i32 Settings::RenderDocFrame() { return REXCVAR_GET(eot_rdc_frame); }
std::string Settings::RenderDocDll() { return std::string(REXCVAR_GET(eot_rdc_dll)); }
std::string Settings::RenderDocPath() { return std::string(REXCVAR_GET(eot_rdc_path)); }
bool Settings::PresentGamma() { return REXCVAR_GET(eot_present_gamma); }
bool Settings::PresentFrameLog() { return REXCVAR_GET(eot_present_log); }

}
