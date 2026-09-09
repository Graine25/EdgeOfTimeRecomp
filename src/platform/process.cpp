#include "platform/process.h"

#include <rex/filesystem.h>

#include "core/logging.h"

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>
#endif

namespace eot::platform {

std::filesystem::path ProgramDir() { return rex::filesystem::GetExecutableFolder(); }

bool SpawnReplacement(const std::filesystem::path &exe) {
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
    EOT_ERROR("[install] CreateProcessW failed for {} (error {})", exe.string(), GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  (void)exe;
  return false;
#endif
}

}
