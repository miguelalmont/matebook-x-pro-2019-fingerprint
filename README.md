# Fingerprint reader for the MateBook X Pro 2019 on Linux

Makes the Goodix **GXFP51B7** (MilanL) fingerprint reader of the Huawei MateBook X Pro
2019 work on Linux with the standard `fprintd` tools: enrolment, verification, and login,
`sudo` and the lock screen.

No Windows installation, TLS key or Goodix binary runs on your system.

> **Experimental, unofficial and tested on one laptop.** Read
> [Security](#security) and [Known limitations](#known-limitations) before relying on it.

## Compatibility

The same ACPI id `GXFP51B7` is used by **different sensors**, so check your model first.

| Laptop | `product_name` | Sensor | Status |
|---|---|---|---|
| MateBook X Pro 2019 | `MACHR-WX9` | MilanL `0x2205`, firmware `GF_9ELIBE_EC_19019` | ✅ Tested on real hardware |
| MateBook with ChicagoHS | `MACHC-WAX9` | ChicagoHS `0x2504` | ❌ Not supported. See [iMMIQ](https://github.com/iMMIQ/gxfp51b7-linux) or [stenerstrom](https://github.com/stenerstrom/gxfp51b7-linux) |
| Anything else with `GXFP51B7` | | Unknown | ❓ Untested |

Check yours:

```bash
cat /sys/class/dmi/id/product_name
ls /sys/bus/acpi/devices/ | grep GXFP51B7
```

On a tested model, the installer goes ahead by itself. On any other model it stops
unless you pass `--allow-untested-model`. Even then, the tool reads the chip id before
writing anything to the sensor and refuses if it is not MilanL. Please
[open an issue](../../issues) with your model and chip id, whatever the result.

**Requirements:** x86_64, kernel 6.12 or newer, systemd, `fprintd`, and internet access
during installation.

| Distribution | Build of library and tool | Kernel module build | Run on the real reader |
|---|---|---|---|
| Fedora 44 | ✅ | ✅ | ✅ |
| Ubuntu 24.04 (HWE kernel) | ✅ | ✅ | Not tested |
| Debian 13 | ✅ | ✅ | Not tested |
| Arch Linux (OpenCV 5) | ✅ | ✅ | Not tested |
| openSUSE Tumbleweed | ✅ | Not tested | Not tested |

Fingerprint login on Ubuntu and Debian, enrolling a new Secure Boot key from scratch and
`uninstall.sh` have not been exercised on real hardware yet. Reports are welcome.

## Install

Download the latest package from [Releases](../../releases/latest):

```bash
tar xf matebook-x-pro-2019-fingerprint-*.tar.gz
cd matebook-x-pro-2019-fingerprint-*/
sudo ./install.sh
```

Keep your finger **off the reader** when the installer asks. It will:

1. Check the laptop and kernel, and install the build dependencies.
2. Build the libfprint library and the kernel module with DKMS.
3. If Secure Boot is on, enrol the signing key. **This needs one reboot:** confirm the
   key in the blue MOK screen, then run `sudo ./install.sh` again to finish.
4. Download the official Windows driver package from Microsoft, verify its checksum and
   read the sensor configuration from it. Offline, use `--cab FILE`.
5. Calibrate the sensor and configure `fprintd` to use the new library.

Your distribution's own packages are not replaced. Run `./install.sh --help` for all options.

## Use

```bash
fprintd-enroll      # touch the reader 10 times, moving your finger a little
fprintd-verify      # a match usually takes about half a second
```

Enable it for login, `sudo` and the lock screen (keep a root shell open while doing this):

```bash
# Fedora
sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes

# Debian / Ubuntu
sudo pam-auth-update --enable fprintd
```

Your password keeps working. On other distributions, add `pam_fprintd.so` to your PAM configuration.

### Uninstall

```bash
fprintd-delete "$USER"
sudo ./uninstall.sh
```

Turn off the PAM feature as well; the script prints the command.

## Features

- Works with unmodified `fprintd` and PAM.
- Image of 128×104 pixels, assembled from 8 sweeps of the sensor's navigation frames.
- Matching on the host with SIGFM, the matcher used by community Goodix projects.
- Rebuilt for new kernels by DKMS, and signed for Secure Boot.
- Survives suspend (S3), including a suspend in the middle of a verification.
- Refuses to run on a sensor with a different chip id or a configuration that does not
  match its factory OTP data.
- Sends no firmware, production or key provisioning commands, and writes no TLS key.

## How it works

```text
 fprintd / pam_fprintd
        │
 libfprint + SIGFM + gxfpnav       private copy, loaded only by fprintd
        │   /dev/gxfp
 gxfp51b7.ko                       ACPI mailbox: MMIO window, _DSM doorbell, GPIO IRQ
        │
 Embedded Controller → MCU → sensor
```

On Windows, full images travel through TLS with a key sealed by Intel SGX. This driver
avoids that key. The sensor's cleartext *navigation* mode returns 24 lines, every 4th
line from a configurable start. Sending the configuration with 8 different start lines
and subtracting a no-finger reference gives lines 8 to 111 of the sensor. The driver
then checks that the finger did not move, using the lines that were read twice.

### Repository layout

| Path | Contents |
|---|---|
| `install.sh`, `uninstall.sh` | Installer and uninstaller |
| `kernel/` | Transport module `gxfp51b7.ko` and its uAPI header |
| `libfprint/libfprint/drivers/gxfpnav*` | The libfprint driver and its image pipeline |
| `libfprint/` | libfprint with SIGFM (only `gxfpnav` is built) |
| `src/`, `include/` | Protocol and command layer, from gxfpmoc |
| `tools/gxfpnav-setup.c` | Chip check and per-machine calibration |
| `packaging/selinux/` | Lets `fprintd` open the device on SELinux systems |
| `docs/DISTRIBUTION.md` | How this could reach `apt`, `dnf` and other package managers |

### What gets installed

| Path | Purpose |
|---|---|
| `/usr/local/lib/gxfpnav/` | Private libfprint, used only by `fprintd` |
| `/usr/local/sbin/gxfpnav-setup` | Calibration tool |
| `/usr/src/gxfp51b7-VERSION/` | Module source for DKMS |
| `/var/lib/fprint/gxfpnav/` | This sensor's `config.bin` and `reference.bin` (root only) |
| `/etc/systemd/system/fprintd.service.d/gxfpnav.conf` | Points `fprintd` to the private library |
| SELinux policy, Fedora | `gxfpnav` module and a label for `/dev/gxfp` |

## Security

A fingerprint here is a **convenience, not a strong security boundary**.

- **The sensor is not authenticated.** Images travel in the clear and the link is not
  encrypted, unlike on Windows. Software with root or kernel access could read them or
  feed in fake images. An attacker with that access already controls the system.
- **Accuracy has been measured on a small set, offline.** The set was 600 touches of 20
  fingers from 2 people, scored with this driver's code against prints built from 10 touches:
  - Impostors: 0 of 11,400 attempts accepted, 6,000 of them between the 2 people. The highest
    score was 40, against a threshold of 1000. With the default threshold of 40, that attempt
    would have been accepted, which is why this driver raises it.
  - Genuine, one person, across 2 sessions on the same day: 90.5 % accepted on the first
    touch. About 5 % were asked to touch again and about 5 % were rejected. One finger
    accounts for many of the failures.
  - Genuine, the other person, within one session: 92 of 100 accepted on the first touch.

  Two people are too few to give a false acceptance rate, so treat it as unknown. Sessions on
  different days, dry or wet fingers and other sensors have not been measured.
- **No liveness detection** was tested. Assume that a good copy of a fingerprint could work.
- **Enrolled fingerprints are stored on the disk**, unencrypted unless the disk is, in
  `/var/lib/fprint`, readable only by root. They do not stay in the sensor or in SGX as on
  Windows. Use full-disk encryption if this matters to you.
- Do not use it as the only protection for sensitive data. On login, the GNOME keyring
  will generally not unlock from a fingerprint and will still ask for your password.

## Known limitations

- **Risk to the hardware:** this driver talks directly to an undocumented embedded
  controller and MCU. It has run without damage on one laptop, but there is **no
  warranty**, and it has no official recovery path if a sensor stops responding.
- A match usually takes about 0.5 s from touch to result, estimated from measured stages. Some
  touches need a longer read, during which the finger must stay still: about 1.5 s, and
  about 1.7 s for a rejection. Lifting the finger earlier can report that it was not centered.
  Enrolling always reads the full image, which takes about 1.5 s per touch.
- To be fast, verification accepts a partial image when the match is very strong. Those
  thresholds have only been checked with the fingers of 2 people.
- While waiting for a finger, the sensor is polled. Together, the interrupt thread and
  `fprintd` were measured at about 10 % of one CPU thread for the first 10 seconds and about
  4 % afterwards, with about 240 interrupts per second.
- Stability of the reference at other temperatures has not been measured. If
  recognition gets worse, run `sudo ./install.sh --recalibrate` with no finger on the reader.
- The module has been rebuilt by DKMS on three installed kernels. A live kernel update
  has not been observed yet.
- The sensor's full-image mode through TLS is not used.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `Key was rejected by service` on `modprobe` | The Secure Boot key is not enrolled: run `sudo ./install.sh` again and complete the MOK screen |
| `Authorization denied ... to call method 'Claim'` in the log while enrolling or deleting | Harmless. The password dialog also tries the reader, which is already in use, and falls back to the password |
| `unsupported sensor` | Your reader is not MilanL. Nothing was written to it. Run `sudo ./uninstall.sh` |
| `a finger seems to be on the sensor` | Lift your finger, clean the reader and try again |
| Download of the driver package fails | Get the file named in the error from the [Microsoft Update Catalog](https://www.catalog.update.microsoft.com/Search.aspx?q=GXFP51B7) and use `--cab FILE` |
| `No devices available` | Check `ls /dev/gxfp`, `systemctl status fprintd` and `journalctl -u fprintd -b` |

Please include the output of `sudo gxfpnav-setup probe` and `dmesg | grep -i gxfp` in bug
reports. **Never attach fingerprint images or anything from `/var/lib/fprint`.**

## License and credits

Each component keeps its license: GPL-2.0-only for the kernel module, LGPL-2.1-or-later
for libfprint and the driver, MIT for the protocol layer and scripts. See
[LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

Built on [gxfpmoc](https://github.com/Void755/gxfpmoc) by Void755,
[libfprint](https://gitlab.freedesktop.org/libfprint/libfprint), the SIGFM work of
[goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev/libfprint), and the analysis in
[OpenGoodixSPI issue #16](https://github.com/PeshalaDilshan/OpenGoodixSPI/issues/16).

Not affiliated with or endorsed by Goodix or Huawei.
