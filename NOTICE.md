# Notices

## Third-party code

| Component | Source | License |
|---|---|---|
| `src/`, `include/` | Subset of [Void755/gxfpmoc](https://github.com/Void755/gxfpmoc), commit `27d709d` | MIT |
| `kernel/` | Adapted from the `kernel/` transport of gxfpmoc | GPL-2.0-only |
| `libfprint/` | [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint), through the SIGFM fork of [goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev/libfprint), as carried in gxfpmoc | LGPL-2.1-or-later |

The transport layout of the GXFP51B7 was first described in
[OpenGoodixSPI issue #16](https://github.com/PeshalaDilshan/OpenGoodixSPI/issues/16).

## Changes to the gxfpmoc copy of libfprint

- Added the `gxfpnav` driver: `libfprint/drivers/gxfpnav.c`, `gxfpnav_core.c`, `gxfpnav_core.h`.
- Added `gxfpnav-setup` to `libfprint/meson.build`.
- Removed the TLS `gxfp` driver, along with its build rules and dependencies.
- Removed `tests/`, `demo/`, `doc/`, the SIGFM unit tests and the upstream CI files.
- Made SIGFM accept OpenCV 5 as well as OpenCV 4.

`src/io/dev.c` gained `GXFP_TRANSPORT_EC_MAILBOX`, which sends the frame without the eSPI wrapper.

## Goodix and Huawei

This project is not affiliated with or endorsed by Goodix or Huawei. Their names
identify the hardware it supports.

The repository contains no Goodix code or data. The sensor needs a 256-byte
configuration found in Goodix's Windows driver, whose license does not allow copying
it here. `install.sh` therefore downloads the official package from Microsoft's
servers, checks its SHA-256 and reads the configuration locally on the machine being
set up. The resulting `config.bin` stays in `/var/lib/fprint/gxfpnav`.
