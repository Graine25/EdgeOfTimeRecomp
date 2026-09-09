#include "platform/process.h"

#include <SDL3/SDL.h>
#include <rex/filesystem.h>

#include "core/logging.h"

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>
#endif

namespace eot::platform {
namespace {

#if defined(_WIN32)
constexpr wchar_t kInstanceLockName[] = L"Local\reeot-single-instance";
HANDLE g_instance_lock = nullptr;
#endif

void ReleaseInstanceLock() {
#if defined(_WIN32)
  if (!g_instance_lock)
    return;
  ::CloseHandle(g_instance_lock);
  g_instance_lock = nullptr;
#endif
}

bool SpawnProcess(const std::filesystem::path &exe) {
#if defined(_WIN32)
  std::wstring cmdline = L"\"" + exe.wstring() + L"\"";
  int argc = 0;
  if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      cmdline += L" \"";
      cmdline += argv[i];
      cmdline += L"\"";
    }
    LocalFree(argv);
  }
  std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
  buffer.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = exe.parent_path().wstring();
  if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    EOT_ERROR("[process] CreateProcessW failed for {} (error {})", exe.string(), GetLastError());
    return false;
  }
  AllowSetForegroundWindow(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  (void)exe;
  return false;
#endif
}

}

std::filesystem::path ProgramDir() { return rex::filesystem::GetExecutableFolder(); }

bool AcquireInstanceLock() {
#if defined(_WIN32)
  HANDLE h = ::CreateMutexW(nullptr, FALSE, kInstanceLockName);
  if (!h) {
    EOT_WARN("[process] instance lock unavailable (error {})", ::GetLastError());
    return true;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(h);
    return false;
  }
  g_instance_lock = h;
  return true;
#else
  return true;
#endif
}

bool SpawnReplacement(const std::filesystem::path &exe) {
  ReleaseInstanceLock();
  if (SpawnProcess(exe))
    return true;
  AcquireInstanceLock();
  return false;
}

bool RelaunchSelf() { return SpawnReplacement(rex::filesystem::GetExecutablePath()); }

void RaiseMainWindow() {
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  if (windows && count > 0)
    SDL_RaiseWindow(windows[0]);
  SDL_free(windows);
}

}
