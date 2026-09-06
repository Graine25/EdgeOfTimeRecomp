#include "core/quit.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <rex/logging.h>

#include "core/logging.h"
#include "gpu/device.h"

namespace eot {

namespace {

constexpr unsigned kQuitDeadlineMs = 3000;

std::atomic<const char *> g_phase{"start"};
std::atomic<bool> g_quitting{false};

[[noreturn]] void EndProcess(int code) {
  rex::FlushLogging();
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
  for (;;)
    ::Sleep(1000);
#else
  std::_Exit(code);
#endif
}

}

void QuitProcess(int code) {
  if (g_quitting.exchange(true, std::memory_order_acq_rel)) {
    for (;;)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EOT_INFO("[quit] shutting down");
  std::thread([code] {
    std::this_thread::sleep_for(std::chrono::milliseconds(kQuitDeadlineMs));
    EOT_WARN("[quit] stalled in '{}' after {} ms; terminating anyway",
             g_phase.load(std::memory_order_relaxed), kQuitDeadlineMs);
    EndProcess(code);
  }).detach();

  g_phase.store("renderer", std::memory_order_relaxed);
  gpu::Video::BeginShutdown();
  g_phase.store("exit", std::memory_order_relaxed);
  EndProcess(code);
}

}

extern "C" __declspec(dllexport) void eot_quit_process(int code) { eot::QuitProcess(code); }
