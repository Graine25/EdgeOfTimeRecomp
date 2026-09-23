#include "mods/mod_manifest.h"

#include <format>
#include <string>

#include <toml++/toml.hpp>

namespace eot::mods {

namespace {

constexpr const char *kTypeNames[] = {"package", "replacement", "model"};

std::string StringAt(const toml::table &table, const char *key) {
  if (const toml::node *node = table.get(key))
    if (const auto *value = node->as_string())
      return value->get();
  return {};
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
    }
  }
  return out + "\"";
}

}

const char *TypeName(ModType type) { return kTypeNames[static_cast<int>(type)]; }

bool TypeFromName(std::string_view name, ModType &out) {
  for (int i = 0; i < 3; ++i) {
    if (name == kTypeNames[i]) {
      out = static_cast<ModType>(i);
      return true;
    }
  }
  return false;
}

bool ParseManifest(std::string_view text, Manifest &out, std::string &error) {
  toml::table table;
  try {
    table = toml::parse(text);
  } catch (const toml::parse_error &e) {
    error = std::format("line {}: {}", e.source().begin.line, e.description());
    return false;
  }
  Manifest m;
  m.name = StringAt(table, "name");
  m.creator = StringAt(table, "creator");
  m.version = StringAt(table, "version");
  m.description = StringAt(table, "description");
  if (m.name.empty()) {
    error = "no name";
    return false;
  }
  const std::string type = StringAt(table, "type");
  if (!TypeFromName(type, m.type)) {
    error = type.empty() ? "no type (package, replacement or model)"
                         : std::format("type {} is not package, replacement or model", Quoted(type));
    return false;
  }
  const toml::table *section = nullptr;
  if (const toml::node *node = table.get(TypeName(m.type)))
    section = node->as_table();
  if (!section) {
    error = std::format("no [{}] section", TypeName(m.type));
    return false;
  }
  m.file = StringAt(*section, "file");
  if (m.file.empty() || m.file.find_first_of("/\\") != std::string::npos) {
    error = std::format("[{}] file must name a file beside the manifest", TypeName(m.type));
    return false;
  }
  if (m.type == ModType::kPackage) {
    int64_t id = 0;
    if (const toml::node *node = section->get("id"))
      if (const auto *value = node->as_integer())
        id = value->get();
    if (id <= 0 || id >= 4096) {
      error = "[package] id must be a package id between 1 and 4095 (the port's own are 0x7EA to 0x7EE)";
      return false;
    }
    m.package_id = static_cast<uint32_t>(id);
    m.language = StringAt(*section, "language");
    m.language_name = StringAt(*section, "language_name");
    if (!m.language.empty() && m.language_name.empty())
      m.language_name = m.language;
  }
  out = std::move(m);
  return true;
}

std::string WriteManifest(const Manifest &manifest) {
  std::string out;
  out += "name = " + Quoted(manifest.name) + "\n";
  out += "creator = " + Quoted(manifest.creator) + "\n";
  if (!manifest.version.empty())
    out += "version = " + Quoted(manifest.version) + "\n";
  if (!manifest.description.empty())
    out += "description = " + Quoted(manifest.description) + "\n";
  out += std::format("type = \"{}\"\n\n[{}]\n", TypeName(manifest.type), TypeName(manifest.type));
  out += "file = " + Quoted(manifest.file) + "\n";
  if (manifest.type == ModType::kPackage) {
    out += std::format("id = {:#x}\n", manifest.package_id);
    if (!manifest.language.empty())
      out += "language = " + Quoted(manifest.language) + "\n";
    if (!manifest.language_name.empty())
      out += "language_name = " + Quoted(manifest.language_name) + "\n";
  }
  return out;
}

}
