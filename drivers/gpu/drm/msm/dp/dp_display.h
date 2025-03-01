/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _DP_DISPLAY_H_
#define _DP_DISPLAY_H_

#include <drm/drmP.h>
#include <drm/msm_drm.h>

#include "dp_panel.h"
#if defined(CONFIG_LGE_DUAL_SCREEN)
#include <linux/extcon.h>
#define EXT_DD_MAX_COUNT 3
#endif
#ifdef CONFIG_LGE_DISPLAY_COMMON
#include "../lge/dp/lge_dp_def.h"
#endif

struct dp_display {
	struct drm_device *drm_dev;
	struct dp_bridge *bridge;
	struct drm_connector *connector;
	bool is_connected;
	bool is_primary;
	u32 max_pclk_khz;
	bool yuv_support;

	int (*enable)(struct dp_display *dp_display);
	int (*post_enable)(struct dp_display *dp_display);

	int (*pre_disable)(struct dp_display *dp_display);
	int (*disable)(struct dp_display *dp_display);

	int (*set_mode)(struct dp_display *dp_display,
			struct dp_display_mode *mode);
	int (*validate_mode)(struct dp_display *dp_display,
			u32 mode_pclk_khz, u32 flags);
	int (*get_modes)(struct dp_display *dp_display,
		struct dp_display_mode *dp_mode);
	int (*get_dc_support)(struct dp_display *dp_display,
		u32 mode_pclk_khz, u32 out_format, bool dc_enable);
	int (*prepare)(struct dp_display *dp_display);
	int (*unprepare)(struct dp_display *dp_display);
	int (*request_irq)(struct dp_display *dp_display);
	struct dp_debug *(*get_debug)(struct dp_display *dp_display);
	void (*post_open)(struct dp_display *dp_display);
	int (*config_hdr)(struct dp_display *dp_display,
				struct drm_msm_ext_hdr_metadata *hdr_meta);
	void (*post_init)(struct dp_display *dp_display);
	int (*get_display_type)(struct dp_display *dp_display,
			const char **display_type);
	bool (*vsc_sdp_supported)(struct dp_display *dp_display);
#ifdef CONFIG_LGE_DISPLAY_COMMON
	struct lge_dp_display lge_dp;
#endif
};

#if defined(CONFIG_LGE_DUAL_SCREEN)
int is_dd_connected(void);
#endif
int dp_display_get_num_of_displays(void);
int dp_display_get_displays(void **displays, int count);
bool dp_connector_mode_needs_full_range(void *display);
bool dp_connector_mode_is_cea_mode(void *display);
enum sde_csc_type dp_connector_get_csc_type(struct drm_connector *conn,
	void *data);
#endif /* _DP_DISPLAY_H_ */
