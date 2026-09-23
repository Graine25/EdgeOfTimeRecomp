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

#include <rex/cvar.h>

#include "core/logging.h"
#include "mods/mod_manager.h"

namespace eot::mods {

namespace {

constexpr int kDone = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;

void AttachConsole() {
#if defined(_WIN32)
  const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != nullptr && out != INVALID_HANDLE_VALUE)
    return;
  if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
  }
#endif
}

void Print(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

bool AskYesNo(const std::string &question) {
  std::fputs((question + " [y/N] ").c_str(), stdout);
  std::fflush(stdout);
  char line[16] = {};
  if (!std::fgets(line, sizeof(line), stdin))
    return false;
  return line[0] == 'y' || line[0] == 'Y';
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
  Print("\n<name> is a mod's folder under mods, or its name; case does not matter.\n"
        "reeot mods --help explains the mods, their kinds and the mod.toml.");
}

void PrintHelp() {
  Print("reeot mods -- the mods of the installed game, from the command line\n");
  Print("usage: reeot mods <command> [<argument>]\n");
  Print(kCommands);
  Print(R"(
<name> is a mod's folder under mods, or its name; case does not matter.
Exit code 0 when the command is done, 1 when it failed (the line says why),
2 for no command or one that is not known.

A MOD
  A folder under <install>\mods holding a mod.toml beside the one file it
  brings. Add copies the folder in (or makes one up for a package file on
  its own); the game reads every folder when it starts.

    name = "Russian"                  what the Mods page and the list show
    creator = "Graine25"
    version = "1.0"                   optional
    description = "..."               optional
    type = "package"                  package, replacement or model

    [package]                         a new package the game loads at boot
    file = "ReeotRussian.pkz"         the file beside the manifest
    id = 0x7EB                        the package id, 1 to 4095
    language = "ru"                   optional: only when the game runs in this
                                      language; the mod is then a language mod
    language_name = "Russian"         optional: what the Options page calls it

    [replacement]                     a package over one of the game's own
    file = "Common.pkz"               named like the file in the game's Data folder;
                                      the game's own is kept in mods\backup

    [model]                           a costume package as downloadable content
    file = "_DLC002.pak"              a raw DLC package (id 0xBB9 or 0xBBA) or a
                                      _DLC00N.pkz; one costume mod is on at a time

  Everything takes effect at the next start of the game. mods\mods.toml keeps
  which mods are off. The port's own mods (russian, spiderverse-2099) come
  out of the program at start unless removed; `reeot mods add <its folder
  name>` puts a removed one back.

LANGUAGE MODS
  A language mod is never chosen on its own. The Options page's Language row
  lists it, and the first start with it in place asks once whether the game
  should run in it; add and enable ask the same here. Switching it off or
  removing it while the game is set to its language puts the language back
  to auto.

OUTPUT
  reeot is a windowed program: the lines go to the console it was started
  from (the prompt may come back before they do), or to a file when
  redirected (reeot mods list > mods.txt).)");
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

void OfferLanguages() {
  for (const LanguageMod &mod : LanguageMods()) {
    if (mod.asked)
      continue;
    Print(std::format("{} brings the game in {}.", mod.name, mod.language_name));
    if (AskYesNo(std::format("Start the game in {} from now on?", mod.language_name))) {
      rex::cvar::SetFlagByName("eot_language", mod.tag);
      rex::cvar::InvokeCommand("eot_save_settings", "");
      Print(std::format("The language is {} (eot_language = {}); Options > Game changes it.", mod.language_name,
                        mod.tag));
    } else {
      Print("The language is unchanged; the row under Options > Game offers it.");
    }
    LanguageAsked(mod.folder);
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

int RunCli(const std::vector<std::string> &positional) {
  AttachConsole();
  const std::vector<std::string> words = WordsAfterCommand(positional);
  const std::string verb = words.empty() ? std::string() : Lower(words[0]);
  const std::string arg = words.size() > 1 ? words[1] : std::string();

  if (std::any_of(words.begin(), words.end(), [](const std::string &w) { return IsHelpWord(w); })) {
    PrintHelp();
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
      OfferLanguages();
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
      OfferLanguages();
  } else {
    Print(std::format("reeot mods: {} is not a command.", words[0]));
    PrintUsage();
    return kUsage;
  }
  EOT_INFO("[mods] cli {} {}: {}", verb, arg, ok ? "ok" : message);
  return ok ? kDone : kFailed;
}

}
