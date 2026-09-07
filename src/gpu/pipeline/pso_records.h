#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rex/types.h>

#include "gpu/d3d.h"
#include "gpu/pipeline/pipeline_cache.h"

namespace eot::gpu {

constexpr u32 kPsoCsvVersion = 2;

struct PsoRecord {
  PipelineState state;
  u32 declCount = 0;
  u8 declRaw[32 * sizeof(DeclElement)] = {};
  u64 frame = 0;
};

std::string PsoCsvHeader();
std::string PsoRecordToCsv(const PsoRecord &r, std::string_view session);
bool PsoRecordFromCsv(std::string_view line, PsoRecord *out);

const std::vector<PsoRecord> &CompiledInPipelines();

struct PsoTemplate {
  u8 technique = 0, pass = 0;
  u32 materialClass = 0;
  PipelineState state;
};
const std::vector<PsoTemplate> &CompiledInTemplates();

std::string PsoSessionStamp();
size_t LoadPsoCsvDir(const std::string &dir, std::vector<PsoRecord> &out);

void PsoCaptureConfigure(const std::string &dir, const std::string &tag);
void PsoCaptureAdd(const PsoRecord &r);
void PsoCaptureFlush(bool force, u64 guest_frame);

}
