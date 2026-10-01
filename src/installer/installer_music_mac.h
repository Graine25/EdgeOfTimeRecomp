#pragma once

#include <cstddef>
#include <string>

namespace eot::installer::mac {

void *StartPlayer(const void *data, size_t size, std::string &error);

void SetPlayerVolume(void *player, float volume);

void StopPlayer(void *player);

}
