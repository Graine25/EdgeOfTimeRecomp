#pragma once

#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace eot {

[[noreturn]] inline void QuitProcessFromModule(int code = 0) {
#if defined(_WIN32)
  if (HMODULE host = ::GetModuleHandleW(nullptr)) {
    auto *quit = reinterpret_cast<void (*)(int)>(
        reinterpret_cast<void *>(::GetProcAddress(host, "eot_quit_process")));
    if (quit)
      quit(code);
  }
#endif
  std::_Exit(code);
}

}
