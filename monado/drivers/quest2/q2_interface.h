// Copyright 2026, quest-kexec contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Meta Quest 2 (running Linux through quest-kexec) driver interface.
 * @ingroup drv_quest2
 */

#pragma once

#include "xrt/xrt_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_quest2 Quest 2 driver
 * @ingroup drv
 *
 * Headset IMU and Touch controllers through the SyncBoss MCU
 * (/dev/syncboss0, Meta's in-kernel driver).
 */

//! True if the SyncBoss device nodes exist.
bool
q2_present(void);

/*!
 * Create the headset and both controllers.
 *
 * @param[out] out_hmd   Head device.
 * @param[out] out_left  Left Touch controller.
 * @param[out] out_right Right Touch controller.
 */
int
q2_create_devices(struct xrt_device **out_hmd, struct xrt_device **out_left, struct xrt_device **out_right);

#ifdef __cplusplus
}
#endif
