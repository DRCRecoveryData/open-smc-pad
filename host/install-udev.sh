#!/usr/bin/env bash
# One-time: let your desktop session open the SMC-PAD application's CDC port, so the web UI
# can do the 1200-baud touch that asks the running app into the bootloader (DFU).
#
#   sudo host/install-udev.sh        (or: host/install-udev.sh, it uses sudo itself)
#
# The port is root:dialout 0660 by default; TAG+="uaccess" adds an ACL for the logged-in user
# at device creation. Power-cycle the pad if the ACL does not appear right away.
set -euo pipefail

RULE=/etc/udev/rules.d/61-smcpad-app.rules
tmp=$(mktemp)
cat >"$tmp" <<'EOF'
# SMC-PAD Pocket application ("open fw", 1209:0001): CDC tty access for the desktop user,
# needed by host/webui/server.py for the 1200-baud touch into the bootloader.
SUBSYSTEM=="tty", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", TAG+="uaccess", MODE="0660", GROUP="dialout"
EOF

sudo install -m 0644 "$tmp" "$RULE"
rm -f "$tmp"
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=tty
sleep 0.5

echo "rule installed: $RULE"
ls -l /dev/ttyACM* 2>/dev/null || echo "no /dev/ttyACM* right now (is the pad on?)"
getfacl -p /dev/ttyACM0 2>/dev/null | sed -n '1,12p' || true
echo "done. If your user has no rw ACL on /dev/ttyACM0, power-cycle the pad once."
