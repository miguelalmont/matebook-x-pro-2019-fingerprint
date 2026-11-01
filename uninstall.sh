#!/usr/bin/env bash
# Removes everything install.sh added; fprintd goes back to the system libfprint.
# Enrolled prints stay in /var/lib/fprint/<user>: remove them first with fprintd-delete.
# The Secure Boot key stays enrolled: 'mokutil --delete' removes it.
set -uo pipefail

[ "$(id -u)" = 0 ] || { echo "!! run with sudo" >&2; exit 1; }

rm -f /etc/systemd/system/fprintd.service.d/gxfpnav.conf
rmdir /etc/systemd/system/fprintd.service.d 2>/dev/null
rm -f /etc/systemd/system/dkms.service.d/gxfp51b7.conf
rmdir /etc/systemd/system/dkms.service.d 2>/dev/null
systemctl daemon-reload
systemctl stop fprintd 2>/dev/null
rmmod gxfp51b7 2>/dev/null

if command -v dkms >/dev/null; then
	dkms status gxfp51b7 2>/dev/null | sed 's/[,:].*//' | sort -u | while read -r mod; do
		[ -z "$mod" ] || dkms remove "$mod" --all
	done
fi
rm -rf /usr/src/gxfp51b7-* /var/lib/dkms/gxfp51b7
rm -f /usr/lib/modules/*/extra/gxfp51b7.ko /usr/lib/modules/*/extra/gxfp51b7.ko.xz
depmod -a

if command -v semanage >/dev/null; then
	semanage fcontext -d /dev/gxfp 2>/dev/null
	semodule -r gxfpnav 2>/dev/null
fi
rm -rf /usr/local/lib/gxfpnav /usr/local/lib64/gxfpnav /var/lib/fprint/gxfpnav
rm -f /usr/local/sbin/gxfpnav-setup

echo "Removed. If you enabled fingerprint login, turn it off too:"
echo "  Fedora:          sudo authselect disable-feature with-fingerprint && sudo authselect apply-changes"
echo "  Debian / Ubuntu: sudo pam-auth-update --remove fprintd"
