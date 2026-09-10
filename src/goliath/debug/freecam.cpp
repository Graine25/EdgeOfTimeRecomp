#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/debug/freecam.h"

#if defined(_WIN32)
#include <windows.h>
#endif

REX_EXTERN(__imp__eot_GRMainCamera_ViewBegin);
REX_EXTERN(__imp__eot_Renderer_SetupCamera);
REX_EXTERN(__imp__eot_Renderer_Present);

REXCVAR_DEFINE_BOOL(eot_freecam, false, "EdgeOfTime/Debug",
                    "Fly the camera with WASD, E/Q and the arrow keys. Game input is blanked "
                    "while it is on. Pair it with eot_debug_pause to fly a frozen scene.");

REXCVAR_DEFINE_DOUBLE(eot_freecam_speed, 20.0, "EdgeOfTime/Debug",
                      "Free camera movement speed in world units a second. Shift multiplies it "
                      "by five, Ctrl divides it by five.");

REXCVAR_DEFINE_DOUBLE(eot_freecam_turn_speed, 120.0, "EdgeOfTime/Debug",
                      "Free camera turn speed in degrees a second, for the arrow keys.");

namespace {

constexpr uint32_t kCommandCameraOwner = 0x2E8;
constexpr uint32_t kActiveCameraRecord = 2124;
constexpr uint32_t kWorldMatrix = 24;

constexpr uint32_t kCameraList = 0x8249C1AC;
constexpr int kCameraListEntries = 16;
constexpr uint32_t kCameraFlags = 412;
constexpr uint32_t kFlagPublished = 1u << 28;
constexpr uint32_t kFlagSkip = 1u << 2;

constexpr double kPi = 3.14159265358979323846;
constexpr double kPitchLimit = kPi * 0.5 - 0.01;

struct Vec3 {
  double x = 0.0, y = 0.0, z = 0.0;
};

Vec3 g_pos;
double g_yaw = 0.0;
double g_pitch = 0.0;

bool g_active = false;
bool g_need_seed = false;

constexpr int kMaxCameras = 8;

constexpr uint64_t kFreshFrames = 1;

struct Adopted {
  uint32_t camera = 0;
  uint64_t last_seen = 0;
  float saved[16] = {};
};

Adopted g_cams[kMaxCameras];
int g_cam_count = 0;
uint64_t g_frame = 0;

std::mutex g_cams_mutex;

bool Fresh(const Adopted &a) { return g_frame - a.last_seen <= kFreshFrames; }

Adopted *FindCamera(uint32_t camera) {
  for (int i = 0; i < g_cam_count; ++i)
    if (g_cams[i].camera == camera)
      return &g_cams[i];
  return nullptr;
}

void ExpireCameras() {
  int keep = 0;
  for (int i = 0; i < g_cam_count; ++i)
    if (Fresh(g_cams[i]))
      g_cams[keep++] = g_cams[i];
  g_cam_count = keep;
}

bool KeyDown(int vk) {
#if defined(_WIN32)
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
  (void)vk;
  return false;
#endif
}

bool WindowHasFocus() {
#if defined(_WIN32)
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
#else
  return false;
#endif
}

double Axis(int positive, int negative) {
  return (KeyDown(positive) ? 1.0 : 0.0) - (KeyDown(negative) ? 1.0 : 0.0);
}

double HostDelta() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  const clock::time_point now = clock::now();
  if (last.time_since_epoch().count() == 0) {
    last = now;
    return 0.0;
  }
  const double dt = std::chrono::duration<double>(now - last).count();
  last = now;
  return std::clamp(dt, 0.0, 0.1);
}

void Basis(Vec3 &right, Vec3 &up, Vec3 &forward) {
  const double cp = std::cos(g_pitch), sp = std::sin(g_pitch);
  const double cy = std::cos(g_yaw), sy = std::sin(g_yaw);
  forward = {sy * cp, sp, cy * cp};
  right = {cy, 0.0, -sy};
  up = {forward.y * right.z - forward.z * right.y, forward.z * right.x - forward.x * right.z,
        forward.x * right.y - forward.y * right.x};
}

void WriteWorld(uint32_t camera) {
  auto *m = camera ? eot::mem::at<eot::be<float>>(camera + kWorldMatrix) : nullptr;
  if (!m)
    return;
  Vec3 right, up, forward;
  Basis(right, up, forward);
  const float rows[16] = {
      static_cast<float>(right.x),   static_cast<float>(right.y),
      static_cast<float>(right.z),   0.0f,
      static_cast<float>(up.x),      static_cast<float>(up.y),
      static_cast<float>(up.z),      0.0f,
      static_cast<float>(forward.x), static_cast<float>(forward.y),
      static_cast<float>(forward.z), 0.0f,
      static_cast<float>(g_pos.x),   static_cast<float>(g_pos.y),
      static_cast<float>(g_pos.z),   1.0f,
  };
  for (int i = 0; i < 16; ++i)
    m[i] = rows[i];
}

}

namespace eot::debug {

bool FreecamActive() { return g_active; }

void FreecamTick() {
  const bool want = REXCVAR_GET(eot_freecam);

  {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    ++g_frame;
    ExpireCameras();
  }

  if (want != g_active) {
    g_active = want;
    g_need_seed = want;
    return;
  }
  if (!g_active || g_need_seed || !WindowHasFocus())
    return;

  const double dt = HostDelta();
  if (dt <= 0.0)
    return;

  double speed = REXCVAR_GET(eot_freecam_speed);
  if (KeyDown(VK_SHIFT))
    speed *= 5.0;
  if (KeyDown(VK_CONTROL))
    speed /= 5.0;

  const double turn = REXCVAR_GET(eot_freecam_turn_speed) * (kPi / 180.0) * dt;
  g_yaw += Axis(VK_RIGHT, VK_LEFT) * turn;
  g_pitch = std::clamp(g_pitch + Axis(VK_UP, VK_DOWN) * turn, -kPitchLimit, kPitchLimit);

  Vec3 right, up, forward;
  Basis(right, up, forward);

  const double move = speed * dt;
  const double f = Axis('W', 'S') * move;
  const double s = Axis('D', 'A') * move;
  const double u = Axis('E', 'Q') * move;

  g_pos.x += forward.x * f + right.x * s;
  g_pos.y += forward.y * f + right.y * s + u;
  g_pos.z += forward.z * f + right.z * s;
}

}

REX_HOOK_RAW(eot_Renderer_Present) {
  if (g_active) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    for (int i = 0; i < kCameraListEntries; ++i) {
      const uint32_t cam = eot::mem::load<uint32_t>(kCameraList + i * 4);
      if (!cam)
        continue;
      const uint32_t flags = eot::mem::load<uint32_t>(cam + kCameraFlags);
      if (!(flags & kFlagPublished) || (flags & kFlagSkip))
        continue;
      const Adopted *known = FindCamera(cam);
      if (known && Fresh(*known))
        WriteWorld(cam);
    }
  }
  __imp__eot_Renderer_Present(ctx, base);
}

REX_HOOK_RAW(eot_Renderer_SetupCamera) {
  if (g_active) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    for (int i = 0; i < g_cam_count; ++i)
      if (Fresh(g_cams[i]))
        WriteWorld(g_cams[i].camera);
  }
  __imp__eot_Renderer_SetupCamera(ctx, base);
}

REX_HOOK_RAW(eot_GRMainCamera_ViewBegin) {
  const uint32_t owner = eot::mem::load<uint32_t>(ctx.r4.u32 + kCommandCameraOwner);
  const uint32_t camera =
      owner ? owner : eot::mem::load<uint32_t>(ctx.r3.u32 + kActiveCameraRecord);

  if (g_active && camera) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    Adopted *known = FindCamera(camera);
    if (known) {
      known->last_seen = g_frame;
    } else if (g_cam_count < kMaxCameras) {
      if (auto *seed = eot::mem::at<eot::be<float>>(camera + kWorldMatrix)) {
        Adopted &slot = g_cams[g_cam_count++];
        slot.camera = camera;
        slot.last_seen = g_frame;
        for (int i = 0; i < 16; ++i)
          slot.saved[i] = seed[i];

        if (g_need_seed) {
          g_need_seed = false;
          g_pos = {slot.saved[12], slot.saved[13], slot.saved[14]};
          const double fx = slot.saved[8], fy = slot.saved[9], fz = slot.saved[10];
          const double len = std::sqrt(fx * fx + fy * fy + fz * fz);
          if (len > 1e-6) {
            g_pitch =
                std::clamp(std::asin(std::clamp(fy / len, -1.0, 1.0)), -kPitchLimit, kPitchLimit);
            g_yaw = std::atan2(fx / len, fz / len);
          }
        }
        EOT_INFO("[freecam] camera {} is 0x{:08X}, at ({:.1f}, {:.1f}, {:.1f})", g_cam_count,
                 camera, slot.saved[12], slot.saved[13], slot.saved[14]);
      }
    }

    WriteWorld(camera);
  } else if (g_cam_count > 0) {
    std::lock_guard<std::mutex> lock(g_cams_mutex);
    int restored = 0;
    for (int i = 0; i < g_cam_count; ++i) {
      if (!Fresh(g_cams[i]))
        continue;
      if (auto *m = eot::mem::at<eot::be<float>>(g_cams[i].camera + kWorldMatrix)) {
        for (int j = 0; j < 16; ++j)
          m[j] = g_cams[i].saved[j];
        ++restored;
      }
    }
    EOT_INFO("[freecam] released {} of {} camera(s)", restored, g_cam_count);
    g_cam_count = 0;
  }

  __imp__eot_GRMainCamera_ViewBegin(ctx, base);
}
