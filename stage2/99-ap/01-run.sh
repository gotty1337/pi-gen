#!/bin/bash
set -e

ROOTFS_DIR="${ROOTFS_DIR}"

echo "Installing AP configuration..."

install -m 644 files/hostapd.conf \
    "${ROOTFS_DIR}/etc/hostapd/hostapd.conf"

install -m 644 files/dnsmasq.conf \
    "${ROOTFS_DIR}/etc/dnsmasq.conf"

# Tell NetworkManager to leave wlan0 alone so hostapd can manage it
install -v -m 644 /dev/stdin \
    "${ROOTFS_DIR}/etc/NetworkManager/conf.d/99-unmanaged-wlan0.conf" <<EOF
[keyfile]
unmanaged-devices=interface-name:wlan0
EOF

# Set static IP on wlan0 via systemd-networkd
install -v -m 644 /dev/stdin \
    "${ROOTFS_DIR}/etc/systemd/network/10-wlan0-ap.network" <<EOF
[Match]
Name=wlan0

[Network]
Address=192.168.4.1/24
EOF

sed -i \
's|#DAEMON_CONF=""|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' \
"${ROOTFS_DIR}/etc/default/hostapd"

echo "net.ipv4.ip_forward=1" >> \
"${ROOTFS_DIR}/etc/sysctl.conf"

# Enable UART
echo "enable_uart=1" >> \
    "${ROOTFS_DIR}/boot/firmware/config.txt"

# Enable USB device mode (idempotent)
if ! grep -qF 'dtoverlay=dwc2,dr_mode=peripheral' \
        "${ROOTFS_DIR}/boot/firmware/config.txt"; then
    echo "dtoverlay=dwc2,dr_mode=peripheral" >> \
        "${ROOTFS_DIR}/boot/firmware/config.txt"
fi

# Enable dwc2 kernel module during boot (idempotent – append once to the
# single non-comment line that makes up cmdline.txt)
if ! grep -qE '(^| )modules-load=[^ ]*dwc2' \
        "${ROOTFS_DIR}/boot/firmware/cmdline.txt"; then
    sed -i '/^[^#]/ s/$/ modules-load=dwc2/' \
        "${ROOTFS_DIR}/boot/firmware/cmdline.txt"
fi

touch "${ROOTFS_DIR}/boot/firmware/ssh"

echo "country=AT" > \
   "${ROOTFS_DIR}/etc/wpa_supplicant/wpa_supplicant.conf"

on_chroot << EOF
systemctl unmask hostapd
systemctl enable hostapd
systemctl enable dnsmasq
systemctl enable ssh
systemctl enable systemd-networkd

systemctl mask wpa_supplicant.service || true
EOF
