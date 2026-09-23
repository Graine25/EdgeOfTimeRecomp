#include "mods/mods_cli.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>

#include "core/logging.h"
#include "mods/mod_manager.h"

namespace eot::mods {

namespace {

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

void Print(const std::string &text) {
  std::fputs(text.c_str(), stdout);
  std::fputs("\n", stdout);
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

std::string Padded(std::string text, size_t width) {
  if (text.size() > width)
    return text.substr(0, width - 1) + "~";
  text.resize(width, ' ');
  return text;
}

std::string Utf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

void PrintTable(const std::vector<Mod> &mods) {
  if (mods.empty()) {
    Print(std::format("No mods in {}.", Utf8(ModsDir())));
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

int Usage() {
  Print("reeot mods                  the mods, on or off\n"
        "reeot mods list\n"
        "reeot mods add <path>       a folder with a mod.toml, or a package file on its own;\n"
        "                            the name of one of the port's own mods puts it back\n"
        "reeot mods remove <name>    the mod's file taken back and its folder deleted\n"
        "reeot mods enable <name>\n"
        "reeot mods disable <name>\n"
        "reeot mods folder           where the mods live\n"
        "\n"
        "<name> is a mod's folder under mods/ or its name. A mod.toml names the mod, its creator\n"
        "and its kind: package (a new package the game loads at boot: [package] file, id,\n"
        "language), replacement (a package in place of one of the game's own: [replacement]\n"
        "file), or model (a costume package published as downloadable content: [model] file).");
  return 2;
}

}

int RunCli(const std::vector<std::string> &args) {
  AttachConsole();
  if (!Ready()) {
    Print("No install: the game has not been installed on this machine (run reeot to install it).");
    return 1;
  }
  const std::string verb = args.empty() ? "list" : args[0];
  const std::string arg = args.size() > 1 ? args[1] : std::string();
  std::string message;
  bool ok = true;
  if (verb == "list") {
    PrintTable(List());
  } else if (verb == "folder") {
    Print(Utf8(ModsDir()));
  } else if (verb == "add") {
    if (arg.empty())
      return Usage();
    ok = Add(std::filesystem::absolute(std::filesystem::path(std::u8string(arg.begin(), arg.end()))), message);
    Print(message);
    if (ok)
      OfferLanguages();
  } else if (verb == "remove") {
    if (arg.empty())
      return Usage();
    ok = Remove(arg, message);
    Print(message);
  } else if (verb == "enable" || verb == "disable") {
    if (arg.empty())
      return Usage();
    ok = SetEnabled(arg, verb == "enable", message);
    Print(message);
    if (ok && verb == "enable")
      OfferLanguages();
  } else {
    return Usage();
  }
  EOT_INFO("[mods] cli {} {}: {}", verb, arg, ok ? "ok" : message);
  return ok ? 0 : 1;
}

}
