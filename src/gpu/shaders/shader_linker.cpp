#include "gpu/shaders/shader_linker.h"

#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/logging.h"
#include "gpu/shaders/dxc_link.h"

namespace eot::gpu {
namespace {

std::mutex g_mutex;

std::unordered_map<u32, std::vector<u8>> g_spec_lib_dxil;

const std::vector<u8> *SpecConstantLib(u32 value) {
  std::lock_guard lock(g_mutex);
  auto it = g_spec_lib_dxil.find(value);
  if (it != g_spec_lib_dxil.end())
    return it->second.empty() ? nullptr : &it->second;

  std::string error;
  auto inserted =
      g_spec_lib_dxil.emplace(value, CompileSpecConstantLib(value, &error));
  if (inserted.first->second.empty()) {
    EOT_ERROR("shader_linker: g_SpecConstants({}) library compile failed - is "
              "dxcompiler.dll next to the executable? {}",
              value, error);
    return nullptr;
  }
  return &inserted.first->second;
}

struct EntryPoint {
  const wchar_t *wide;
  const char *narrow;
};
constexpr EntryPoint kEntryPoints[] = {{L"shaderMain", "shaderMain"},
                                       {L"main", "main"}};
std::atomic<int> g_entry_point{-1};

}

std::vector<u8> LinkSpecConstant(const u8 *libraryDxil, u32 libraryDxilSize,
                                 bool isPixelShader, u32 specConstants) {
  const std::vector<u8> *specLib = SpecConstantLib(specConstants);
  if (!specLib)
    return {};

  const wchar_t *profile = isPixelShader ? L"ps_6_0" : L"vs_6_0";
  const int resolved = g_entry_point.load(std::memory_order_acquire);
  const int first = resolved >= 0 ? resolved : 0;
  const int last =
      resolved >= 0 ? resolved : static_cast<int>(std::size(kEntryPoints)) - 1;

  std::string error;
  for (int i = first; i <= last; ++i) {
    std::vector<u8> linked =
        LinkSpecConstantLib(libraryDxil, libraryDxilSize, specLib->data(),
                            specLib->size(), profile, kEntryPoints[i].wide,
                            &error);
    if (linked.empty())
      continue;
    if (resolved < 0) {
      g_entry_point.store(i, std::memory_order_release);
      EOT_INFO("shader_linker: shader cache exports entry point '{}'",
               kEntryPoints[i].narrow);
    }
    return linked;
  }
  EOT_ERROR("shader_linker: link failed ({}, specConstants=0x{:X}): {}",
            isPixelShader ? "ps_6_0" : "vs_6_0", specConstants, error);
  return {};
}

const char *LinkedEntryPointName() {
  const int resolved = g_entry_point.load(std::memory_order_acquire);
  return kEntryPoints[resolved >= 0 ? resolved : 0].narrow;
}

}
