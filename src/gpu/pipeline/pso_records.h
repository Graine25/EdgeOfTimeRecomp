#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rex/types.h>

#include "gpu/d3d.h"
#include "gpu/pipeline/pipeline_cache.h"

namespace eot::gpu {

constexpr u32 kPsoCsvVersion = 5;

constexpr const char *kPsoDir = "pso";
constexpr u32 kPsoHoldMaxMs = 8000;
constexpr u32 kPsoEvictAfterMs = 60000;
constexpr u32 kPsoMaxThreads = 8;
constexpr u32 kPsoMinThreads = 1;

constexpr u32 kMaxRecordPackages = 8;

struct PsoRecord {
  PipelineState state;
  u32 declCount = 0;
  u8 declRaw[32 * sizeof(DeclElement)] = {};
  u8 msaa = 0;
  u64 frame = 0;
  u16 packages[kMaxRecordPackages] = {};
  u32 packageCount = 0;
};

constexpr u8 kPsoMsaaSingle = 1;
constexpr u8 kPsoMsaaMulti = 2;

struct PsoCsvLayout {
  static constexpr u32 kColumns = 36;
  i8 index[kColumns];
  u32 fieldCount = 0;
  u32 version = kPsoCsvVersion;
};

std::string PsoCsvHeader();
std::string PsoRecordToCsv(const PsoRecord &r, std::string_view session);
bool PsoCsvParseHeader(std::string_view line, PsoCsvLayout *out);
bool PsoRecordFromCsv(const PsoCsvLayout &layout, std::string_view line, PsoRecord *out);

void PsoApplyTargetScale(PipelineState &s);

std::string PsoSessionStamp();
std::string PsoSessionTag();
size_t LoadPsoCsvDir(const std::string &dir, std::vector<PsoRecord> &out);

void PsoCaptureConfigure();
void PsoCaptureAdd(const PsoRecord &r);
void PsoCaptureFlush(bool force, u64 guest_frame);
void PsoWriteSessionFile(const std::string &name, const std::string &header,
                         const std::vector<std::string> &rows);

}
