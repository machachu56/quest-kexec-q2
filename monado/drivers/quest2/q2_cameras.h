// Copyright 2026, quest-kexec contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 2 tracking cameras: replay of the recorded camera start-up.
 * @ingroup drv_quest2
 */

#pragma once

#include "xrt/xrt_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define Q2_CAMERA_COUNT 4
#define Q2_CAMERA_W 640
#define Q2_CAMERA_H 480

struct q2_cameras;

//! Sends one SyncBoss packet (type, seq placeholder, len, payload).
typedef int (*q2_syncboss_send_fn)(void *ctx, const uint8_t *packet, uint32_t len);

/*!
 * Start the four tracking cameras by executing a camera script made by
 * quest-kexec tools/camtrace/mkscript.py from a recording of Android.
 *
 * @param script_path Path of the script.
 * @param sb_send     SyncBoss sender (the script also contains MCU commands).
 * @param sb_ctx      Context for @p sb_send.
 * @param sinks       One frame sink per camera (may be NULL): L8 640x480,
 *                    timestamp = start of frame, monotonic clock.
 */
struct q2_cameras *
q2_cameras_start(const char *script_path,
                 q2_syncboss_send_fn sb_send,
                 void *sb_ctx,
                 struct xrt_frame_sink *sinks[Q2_CAMERA_COUNT]);

//! Stop streaming, release the camera devices and free everything.
void
q2_cameras_stop(struct q2_cameras **cams_ptr);

#ifdef __cplusplus
}
#endif
