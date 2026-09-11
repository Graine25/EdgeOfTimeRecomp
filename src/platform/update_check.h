#pragma once

#include <optional>
#include <string>

namespace eot::platform {

struct AvailableUpdate {
  std::string stamp;
  std::string version;
  std::string location;
};

void BeginUpdateCheck();

std::optional<AvailableUpdate> NewerBuildAvailable();

}
