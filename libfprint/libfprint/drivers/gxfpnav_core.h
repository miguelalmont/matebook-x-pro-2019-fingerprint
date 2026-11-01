/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Core of the gxfpnav driver: fingerprint images through cleartext NAV commands, with
 * no TLS and no PSK. With register 0x62 = 0x04LL the MilanL sensor reads 24 lines, every
 * 4th line starting at line LL. That register is part of the 0x90 config, and combining
 * 8 values yields lines 8..111. A capture can stop early with fewer frames: see
 * gxfpnav_capture. This file does not depend on libfprint.
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
	int quiet_ms;	/* bus silence before each command */
	uint8_t cur_ll;	/* value of register 0x62 in the MCU; 0 if unknown */
	uint16_t *det_frame;	/* last detection frame, reusable by a capture */
	float *det_tex;		/* its texture, to tell whether the finger has settled */
	uint8_t det_crc[4];	/* its CRC, to spot a stale copy of the previous response */
	int det_valid;
};

typedef int (*gxfpnav_cancel_fn)(void *ctx);

/* Validates the files, chip id and OTP; leaves the sensor with the original config. */
int gxfpnav_open(struct gxfpnav_core *c, const char *dev_path, const char *cfg_path,
		 const char *ref_path);
void gxfpnav_close(struct gxfpnav_core *c);

/* Reset, chip and OTP checks, and config: what open does, on an already open device. */
int gxfpnav_sensor_init(struct gxfpnav_core *c);

/* Before waiting for a finger: selects the original config and forgets the last frame. */
int gxfpnav_detect_begin(struct gxfpnav_core *c);

/*
 * One NAV frame with the current config: mean difference from the reference, and the
 * correlation with the previous frame of this detection (0 if there is none). A frame
 * identical to the previous one is a stale copy and returns -EAGAIN.
 */
int gxfpnav_finger_delta(struct gxfpnav_core *c, double *out_delta, double *out_corr);

/*
 * Decides whether a partial image is enough. level is 1 (2 frames, interpolated) or 2
 * (4 frames). It must only accept with strong evidence: there is no way back to a longer
 * read, and only the full image may lead to a rejection.
 */
typedef int (*gxfpnav_accept_fn)(const uint8_t *img, int width, int height, int level, void *ctx);

/*
 * Reads frames until accept takes a partial image or the full sweep is done. With no
 * accept it always reads the full sweep, using no detection frame. Returns 0 with img
 * filled (GXFPNAV_IMG_W x *out_height), -EAGAIN if the full image shows that the finger
 * moved or a frame arrived damaged, -ECANCELED if cancelled, or another -errno.
 * Leaves the sensor on the last value read: see cur_ll.
 */
int gxfpnav_capture(struct gxfpnav_core *c, uint8_t *img, int *out_height, double *out_overlap,
		    gxfpnav_accept_fn accept, void *accept_ctx, gxfpnav_cancel_fn cancel, void *ctx);

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
