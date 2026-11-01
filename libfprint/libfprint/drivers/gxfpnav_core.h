/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Core of the gxfpnav driver: fingerprint images through cleartext NAV commands, with
 * no TLS and no PSK. With register 0x62 = 0x04LL the MilanL sensor reads 24 lines, every
 * 4th line starting at line LL. That register is part of the 0x90 config, and combining
 * 8 values yields lines 8..111. This file does not depend on libfprint.
 */
#pragma once

#include "gxfp/io/dev.h"

#include <stddef.h>
#include <stdint.h>

#define GXFPNAV_IMG_W 128
#define GXFPNAV_IMG_H 104
#define GXFPNAV_CFG_LEN 0x100u
#define GXFPNAV_NUM_VALS 8

struct gxfpnav_core {
	struct gxfp_dev dev;
	int dev_open;
	uint8_t cfg[GXFPNAV_CFG_LEN];
	float *ref;	/* no-finger reference: [GXFPNAV_NUM_VALS][24][132] */
};

typedef int (*gxfpnav_cancel_fn)(void *ctx);

/* Validates the files, chip id and OTP; leaves the sensor with the original config. */
int gxfpnav_open(struct gxfpnav_core *c, const char *dev_path, const char *cfg_path,
		 const char *ref_path);
void gxfpnav_close(struct gxfpnav_core *c);

/* Reset, chip and OTP checks, and config: what open does, on an already open device. */
int gxfpnav_sensor_init(struct gxfpnav_core *c);

/* One NAV frame with the original config: mean difference from the reference. */
int gxfpnav_finger_delta(struct gxfpnav_core *c, double *out_delta);

/*
 * Full sweep. Returns 0 with img filled (GXFPNAV_IMG_W x GXFPNAV_IMG_H), -EAGAIN if the
 * finger moved or a frame arrived damaged, -ECANCELED if cancelled, or another -errno.
 * Always restores the original config.
 */
int gxfpnav_capture(struct gxfpnav_core *c, uint8_t *img, double *out_overlap,
		    gxfpnav_cancel_fn cancel, void *ctx);

/* Offline: per-sensor config from the vendor template and the 32-byte OTP. */
int gxfpnav_build_config(const uint8_t *tpl, const uint8_t *otp, uint8_t *cfg);

/* Reads the chip id without resetting anything. Returns 0 and sets *chip. */
int gxfpnav_probe_chip(const char *dev_path, uint32_t *chip);

/*
 * Provisions one machine: checks the chip before any write, builds cfg from tpl and the
 * live OTP, and measures the no-finger reference. -ENODEV on another chip, -EBUSY if a
 * finger seems to be on the sensor.
 */
int gxfpnav_calibrate(const char *dev_path, const uint8_t *tpl, uint8_t *cfg, const char *ref_path);
