#!/bin/bash -e
#
# setup_gadget.sh  –  Configure USB gadget and start IPP printer daemon
#
# Called by: ipp-printer.service ExecStart
#

GADGET=/sys/kernel/config/usb_gadget/ipp_printer
FFSDIR=/dev/ffs-ipp0
SRC=/home/pi/ipp-printer/src/IppPrinter
BINARY=$SRC/bin/Release/net10.0/ipp_printer

export DOTNET_ROOT=/usr/share/dotnet
export PATH="$PATH:/usr/share/dotnet"

modprobe libcomposite

# ── Tear down any leftover gadget state from a previous run ──────────────────
if [ -f "$GADGET/UDC" ] && [ -n "$(cat $GADGET/UDC 2>/dev/null)" ]; then
    echo "" > "$GADGET/UDC" 2>/dev/null || true
fi
[ -L "$GADGET/configs/c.1/ffs.ipp0" ] && rm -f "$GADGET/configs/c.1/ffs.ipp0"
[ -L "$GADGET/os_desc/c.1"          ] && rm -f "$GADGET/os_desc/c.1"
[ -f "$GADGET/os_desc/use"          ] && echo 0 > "$GADGET/os_desc/use"
if mountpoint -q "$FFSDIR"; then
    umount "$FFSDIR" 2>/dev/null || true
fi
[ -d "$GADGET/configs/c.1"        ] && rmdir "$GADGET/configs/c.1/strings/0x409" 2>/dev/null || true
[ -d "$GADGET/configs/c.1"        ] && rmdir "$GADGET/configs/c.1"               2>/dev/null || true
[ -d "$GADGET/functions/ffs.ipp0" ] && rmdir "$GADGET/functions/ffs.ipp0"        2>/dev/null || true
[ -d "$GADGET/strings/0x409"      ] && rmdir "$GADGET/strings/0x409"             2>/dev/null || true
[ -d "$GADGET"                    ] && rmdir "$GADGET"                            2>/dev/null || true

# ── Build C# daemon (dotnet skips recompile if source is unchanged) ───────────
echo "Building IPP daemon..."
dotnet build -c Release "$SRC/IppPrinter.csproj" 2>&1 | logger -t ipp-printer

# ── Create gadget ─────────────────────────────────────────────────────────────
mkdir -p "$GADGET"
echo 0x0000 > "$GADGET/idVendor"
echo 0x0002 > "$GADGET/idProduct"
echo 0x0200 > "$GADGET/bcdDevice"
echo 0x0200 > "$GADGET/bcdUSB"

mkdir -p "$GADGET/strings/0x409"
echo "000000001"       > "$GADGET/strings/0x409/serialnumber"
echo "RaspberryPi"     > "$GADGET/strings/0x409/manufacturer"
echo "IPP USB Printer" > "$GADGET/strings/0x409/product"

mkdir -p "$GADGET/configs/c.1/strings/0x409"
echo "IPP Config" > "$GADGET/configs/c.1/strings/0x409/configuration"
echo 500          > "$GADGET/configs/c.1/MaxPower"

mkdir -p "$GADGET/functions/ffs.ipp0"
ln -s "$GADGET/functions/ffs.ipp0" "$GADGET/configs/c.1/ffs.ipp0"

# ── Mount FunctionFS ──────────────────────────────────────────────────────────
mkdir -p "$FFSDIR"
mount -t functionfs ipp0 "$FFSDIR"

# ── Start daemon (writes descriptors, waits for bind) ────────────────────────
echo "Starting IPP daemon..."
"$BINARY" &
DAEMON_PID=$!
echo "$DAEMON_PID" > /run/ipp-printer.pid

# ── Give daemon time to write descriptors, then bind to UDC ──────────────────
sleep 1

UDC=$(ls /sys/class/udc | head -1)
if [ -z "$UDC" ]; then
    echo "ERROR: No UDC found" >&2
    kill "$DAEMON_PID" 2>/dev/null || true
    exit 1
fi
echo "$UDC" > "$GADGET/UDC"
echo "Bound to UDC: $UDC  |  Daemon PID: $DAEMON_PID"


# ── Tear down any leftover gadget state from a previous run ──────────────────
if [ -f "$GADGET/UDC" ] && [ -n "$(cat $GADGET/UDC 2>/dev/null)" ]; then
    echo "" > "$GADGET/UDC" 2>/dev/null || true
fi
[ -L "$GADGET/configs/c.1/ffs.ipp0" ] && rm -f "$GADGET/configs/c.1/ffs.ipp0"
[ -L "$GADGET/os_desc/c.1"          ] && rm -f "$GADGET/os_desc/c.1"
[ -f "$GADGET/os_desc/use"          ] && echo 0 > "$GADGET/os_desc/use"
if mountpoint -q "$FFSDIR"; then
    umount "$FFSDIR" 2>/dev/null || true
fi
[ -d "$GADGET/configs/c.1"   ] && rmdir "$GADGET/configs/c.1/strings/0x409" 2>/dev/null || true
[ -d "$GADGET/configs/c.1"   ] && rmdir "$GADGET/configs/c.1" 2>/dev/null || true
[ -d "$GADGET/functions/ffs.ipp0" ] && rmdir "$GADGET/functions/ffs.ipp0" 2>/dev/null || true
[ -d "$GADGET/strings/0x409" ] && rmdir "$GADGET/strings/0x409" 2>/dev/null || true
[ -d "$GADGET" ] && rmdir "$GADGET" 2>/dev/null || true

# ── Create gadget ─────────────────────────────────────────────────────────────
mkdir -p "$GADGET"
echo 0x0000 > "$GADGET/idVendor"
echo 0x0002 > "$GADGET/idProduct"
echo 0x0200 > "$GADGET/bcdDevice"
echo 0x0200 > "$GADGET/bcdUSB"

mkdir -p "$GADGET/strings/0x409"
echo "000000001"       > "$GADGET/strings/0x409/serialnumber"
echo "RaspberryPi"     > "$GADGET/strings/0x409/manufacturer"
echo "IPP USB Printer" > "$GADGET/strings/0x409/product"

mkdir -p "$GADGET/configs/c.1/strings/0x409"
echo "IPP Config" > "$GADGET/configs/c.1/strings/0x409/configuration"
echo 500          > "$GADGET/configs/c.1/MaxPower"

mkdir -p "$GADGET/functions/ffs.ipp0"
ln -s "$GADGET/functions/ffs.ipp0" "$GADGET/configs/c.1/ffs.ipp0"

# ── Mount FunctionFS ──────────────────────────────────────────────────────────
mkdir -p "$FFSDIR"
mount -t functionfs ipp0 "$FFSDIR"

# ── Start daemon (writes descriptors, waits for bind) ────────────────────────
echo "Starting IPP daemon..."
"$DAEMON" &
DAEMON_PID=$!
echo "$DAEMON_PID" > /run/ipp-printer.pid

# ── Give daemon time to write descriptors, then bind to UDC ──────────────────
sleep 1

UDC=$(ls /sys/class/udc | head -1)
if [ -z "$UDC" ]; then
    echo "ERROR: No UDC found" >&2
    kill "$DAEMON_PID" 2>/dev/null || true
    exit 1
fi
echo "$UDC" > "$GADGET/UDC"
echo "Bound to UDC: $UDC  |  Daemon PID: $DAEMON_PID"
