#!/bin/bash
# gadget_build.sh – Compile usb_gadget from source if necessary.
#
# Rebuilds the binary whenever the source is newer or the binary is missing.
# Edit usb_gadget.c and restart the service to pick up changes.

set -euo pipefail

GADGET_DIR="$(dirname "$(realpath "$0")")"
SRC="${GADGET_DIR}/usb_gadget.c"
BIN="${GADGET_DIR}/usb_gadget"

if [ ! -f "${BIN}" ] || [ "${SRC}" -nt "${BIN}" ]; then
    echo "gadget_build: compiling ${SRC}"
    gcc -O2 -Wall -Wextra -o "${BIN}" "${SRC}" -lcups
    chmod 755 "${BIN}"
    echo "gadget_build: done"
else
    echo "gadget_build: binary is up to date"
fi
