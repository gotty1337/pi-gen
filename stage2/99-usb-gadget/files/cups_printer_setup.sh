#!/bin/bash
# cups_printer_setup.sh – First-boot one-shot: configure cups-pdf and add the
# PDF printer queue.  Run once by cups-printer-setup.service; guarded by a
# sentinel file so it is never executed twice.

set -euo pipefail

# Create the output directory for saved PDFs.
mkdir -p /home/pi/prints
chown pi:pi /home/pi/prints

# Point cups-pdf at our output directory, and allow anonymous (Windows) users.
if [ -f /etc/cups/cups-pdf.conf ]; then
    sed -i \
        's|^#\?Out .*|Out /home/pi/prints|; s|^#\?AnonDirName .*|AnonDirName /home/pi/prints|' \
        /etc/cups/cups-pdf.conf
fi

# Allow CUPS to accept IPP requests addressed to any hostname/port.
# ipp-usb probes via its own proxy (e.g. http://localhost:60000/ipp/print);
# without ServerAlias * CUPS rejects the printer-uri as "not my server"
# and returns client-error-not-found, causing ipp-usb to reset the device.
if ! grep -q "^ServerAlias \*" /etc/cups/cupsd.conf; then
    echo "ServerAlias *" >> /etc/cups/cupsd.conf
fi

# Enable CUPS access log and global printer sharing.
# --share-printers activates the /ipp/print endpoint that ipp-usb probes.
cupsctl LogLevel=info --share-printers

# Add the PDF printer queue.  Shared=true so Windows can reach it at /ipp/print.
PPD="/usr/share/ppd/cups-pdf/CUPS-PDF_opt.ppd"
if [ ! -f "${PPD}" ]; then
    PPD="/usr/share/ppd/cups-pdf/CUPS-PDF_noopt.ppd"
fi

lpadmin -p PDF -E -v "cups-pdf:/" -P "${PPD}" -o printer-is-shared=true
lpadmin -d PDF

echo "cups_printer_setup: done"
