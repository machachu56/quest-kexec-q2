// Copyright 2026, quest-kexec contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Builder for the Meta Quest 2 running Linux (quest-kexec).
 * @ingroup xrt_iface
 */

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_tracking.h"

#include "util/u_misc.h"

#include "target_builder_helpers.h"
#include "target_builder_interface.h"

#include "quest2/q2_interface.h"

#ifndef XRT_BUILD_DRIVER_QUEST2
#error "Must only be built with XRT_BUILD_DRIVER_QUEST2 set"
#endif

static const char *driver_list[] = {
    "quest2",
};

static xrt_result_t
q2_estimate_system(struct xrt_builder *xb, cJSON *config, struct xrt_prober *xp, struct xrt_builder_estimate *estimate)
{
	if (q2_present()) {
		estimate->certain.head = true;
		estimate->certain.left = true;
		estimate->certain.right = true;
		estimate->priority = 50;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
q2_open_system_impl(struct xrt_builder *xb,
                    cJSON *config,
                    struct xrt_prober *xp,
                    struct xrt_tracking_origin *origin,
                    struct xrt_system_devices *xsysd,
                    struct xrt_frame_context *xfctx,
                    struct t_builder_options *tbo)
{
	struct xrt_device *head = NULL, *left = NULL, *right = NULL;

	if (q2_create_devices(&head, &left, &right) != 0) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	head->tracking_origin = origin;
	left->tracking_origin = origin;
	right->tracking_origin = origin;
	origin->type = XRT_TRACKING_TYPE_OTHER;
	// The origin comes zero-filled: identity, not a zero quaternion.
	origin->initial_offset = (struct xrt_pose)XRT_POSE_IDENTITY;

	xsysd->static_xdevs[xsysd->static_xdev_count++] = head;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = left;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = right;

	tbo->head = head;
	tbo->left = left;
	tbo->right = right;
	return XRT_SUCCESS;
}

static void
q2_destroy(struct xrt_builder *xb)
{
	free(xb);
}

struct xrt_builder *
t_builder_quest2_create(void)
{
	struct t_builder *ub = U_TYPED_CALLOC(struct t_builder);

	ub->base.estimate_system = q2_estimate_system;
	ub->base.open_system = t_builder_open_system_static_roles;
	ub->base.destroy = q2_destroy;
	ub->base.identifier = "quest2";
	ub->base.name = "Meta Quest 2 (quest-kexec)";
	ub->base.driver_identifiers = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);
	ub->open_system_static_roles = q2_open_system_impl;
	return &ub->base;
}
