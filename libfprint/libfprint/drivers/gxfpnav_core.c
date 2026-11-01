/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gxfpnav_core.h"

#include "gxfp/cmd/device_recovery_cmd.h"
#include "gxfp/cmd/goodix_xfer.h"
#include "gxfp/cmd/nav_cmd.h"
#include "gxfp/cmd/register_cmd.h"
#include "gxfp/cmd/sensor_cfg_cmd.h"
#include "gxfp/proto/goodix_cmd.h"
#include "gxfp/proto/goodix_proto.h"
#include "gxfp/proto/mp_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COLS 132
#define ROWS 24
#define LINE_FIRST 8
#define LINE_STEP 4
#define NAV_HDR 5u
#define NAV_CRC 4u
#define NAV_PIXEL_BYTES (ROWS * 198u)
#define NAV_PAYLOAD (NAV_HDR + NAV_PIXEL_BYTES + NAV_CRC)
#define CFG_ID 0x18u
#define CFG_OFF_NAV_62 0xCFu
#define CFG_OFF_TCODE 0xDBu
#define CFG_OFF_FDT 0xC3u
#define NAV_62_BASE 0x0400u
#define LL_ORIG 0x0Au
#define IDX_ORIG 2		/* position of LL_ORIG in VALS */
#define CHIP_MILANL 0x2205u
#define OTP_LEN 0x20u
#define RESET_SENSOR 0x05u
#define CFG_REPLY_MS 1000
#define NAV_REPLY_MS 800
#define QUIET_MS 50		/* init, calibration and any unexpected state */
#define QUIET_FAST_MS 5		/* measured on HW: sweeps stay correct, with no stale frames */
#define QUIESCE_MAX_MS 3000
#define XFER_TRIES 3
#define OVERLAP_MIN 0.90
#define TEX_RADIUS 6
#define TEX_TRIM 2
#define HP_SIGMA 8.0
#define HP_RADIUS 32
#define REF_MAGIC "GXNR"
#define REF_VERSION 1u
#define REF_PIXELS (GXFPNAV_NUM_VALS * ROWS * COLS)
#define REF_FRAMES 2
#define REF_FRAME_TRIES 5
#define REF_TEXTURE_MAX 100.0	/* measured stddev: about 39 without a finger, over 200 with one */
#define REF_DRIFT_MAX 30.0	/* counts between frames; a moving finger is far above this */

static const uint8_t VALS[GXFPNAV_NUM_VALS] = { 0x08, 0x09, 0x0A, 0x0B, 0x10, 0x11, 0x12, 0x13 };

static long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Without an IRQ ack, copies of the previous response linger: wait for bus silence. */
static int quiesce_ms(struct gxfp_dev *dev, int quiet_ms)
{
	long start = mono_ms();

	for (;;) {
		int rc = gxfp_dev_flush_rxq(dev);

		if (rc)
			return rc;
		rc = gxfp_dev_poll_readable(dev, quiet_ms);
		if (rc == -EAGAIN)
			return 0;
		if (rc)
			return rc;
		if (mono_ms() - start > QUIESCE_MAX_MS)
			return -ETIMEDOUT;
		usleep(1000);
	}
}

#define QUIET_XFER_MS(dev, quiet_ms, rc, call)				\
	do {								\
		for (int try_ = 0; try_ < XFER_TRIES; try_++) {		\
			(rc) = quiesce_ms(dev, quiet_ms);		\
			if (rc)						\
				break;					\
			(rc) = (call);					\
			if ((rc) != -ENOBUFS)				\
				break;					\
		}							\
	} while (0)

#define QUIET_XFER(dev, rc, call) QUIET_XFER_MS(dev, QUIET_MS, rc, call)

static int read_exact(const char *path, void *buf, size_t want)
{
	FILE *f = fopen(path, "rb");
	size_t got;
	int extra;

	if (!f)
		return -errno;
	got = fread(buf, 1, want, f);
	extra = fgetc(f);
	fclose(f);
	return (got == want && extra == EOF) ? 0 : -EBADMSG;
}

/* -(0xA5A5 + sum of 0x7f LE words). */
static uint16_t cfg_checksum(const uint8_t *c)
{
	uint16_t sum = 0xA5A5;

	for (size_t i = 0; i < 0xFE; i += 2)
		sum = (uint16_t)(sum + (uint16_t)(c[i] | (c[i + 1] << 8)));
	return (uint16_t)-sum;
}

/* Inverted CRC8, polynomial 0x07. */
static uint8_t crc8_inv(const uint8_t *d, size_t n)
{
	uint8_t c = 0;

	while (n--) {
		c ^= *d++;
		for (int b = 0; b < 8; b++)
			c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
	}
	return (uint8_t)~c;
}

/* OTP CRC, plus the Tcode and diff values: the config must belong to this sensor. */
static int check_otp_cfg(const uint8_t *otp, const uint8_t *cfg)
{
	uint8_t b[0x16];
	unsigned int tcode, diff;

	memcpy(b, otp, 0x14);
	b[0x14] = otp[0x1d];
	b[0x15] = otp[0x1f];
	if (crc8_inv(b, sizeof(b)) != otp[0x1e])
		return -EBADMSG;
	if (otp[0x16] == 0 || otp[0x16] + otp[0x17] != 0xFF)
		return -EBADMSG;
	tcode = (((unsigned int)otp[0x16] >> 4) + 1u) * 0x10u;
	diff = ((((otp[0x16] & 0xFu) + 2u) * 0x6400u) / tcode / 3u) >> 4;
	if (((unsigned int)cfg[CFG_OFF_TCODE] | ((unsigned int)cfg[CFG_OFF_TCODE + 1] << 8)) != tcode ||
	    cfg[CFG_OFF_FDT + 1] != diff)
		return -EBADMSG;
	return 0;
}

static int check_cfg(const uint8_t *cfg)
{
	if (cfg[0] != CFG_ID)
		return -EBADMSG;
	if (((unsigned int)cfg[0xFE] | ((unsigned int)cfg[0xFF] << 8)) != cfg_checksum(cfg))
		return -EBADMSG;
	if ((cfg[CFG_OFF_NAV_62] | ((unsigned int)cfg[CFG_OFF_NAV_62 + 1] << 8)) !=
	    (NAV_62_BASE | LL_ORIG))
		return -EBADMSG;
	return 0;
}

static int load_ref(float *ref, const char *path)
{
	FILE *f = fopen(path, "rb");
	char magic[4];
	uint32_t version;
	size_t got;
	int extra;

	if (!f)
		return -errno;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, REF_MAGIC, 4) ||
	    fread(&version, sizeof(version), 1, f) != 1 || version != REF_VERSION) {
		fclose(f);
		return -EBADMSG;
	}
	got = fread(ref, sizeof(float), REF_PIXELS, f);
	extra = fgetc(f);
	fclose(f);
	return (got == REF_PIXELS && extra == EOF) ? 0 : -EBADMSG;
}

/* A single attempt: the effect of resending the config after a timeout is unknown. */
static int send_config(struct gxfpnav_core *c, uint8_t ll)
{
	static uint8_t rx[GXFP_MP_FRAME_MAX];
	uint8_t cfg[GXFPNAV_CFG_LEN];
	uint32_t rx_len = 0;
	uint16_t ck;
	int rc = quiesce_ms(&c->dev, c->quiet_ms);

	if (rc)
		return rc;
	c->cur_ll = 0;
	memcpy(cfg, c->cfg, sizeof(cfg));
	cfg[CFG_OFF_NAV_62] = ll;
	ck = cfg_checksum(cfg);
	cfg[0xFE] = (uint8_t)ck;
	cfg[0xFF] = (uint8_t)(ck >> 8);
	rc = gxfp_goodix_request_selected(&c->dev, GXFP_CMD_UPLOAD_CONFIG_MCU,
					  GXFP_CMD_UPLOAD_CONFIG_MCU, cfg, sizeof(cfg),
					  rx, sizeof(rx), &rx_len, 1, CFG_REPLY_MS, NULL, NULL);
	if (!rc)
		c->cur_ll = ll;
	return rc;
}

/* CRC-32/MPEG-2. */
static uint32_t frame_crc(const uint8_t *d, size_t n)
{
	uint32_t v = 0xFFFFFFFFu;

	while (n--) {
		v ^= (uint32_t)*d++ << 24;
		for (int b = 0; b < 8; b++)
			v = (v & 0x80000000u) ? (v << 1) ^ 0x04C11DB7u : v << 1;
	}
	return v;
}

/* One unpacked NAV frame (6 bytes -> 4 pixels). -EAGAIN if it arrives damaged. */
static int nav_frame(struct gxfpnav_core *c, uint16_t *px, uint8_t *crc_out)
{
	static uint8_t pl[GXFP_MP_FRAME_MAX];
	const uint8_t *body = pl + NAV_HDR;
	size_t len = 0;
	uint32_t tail;
	int rc;

	QUIET_XFER_MS(&c->dev, c->quiet_ms, rc,
		      gxfp_cmd_nav_frame(&c->dev, 1, NAV_REPLY_MS, pl, sizeof(pl), &len));
	if (rc)
		return rc;
	if (len != NAV_PAYLOAD)
		return -EAGAIN;
	tail = ((uint32_t)body[NAV_PIXEL_BYTES + 2] << 24) | ((uint32_t)body[NAV_PIXEL_BYTES + 3] << 16) |
	       ((uint32_t)body[NAV_PIXEL_BYTES] << 8) | (uint32_t)body[NAV_PIXEL_BYTES + 1];
	if (tail != frame_crc(body, NAV_PIXEL_BYTES))
		return -EAGAIN;
	if (crc_out) {
		crc_out[0] = (uint8_t)(tail >> 24);
		crc_out[1] = (uint8_t)(tail >> 16);
		crc_out[2] = (uint8_t)(tail >> 8);
		crc_out[3] = (uint8_t)tail;
	}

	for (int r = 0; r < ROWS; r++) {
		const uint8_t *row = body + (size_t)r * 198u;

		for (int i = 0; i < COLS / 4; i++) {
			const uint8_t *b = row + i * 6;
			uint16_t *o = px + r * COLS + i * 4;

			o[0] = (uint16_t)(((b[0] & 0x0F) << 8) | b[1]);
			o[1] = (uint16_t)((b[3] << 4) | (b[0] >> 4));
			o[2] = (uint16_t)(((b[5] & 0x0F) << 8) | b[2]);
			o[3] = (uint16_t)((b[4] << 4) | (b[5] >> 4));
		}
	}
	return 0;
}

/* Removes the horizontal trend, which changes between values of register 0x62. */
static void texture(const float *row, float *out)
{
	for (int i = 0; i < COLS; i++) {
		int lo = i - TEX_RADIUS < 0 ? 0 : i - TEX_RADIUS;
		int hi = i + TEX_RADIUS >= COLS ? COLS - 1 : i + TEX_RADIUS;
		double sum = 0;

		for (int k = lo; k <= hi; k++)
			sum += row[k];
		out[i] = (float)(row[i] - sum / (hi - lo + 1));
	}
}

static double corr(const float *a, const float *b, int n)
{
	double ma = 0, mb = 0, num = 0, da = 0, db = 0;

	for (int i = 0; i < n; i++) {
		ma += a[i];
		mb += b[i];
	}
	ma /= n;
	mb /= n;
	for (int i = 0; i < n; i++) {
		num += (a[i] - ma) * (b[i] - mb);
		da += (a[i] - ma) * (a[i] - ma);
		db += (b[i] - mb) * (b[i] - mb);
	}
	return (da > 0 && db > 0) ? num / sqrt(da * db) : 0;
}

static int val_index(uint8_t ll)
{
	for (int v = 0; v < GXFPNAV_NUM_VALS; v++) {
		if (VALS[v] == ll)
			return v;
	}
	return -1;
}

int gxfpnav_detect_begin(struct gxfpnav_core *c)
{
	c->det_valid = 0;
	return c->cur_ll == LL_ORIG ? 0 : send_config(c, LL_ORIG);
}

int gxfpnav_finger_delta(struct gxfpnav_core *c, double *out_delta, double *out_corr)
{
	static uint16_t px[ROWS * COLS];
	float *tex = malloc(ROWS * COLS * sizeof(float));
	uint8_t crc[4];
	int idx, rc;
	const float *ref;
	double sum = 0;

	if (!tex)
		return -ENOMEM;
	idx = val_index(c->cur_ll);
	rc = idx < 0 ? send_config(c, LL_ORIG) : 0;
	if (idx < 0)
		idx = IDX_ORIG;
	if (!rc)
		rc = nav_frame(c, px, crc);
	if (!rc && c->det_valid && !memcmp(crc, c->det_crc, sizeof(crc)))
		rc = -EAGAIN;
	if (rc) {
		free(tex);
		return rc;
	}
	ref = c->ref + (size_t)idx * ROWS * COLS;
	for (int r = 0; r < ROWS; r++) {
		float row[COLS];

		for (int i = 0; i < COLS; i++) {
			row[i] = (float)px[r * COLS + i] - ref[r * COLS + i];
			sum += row[i];
		}
		texture(row, tex + r * COLS);
	}
	*out_delta = sum / (ROWS * COLS);
	*out_corr = c->det_valid ? corr(tex, c->det_tex, ROWS * COLS) : 0;
	memcpy(c->det_tex, tex, ROWS * COLS * sizeof(float));
	memcpy(c->det_frame, px, sizeof(px));
	memcpy(c->det_crc, crc, sizeof(crc));
	c->det_valid = 1;
	free(tex);
	return 0;
}

static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return (x > y) - (x < y);
}

static float percentile(const float *sorted, int n, double p)
{
	double pos = p / 100.0 * (n - 1);
	int i = (int)pos;

	return (i + 1 < n) ? (float)(sorted[i] + (pos - i) * (sorted[i + 1] - sorted[i])) : sorted[i];
}

static int reflect101(int i, int n)
{
	while (i < 0 || i >= n)
		i = i < 0 ? -i : 2 * n - 2 - i;
	return i;
}

/* Removes the background with a Gaussian filter and scales percentiles 1..99 to 8 bits. */
static int to_8bit(const float *src, uint8_t *dst, int W, int H)
{
	const int N = W * H;
	double kernel[2 * HP_RADIUS + 1], total = 0;
	float *tmp = malloc((size_t)N * sizeof(float));
	float *hp = malloc((size_t)N * sizeof(float));
	float *sorted = malloc((size_t)N * sizeof(float));
	float lo, hi;
	int rc = 0;

	if (!tmp || !hp || !sorted) {
		rc = -ENOMEM;
		goto out;
	}
	for (int i = -HP_RADIUS; i <= HP_RADIUS; i++) {
		kernel[i + HP_RADIUS] = exp(-(double)(i * i) / (2 * HP_SIGMA * HP_SIGMA));
		total += kernel[i + HP_RADIUS];
	}
	for (int i = 0; i <= 2 * HP_RADIUS; i++)
		kernel[i] /= total;

	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			double s = 0;

			for (int k = -HP_RADIUS; k <= HP_RADIUS; k++)
				s += kernel[k + HP_RADIUS] * src[y * W + reflect101(x + k, W)];
			tmp[y * W + x] = (float)s;
		}
	}
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			double s = 0;

			for (int k = -HP_RADIUS; k <= HP_RADIUS; k++)
				s += kernel[k + HP_RADIUS] * tmp[reflect101(y + k, H) * W + x];
			hp[y * W + x] = src[y * W + x] - (float)s;
		}
	}
	memcpy(sorted, hp, (size_t)N * sizeof(float));
	qsort(sorted, (size_t)N, sizeof(float), cmp_float);
	lo = percentile(sorted, N, 1.0);
	hi = percentile(sorted, N, 99.0);
	if (hi <= lo) {
		rc = -EAGAIN;
		goto out;
	}
	for (int i = 0; i < N; i++) {
		double v = 255.0 * (hp[i] - lo) / (hi - lo);

		dst[i] = v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v;
	}
out:
	free(tmp);
	free(hp);
	free(sorted);
	return rc;
}

/* Combines 8 frames minus their reference; repeated lines reveal whether the finger moved. */
static int assemble(struct gxfpnav_core *c, const uint16_t *frames, uint8_t *img, double *out_overlap)
{
	enum { LINES = GXFPNAV_IMG_H };
	float *diff = malloc(GXFPNAV_NUM_VALS * ROWS * COLS * sizeof(float));
	float *lines = calloc(LINES * COLS, sizeof(float));
	float *crop = malloc(LINES * GXFPNAV_IMG_W * sizeof(float));
	int seen[LINES], first_v[LINES], first_r[LINES];
	double sum_corr = 0;
	int n_corr = 0, rc;

	if (!diff || !lines || !crop) {
		rc = -ENOMEM;
		goto out;
	}
	memset(seen, 0, sizeof(seen));
	for (int v = 0; v < GXFPNAV_NUM_VALS; v++) {
		for (int r = 0; r < ROWS; r++) {
			const uint16_t *f = frames + ((size_t)v * ROWS + r) * COLS;
			const float *ref = c->ref + ((size_t)v * ROWS + r) * COLS;
			float *d = diff + ((size_t)v * ROWS + r) * COLS;
			int line = VALS[v] + LINE_STEP * r - LINE_FIRST;

			for (int i = 0; i < COLS; i++)
				d[i] = (float)f[i] - ref[i];
			if (seen[line] == 1) {
				const float *d0 = diff + ((size_t)first_v[line] * ROWS + first_r[line]) * COLS;
				float t0[COLS], t1[COLS];

				texture(d0, t0);
				texture(d, t1);
				sum_corr += corr(t0 + TEX_TRIM, t1 + TEX_TRIM, COLS - 2 * TEX_TRIM);
				n_corr++;
			} else {
				first_v[line] = v;
				first_r[line] = r;
			}
			for (int i = 0; i < COLS; i++)
				lines[line * COLS + i] += d[i];
			seen[line]++;
		}
	}
	for (int y = 0; y < LINES; y++) {
		if (!seen[y]) {
			rc = -EIO;
			goto out;
		}
		for (int x = 0; x < GXFPNAV_IMG_W; x++)
			crop[y * GXFPNAV_IMG_W + x] = lines[y * COLS + x + TEX_TRIM] / seen[y];
	}
	*out_overlap = n_corr ? sum_corr / n_corr : 0;
	rc = *out_overlap < OVERLAP_MIN ? -EAGAIN : to_8bit(crop, img, GXFPNAV_IMG_W, LINES);
out:
	free(diff);
	free(lines);
	free(crop);
	return rc;
}

/*
 * Image from a subset of the frames. Gaps between the lines read are filled per column
 * with a Catmull-Rom spline, enough for ridges sampled every 2 lines.
 */
static int build_partial(struct gxfpnav_core *c, const uint16_t *frames, const int *have, uint8_t *img,
			 int *out_height)
{
	enum { LINES = GXFPNAV_IMG_H, W = GXFPNAV_IMG_W };
	float *lines = calloc(LINES * COLS, sizeof(float));
	float *crop = malloc(LINES * W * sizeof(float));
	int seen[LINES], ys[LINES], n = 0, h, step, rc = 0;

	if (!lines || !crop) {
		rc = -ENOMEM;
		goto out;
	}
	memset(seen, 0, sizeof(seen));
	for (int v = 0; v < GXFPNAV_NUM_VALS; v++) {
		if (!have[v])
			continue;
		for (int r = 0; r < ROWS; r++) {
			const uint16_t *f = frames + ((size_t)v * ROWS + r) * COLS;
			const float *ref = c->ref + ((size_t)v * ROWS + r) * COLS;
			int line = VALS[v] + LINE_STEP * r - LINE_FIRST;

			for (int i = 0; i < COLS; i++)
				lines[line * COLS + i] += (float)f[i] - ref[i];
			seen[line]++;
		}
	}
	for (int y = 0; y < LINES; y++) {
		if (!seen[y])
			continue;
		for (int i = 0; i < COLS; i++)
			lines[y * COLS + i] /= seen[y];
		ys[n++] = y;
	}
	if (n < 2) {
		rc = -EIO;
		goto out;
	}
	step = ys[1] - ys[0];
	for (int i = 1; i < n; i++) {
		if (ys[i] - ys[i - 1] != step) {
			rc = -EIO;
			goto out;
		}
	}
	h = ys[n - 1] - ys[0] + 1;
	for (int y = 0; y < h; y++) {
		int i = y / step;
		float t;
		const float *p0, *p1, *p2, *p3;

		if (i > n - 2)
			i = n - 2;
		t = (float)(y - i * step) / (float)step;
		p0 = lines + (size_t)ys[i > 0 ? i - 1 : 0] * COLS + TEX_TRIM;
		p1 = lines + (size_t)ys[i] * COLS + TEX_TRIM;
		p2 = lines + (size_t)ys[i + 1] * COLS + TEX_TRIM;
		p3 = lines + (size_t)ys[i + 2 < n ? i + 2 : n - 1] * COLS + TEX_TRIM;
		for (int x = 0; x < W; x++)
			crop[y * W + x] = 0.5f * (2 * p1[x] + (p2[x] - p0[x]) * t +
						  (2 * p0[x] - 5 * p1[x] + 4 * p2[x] - p3[x]) * t * t +
						  (3 * p1[x] - p0[x] - 3 * p2[x] + p3[x]) * t * t * t);
	}
	*out_height = h;
	rc = to_8bit(crop, img, W, h);
out:
	free(lines);
	free(crop);
	return rc;
}

/* Masks over VALS. Levels add frames: 0x0A + 0x08, then 0x09 + 0x0B, then the rest. */
static const uint8_t LEVEL_VALS[3] = { 0x05, 0x0F, 0xFF };

int gxfpnav_capture(struct gxfpnav_core *c, uint8_t *img, int *out_height, double *out_overlap,
		    gxfpnav_accept_fn accept, void *accept_ctx, gxfpnav_cancel_fn cancel, void *ctx)
{
	uint16_t *frames = malloc(GXFPNAV_NUM_VALS * ROWS * COLS * sizeof(uint16_t));
	int have[GXFPNAV_NUM_VALS] = { 0 };
	int rc = 0;

	if (!frames)
		return -ENOMEM;
	*out_overlap = 0;
	if (accept && c->det_valid && c->cur_ll == LL_ORIG) {
		memcpy(frames + (size_t)IDX_ORIG * ROWS * COLS, c->det_frame,
		       ROWS * COLS * sizeof(uint16_t));
		have[IDX_ORIG] = 1;
	}
	c->det_valid = 0;

	for (int level = accept ? 0 : 2; level < 3 && !rc; level++) {
		for (int v = 0; v < GXFPNAV_NUM_VALS && !rc; v++) {
			if (!(LEVEL_VALS[level] & (1u << v)) || have[v])
				continue;
			if (cancel && cancel(ctx))
				rc = -ECANCELED;
			if (!rc)
				rc = send_config(c, VALS[v]);
			if (!rc)
				rc = nav_frame(c, frames + (size_t)v * ROWS * COLS, NULL);
			if (!rc)
				have[v] = 1;
		}
		if (rc)
			break;
		if (level == 2) {
			*out_height = GXFPNAV_IMG_H;
			rc = assemble(c, frames, img, out_overlap);
			break;
		}
		rc = build_partial(c, frames, have, img, out_height);
		if (rc == -EAGAIN) {
			rc = 0;		/* flat image: not enough to decide, keep reading */
			continue;
		}
		if (!rc && accept(img, GXFPNAV_IMG_W, *out_height, level + 1, accept_ctx))
			break;
	}
	free(frames);
	return rc;
}

int gxfpnav_sensor_init(struct gxfpnav_core *c)
{
	static uint8_t otp_live[256];
	uint8_t raw[4];
	uint16_t otp_len = 0;
	uint32_t chip;
	int rc;

	c->quiet_ms = QUIET_MS;
	c->cur_ll = 0;
	c->det_valid = 0;
	QUIET_XFER(&c->dev, rc, gxfp_cmd_reset_device(&c->dev, RESET_SENSOR));
	if (rc)
		return rc;
	usleep(1000);
	QUIET_XFER(&c->dev, rc, gxfp_cmd_read_reg(&c->dev, 0x0000, raw, sizeof(raw)));
	if (rc)
		return rc;
	/* The raw value is 05 A2 00 22: pairs are swapped before reading bytes 1..3. */
	chip = (uint32_t)raw[0] | ((uint32_t)raw[3] << 8) | ((uint32_t)raw[2] << 16);
	if (chip != CHIP_MILANL)
		return -ENODEV;
	QUIET_XFER(&c->dev, rc, gxfp_cmd_read_otp(&c->dev, otp_live, sizeof(otp_live), &otp_len));
	if (!rc && otp_len < OTP_LEN)
		rc = -EBADMSG;
	if (!rc)
		rc = check_otp_cfg(otp_live, c->cfg);
	if (!rc)
		rc = send_config(c, LL_ORIG);
	if (!rc)
		c->quiet_ms = QUIET_FAST_MS;
	return rc;
}

int gxfpnav_open(struct gxfpnav_core *c, const char *dev_path, const char *cfg_path,
		 const char *ref_path)
{
	int rc;

	memset(c, 0, sizeof(*c));
	c->ref = malloc(REF_PIXELS * sizeof(float));
	c->det_frame = malloc(ROWS * COLS * sizeof(uint16_t));
	c->det_tex = malloc(ROWS * COLS * sizeof(float));
	if (!c->ref || !c->det_frame || !c->det_tex) {
		gxfpnav_close(c);
		return -ENOMEM;
	}
	rc = read_exact(cfg_path, c->cfg, GXFPNAV_CFG_LEN);
	if (!rc)
		rc = check_cfg(c->cfg);
	if (!rc)
		rc = load_ref(c->ref, ref_path);
	if (!rc)
		rc = gxfp_dev_open(&c->dev, dev_path, O_RDWR);
	if (rc)
		goto fail;
	c->dev_open = 1;

	rc = gxfpnav_sensor_init(c);
	if (!rc)
		return 0;
fail:
	gxfpnav_close(c);
	return rc;
}

void gxfpnav_close(struct gxfpnav_core *c)
{
	if (c->dev_open)
		gxfp_dev_close(&c->dev);
	c->dev_open = 0;
	free(c->ref);
	c->ref = NULL;
	free(c->det_frame);
	c->det_frame = NULL;
	free(c->det_tex);
	c->det_tex = NULL;
}

static int patch_reg(uint8_t *cfg, unsigned int off, unsigned int len, uint16_t reg,
		     uint16_t keep_mask, uint16_t value)
{
	for (unsigned int i = 0; i + 4 <= len && off + i + 4 <= GXFPNAV_CFG_LEN; i += 4) {
		uint8_t *p = cfg + off + i;

		if (((unsigned int)p[0] | ((unsigned int)p[1] << 8)) == reg) {
			uint16_t v = (uint16_t)(p[2] | (p[3] << 8));

			v = (uint16_t)((v & keep_mask) | value);
			p[2] = (uint8_t)v;
			p[3] = (uint8_t)(v >> 8);
			return 0;
		}
	}
	return -EBADMSG;
}

int gxfpnav_build_config(const uint8_t *tpl, const uint8_t *otp, uint8_t *cfg)
{
	unsigned int tcode, diff;
	uint16_t ck;
	int rc;

	if (tpl[0] != CFG_ID || otp[0x16] == 0 || otp[0x16] + otp[0x17] != 0xFF)
		return -EBADMSG;
	tcode = (((unsigned int)otp[0x16] >> 4) + 1u) * 0x10u;
	diff = ((((otp[0x16] & 0xFu) + 2u) * 0x6400u) / tcode / 3u) >> 4;

	memcpy(cfg, tpl, GXFPNAV_CFG_LEN);
	/* Section offsets and lengths: header bytes 9/10 for Tcode, 5/6 for the FDT delta. */
	rc = patch_reg(cfg, cfg[9], cfg[10], 0x5C, 0, (uint16_t)tcode);
	if (!rc)
		rc = patch_reg(cfg, cfg[5], cfg[6], 0x82, 0x00FF, (uint16_t)(diff << 8));
	if (rc)
		return rc;
	ck = cfg_checksum(cfg);
	cfg[0xFE] = (uint8_t)ck;
	cfg[0xFF] = (uint8_t)(ck >> 8);
	rc = check_cfg(cfg);
	return rc ? rc : check_otp_cfg(otp, cfg);
}

static int read_chip(struct gxfp_dev *dev, uint32_t *chip)
{
	uint8_t raw[4];
	int rc;

	QUIET_XFER(dev, rc, gxfp_cmd_read_reg(dev, 0x0000, raw, sizeof(raw)));
	if (rc)
		return rc;
	*chip = (uint32_t)raw[0] | ((uint32_t)raw[3] << 8) | ((uint32_t)raw[2] << 16);
	return 0;
}

int gxfpnav_probe_chip(const char *dev_path, uint32_t *chip)
{
	struct gxfp_dev dev;
	int rc = gxfp_dev_open(&dev, dev_path, O_RDWR);

	if (rc)
		return rc;
	rc = read_chip(&dev, chip);
	gxfp_dev_close(&dev);
	return rc;
}

static double texture_stddev(const uint16_t *px)
{
	double sum = 0, sum2 = 0;
	int n = 0;

	for (int r = 0; r < ROWS; r++) {
		float row[COLS], tex[COLS];

		for (int i = 0; i < COLS; i++)
			row[i] = px[r * COLS + i];
		texture(row, tex);
		for (int i = TEX_RADIUS; i < COLS - TEX_RADIUS; i++) {
			sum += tex[i];
			sum2 += (double)tex[i] * tex[i];
			n++;
		}
	}
	return sqrt(sum2 / n - (sum / n) * (sum / n));
}

static double frame_mean(const uint16_t *px)
{
	double sum = 0;

	for (int i = 0; i < ROWS * COLS; i++)
		sum += px[i];
	return sum / (ROWS * COLS);
}

static int nav_frame_retry(struct gxfpnav_core *c, uint16_t *px)
{
	int rc = -EAGAIN;

	for (int i = 0; i < REF_FRAME_TRIES && rc == -EAGAIN; i++)
		rc = nav_frame(c, px, NULL);
	return rc;
}

/* Same sweep as a capture, so the reference matches what gxfpnav_capture subtracts. */
static int measure_reference(struct gxfpnav_core *c)
{
	static uint16_t px[ROWS * COLS];
	int rc = 0;

	for (int v = 0; v < GXFPNAV_NUM_VALS && !rc; v++) {
		float *ref = c->ref + (size_t)v * ROWS * COLS;
		double first_mean = 0;

		for (int i = 0; i < ROWS * COLS; i++)
			ref[i] = 0;
		rc = send_config(c, VALS[v]);
		for (int f = 0; f < REF_FRAMES && !rc; f++) {
			rc = nav_frame_retry(c, px);
			if (rc)
				break;
			if (texture_stddev(px) > REF_TEXTURE_MAX) {
				rc = -EBUSY;
				break;
			}
			if (f == 0)
				first_mean = frame_mean(px);
			else if (fabs(frame_mean(px) - first_mean) > REF_DRIFT_MAX)
				rc = -EBUSY;
			for (int i = 0; i < ROWS * COLS; i++)
				ref[i] += (float)px[i] / REF_FRAMES;
		}
	}
	return rc;
}

static int write_ref(const float *ref, const char *path)
{
	static const uint32_t version = REF_VERSION;
	FILE *f = fopen(path, "wb");
	int ok;

	if (!f)
		return -errno;
	ok = fwrite(REF_MAGIC, 1, 4, f) == 4 && fwrite(&version, sizeof(version), 1, f) == 1 &&
	     fwrite(ref, sizeof(float), REF_PIXELS, f) == REF_PIXELS;
	if (fclose(f) != 0)
		ok = 0;
	return ok ? 0 : -EIO;
}

int gxfpnav_calibrate(const char *dev_path, const uint8_t *tpl, uint8_t *cfg, const char *ref_path)
{
	static uint8_t otp[256];
	struct gxfpnav_core c;
	uint16_t otp_len = 0;
	uint32_t chip = 0;
	int rc, restore;

	memset(&c, 0, sizeof(c));
	c.quiet_ms = QUIET_MS;
	c.ref = malloc(REF_PIXELS * sizeof(float));
	if (!c.ref)
		return -ENOMEM;
	rc = gxfp_dev_open(&c.dev, dev_path, O_RDWR);
	if (rc) {
		free(c.ref);
		return rc;
	}
	c.dev_open = 1;

	/* Other sensors share this ACPI id: nothing is written before this check. */
	rc = read_chip(&c.dev, &chip);
	if (!rc && chip != CHIP_MILANL)
		rc = -ENODEV;
	if (!rc)
		QUIET_XFER(&c.dev, rc, gxfp_cmd_reset_device(&c.dev, RESET_SENSOR));
	if (!rc) {
		usleep(1000);
		QUIET_XFER(&c.dev, rc, gxfp_cmd_read_otp(&c.dev, otp, sizeof(otp), &otp_len));
	}
	if (!rc && otp_len < OTP_LEN)
		rc = -EBADMSG;
	if (!rc)
		rc = gxfpnav_build_config(tpl, otp, c.cfg);
	if (!rc)
		rc = send_config(&c, LL_ORIG);
	if (!rc) {
		rc = measure_reference(&c);
		restore = send_config(&c, LL_ORIG);
		if (!rc)
			rc = restore;
	}
	if (!rc)
		rc = write_ref(c.ref, ref_path);
	if (!rc)
		memcpy(cfg, c.cfg, GXFPNAV_CFG_LEN);
	gxfpnav_close(&c);
	return rc;
}
