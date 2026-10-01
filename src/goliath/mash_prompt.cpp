#include "goliath/mash_prompt.h"

#include <atomic>
#include <bit>

#include "core/memory_helpers.h"

namespace {

constexpr uint32_t kRenderParams = 340;
constexpr uint32_t kGeometry = 48;
constexpr uint32_t kGeometryCrc = 4;
constexpr uint32_t kRecordWorld = 92;
constexpr float kWidth = 3.0f;
constexpr float kHeight = 0.75f;

std::atomic<uint32_t> g_models[2] = {0, 0};

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

void ScaleRow(uint32_t world, uint32_t row, float s) {
  for (uint32_t k = 0; k < 3; ++k) {
    const uint32_t at = world + (row * 4 + k) * 4;
    StoreF(at, LoadF(at) * s);
  }
}

}

namespace eot::goliath {

void SetWidePromptModels(uint32_t first, uint32_t second) {
  g_models[0].store(first, std::memory_order_relaxed);
  g_models[1].store(second, std::memory_order_relaxed);
}

void MashPromptDrawRecord(uint32_t object, uint32_t record) {
  const uint32_t first = g_models[0].load(std::memory_order_relaxed);
  const uint32_t second = g_models[1].load(std::memory_order_relaxed);
  if ((!first && !second) || !object || !record)
    return;
  const uint32_t params = eot::mem::load<uint32_t>(object + kRenderParams);
  const uint32_t geometry = params ? eot::mem::load<uint32_t>(params + kGeometry) : 0;
  const uint32_t crc = geometry ? eot::mem::load<uint32_t>(geometry + kGeometryCrc) : 0;
  if (!crc || (crc != first && crc != second))
    return;
  ScaleRow(record + kRecordWorld, 0, kWidth);
  ScaleRow(record + kRecordWorld, 1, kHeight);
}

}
