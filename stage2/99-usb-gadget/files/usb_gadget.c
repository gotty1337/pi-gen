/* usb_gadget.c – USB FunctionFS IPP-over-USB printer gadget daemon.
 *
 * Presents a USB Printer Class interface (class 0x07, subclass 0x01,
 * protocol 0x04 = IPP over USB) to the host and bridges the bulk
 * endpoints bidirectionally to the local CUPS daemon via its Unix
 * domain socket at /run/cups/cups.sock.
 *
 * Windows' built-in driverless IPP-over-USB driver picks this up
 * automatically; no INF file or separate driver install is required.
 * Print jobs are processed by CUPS and saved as PDFs via cups-pdf.
 *
 * Usage:
 *   usb_gadget [ffs-mount-dir]
 *
 * The FFS directory must already be mounted (see gadget_setup.sh).
 * Defaults to /dev/ffs-loopback.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/usb/functionfs.h>
#include <linux/usb/ch9.h>

/*
 * htole16/32 from <endian.h> are glibc inline functions and are not
 * accepted as constant expressions in static initialisers.
 * These macros use __builtin_bswap* which GCC treats as compile-time
 * constants.
 */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define LE16(x)  ((uint16_t)(x))
#  define LE32(x)  ((uint32_t)(x))
#else
#  define LE16(x)  ((uint16_t)__builtin_bswap16(x))
#  define LE32(x)  ((uint32_t)__builtin_bswap32(x))
#endif

/* ------------------------------------------------------------------ */
/* USB descriptor helpers                                              */
/* ------------------------------------------------------------------ */

struct ep_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

#define EP_DESC(addr, attr, pkt) {              \
    .bLength          = 7,                      \
    .bDescriptorType  = USB_DT_ENDPOINT,        \
    .bEndpointAddress = (addr),                 \
    .bmAttributes     = (attr),                 \
    .wMaxPacketSize   = LE16(pkt),              \
    .bInterval        = 0,                      \
}

/* ------------------------------------------------------------------ */
/* Descriptor table (full-speed + high-speed)                         */
/* Printer class: bInterfaceClass=0x07, subclass=0x01, protocol=0x04  */
/* Protocol 0x04 = IPP over USB (triggers Windows driverless driver)  */
/* ------------------------------------------------------------------ */

static const struct {
    struct usb_functionfs_descs_head_v2 head;
    __le32 fs_count;
    __le32 hs_count;
    struct usb_interface_descriptor fs_intf;
    struct ep_desc                  fs_in;
    struct ep_desc                  fs_out;
    struct usb_interface_descriptor hs_intf;
    struct ep_desc                  hs_in;
    struct ep_desc                  hs_out;
} __attribute__((packed)) descriptors = {
    .head = {
        .magic  = LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
        .length = LE32(sizeof(descriptors)),
        .flags  = LE32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
    },
    .fs_count = LE32(3),
    .hs_count = LE32(3),

    .fs_intf = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = USB_CLASS_PRINTER,
        .bInterfaceSubClass = 0x01,           /* Printer */
        .bInterfaceProtocol = 0x04,           /* IPP over USB */
        .iInterface         = 1,
    },
    .fs_in  = EP_DESC(USB_DIR_IN  | 1, USB_ENDPOINT_XFER_BULK,  64),
    .fs_out = EP_DESC(USB_DIR_OUT | 2, USB_ENDPOINT_XFER_BULK,  64),

    .hs_intf = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = USB_CLASS_PRINTER,
        .bInterfaceSubClass = 0x01,           /* Printer */
        .bInterfaceProtocol = 0x04,           /* IPP over USB */
        .iInterface         = 1,
    },
    .hs_in  = EP_DESC(USB_DIR_IN  | 1, USB_ENDPOINT_XFER_BULK, 512),
    .hs_out = EP_DESC(USB_DIR_OUT | 2, USB_ENDPOINT_XFER_BULK, 512),
};

/* ------------------------------------------------------------------ */
/* String table                                                        */
/* ------------------------------------------------------------------ */

#define IFACE_STR "IPP Printer"

static const struct {
    struct usb_functionfs_strings_head head;
    struct {
        __le16 code;
        char   str[sizeof(IFACE_STR)];
    } __attribute__((packed)) lang;
} __attribute__((packed)) strings = {
    .head = {
        .magic      = LE32(FUNCTIONFS_STRINGS_MAGIC),
        .length     = LE32(sizeof(strings)),
        .str_count  = LE32(1),
        .lang_count = LE32(1),
    },
    .lang = {
        .code = LE16(0x0409), /* en-US */
        .str  = IFACE_STR,
    },
};

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

#define BULK_BUF_SIZE    65536
#define FFS_DEFAULT      "/dev/ffs-loopback"
#define CUPS_SOCKET      "/run/cups/cups.sock"

/*
 * IEEE 1284 Device ID string.  CMD:IPP is what triggers Windows' built-in
 * driverless IPP-over-USB driver; the rest is informational.
 */
#define PRINTER_DEV_ID \
    "MFG:Raspberry Pi;MDL:USB Printer;CMD:IPP,PDF,PWGRaster;" \
    "CLS:PRINTER;DRV:DRVLESS;"

static volatile sig_atomic_t running = 1;
static int cups_fd = -1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/* CUPS socket helpers                                                 */
/* ------------------------------------------------------------------ */

static void cups_close(void)
{
    if (cups_fd >= 0) {
        close(cups_fd);
        cups_fd = -1;
    }
}

static int cups_connect(void)
{
    if (cups_fd >= 0)
        return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CUPS_SOCKET, sizeof(addr.sun_path) - 1);

    cups_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cups_fd < 0) {
        perror("cups socket");
        return -1;
    }

    if (connect(cups_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("cups connect");
        close(cups_fd);
        cups_fd = -1;
        return -1;
    }
    fprintf(stderr, "gadget: connected to CUPS\n");
    return 0;
}

/* Write all len bytes to fd, retrying on EINTR. */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* USB Printer class control request handler                          */
/* bRequest values from USB Printer Class Specification 1.1 §4.2     */
/* ------------------------------------------------------------------ */

#define PRINTER_REQ_GET_DEVICE_ID   0x00
#define PRINTER_REQ_GET_PORT_STATUS 0x01
#define PRINTER_REQ_SOFT_RESET      0x02

static void handle_setup(int ep0, const struct usb_ctrlrequest *req)
{
    uint8_t  type = req->bRequestType;
    uint8_t  rq   = req->bRequest;
    uint16_t wlen = (uint16_t)req->wLength; /* LE on LE system */

    if ((type & USB_TYPE_MASK)  == USB_TYPE_CLASS &&
        (type & USB_RECIP_MASK) == USB_RECIP_INTERFACE) {

        if (rq == PRINTER_REQ_GET_DEVICE_ID && (type & USB_DIR_IN)) {
            /* IEEE 1284: 2-byte big-endian total length then PnP string. */
            static const char pnp[] = PRINTER_DEV_ID;
            uint16_t total = (uint16_t)(2 + sizeof(pnp) - 1);
            uint8_t  buf[2 + sizeof(pnp) - 1];
            buf[0] = (uint8_t)(total >> 8);
            buf[1] = (uint8_t)(total & 0xff);
            memcpy(buf + 2, pnp, sizeof(pnp) - 1);
            size_t send = (wlen < total) ? wlen : total;
            write(ep0, buf, send);

        } else if (rq == PRINTER_REQ_GET_PORT_STATUS && (type & USB_DIR_IN)) {
            /*
             * Port status byte (USB Printer Class 1.1 Table 3):
             *   bit5 = Paper Empty (0 = paper present)
             *   bit4 = Select      (1 = selected/online)
             *   bit3 = Not Error   (1 = no error condition)
             */
            uint8_t status = 0x18; /* selected, no error, paper loaded */
            write(ep0, &status, (wlen < 1) ? 0 : 1);

        } else if (rq == PRINTER_REQ_SOFT_RESET && !(type & USB_DIR_IN)) {
            /* OUT request, no data — ACK and drop the CUPS connection. */
            read(ep0, NULL, 0);
            cups_close();

        } else {
            /* Unknown class request — send empty response. */
            if (type & USB_DIR_IN)
                write(ep0, NULL, 0);
            else
                read(ep0, NULL, 0);
        }
    } else {
        if (type & USB_DIR_IN)
            write(ep0, NULL, 0);
        else
            read(ep0, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* ep0 event processing                                               */
/* Returns:  1 = ENABLE,  -2 = DISABLE,  -1 = fatal,  0 = other      */
/* ------------------------------------------------------------------ */

static int process_ep0_event(int ep0)
{
    struct usb_functionfs_event ev;

    ssize_t n = read(ep0, &ev, sizeof(ev));
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        perror("ep0 read");
        return -1;
    }
    if ((size_t)n < sizeof(ev))
        return 0;

    switch (ev.type) {
    case FUNCTIONFS_BIND:
        fprintf(stderr, "gadget: BIND\n");
        break;
    case FUNCTIONFS_UNBIND:
        fprintf(stderr, "gadget: UNBIND\n");
        cups_close();
        running = 0;
        break;
    case FUNCTIONFS_ENABLE:
        fprintf(stderr, "gadget: ENABLE\n");
        return 1;
    case FUNCTIONFS_DISABLE:
        fprintf(stderr, "gadget: DISABLE\n");
        cups_close();
        return -2;
    case FUNCTIONFS_SETUP:
        handle_setup(ep0, &ev.u.setup);
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *dir = (argc > 1) ? argv[1] : FFS_DEFAULT;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    char ep0_path[256], ep1_path[256], ep2_path[256];
    snprintf(ep0_path, sizeof(ep0_path), "%s/ep0", dir);
    snprintf(ep1_path, sizeof(ep1_path), "%s/ep1", dir);
    snprintf(ep2_path, sizeof(ep2_path), "%s/ep2", dir);

    int ep0 = open(ep0_path, O_RDWR);
    if (ep0 < 0) {
        perror(ep0_path);
        return EXIT_FAILURE;
    }

    if (write(ep0, &descriptors, sizeof(descriptors)) != sizeof(descriptors)) {
        perror("write descriptors");
        close(ep0);
        return EXIT_FAILURE;
    }
    if (write(ep0, &strings, sizeof(strings)) != sizeof(strings)) {
        perror("write strings");
        close(ep0);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "gadget: descriptors written, waiting for host...\n");

    int ep_in = -1, ep_out = -1;
    static char buf[BULK_BUF_SIZE];

    while (running) {
        /*
         * poll() ignores entries where fd < 0 (sets revents = 0).
         * cups_fd and ep_out are -1 until ENABLE; no special casing needed.
         */
        struct pollfd fds[3] = {
            { .fd = ep0,     .events = POLLIN },
            { .fd = ep_out,  .events = POLLIN },
            { .fd = cups_fd, .events = POLLIN },
        };

        if (poll(fds, 3, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* ep0 control events */
        if (fds[0].revents & POLLIN) {
            int rc = process_ep0_event(ep0);
            if (rc == 1) {
                ep_in  = open(ep1_path, O_RDWR);
                ep_out = open(ep2_path, O_RDWR);
                if (ep_in < 0 || ep_out < 0) {
                    perror("open data endpoints");
                    break;
                }
                /* Eagerly connect to CUPS; retry lazily on first write if not ready. */
                if (cups_connect() < 0)
                    fprintf(stderr, "gadget: CUPS not ready, will retry\n");
                fprintf(stderr, "gadget: IPP bridge active\n");
            } else if (rc == -2) {
                if (ep_in  >= 0) { close(ep_in);  ep_in  = -1; }
                if (ep_out >= 0) { close(ep_out); ep_out = -1; }
                fprintf(stderr, "gadget: IPP bridge suspended\n");
            } else if (rc < 0) {
                break;
            }
        }

        /* USB OUT → CUPS */
        if (ep_out >= 0 && (fds[1].revents & POLLIN)) {
            ssize_t n = read(ep_out, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR || errno == ESHUTDOWN)
                    continue;
                perror("ep_out read");
                break;
            }
            if (n > 0) {
                if (cups_connect() < 0) {
                    fprintf(stderr, "gadget: CUPS unavailable, dropping %zd bytes\n", n);
                } else if (write_all(cups_fd, buf, (size_t)n) < 0) {
                    fprintf(stderr, "gadget: CUPS write error, reconnecting\n");
                    cups_close();
                }
            }
        }

        /* CUPS → USB IN */
        if (cups_fd >= 0 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(cups_fd, buf, sizeof(buf));
            if (n <= 0) {
                fprintf(stderr, "gadget: CUPS closed connection\n");
                cups_close();
            } else if (ep_in >= 0) {
                if (write_all(ep_in, buf, (size_t)n) < 0) {
                    if (errno != ESHUTDOWN)
                        perror("ep_in write");
                }
            }
        }
    }

    if (ep_in  >= 0) close(ep_in);
    if (ep_out >= 0) close(ep_out);
    cups_close();
    close(ep0);

    fprintf(stderr, "gadget: stopped\n");
    return EXIT_SUCCESS;
}
