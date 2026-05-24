#!/bin/bash
# gadget_build.sh – Compile usb_gadget from source if necessary.
#
# Rebuilds /usr/local/sbin/usb_gadget whenever the source file is newer than
# the binary, or when the binary does not yet exist.  This lets you edit
# /usr/local/src/usb-gadget/usb_gadget.c and pick up changes on the next
# service start without rebuilding the whole SD-card image.

set -euo pipefail

SRC="/usr/local/src/usb-gadget/usb_gadget.c"
BIN="/usr/local/sbin/usb_gadget"

if [ ! -f "${BIN}" ] || [ "${SRC}" -nt "${BIN}" ]; then
    echo "gadget_build: compiling ${SRC} -> ${BIN}"
    gcc -O2 -Wall -Wextra -o "${BIN}" "${SRC}"
    chmod 755 "${BIN}"
    echo "gadget_build: done"
else
    echo "gadget_build: binary is up to date"
fi
