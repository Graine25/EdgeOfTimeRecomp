/**
 * @file    gpu/gpu.h
 * @brief   Public surface of the gpu module: everything outside src/gpu talks
 *          to the renderer through this header and nothing else.
 *
 * Narrow slice: device creation + resource creation only. Present, draw,
 * pipelines, and the rest of re:Blue's gpu/ surface aren't ported yet - add
 * their headers here as they land.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include "gpu/device/device.h"
