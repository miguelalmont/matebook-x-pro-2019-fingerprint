/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * gxfpnav-setup: prepares one machine for the gxfpnav libfprint driver.
 *
 *   probe                  reads the chip id only; exit 0 if this sensor is supported
 *   calibrate TEMPLATE     builds config.bin from the vendor template and this sensor's
 *                          OTP, and measures reference.bin with no finger on the sensor
 *   build-config TPL OTP OUT   offline: only builds the config from two files
 */
#include "gxfpnav_core.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/gxfp"
#define DEFAULT_OUTDIR "/var/lib/fprint/gxfpnav"
#define CHIP_SUPPORTED 0x2205u

enum { EXIT_UNSUPPORTED = 3, EXIT_FINGER = 4 };

static int read_file(const char *path, uint8_t *buf, size_t want)
{
	FILE *f = fopen(path, "rb");
	size_t got;

	if (!f) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	got = fread(buf, 1, want, f);
	if (got != want || fgetc(f) != EOF) {
		fclose(f);
		fprintf(stderr, "%s: expected exactly %zu bytes\n", path, want);
		return -1;
	}
	fclose(f);
	return 0;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	ssize_t wr;

	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	wr = write(fd, buf, len);
	if (close(fd) != 0 || wr != (ssize_t)len) {
		fprintf(stderr, "%s: short write\n", path);
		return -1;
	}
	return 0;
}

static int publish(const char *tmp, const char *final)
{
	if (rename(tmp, final) != 0) {
		fprintf(stderr, "%s: %s\n", final, strerror(errno));
		return -1;
	}
	return 0;
}

static int cmd_probe(const char *device)
{
	uint32_t chip = 0;
	int rc = gxfpnav_probe_chip(device, &chip);

	if (rc) {
		fprintf(stderr, "cannot read the chip id from %s: %s\n", device, strerror(-rc));
		return 1;
	}
	printf("chip id: 0x%04x\n", chip);
	if (chip != CHIP_SUPPORTED) {
		fprintf(stderr, "unsupported sensor: this driver only supports MilanL (0x%04x)\n",
			CHIP_SUPPORTED);
		return EXIT_UNSUPPORTED;
	}
	printf("supported sensor: MilanL\n");
	return 0;
}

static int cmd_calibrate(const char *tpl_path, const char *outdir, const char *device)
{
	uint8_t tpl[GXFPNAV_CFG_LEN], cfg[GXFPNAV_CFG_LEN];
	char cfg_tmp[4096], cfg_out[4096], ref_tmp[4096], ref_out[4096];
	int rc;

	if (read_file(tpl_path, tpl, sizeof(tpl)))
		return 1;
	if (mkdir(outdir, 0700) != 0 && errno != EEXIST) {
		fprintf(stderr, "%s: %s\n", outdir, strerror(errno));
		return 1;
	}
	snprintf(cfg_tmp, sizeof(cfg_tmp), "%s/config.bin.new", outdir);
	snprintf(cfg_out, sizeof(cfg_out), "%s/config.bin", outdir);
	snprintf(ref_tmp, sizeof(ref_tmp), "%s/reference.bin.new", outdir);
	snprintf(ref_out, sizeof(ref_out), "%s/reference.bin", outdir);

	rc = gxfpnav_calibrate(device, tpl, cfg, ref_tmp);
	if (rc == -ENODEV) {
		fprintf(stderr, "unsupported sensor: this driver only supports MilanL (0x%04x)\n",
			CHIP_SUPPORTED);
		return EXIT_UNSUPPORTED;
	}
	if (rc == -EBUSY) {
		unlink(ref_tmp);
		fprintf(stderr, "a finger seems to be on the sensor: lift it and try again\n");
		return EXIT_FINGER;
	}
	if (rc) {
		unlink(ref_tmp);
		fprintf(stderr, "calibration failed: %s\n", strerror(-rc));
		return 1;
	}
	if (write_file(cfg_tmp, cfg, sizeof(cfg)) || publish(cfg_tmp, cfg_out) ||
	    chmod(ref_tmp, 0600) != 0 || publish(ref_tmp, ref_out))
		return 1;
	printf("calibrated: %s, %s\n", cfg_out, ref_out);
	return 0;
}

static int cmd_build_config(const char *tpl_path, const char *otp_path, const char *out)
{
	uint8_t tpl[GXFPNAV_CFG_LEN], otp[32], cfg[GXFPNAV_CFG_LEN];
	int rc;

	if (read_file(tpl_path, tpl, sizeof(tpl)) || read_file(otp_path, otp, sizeof(otp)))
		return 1;
	rc = gxfpnav_build_config(tpl, otp, cfg);
	if (rc) {
		fprintf(stderr, "invalid template or OTP: %s\n", strerror(-rc));
		return 1;
	}
	return write_file(out, cfg, sizeof(cfg)) ? 1 : 0;
}

static int usage(void)
{
	fprintf(stderr,
		"usage: gxfpnav-setup [--device PATH] probe\n"
		"       gxfpnav-setup [--device PATH] [--outdir DIR] calibrate TEMPLATE\n"
		"       gxfpnav-setup build-config TEMPLATE OTP OUT\n");
	return 2;
}

int main(int argc, char **argv)
{
	const char *device = DEFAULT_DEVICE, *outdir = DEFAULT_OUTDIR;
	int i = 1;

	for (; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--device"))
			device = argv[i + 1];
		else if (!strcmp(argv[i], "--outdir"))
			outdir = argv[i + 1];
		else
			break;
	}
	if (i >= argc)
		return usage();
	if (!strcmp(argv[i], "probe") && argc == i + 1)
		return cmd_probe(device);
	if (!strcmp(argv[i], "calibrate") && argc == i + 2)
		return cmd_calibrate(argv[i + 1], outdir, device);
	if (!strcmp(argv[i], "build-config") && argc == i + 4)
		return cmd_build_config(argv[i + 1], argv[i + 2], argv[i + 3]);
	return usage();
}
