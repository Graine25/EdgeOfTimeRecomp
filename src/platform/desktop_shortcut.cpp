#include "platform/desktop_shortcut.h"

#if defined(_WIN32)
#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

#include "core/encoding.h"

namespace eot::platform {
namespace {

std::wstring ShortcutFileName(std::string_view name) {
  std::wstring out = Utf8ToWide(name);
  std::erase_if(out, [](wchar_t c) {
    return c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' || c == L'|' ||
           c == L'?' || c == L'*' || c < 32;
  });
  return out;
}

template <typename T> struct ComRelease {
  T *p;
  ~ComRelease() {
    if (p)
      p->Release();
  }
};

bool WriteShortcut(const std::filesystem::path &target, std::string_view name, std::string &error) {
  IShellLinkW *link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                              reinterpret_cast<void **>(&link)))) {
    error = "CoCreateInstance(CLSID_ShellLink) failed";
    return false;
  }
  ComRelease<IShellLinkW> link_guard{link};
  if (FAILED(link->SetPath(target.c_str()))) {
    error = "IShellLinkW::SetPath failed";
    return false;
  }
  if (FAILED(link->SetWorkingDirectory(target.parent_path().c_str()))) {
    error = "IShellLinkW::SetWorkingDirectory failed";
    return false;
  }
  IPersistFile *persist = nullptr;
  if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist)))) {
    error = "QueryInterface(IID_IPersistFile) failed";
    return false;
  }
  ComRelease<IPersistFile> persist_guard{persist};

  PWSTR desktop = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) {
    error = "SHGetKnownFolderPath(FOLDERID_Desktop) failed";
    return false;
  }
  const std::wstring file = ShortcutFileName(name);
  const std::filesystem::path lnk = std::filesystem::path(desktop) / (file + L".lnk");
  CoTaskMemFree(desktop);
  if (file.empty()) {
    error = "the shortcut name is empty once the reserved characters are removed";
    return false;
  }
  if (FAILED(persist->Save(lnk.c_str(), TRUE))) {
    error = "IPersistFile::Save failed for " + lnk.string();
    return false;
  }
  return true;
}

}

bool CreateDesktopShortcut(const std::filesystem::path &target, std::string_view name,
                           std::string &error) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool ok = WriteShortcut(target, name, error);
  if (SUCCEEDED(init))
    CoUninitialize();
  return ok;
}

}

#else

namespace eot::platform {

bool CreateDesktopShortcut(const std::filesystem::path &, std::string_view, std::string &error) {
  error = "desktop shortcuts are only made on Windows";
  return false;
}

}

#endif
