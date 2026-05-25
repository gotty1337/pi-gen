#!/bin/bash
# cups_printer_setup.sh – First-boot one-shot: configure cups-pdf and add the
# PDF printer queue.  Run once by cups-printer-setup.service; guarded by a
# sentinel file so it is never executed twice.

set -euo pipefail

# Create the output directory for saved PDFs.
mkdir -p /home/pi/prints
chown pi:pi /home/pi/prints

# Point cups-pdf at our output directory.
if [ -f /etc/cups/cups-pdf.conf ]; then
    sed -i 's|^#\?Out .*|Out /home/pi/prints|' /etc/cups/cups-pdf.conf
fi

# Add the PDF printer queue.
PPD="/usr/share/ppd/cups-pdf/CUPS-PDF_opt.ppd"
if [ ! -f "${PPD}" ]; then
    PPD="/usr/share/ppd/cups-pdf/CUPS-PDF_noopt.ppd"
fi

lpadmin -p PDF -E -v "cups-pdf:/" -P "${PPD}" -o printer-is-shared=false
lpadmin -d PDF

echo "cups_printer_setup: done"
