/**
 * @file    gpu/hooks/stubs.cpp
 * @brief   Headless GPU ring/fence no-op stubs.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/hooks.h"

EOT_NOOP(eot_D3D_CDevice_BlockOnFence);
EOT_NOOP(eot_D3D_CDevice_BlockOnSecondaryPosition);
EOT_NOOP(eot_D3D_CDevice_WaitPrimaryRingSpace);
EOT_NOOP_RETURN(D3DDevice_IsFencePending, 0);
EOT_NOOP(D3DDevice_BlockUntilIdle);
