/* usb_gadget.c – Simple USB FunctionFS bulk-loopback gadget daemon.
 *
 * Reads data from the USB bulk-OUT endpoint and echoes it straight
 * back through the bulk-IN endpoint.
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
        .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
        .iInterface         = 1,
    },
    .fs_in  = EP_DESC(USB_DIR_IN  | 1, USB_ENDPOINT_XFER_BULK,  64),
    .fs_out = EP_DESC(USB_DIR_OUT | 2, USB_ENDPOINT_XFER_BULK,  64),

    .hs_intf = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = USB_CLASS_VENDOR_SPEC,
        .iInterface         = 1,
    },
    .hs_in  = EP_DESC(USB_DIR_IN  | 1, USB_ENDPOINT_XFER_BULK, 512),
    .hs_out = EP_DESC(USB_DIR_OUT | 2, USB_ENDPOINT_XFER_BULK, 512),
};

/* ------------------------------------------------------------------ */
/* String table                                                        */
/* ------------------------------------------------------------------ */

#define IFACE_STR "Bulk Loopback"

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

#define BULK_BUF_SIZE 4096
#define FFS_DEFAULT   "/dev/ffs-loopback"

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
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
        running = 0;
        break;
    case FUNCTIONFS_ENABLE:
        fprintf(stderr, "gadget: ENABLE\n");
        return 1;
    case FUNCTIONFS_DISABLE:
        fprintf(stderr, "gadget: DISABLE\n");
        return -2;
    case FUNCTIONFS_SETUP:
        if (ev.u.setup.bRequestType & USB_DIR_IN)
            write(ep0, NULL, 0);
        else
            read(ep0, NULL, 0);
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* One loopback transfer: OUT -> IN                                   */
/* ------------------------------------------------------------------ */

static int do_loopback(int ep_in, int ep_out)
{
    static char buf[BULK_BUF_SIZE];

    ssize_t n = read(ep_out, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EINTR || errno == ESHUTDOWN)
            return 0;
        perror("ep_out read");
        return -1;
    }

    for (ssize_t off = 0; off < n; ) {
        ssize_t w = write(ep_in, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ESHUTDOWN)
                return 0;
            perror("ep_in write");
            return -1;
        }
        off += w;
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

    while (running) {
        struct pollfd fds[2] = {
            { .fd = ep0,    .events = POLLIN },
            { .fd = ep_out, .events = POLLIN },
        };
        int nfds = (ep_out >= 0) ? 2 : 1;

        if (poll(fds, nfds, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            int rc = process_ep0_event(ep0);
            if (rc == 1) {
                ep_in  = open(ep1_path, O_RDWR);
                ep_out = open(ep2_path, O_RDWR);
                if (ep_in < 0 || ep_out < 0) {
                    perror("open data endpoints");
                    break;
                }
                fprintf(stderr, "gadget: loopback active\n");
            } else if (rc == -2) {
                close(ep_in);  ep_in  = -1;
                close(ep_out); ep_out = -1;
                fprintf(stderr, "gadget: loopback suspended\n");
            } else if (rc < 0) {
                break;
            }
        }

        if (ep_out >= 0 && (fds[1].revents & POLLIN)) {
            if (do_loopback(ep_in, ep_out) < 0)
                break;
        }
    }

    if (ep_in  >= 0) close(ep_in);
    if (ep_out >= 0) close(ep_out);
    close(ep0);

    fprintf(stderr, "gadget: stopped\n");
    return EXIT_SUCCESS;
}
