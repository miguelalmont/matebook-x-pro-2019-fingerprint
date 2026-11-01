#!/usr/bin/env bash
# Installs Linux fingerprint support for the Goodix GXFP51B7 (MilanL) reader of the
# Huawei MateBook X Pro 2019. Safe to run again: re-run it after the Secure Boot reboot.
# Undo with: sudo ./uninstall.sh
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
VERSION=$(cat "$HERE/VERSION")
TESTED_PRODUCT="MACHR-WX9"
ACPI_DEV=/sys/bus/acpi/devices/GXFP51B7:00
LIBDIR=/usr/local/lib/gxfpnav
SETUP_BIN=/usr/local/sbin/gxfpnav-setup
DATA=/var/lib/fprint/gxfpnav
SRC="/usr/src/gxfp51b7-$VERSION"
DROPIN=/etc/systemd/system/fprintd.service.d/gxfpnav.conf
DKMS_DROPIN=/etc/systemd/system/dkms.service.d/gxfp51b7.conf
KVER=$(uname -r)

# The sensor config comes from the official Windows driver package, which cannot be
# redistributed here. It is fetched from Microsoft and checked against these hashes.
CAB_NAME=d46a8bda-f341-494a-98d3-dd60f5def72a_b94739a48bf5242dffc24d45fd51a763776ecdea.cab
CAB_URL="https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2019/04/$CAB_NAME"
CAB_SHA256=fec70763c4ed282db1c1153f1c033b703375a27afa7be1f901893688062d454b
TPL_OFFSET=502288
TPL_SHA256=b9f1662c0ab804a506f1f4c575db40dfcd2d67f424a80a42dc2a869062096dc6

CAB_FILE=""
RECALIBRATE=0
ALLOW_UNTESTED=0
INSTALL_DEPS=1
BUILD_ONLY=0
TMP=""

usage() {
	cat <<USAGE
Usage: sudo ./install.sh [options]

  --cab FILE              use a local copy of the Windows driver package instead of
                          downloading it ($CAB_NAME)
  --recalibrate           measure the no-finger reference again
  --no-deps               do not install packages with the system package manager
  --allow-untested-model  continue on a laptop other than the tested $TESTED_PRODUCT
  --build-only            only install dependencies and build; touches no hardware
  -h, --help              show this help
USAGE
}

say() { printf '\n== %s\n' "$*"; }
die() { printf '!! %s\n' "$*" >&2; exit 1; }
cleanup() { [ -z "$TMP" ] || rm -rf "$TMP"; }
trap cleanup EXIT

while [ $# -gt 0 ]; do
	case "$1" in
	--cab) [ $# -ge 2 ] || die "--cab needs a file"; CAB_FILE=$(realpath "$2"); shift ;;
	--recalibrate) RECALIBRATE=1 ;;
	--no-deps) INSTALL_DEPS=0 ;;
	--allow-untested-model) ALLOW_UNTESTED=1 ;;
	--build-only) BUILD_ONLY=1 ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
	shift
done

[ "$(id -u)" = 0 ] || die "run with sudo"
TMP=$(mktemp -d /var/tmp/gxfp51b7-install.XXXXXX)

preflight() {
	local vendor product major minor
	[ "$(uname -m)" = x86_64 ] || die "only x86_64 is supported"
	major=${KVER%%.*}
	minor=${KVER#*.}; minor=${minor%%.*}
	if [ "$major" -lt 6 ] || { [ "$major" -eq 6 ] && [ "$minor" -lt 12 ]; }; then
		die "kernel 6.12 or newer is required (running $KVER)"
	fi
	[ -e "$ACPI_DEV" ] || die "no GXFP51B7 reader found ($ACPI_DEV is missing)"
	vendor=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || echo unknown)
	product=$(cat /sys/class/dmi/id/product_name 2>/dev/null || echo unknown)
	echo "Laptop: $vendor $product, kernel $KVER"
	if [ "$product" != "$TESTED_PRODUCT" ] && [ "$ALLOW_UNTESTED" = 0 ]; then
		die "only $TESTED_PRODUCT has been tested. Other laptops with this ACPI id use a different sensor, such as ChicagoHS on MACHC-WAX9. See the README before using --allow-untested-model."
	fi
}

arch_headers() {
	local pkg
	pkg=$(pacman -Qqo "/usr/lib/modules/$KVER/vmlinuz" 2>/dev/null || echo linux)
	echo "$pkg-headers"
}

install_deps() {
	local headers=1
	[ "$BUILD_ONLY" = 0 ] || headers=0
	if command -v dnf >/dev/null; then
		dnf install -y gcc gcc-c++ make meson ninja-build pkgconf-pkg-config glib2-devel \
			libgusb-devel libgudev-devel opencv-devel systemd-devel cabextract curl \
			fprintd fprintd-pam dkms mokutil policycoreutils-python-utils checkpolicy
		[ "$headers" = 0 ] || dnf install -y "kernel-devel-$KVER"
	elif command -v apt-get >/dev/null; then
		apt-get update
		DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential meson ninja-build \
			pkg-config libglib2.0-dev libgusb-dev libgudev-1.0-dev libopencv-dev libudev-dev \
			cabextract curl ca-certificates fprintd libpam-fprintd dkms mokutil
		[ "$headers" = 0 ] || DEBIAN_FRONTEND=noninteractive apt-get install -y "linux-headers-$KVER"
	elif command -v pacman >/dev/null; then
		pacman -Sy --needed --noconfirm base-devel meson ninja pkgconf glib2 glib2-devel libgusb libgudev \
			opencv cabextract curl fprintd dkms
		[ "$headers" = 0 ] || pacman -S --needed --noconfirm "$(arch_headers)"
	elif command -v zypper >/dev/null; then
		zypper --non-interactive install gcc gcc-c++ make meson ninja pkgconf glib2-devel \
			libgusb-devel libgudev-1_0-devel opencv-devel systemd-devel cabextract curl \
			fprintd fprintd-pam dkms mokutil
		[ "$headers" = 0 ] || zypper --non-interactive install kernel-devel
	else
		die "unknown package manager: install the dependencies listed in the README and use --no-deps"
	fi
}

check_fprintd_abi() {
	local bin=""
	for bin in /usr/libexec/fprintd /usr/lib/fprintd/fprintd /usr/lib/x86_64-linux-gnu/fprintd/fprintd /usr/lib/fprintd; do
		[ -f "$bin" ] && [ -x "$bin" ] && break
		bin=""
	done
	[ -n "$bin" ] || { echo "fprintd binary not found: skipping the symbol check"; return 0; }
	if LD_LIBRARY_PATH="$1" ldd -r "$bin" 2>&1 | grep -E 'undefined symbol|not found'; then
		die "the system fprintd does not work with this libfprint build"
	fi
	echo "fprintd resolves every symbol against this libfprint"
}

build_userspace() {
	if [ -f "$LIBDIR/.version" ] && [ "$(cat "$LIBDIR/.version")" = "$VERSION" ] && [ -x "$SETUP_BIN" ] && [ "$BUILD_ONLY" = 0 ]; then
		echo "version $VERSION is already built"
		return 0
	fi
	# meson only needs read access to the tree; the build stays out of it.
	meson setup "$TMP/build" "$HERE/libfprint" --buildtype=debugoptimized \
		-Ddrivers=gxfpnav -Dintrospection=false -Ddoc=false -Dgtk-examples=false \
		-Dudev_rules=disabled -Dudev_hwdb=disabled
	meson compile -C "$TMP/build"
	check_fprintd_abi "$TMP/build/libfprint"
	[ "$BUILD_ONLY" = 0 ] || return 0
	install -d -m 755 "$LIBDIR"
	install -m 755 "$TMP/build/libfprint/libfprint-2.so.2.0.0" "$LIBDIR/"
	ln -sf libfprint-2.so.2.0.0 "$LIBDIR/libfprint-2.so.2"
	install -D -m 755 "$TMP/build/libfprint/gxfpnav-setup" "$SETUP_BIN"
	echo "$VERSION" > "$LIBDIR/.version"
	selinux_on && restorecon -R "$LIBDIR" "$SETUP_BIN" || true
}

selinux_on() { command -v selinuxenabled >/dev/null && selinuxenabled; }

dkms_has() {
	local status
	status=$(dkms status "gxfp51b7/$VERSION" "$@" 2>/dev/null || true)
	case "$status" in
	*installed*) return 0 ;;
	esac
	return 1
}

install_module() {
	local old
	systemctl stop fprintd 2>/dev/null || true
	# Earlier versions of this module would keep their own DKMS entry.
	while read -r old; do
		[ -n "$old" ] && [ "$old" != "gxfp51b7/$VERSION" ] || continue
		dkms remove "$old" --all || true
		rm -rf "/usr/src/${old/\//-}"
	done < <(dkms status gxfp51b7 2>/dev/null | sed 's/[,:].*//' | sort -u)
	if ! dkms_has -k "$KVER"; then
		rm -rf "$SRC"
		install -d "$SRC"
		cp -r "$HERE/kernel/." "$SRC/"
		sed "s/@VERSION@/$VERSION/" "$HERE/kernel/dkms.conf.in" > "$SRC/dkms.conf"
		rm -f "$SRC/dkms.conf.in"
		selinux_on && restorecon -R "$SRC" || true
		[ -n "$(dkms status "gxfp51b7/$VERSION" 2>/dev/null || true)" ] || dkms add "gxfp51b7/$VERSION"
		dkms install "gxfp51b7/$VERSION" -k "$KVER" --force
	fi
	# Other installed kernels: a failure here does not affect the running one.
	local dir k
	for dir in /lib/modules/*; do
		k=${dir##*/}
		[ "$k" != "$KVER" ] && [ -d "$dir/build" ] || continue
		dkms_has -k "$k" && continue
		dkms install "gxfp51b7/$VERSION" -k "$k" --force >/dev/null ||
			echo "warning: could not build the module for kernel $k"
	done
	# On distros that build DKMS modules at boot, load it right after that build.
	if systemctl cat dkms.service >/dev/null 2>&1; then
		install -d -m 755 "$(dirname "$DKMS_DROPIN")"
		printf '[Service]\nExecStartPost=-/usr/sbin/modprobe gxfp51b7\n' > "$DKMS_DROPIN"
		systemctl daemon-reload
		systemctl enable dkms.service
	fi
}

secure_boot_on() {
	local var=/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c
	[ -r "$var" ] && [ "$(od -An -tu1 -j4 -N1 "$var" | tr -d ' ')" = 1 ]
}

signing_cert() {
	local conf cert=""
	for conf in /etc/dkms/framework.conf /etc/dkms/framework.conf.d/*.conf; do
		[ -f "$conf" ] || continue
		cert=$(sed -n 's/^mok_certificate=["'\'']\?\([^"'\'']*\).*/\1/p' "$conf" | tail -1)
		[ -z "$cert" ] || { echo "$cert"; return 0; }
	done
	for cert in /var/lib/shim-signed/mok/MOK.der /var/lib/dkms/mok.pub; do
		[ -f "$cert" ] && { echo "$cert"; return 0; }
	done
	return 1
}

# Returns 1 when a reboot is needed before the module can load.
ensure_mok() {
	local cert out
	secure_boot_on || return 0
	cert=$(signing_cert) || die "Secure Boot is on, but DKMS left no signing certificate"
	command -v mokutil >/dev/null || die "mokutil is needed to enrol $cert"
	# mokutil exits with 1 when the key IS enrolled, so read its output instead.
	out=$(mokutil --test-key "$cert" 2>&1 || true)
	case "$out" in
	*"already enrolled"*) return 0 ;;
	esac
	say "Secure Boot: enrolling the module signing key $cert"
	echo "Choose a one-time password; you will type it once in the blue MOK screen after rebooting."
	mokutil --import "$cert"
	cat <<MSG

Reboot now. In the blue "MOK management" screen choose:
  Enroll MOK > Continue > Yes, then type that password and reboot.
Then run this command again: sudo $HERE/install.sh
MSG
	return 1
}

load_module() {
	rmmod gxfp51b7 2>/dev/null || true
	modprobe gxfp51b7 || die "could not load gxfp51b7: see 'dmesg | tail'"
	udevadm settle 2>/dev/null || true
	[ -e /dev/gxfp ] || die "gxfp51b7 loaded but /dev/gxfp did not appear: see 'dmesg | tail'"
}

fetch_template() {
	local cab="$CAB_FILE"
	if [ -z "$cab" ]; then
		cab="$TMP/$CAB_NAME"
		echo "Downloading the official driver package from Microsoft..."
		curl -fsSL --proto '=https' --retry 2 -o "$cab" "$CAB_URL" ||
			die "download failed. Get $CAB_NAME from the Microsoft Update Catalog and use --cab FILE"
	fi
	echo "$CAB_SHA256  $cab" | sha256sum -c --quiet - || die "$cab is not the expected package"
	mkdir "$TMP/cab"
	# The package has a trailing signature, which cabextract warns about; the hash below decides.
	cabextract -q -F gfspi.dll -d "$TMP/cab" "$cab" 2>/dev/null
	dd if="$TMP/cab/gfspi.dll" of="$TMP/template.bin" bs=1 skip="$TPL_OFFSET" count=256 status=none
	echo "$TPL_SHA256  $TMP/template.bin" | sha256sum -c --quiet - ||
		die "unexpected sensor config in the driver package"
}

calibrate() {
	local rc=0
	"$SETUP_BIN" probe || die "this sensor is not supported; nothing was written to it"
	if [ -f "$DATA/config.bin" ] && [ -f "$DATA/reference.bin" ] && [ "$RECALIBRATE" = 0 ]; then
		echo "keeping the existing calibration in $DATA (use --recalibrate to redo it)"
		return 0
	fi
	fetch_template
	install -d -m 700 /var/lib/fprint "$DATA"
	if [ -t 0 ]; then
		echo
		read -r -p "Keep your finger OFF the reader, then press Enter to calibrate... " _
	fi
	"$SETUP_BIN" --outdir "$DATA" calibrate "$TMP/template.bin" || rc=$?
	[ "$rc" = 0 ] || die "calibration failed (exit $rc): keep the reader clean and uncovered, and run this again"
	selinux_on && restorecon -R "$DATA" || true
}

install_selinux() {
	selinux_on || return 0
	semanage fcontext -a -t usb_device_t /dev/gxfp 2>/dev/null ||
		semanage fcontext -m -t usb_device_t /dev/gxfp
	checkmodule -M -m -o "$TMP/gxfpnav.mod" "$HERE/packaging/selinux/gxfpnav.te"
	semodule_package -o "$TMP/gxfpnav.pp" -m "$TMP/gxfpnav.mod"
	semodule -i "$TMP/gxfpnav.pp"
	restorecon /dev/gxfp
}

install_fprintd_dropin() {
	local pid
	install -d -m 755 "$(dirname "$DROPIN")"
	printf '[Service]\nEnvironment=LD_LIBRARY_PATH=%s\nDeviceAllow=/dev/gxfp rw\n' "$LIBDIR" > "$DROPIN"
	systemctl daemon-reload
	systemctl restart fprintd
	sleep 1
	pid=$(systemctl show -p MainPID --value fprintd)
	grep -q "$LIBDIR" "/proc/$pid/maps" 2>/dev/null ||
		die "fprintd is not using the gxfpnav libfprint: see 'systemctl status fprintd'"
	echo "fprintd is using the gxfpnav libfprint"
}

next_steps() {
	cat <<MSG

Done. Try it now:
  fprintd-enroll
  fprintd-verify

To use the fingerprint for login, sudo and the lock screen:
  Fedora:          sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes
  Debian / Ubuntu: sudo pam-auth-update --enable fprintd
  Others:          add pam_fprintd.so to your PAM configuration
Keep a root shell open while changing PAM, and read the README's security notes first.
MSG
}

if [ "$BUILD_ONLY" = 1 ]; then
	[ "$INSTALL_DEPS" = 0 ] || install_deps
	build_userspace
	echo "BUILD-ONLY OK"
	exit 0
fi

say "Checking this laptop"
preflight
if [ "$INSTALL_DEPS" = 1 ]; then
	say "Installing dependencies"
	install_deps
fi
say "Building libfprint with the gxfpnav driver (a few minutes)"
build_userspace
say "Installing the kernel module with DKMS"
install_module
ensure_mok || exit 0
load_module
say "Configuring SELinux"
install_selinux
say "Preparing this sensor"
calibrate
say "Configuring fprintd"
install_fprintd_dropin
next_steps
