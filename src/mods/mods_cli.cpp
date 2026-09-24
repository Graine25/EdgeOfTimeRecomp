#include "mods/mods_cli.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#include "core/logging.h"
#include "mods/mod_manager.h"

namespace eot::mods {

namespace {

constexpr int kDone = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;

bool g_attached = false;

void AttachConsole() {
#if defined(_WIN32)
  const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != nullptr && out != INVALID_HANDLE_VALUE)
    return;
  if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
    g_attached = true;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
  }
#endif
}

void ReturnToPrompt() {
#if defined(_WIN32)
  if (!g_attached)
    return;
  const HANDLE in = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
  if (in == INVALID_HANDLE_VALUE)
    return;
  INPUT_RECORD keys[2] = {};
  for (int i = 0; i < 2; ++i) {
    keys[i].EventType = KEY_EVENT;
    keys[i].Event.KeyEvent.bKeyDown = i == 0 ? TRUE : FALSE;
    keys[i].Event.KeyEvent.wRepeatCount = 1;
    keys[i].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    keys[i].Event.KeyEvent.wVirtualScanCode = static_cast<WORD>(::MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    keys[i].Event.KeyEvent.uChar.UnicodeChar = L'\r';
  }
  DWORD written = 0;
  ::WriteConsoleInputW(in, keys, 2, &written);
  ::CloseHandle(in);
#endif
}

void Print(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

std::string Utf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::filesystem::path PathFromUtf8(const std::string &text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string Lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::vector<std::string> WordsAfterCommand(const std::vector<std::string> &positional) {
  std::vector<std::string> words;
#if defined(_WIN32)
  int argc = 0;
  wchar_t **argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (argv) {
    bool after = false;
    for (int i = 1; i < argc; ++i) {
      const int n = ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string word(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
      if (n > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, word.data(), n, nullptr, nullptr);
      if (after)
        words.push_back(word);
      else if (Lower(word) == kCliCommand)
        after = true;
    }
    ::LocalFree(argv);
    if (after)
      return words;
  }
#endif
  return positional;
}

bool IsHelpWord(std::string_view word) {
  const std::string w = Lower(std::string(word));
  return w == "--help" || w == "-h" || w == "--h" || w == "-?" || w == "/?" || w == "help";
}

constexpr std::string_view kCommands =
    "  reeot mods list              every mod: on or off, name, creator, kind, folder, file\n"
    "  reeot mods add <path>        add a mod: a mod folder (or its mod.toml), or a package file\n"
    "  reeot mods remove <name>     take the mod's file back and delete its folder\n"
    "  reeot mods enable <name>     switch a mod on\n"
    "  reeot mods disable <name>    switch a mod off\n"
    "  reeot mods folder            print where the mods live";

void PrintUsage() {
  Print("usage: reeot mods <command> [<argument>]\n");
  Print(kCommands);
  Print("\n<name> is a mod's folder under mods, or its name.");
}

std::string Padded(std::string text, size_t width) {
  if (text.size() > width)
    return text.substr(0, width - 1) + "~";
  text.resize(width, ' ');
  return text;
}

void PrintTable(const std::vector<Mod> &mods) {
  if (mods.empty()) {
    Print(std::format("No mods in {}. `reeot mods add <path>` adds one.", Utf8(ModsDir())));
    return;
  }
  size_t name_w = 4, creator_w = 7, folder_w = 6;
  for (const Mod &mod : mods) {
    name_w = std::max(name_w, mod.manifest.name.size());
    creator_w = std::max(creator_w, mod.manifest.creator.size());
    folder_w = std::max(folder_w, mod.folder.size());
  }
  name_w = std::min(name_w, size_t(40));
  creator_w = std::min(creator_w, size_t(24));
  folder_w = std::min(folder_w, size_t(32));
  Print(std::format("{}  {}  {}  {}  {}  {}", Padded("", 3), Padded("name", name_w), Padded("creator", creator_w),
                    Padded("kind", 11), Padded("folder", folder_w), "file / state"));
  for (const Mod &mod : mods) {
    std::string tail = mod.manifest.file;
    if (mod.manifest.type == ModType::kPackage)
      tail += std::format(" (package {:#x}{})", mod.manifest.package_id,
                          mod.manifest.language.empty() ? std::string() : ", " + mod.manifest.language);
    if (!mod.enabled)
      tail += mod.status.empty() ? "" : " -- " + mod.status;
    else if (!mod.active)
      tail += " -- " + mod.status;
    Print(std::format("{}  {}  {}  {}  {}  {}", Padded(mod.enabled ? "on" : "off", 3),
                      Padded(mod.manifest.name, name_w), Padded(mod.manifest.creator, creator_w),
                      Padded(TypeName(mod.manifest.type), 11), Padded(mod.folder, folder_w), tail));
  }
  Print(std::format("\n{} mod(s) in {}. Changes take effect at the next start of the game.", mods.size(),
                    Utf8(ModsDir())));
}

void NoteLanguages() {
  for (const LanguageMod &mod : LanguageMods()) {
    if (mod.asked)
      continue;
    Print(std::format("{} brings the game in {}: the next start of the game asks whether to run in it, and the "
                      "Language row under Options > Game lists it either way.",
                      mod.name, mod.language_name));
  }
}

bool NeedsArgument(const std::string &verb, const std::string &arg, const char *what) {
  if (!arg.empty())
    return false;
  Print(std::format("reeot mods {} needs {}.", verb, what));
  PrintUsage();
  return true;
}

}

static int Run(const std::vector<std::string> &positional);

int RunCli(const std::vector<std::string> &positional) {
  AttachConsole();
  const int code = Run(positional);
  ReturnToPrompt();
  return code;
}

static int Run(const std::vector<std::string> &positional) {
  const std::vector<std::string> words = WordsAfterCommand(positional);
  const std::string verb = words.empty() ? std::string() : Lower(words[0]);
  const std::string arg = words.size() > 1 ? words[1] : std::string();

  if (std::any_of(words.begin(), words.end(), [](const std::string &w) { return IsHelpWord(w); })) {
    PrintUsage();
    return kDone;
  }
  if (verb.empty()) {
    PrintUsage();
    return kUsage;
  }
  if (!Ready()) {
    Print("No install: the game has not been installed on this machine (run reeot to install it).");
    return kFailed;
  }

  std::string message;
  bool ok = true;
  if (verb == "list") {
    PrintTable(List());
  } else if (verb == "folder") {
    Print(Utf8(ModsDir()));
  } else if (verb == "add") {
    if (NeedsArgument(verb, arg, "the path of a mod folder, its mod.toml, or a package file"))
      return kUsage;
    ok = Add(std::filesystem::absolute(PathFromUtf8(arg)), message);
    Print(message);
    if (ok)
      NoteLanguages();
  } else if (verb == "remove") {
    if (NeedsArgument(verb, arg, "the name of a mod"))
      return kUsage;
    ok = Remove(arg, message);
    Print(message);
  } else if (verb == "enable" || verb == "disable") {
    if (NeedsArgument(verb, arg, "the name of a mod"))
      return kUsage;
    ok = SetEnabled(arg, verb == "enable", message);
    Print(message);
    if (ok && verb == "enable")
      NoteLanguages();
  } else {
    Print(std::format("reeot mods: {} is not a command.", words[0]));
    PrintUsage();
    return kUsage;
  }
  EOT_INFO("[mods] cli {} {}: {}", verb, arg, ok ? "ok" : message);
  return ok ? kDone : kFailed;
}

}
