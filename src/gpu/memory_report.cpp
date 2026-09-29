#include "gpu/memory_report.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <plume_render_interface.h>
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <psapi.h>
#endif

#include "core/logging.h"
#include "gpu/device.h"

namespace eot::gpu {
namespace {

constexpr auto kInterval = std::chrono::seconds(30);
constexpr uint64_t kMB = 1024ull * 1024ull;

#if defined(EOT_D3D12)
void Tag(D3D12MA::Allocation *allocation, const std::string &tag) {
  if (!allocation || tag.empty())
    return;
  std::wstring wide;
  for (char c : tag)
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  allocation->SetName(wide.c_str());
}

const char *FormatName(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R32G32B32A32_FLOAT: return "RGBA32F";
  case plume::RenderFormat::R16G16B16A16_FLOAT: return "RGBA16F";
  case plume::RenderFormat::R16G16B16A16_UNORM: return "RGBA16";
  case plume::RenderFormat::R32G32_FLOAT: return "RG32F";
  case plume::RenderFormat::R8G8B8A8_UNORM: return "RGBA8";
  case plume::RenderFormat::B8G8R8A8_UNORM: return "BGRA8";
  case plume::RenderFormat::R16G16_FLOAT: return "RG16F";
  case plume::RenderFormat::R16G16_UNORM: return "RG16";
  case plume::RenderFormat::D32_FLOAT: return "D32";
  case plume::RenderFormat::D32_FLOAT_S8_UINT: return "D32S8";
  case plume::RenderFormat::R32_FLOAT: return "R32F";
  case plume::RenderFormat::R32_UINT: return "R32U";
  case plume::RenderFormat::R8G8_UNORM: return "RG8";
  case plume::RenderFormat::R16_FLOAT: return "R16F";
  case plume::RenderFormat::R16_UNORM: return "R16";
  case plume::RenderFormat::R8_UNORM: return "R8";
  case plume::RenderFormat::BC1_UNORM: return "BC1";
  case plume::RenderFormat::BC2_UNORM: return "BC2";
  case plume::RenderFormat::BC3_UNORM: return "BC3";
  case plume::RenderFormat::BC4_UNORM: return "BC4";
  case plume::RenderFormat::BC5_UNORM: return "BC5";
  default: return nullptr;
  }
}

std::string Shape(const plume::RenderTextureDesc &desc) {
  std::string shape = std::format("{}x{}", desc.width, desc.height);
  if (desc.depth > 1 || desc.arraySize > 1)
    shape += std::format("x{}", std::max<uint32_t>(desc.depth, desc.arraySize));
  const uint32_t samples = static_cast<uint32_t>(desc.multisampling.sampleCount);
  if (samples > 1)
    shape += std::format(" {}x", samples);
  if (desc.mipLevels > 1)
    shape += std::format(" m{}", desc.mipLevels);
  const char *format = FormatName(desc.format);
  shape += format ? std::format(" {}", format) : std::format(" f{}", static_cast<uint32_t>(desc.format));
  return shape;
}

struct Allocation {
  std::wstring heap;
  std::wstring type;
  std::wstring name;
  uint64_t size = 0;
  uint32_t usage = 0;
};

const wchar_t *ReadValue(const wchar_t *p, std::wstring &text, uint64_t &number) {
  while (*p == L' ' || *p == L':' || *p == L'\n' || *p == L'\r' || *p == L'\t')
    ++p;
  text.clear();
  number = 0;
  if (*p == L'"') {
    ++p;
    while (*p && *p != L'"') {
      if (*p == L'\\' && p[1])
        ++p;
      text.push_back(*p++);
    }
    return *p ? p + 1 : p;
  }
  while (*p >= L'0' && *p <= L'9')
    number = number * 10 + uint64_t(*p++ - L'0');
  return p;
}

std::vector<Allocation> ParseAllocations(const wchar_t *json) {
  static const wchar_t *const kHeaps[] = {L"\"DEFAULT\"", L"\"UPLOAD\"", L"\"READBACK\"", L"\"GPU_UPLOAD\"",
                                          L"\"CUSTOM\""};
  std::vector<Allocation> out;
  std::wstring heap = L"?";
  for (const wchar_t *p = json; *p; ++p) {
    if (*p != L'"')
      continue;
    for (const wchar_t *h : kHeaps) {
      const size_t n = std::wcslen(h);
      if (std::wcsncmp(p, h, n) == 0 && p[n] == L':') {
        heap.assign(h + 1, n - 2);
        break;
      }
    }
    if (std::wcsncmp(p, L"\"Type\"", 6) != 0)
      continue;
    Allocation a;
    a.heap = heap;
    const wchar_t *q = p;
    std::wstring key, text;
    uint64_t number = 0;
    while (*q && *q != L'}') {
      if (*q != L'"') {
        ++q;
        continue;
      }
      q = ReadValue(q, key, number);
      q = ReadValue(q, text, number);
      if (key == L"Type")
        a.type = text;
      else if (key == L"Size")
        a.size = number;
      else if (key == L"Usage")
        a.usage = static_cast<uint32_t>(number);
      else if (key == L"Name")
        a.name = text;
    }
    out.push_back(std::move(a));
    p = *q ? q : q - 1;
  }
  return out;
}

std::string Narrow(const std::wstring &w) {
  std::string s;
  for (wchar_t c : w)
    s.push_back(c < 128 ? static_cast<char>(c) : '?');
  return s;
}

void Report() {
  VideoState &s = state();
  auto *device = static_cast<plume::D3D12Device *>(s.device.get());
  if (!device || !device->allocator)
    return;

  DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
  IDXGIAdapter3 *adapter3 = nullptr;
  if (device->adapter && SUCCEEDED(device->adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
    adapter3->Release();
  }

  D3D12MA::TotalStatistics totals{};
  device->allocator->CalculateStatistics(&totals);
  WCHAR *json = nullptr;
  device->allocator->BuildStatsString(&json, TRUE);
  std::vector<Allocation> allocations = json ? ParseAllocations(json) : std::vector<Allocation>{};
  if (json)
    device->allocator->FreeStatsString(json);

  const auto &dflt = totals.HeapType[0].Stats; // D3D12_HEAP_TYPE_DEFAULT
  EOT_INFO("[vram] process {} MB of a {} MB budget in video memory, {} MB in shared memory | allocator: {} MB in "
           "{} allocations in video memory ({} MB of blocks), {} MB in all heaps",
           local.CurrentUsage / kMB, local.Budget / kMB, shared.CurrentUsage / kMB, dflt.AllocationBytes / kMB,
           dflt.AllocationCount, dflt.BlockBytes / kMB, totals.Total.Stats.AllocationBytes / kMB);

  struct Sum {
    uint64_t bytes = 0;
    uint32_t count = 0;
  };
  std::map<std::string, Sum> by_tag;
  std::map<std::string, Sum> by_shape;
  for (const Allocation &a : allocations) {
    const std::string name = a.name.empty() ? "(" + Narrow(a.type) + ")" : Narrow(a.name);
    Sum &sum = by_tag[Narrow(a.heap) + ":" + name.substr(0, name.find(' '))];
    sum.bytes += a.size;
    ++sum.count;
    if (a.heap == L"DEFAULT" && a.type != L"FREE" && a.type != L"BUFFER") {
      Sum &shape = by_shape[name];
      shape.bytes += a.size;
      ++shape.count;
    }
  }
  std::vector<std::pair<std::string, Sum>> sorted(by_tag.begin(), by_tag.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto &x, const auto &y) { return x.second.bytes > y.second.bytes; });
  std::string line;
  for (size_t i = 0; i < sorted.size() && i < 16; ++i) {
    if (!line.empty())
      line += ", ";
    line += std::format("{} {} MB ({})", sorted[i].first, sorted[i].second.bytes / kMB, sorted[i].second.count);
  }
  EOT_INFO("[vram-by-tag] {}", line);

  std::vector<std::pair<std::string, Sum>> shapes(by_shape.begin(), by_shape.end());
  std::sort(shapes.begin(), shapes.end(), [](const auto &x, const auto &y) { return x.second.bytes > y.second.bytes; });
  line.clear();
  for (size_t i = 0; i < shapes.size() && i < 40; ++i) {
    if (!line.empty())
      line += ", ";
    line += std::format("{} x{} = {} MB", shapes[i].first, shapes[i].second.count, shapes[i].second.bytes / kMB);
  }
  EOT_INFO("[vram-shapes] {}", line);

  allocations.erase(std::remove_if(allocations.begin(), allocations.end(),
                                   [](const Allocation &a) { return a.type == L"FREE"; }),
                    allocations.end());
  std::sort(allocations.begin(), allocations.end(), [](const auto &x, const auto &y) { return x.size > y.size; });
  line.clear();
  for (size_t i = 0; i < allocations.size() && i < 12; ++i) {
    if (!line.empty())
      line += ", ";
    const Allocation &a = allocations[i];
    line += std::format("{} {} MB", a.name.empty() ? Narrow(a.type) : Narrow(a.name), a.size / kMB);
  }
  EOT_INFO("[vram-largest] {}", line);

  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                              sizeof(counters)))
    EOT_INFO("[ram] {} MB private, {} MB working set, {} MB peak working set", counters.PrivateUsage / kMB,
             counters.WorkingSetSize / kMB, counters.PeakWorkingSetSize / kMB);
}
#endif

std::chrono::steady_clock::time_point g_next{};

}

void TagHostAllocation(plume::RenderTexture *texture, const char *tag, const plume::RenderTextureDesc &desc) {
#if defined(EOT_D3D12)
  if (texture && tag)
    Tag(static_cast<plume::D3D12Texture *>(texture)->allocation, std::string(tag) + " " + Shape(desc));
#else
  (void)texture;
  (void)tag;
  (void)desc;
#endif
}

void TagHostAllocation(plume::RenderBuffer *buffer, const char *tag) {
#if defined(EOT_D3D12)
  if (buffer && tag)
    Tag(static_cast<plume::D3D12Buffer *>(buffer)->allocation, tag);
#else
  (void)buffer;
  (void)tag;
#endif
}

void MemoryReportTick() {
  const auto now = std::chrono::steady_clock::now();
  if (now < g_next)
    return;
  const bool first = g_next.time_since_epoch().count() == 0;
  g_next = now + kInterval;
  if (first)
    return;
#if defined(EOT_D3D12)
  Report();
#endif
}

}
