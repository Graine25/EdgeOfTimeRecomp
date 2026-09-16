#include "goliath/controller/pad_identity.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include "goliath/controller/native_input.h"

namespace eot::controller {

namespace {

constexpr uint16_t kMicrosoft = 0x045E;
constexpr uint16_t kSony = 0x054C;
constexpr uint16_t kNintendo = 0x057E;
constexpr uint16_t kValve = 0x28DE;
constexpr uint16_t kSteamDeck = 0x1205;

uint16_t GuidWord(std::string_view guid, size_t byte) {
  if (guid.size() < (byte + 2) * 2)
    return 0;
  const std::string lo(guid.substr(byte * 2, 2));
  const std::string hi(guid.substr(byte * 2 + 2, 2));
  return static_cast<uint16_t>(std::strtoul(lo.c_str(), nullptr, 16) |
                               (std::strtoul(hi.c_str(), nullptr, 16) << 8));
}

bool Has(std::string_view name, std::string_view word) { return name.find(word) != std::string_view::npos; }

PadBrand BrandOf(const rex::input::DeviceInfo &info) {
  if (info.synthetic)
    return PadBrand::Keyboard;
  const uint16_t vendor = GuidWord(info.guid, 4);
  const uint16_t product = GuidWord(info.guid, 8);
  const std::string_view name = info.name;
  if (vendor == kValve)
    return product == kSteamDeck ? PadBrand::SteamDeck : PadBrand::Unknown;
  if (vendor == kSony || Has(name, "PS4") || Has(name, "PS5") || Has(name, "DualSense") || Has(name, "DualShock"))
    return PadBrand::PlayStation;
  if (vendor == kNintendo || Has(name, "Switch") || Has(name, "Joy-Con"))
    return PadBrand::Switch;
  if (Has(name, "Steam Deck"))
    return PadBrand::SteamDeck;
  if (vendor == kMicrosoft || Has(name, "Xbox"))
    return Has(name, "360") ? PadBrand::Xbox360 : PadBrand::XboxSeries;
  return PadBrand::Unknown;
}

}

const char *ToString(PadBrand brand) {
  switch (brand) {
  case PadBrand::Xbox360:
    return "xbox";
  case PadBrand::XboxSeries:
    return "xboxseries";
  case PadBrand::PlayStation:
    return "playstation";
  case PadBrand::Switch:
    return "switch";
  case PadBrand::SteamDeck:
    return "steamdeck";
  case PadBrand::Keyboard:
    return "keyboard";
  default:
    return "unknown";
  }
}

PadBrand ActivePad() {
  if (NativeInputActive()) {
    const uint64_t host = LastHostInputPoll();
    if (host && host >= LastPadInputPoll())
      return PadBrand::Keyboard;
  }
  rex::Runtime *runtime = rex::Runtime::instance();
  if (!runtime || !runtime->input_system())
    return PadBrand::Unknown;
  auto *input = static_cast<rex::input::InputSystem *>(runtime->input_system());
  rex::input::DeviceInfo info;
  if (!input->ActiveDevice(0, &info))
    return PadBrand::Unknown;
  return BrandOf(info);
}

}
