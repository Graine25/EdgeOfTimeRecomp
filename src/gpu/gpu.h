/**
 * @file    gpu/gpu.h
 * @brief   Public surface of the gpu module: everything outside src/gpu talks
 *          to the renderer through this header and nothing else.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include "gpu/device/device.h"
#include "gpu/device/host_heap_arena.h"
#include "gpu/device/settings.h"
#include "gpu/device/surface_pool.h"
#include "gpu/frame/frame_stats.h"
#include "gpu/frame/imgui_overlay_drawer.h"
#include "gpu/frame/output_resolution.h"
#include "gpu/frame/screenshot.h"
#include "gpu/pipeline/pso_recorder.h"
