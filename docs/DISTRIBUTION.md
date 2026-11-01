# Getting this driver into package managers

An introduction to making this driver installable with `apt`, `dnf` and similar tools,
and to the harder goal of having it installed automatically on machines with this reader.

Most of this is not decided by the companies behind each distribution. Community
maintainers apply public rules, and anyone can publish their own repository without
approval. The realistic path has three levels.

## Level 1: your own repositories (no review)

Users add one repository, then install normally.

| Family | Service | User runs | Notes |
|---|---|---|---|
| Fedora, RHEL | [Copr](https://copr.fedorainfracloud.org/) | `dnf copr enable OWNER/PROJECT` then `dnf install ...` | The Fedora Project does not review what a Copr contains, beyond its build rules |
| Ubuntu | [Launchpad PPA](https://launchpad.net/ubuntu/+ppas) | `add-apt-repository ppa:OWNER/NAME` then `apt install ...` | Source packages are uploaded and built by Launchpad |
| Debian | Self-hosted apt repository, or OBS | `deb` line plus signing key, then `apt install ...` | Debian has no PPA service |
| Arch | [AUR](https://wiki.archlinux.org/title/AUR_submission_guidelines) | `yay -S ...` or `makepkg` | Follow the [DKMS package guidelines](https://wiki.archlinux.org/title/DKMS_package_guidelines): depend on `dkms` and let the pacman hook build the module |
| Several at once | [openSUSE Build Service](https://build.opensuse.org/) | Depends on the distribution | One project can build for openSUSE, Fedora, Debian, Ubuntu and Arch |

**Suggested packages**, each built from a tagged release:

1. `gxfp51b7-dkms`: the contents of `kernel/` in `/usr/src/gxfp51b7-VERSION`.
2. `libfprint-gxfpnav`: the private library in its own directory, plus the `fprintd`
   systemd drop-in and, on Fedora, the SELinux module.
3. `gxfpnav-setup`: the tool, plus the template download from `install.sh`.

A package must not calibrate or download while installing, because scripts run
unattended and the finger must stay off the sensor. Installing should only explain the
one remaining step, a command that runs the calibration while the user is present. Secure Boot enrolment
also stays interactive: the user must confirm the key at the next boot.

## Level 2: the official repositories

| Distribution | Process | What blocks this project today |
|---|---|---|
| Debian, and Ubuntu by syncing | A Debian Developer sponsors the upload and the FTP masters review it | Needing software from outside the archive points to [`contrib`](https://www.debian.org/doc/debian-policy/ch-archive.html). `firmware-b43-installer` is a precedent for a contrib package that downloads what it needs. |
| Fedora | [Package review](https://docs.fedoraproject.org/en-US/packaging-guidelines/) by another packager | [RPM Fusion's guidelines](https://rpmfusion.net/Guidelines) state that separate kernel-module packages are forbidden in Fedora. RPM Fusion accepts them as `kmod`/`akmod`. Replacing libfprint is a second obstacle. |
| Arch | Trusted Users adopt popular AUR packages | Needs demand and a maintainer |
| openSUSE | Submit through OBS to a devel project, then Factory | Same libfprint and vendor-file questions |

## Level 3: no extra packages at all (upstream)

This is the only route that makes it work out of the box everywhere, without
per-distribution work:

1. **Kernel:** get the transport module accepted into mainline Linux. Distributions
   then ship it and it loads by itself through its ACPI alias. Expect review of the
   driver's quality and of its IRQ behaviour.
2. **libfprint:** get the driver merged [upstream](https://gitlab.freedesktop.org/libfprint/libfprint).
   This is the harder step: it relies on SIGFM, which remains in a community fork
   because upstream's matcher finds too few minutiae on small sensors. It would need
   SIGFM, or a replacement, to be accepted first.
3. **Configuration:** a required file that cannot be redistributed is an obstacle for
   both. The clean solution is permission from Goodix, for example for
   [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware), or a
   configuration built without their data.

## Automatic installation when the hardware is present

Linux describes hardware with modalias strings. This reader is `acpi:GXFP51B7:`, as
`/sys/bus/acpi/devices/GXFP51B7:00/modalias` shows. Packages can declare what they serve:

| Mechanism | Declared with | Used by |
|---|---|---|
| [AppStream](https://www.freedesktop.org/software/appstream/docs/chap-Metadata.html) | `<provides><modalias>acpi:GXFP51B7:*</modalias></provides>` | `appstreamcli what-provides modalias` and tools built on it, such as Debian's [isenkram](https://wiki.debian.org/Isenkram) |
| Debian/Ubuntu control field | [`XB-Modaliases`, through `dh_modaliases`](https://manpages.ubuntu.com/manpages/noble/man1/dh_modaliases.1.html) | `ubuntu-drivers` and other driver tools |
| In-tree module | `MODULE_DEVICE_TABLE`, already in this driver | udev loads an installed module automatically |

These tools usually suggest a package rather than install it unprompted, and only look
in repositories the machine already has enabled. So automatic installation in practice
requires Level 2 or 3.

**Warning specific to this reader:** the ACPI id alone is not enough. `GXFP51B7` also
covers other sensors, such as the ChicagoHS on the MACHC-WAX9. Any automatic rule must
also match the laptop, for example with a DMI modalias containing `pnMACHR-WX9`, and
the chip check in `gxfpnav-setup` has to remain.

## Recommended order

1. Publish tagged releases with `install.sh`, as this repository does.
2. Gather reports from other owners to learn which models really match.
3. Add an AUR package and a Copr project, which are the fastest to publish and need no review.
4. Add a PPA or an OBS project for Debian and Ubuntu, with the modalias metadata.
5. Clean up and propose the kernel module upstream, then start the discussion with libfprint.
