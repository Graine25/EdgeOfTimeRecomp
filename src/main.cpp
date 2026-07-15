#include "generated/default/reeot_init.h"

#include "reeot_app.h"

#ifndef NDEBUG
#include <crtdbg.h>
#include <cstdio>

namespace {

struct AssertsToStderr {
  AssertsToStderr() {
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_error_mode(_OUT_TO_STDERR);
  }
} g_asserts_to_stderr;

}
#endif

REX_DEFINE_APP(reeot, ReeotApp::Create)
