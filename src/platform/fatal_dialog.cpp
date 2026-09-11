#include "platform/fatal_dialog.h"

#include <string>

#include <SDL3/SDL.h>

#include "core/encoding.h"
#include "core/logging.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace eot::platform {
namespace {

#if defined(_WIN32)

BOOL CALLBACK MinimizeOwnTopLevelWindow(HWND hwnd, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr)
    ShowWindowAsync(hwnd, SW_FORCEMINIMIZE);
  return TRUE;
}

void ShowModal(std::string_view title, std::string_view body, bool warning) {
  EnumWindows(MinimizeOwnTopLevelWindow, 0);
  MessageBoxW(nullptr, Utf8ToWide(std::string(body)).c_str(), Utf8ToWide(std::string(title)).c_str(),
              MB_OK | (warning ? MB_ICONWARNING : MB_ICONERROR) | MB_TOPMOST | MB_SETFOREGROUND);
}

void ShowInfoModal(std::string_view title, std::string_view body) {
  MessageBoxW(nullptr, Utf8ToWide(std::string(body)).c_str(), Utf8ToWide(std::string(title)).c_str(),
              MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
}

#else

void ShowModal(std::string_view title, std::string_view body, bool warning) {
  const std::string t(title);
  const std::string b(body);
  SDL_ShowSimpleMessageBox(warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_ERROR, t.c_str(), b.c_str(),
                           nullptr);
}

void ShowInfoModal(std::string_view title, std::string_view body) {
  const std::string t(title);
  const std::string b(body);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, t.c_str(), b.c_str(), nullptr);
}

#endif

constexpr int kDeclineButtonId = 0;
constexpr int kAcceptButtonId = 1;

bool ShowChoice(std::string_view title, std::string_view body, std::string_view accept, std::string_view decline,
                SDL_MessageBoxFlags flags, rex::ui::Window *parent) {
  SDL_Window *sdl_parent = nullptr;
  if (parent) {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    sdl_parent = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
  }

  const std::string t(title);
  const std::string b(body);
  const std::string yes(accept);
  const std::string no(decline);
  const SDL_MessageBoxButtonData buttons[] = {
      {0, kAcceptButtonId, yes.c_str()},
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kDeclineButtonId,
       no.c_str()},
  };
  const SDL_MessageBoxData data{flags, sdl_parent, t.c_str(), b.c_str(), 2, buttons, nullptr};

  int button_id = kDeclineButtonId;
  if (!SDL_ShowMessageBox(&data, &button_id)) {
    EOT_WARN("[dialog] {} could not be shown: {}", t, SDL_GetError());
    return false;
  }
  return button_id == kAcceptButtonId;
}

}

void ShowFatalError(std::string_view title, std::string_view body) {
  EOT_ERROR("Fatal: {} - {}", title, body);
  ShowModal(title, body, false);
}

void ShowWarning(std::string_view title, std::string_view body) {
  EOT_WARN("{} - {}", title, body);
  ShowModal(title, body, true);
}

void ShowInfo(std::string_view title, std::string_view body) {
  EOT_INFO("{} - {}", title, body);
  ShowInfoModal(title, body);
}

bool ShowFatalErrorWithAction(std::string_view title, std::string_view body, std::string_view action,
                              rex::ui::Window *parent) {
  EOT_ERROR("Fatal: {} - {}", title, body);
  return ShowChoice(title, body, action, "Quit", SDL_MESSAGEBOX_ERROR, parent);
}

}
