#pragma once

namespace eot::controller {

enum class PadBrand {
  Unknown,
  Xbox360,
  XboxSeries,
  PlayStation,
  Switch,
  SteamDeck,
  Keyboard,
};

const char *ToString(PadBrand brand);

PadBrand ActivePad();

}
