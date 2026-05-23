#!/bin/bash -e
#
# pi-gen stage2/99-ipp-printer/01-run.sh
#
# Installs .NET SDK 10, deploys the C# IPP-over-USB daemon source, and
# pre-restores NuGet packages so the Pi can compile and run offline at boot.
#
# Uses the official dotnet-install.sh script (same approach as commtest-hive-image)
# to avoid APT repo GPG issues on Debian Bookworm/Trixie.
#

# ── Install .NET SDK 10 ───────────────────────────────────────────────────────
on_chroot << 'CHROOT'
echo "check-certificate = off" >> /etc/wgetrc
wget https://dot.net/v1/dotnet-install.sh -O /tmp/dotnet-install.sh
chmod +x /tmp/dotnet-install.sh
/tmp/dotnet-install.sh --channel 10.0 --install-dir /usr/share/dotnet
ln -sf /usr/share/dotnet/dotnet /usr/bin/dotnet
rm /tmp/dotnet-install.sh
sed -i '/check-certificate/d' /etc/wgetrc
CHROOT

# ── Deploy shell scripts and service file ─────────────────────────────────────
install -v -d "${ROOTFS_DIR}/home/pi/ipp-printer"
install -v -m 755 files/setup_gadget.sh     "${ROOTFS_DIR}/home/pi/ipp-printer/"
install -v -m 755 files/teardown_gadget.sh  "${ROOTFS_DIR}/home/pi/ipp-printer/"
install -v -m 644 files/ipp-printer.service "${ROOTFS_DIR}/etc/systemd/system/"

# Strip Windows CRLF line endings (files edited on Windows may have \r\n)
sed -i 's/\r$//' "${ROOTFS_DIR}/home/pi/ipp-printer/setup_gadget.sh"
sed -i 's/\r$//' "${ROOTFS_DIR}/home/pi/ipp-printer/teardown_gadget.sh"

# ── Deploy C# source (the Pi builds the binary at service start) ──────────────
install -v -d "${ROOTFS_DIR}/home/pi/ipp-printer/src/IppPrinter"
install -v -m 644 src/IppPrinter/IppPrinter.csproj "${ROOTFS_DIR}/home/pi/ipp-printer/src/IppPrinter/"
install -v -m 644 src/IppPrinter/Program.cs         "${ROOTFS_DIR}/home/pi/ipp-printer/src/IppPrinter/"
install -v -m 644 src/IppPrinter/IppGadget.cs       "${ROOTFS_DIR}/home/pi/ipp-printer/src/IppPrinter/"

# ── Pre-restore NuGet packages while build host has internet access ───────────
# Bakes the package cache into the image so the first boot compiles offline.
on_chroot << 'CHROOT'
export DOTNET_ROOT=/usr/share/dotnet
export PATH="$PATH:/usr/share/dotnet"
dotnet restore /home/pi/ipp-printer/src/IppPrinter/IppPrinter.csproj
chown -R pi:pi /home/pi/ipp-printer
systemctl enable ipp-printer.service
CHROOT
