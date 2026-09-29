#include "gpu/adapter_sensors.h"

#include <format>
#include <mutex>

#include <plume_render_interface.h>
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#endif

#include "core/logging.h"
#include "gpu/device.h"

namespace eot::gpu {

#if defined(EOT_D3D12)
namespace {

using OpenAdapterFromLuidFn = NTSTATUS(APIENTRY *)(D3DKMT_OPENADAPTERFROMLUID *);
using QueryAdapterInfoFn = NTSTATUS(APIENTRY *)(const D3DKMT_QUERYADAPTERINFO *);

struct Sensors {
  std::once_flag open;
  QueryAdapterInfoFn query = nullptr;
  D3DKMT_HANDLE adapter = 0;
  bool reported_failure = false;
};
Sensors g_sensors;

void Open() {
  VideoState &s = state();
  auto *device = static_cast<plume::D3D12Device *>(s.device.get());
  if (!device || !device->d3d)
    return;
  HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  if (!gdi)
    gdi = LoadLibraryW(L"gdi32.dll");
  if (!gdi)
    return;
  auto open = reinterpret_cast<OpenAdapterFromLuidFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
  auto query = reinterpret_cast<QueryAdapterInfoFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
  if (!open || !query)
    return;
  D3DKMT_OPENADAPTERFROMLUID request{};
  request.AdapterLuid = device->d3d->GetAdapterLuid();
  if (open(&request) != 0) {
    EOT_INFO("[gpu-sensors] the kernel would not open the adapter; no clock or temperature readings");
    return;
  }
  g_sensors.adapter = request.hAdapter;
  g_sensors.query = query;
}

template <typename T> bool Query(KMTQUERYADAPTERINFOTYPE type, T &data, NTSTATUS &status) {
  D3DKMT_QUERYADAPTERINFO info{};
  info.hAdapter = g_sensors.adapter;
  info.Type = type;
  info.pPrivateDriverData = &data;
  info.PrivateDriverDataSize = sizeof(data);
  status = g_sensors.query(&info);
  return status == 0;
}

}

std::string AdapterSensorsSummary() {
  std::call_once(g_sensors.open, Open);
  if (!g_sensors.query)
    return {};
  D3DKMT_NODE_PERFDATA node{};
  D3DKMT_ADAPTER_PERFDATA adapter{};
  NTSTATUS node_status = 0, adapter_status = 0;
  const bool have_node = Query(KMTQAITYPE_NODEPERFDATA, node, node_status);
  const bool have_adapter = Query(KMTQAITYPE_ADAPTERPERFDATA, adapter, adapter_status);
  if (!have_node && !have_adapter) {
    if (!g_sensors.reported_failure) {
      g_sensors.reported_failure = true;
      EOT_INFO("[gpu-sensors] the driver reports no performance data (status {:#x} / {:#x})",
               static_cast<u32>(node_status), static_cast<u32>(adapter_status));
    }
    return {};
  }
  auto mhz = [](u64 f) { return f >= 1000000 ? f / 1000000 : f; };
  std::string out;
  auto add = [&](const std::string &field) { out += (out.empty() ? " | " : ", ") + field; };
  if (have_node && node.Frequency)
    add(std::format("clock {}/{} MHz", mhz(node.Frequency),
                    mhz(node.MaxFrequency ? node.MaxFrequency : node.MaxFrequencyOC)));
  if (have_node && node.Voltage)
    add(std::format("{} mV", node.Voltage));
  if (have_adapter && adapter.MemoryFrequency)
    add(std::format("memory {} MHz", mhz(adapter.MemoryFrequency)));
  if (have_adapter && adapter.Temperature)
    add(std::format("{:.1f} C", adapter.Temperature / 10.0));
  if (have_adapter && adapter.Power)
    add(std::format("power {:.1f}%", adapter.Power / 10.0));
  return out;
}
#else
std::string AdapterSensorsSummary() { return {}; }
#endif

}
