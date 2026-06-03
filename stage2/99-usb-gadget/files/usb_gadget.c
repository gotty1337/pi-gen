/* usb_gadget.c – USB FunctionFS IPP-over-USB printer gadget daemon.
 *
 * Presents a USB Printer Class interface (class 0x07, subclass 0x01,
 * protocol 0x04 = IPP over USB) to the host and bridges the bulk
 * endpoints bidirectionally to a local ippeveprinter instance via TCP.
 *
 * Windows' built-in driverless IPP-over-USB driver picks this up
 * automatically; no INF file or separate driver install is required.
 * Print jobs are handled by ippeveprinter and saved to /home/pi/PDF.
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
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <linux/usb/functionfs.h>
#include <linux/usb/ch9.h>
#include <stdarg.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

#define FFS_DEFAULT         "/dev/ffs-loopback"
#define IFACE_STR           "IPP Printer"
#define IPP_SERVER_HOST     "127.0.0.1"
#define IPP_SERVER_PORT     8631
#define CUPS_BUF_SIZE       (64 * 1024)  /* 64 KiB I/O chunks */
#define IPP_RESP_POLL_MS    5            /* per-chunk inline response poll timeout */

/*
 * IEEE 1284 Device ID string.  CMD:IPP is what triggers Windows' built-in
 * driverless IPP-over-USB driver; the rest is informational.
 */
#define PRINTER_DEV_ID \
    "MFG:Raspberry Pi;MDL:USB Printer;CMD:IPP,PDF,PWGRaster;" \
    "CLS:PRINTER;DRV:DRVLESS;"

#define PRINTER_REQ_GET_DEVICE_ID   0x00
#define PRINTER_REQ_GET_PORT_STATUS 0x01
#define PRINTER_REQ_SOFT_RESET      0x02


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
/* USB descriptor helpers                                             */
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
/* Descriptor table (full-speed only)                                 */
/* Printer class: bInterfaceClass=0x07, subclass=0x01, protocol=0x04  */
/* Protocol 0x04 = IPP over USB (triggers Windows driverless driver)  */
/* ------------------------------------------------------------------ */

/*
 * ipp-usb requires >= 2 IPP-over-USB interfaces per device (it uses one
 * for the forward channel and one for the reverse/status channel).
 * We expose three identical Printer class interfaces (all class=0x07,
 * subclass=0x01, protocol=0x04) each with their own IN+OUT endpoint pair.
 */
static const struct {
    struct usb_functionfs_descs_head_v2 head;
    __le32 fs_count;
    /* Full-speed */
    struct usb_interface_descriptor fs_intf0;
    struct ep_desc                  fs_in0;
    struct ep_desc                  fs_out0;
    struct usb_interface_descriptor fs_intf1;
    struct ep_desc                  fs_in1;
    struct ep_desc                  fs_out1;
    struct usb_interface_descriptor fs_intf2;
    struct ep_desc                  fs_in2;
    struct ep_desc                  fs_out2;
} __attribute__((packed)) descriptors = {
    .head = {
        .magic  = LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
        .length = LE32(sizeof(descriptors)),
        .flags  = LE32(FUNCTIONFS_HAS_FS_DESC),
    },
    .fs_count = LE32(9),  /* 3 interfaces + 6 endpoints */

    /* Interface 0 – forward channel (print jobs) */
    .fs_intf0 = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = USB_CLASS_PRINTER,
        .bInterfaceSubClass = 0x01,           /* Printer */
        .bInterfaceProtocol = 0x04,           /* IPP over USB */
        .iInterface         = 1,
    },
    .fs_in0  = EP_DESC(USB_DIR_IN  | 1, USB_ENDPOINT_XFER_BULK, 64),
    .fs_out0 = EP_DESC(USB_DIR_OUT | 2, USB_ENDPOINT_XFER_BULK, 64),

    /* Interface 1 – reverse channel (status / scan queries) */
    .fs_intf1 = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 1,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = USB_CLASS_PRINTER,
        .bInterfaceSubClass = 0x01,
        .bInterfaceProtocol = 0x04,
        .iInterface         = 1,
    },
    .fs_in1  = EP_DESC(USB_DIR_IN  | 3, USB_ENDPOINT_XFER_BULK, 64),
    .fs_out1 = EP_DESC(USB_DIR_OUT | 4, USB_ENDPOINT_XFER_BULK, 64),

    /* Interface 2 – additional IPP channel */
    .fs_intf2 = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 2,
        .bNumEndpoints      = 2,
        .bInterfaceClass    = USB_CLASS_PRINTER,
        .bInterfaceSubClass = 0x01,
        .bInterfaceProtocol = 0x04,
        .iInterface         = 1,
    },
    .fs_in2  = EP_DESC(USB_DIR_IN  | 5, USB_ENDPOINT_XFER_BULK, 64),
    .fs_out2 = EP_DESC(USB_DIR_OUT | 6, USB_ENDPOINT_XFER_BULK, 64),
};

/* ------------------------------------------------------------------ */
/* String table                                                       */
/* ------------------------------------------------------------------ */
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
/* Global USB gadget context                                          */
/* ------------------------------------------------------------------ */
static struct usb_gadget {
    /*
     * Running flag, set to 0 by signal handler for clean shutdown. Declared
     * volatile sig_atomic_t to be safely modified in signal handler context.
    */
    volatile sig_atomic_t running;

    /*
     * Identifies when the data endpoints are ready (after ENABLE event) so the endpoint worker threads can open them.
     * Set to 1 by the ep0 worker thread after processing ENABLE, and reset to 0 on DISABLE or UNBIND.
    */
    volatile sig_atomic_t fds_ready;
} __attribute__((packed)) g_ctx = {
    .running = 1,
    .fds_ready = 0,
};

/* ------------------------------------------------------------------ */
/* EP0 event types                                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    EP0_EVENT_OK,
    EP0_EVENT_SETUP,
    EP0_EVENT_FDS_READY,
    EP0_EVENT_FDS_NOT_READY,
    EP0_EVENT_ERROR,
} ep0_event_type_t;

/* 
 * EP0 worker thread context
 */
typedef struct {
    /* EP0 path */
    const char *acEp0Path;
} ep0_worker_ctx_t;

/* 
 * EP worker thread context
 */
typedef struct {
    /* Interface number for logging purposes */
    const int iIfaceNum;
    /* Paths for the IN and OUT endpoints of this interface */
    const char *acEpInPath;
    /* Paths for the IN and OUT endpoints of this interface */
    const char *acEpOutPath;
} ep_worker_ctx_t;


/*
 * Simple logging helper.
 */
static void LogPrintf(const char *fmt, ...)
{
    va_list ap = {};
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/*
 * Signal handler for clean shutdown on SIGINT/SIGTERM.
 */
static void on_signal(int sig)
{
    (void)sig;
    g_ctx.running = 0;
    /* LogPrintf is not async-signal-safe; log is emitted by ep0 thread. */
} 

/* 
 * handle_setup - Process a control request received on ep0. This is called
 * from the ep0 worker thread when a SETUP event is received. It handles
 * class-specific requests for the Printer interface and sends appropriate
 * responses back to the host.
 */
static void handle_setup(int ep0, const struct usb_ctrlrequest *req)
{
    uint8_t  type = req->bRequestType;
    uint8_t  rq   = req->bRequest;
    uint16_t wlen = (uint16_t)req->wLength; /* LE on LE system */

    if ((type & USB_TYPE_MASK)  == USB_TYPE_CLASS && (type & USB_RECIP_MASK) == USB_RECIP_INTERFACE) 
    {

        if (rq == PRINTER_REQ_GET_DEVICE_ID && (type & USB_DIR_IN))
        {
            /* IEEE 1284: 2-byte big-endian total length then PnP string. */
            static const char pnp[] = PRINTER_DEV_ID;
            uint16_t total = (uint16_t)(2 + sizeof(pnp) - 1);
            uint8_t  buf[2 + sizeof(pnp) - 1];
            buf[0] = (uint8_t)(total >> 8);
            buf[1] = (uint8_t)(total & 0xff);
            memcpy(buf + 2, pnp, sizeof(pnp) - 1);
            size_t send = (wlen < total) ? wlen : total;
            write(ep0, buf, send);
        }
        else if (rq == PRINTER_REQ_GET_PORT_STATUS && (type & USB_DIR_IN))
        {
            /*
             * Port status byte (USB Printer Class 1.1 Table 3):
             *   bit5 = Paper Empty (0 = paper present)
             *   bit4 = Select      (1 = selected/online)
             *   bit3 = Not Error   (1 = no error condition)
             */
            uint8_t status = 0x18; /* selected, no error, paper loaded */
            write(ep0, &status, (wlen < 1) ? 0 : 1);
        }
        else if (rq == PRINTER_REQ_SOFT_RESET && !(type & USB_DIR_IN))
        {
            /* OUT request, no data — ACK and drop both CUPS connections. */
            read(ep0, NULL, 0);

        }
        else
        {
            /* Unknown class request — send empty response. */
            if (type & USB_DIR_IN)
            {
                write(ep0, NULL, 0);
            }
            else
            {
                read(ep0, NULL, 0);
            }
        }
    } 
    else 
    {
        if (type & USB_DIR_IN)
        {
            write(ep0, NULL, 0);
        }
        else
        {
            read(ep0, NULL, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* ep0 event processing                                               */
/* ------------------------------------------------------------------ */
    static ep0_event_type_t process_ep0_event(int ep0)
{
    struct usb_functionfs_event ev = {};

    ssize_t n = read(ep0, &ev, sizeof(ev));
    if (n < 0) {
        LogPrintf("%s: error reading ep0 event: %s\n", __FUNCTION__, strerror(errno));
        return EP0_EVENT_OK;
    }

    if ((size_t)n < sizeof(ev))
    {
        LogPrintf("%s: incomplete ep0 event read: %zd bytes\n", __FUNCTION__, n);
        return EP0_EVENT_OK;
    }

    switch (ev.type) {
        case FUNCTIONFS_SETUP:
            LogPrintf("%s: SETUP\n", __FUNCTION__);
            handle_setup(ep0, &ev.u.setup);
            return EP0_EVENT_OK;
        case FUNCTIONFS_BIND:
            LogPrintf("%s: BIND\n", __FUNCTION__);
            return EP0_EVENT_OK;
        case FUNCTIONFS_ENABLE:
            LogPrintf("%s: ENABLE\n", __FUNCTION__);
            return EP0_EVENT_FDS_READY;
        case FUNCTIONFS_DISABLE:
            LogPrintf("%s: DISABLE\n", __FUNCTION__);
            return EP0_EVENT_FDS_NOT_READY;
        case FUNCTIONFS_UNBIND:
            LogPrintf("%s: UNBIND\n", __FUNCTION__);
            return EP0_EVENT_ERROR;
        default:
            LogPrintf("%s: unknown event type %d\n", __FUNCTION__, ev.type);
            return EP0_EVENT_ERROR;
    }
}

/*
 * write_all - Write exactly nLen bytes from pBuf to fd.
 * Aborts early if g_ctx.running == 0 (shutdown in progress).
 * Returns 0 on success, -1 on error or shutdown.
 */
static int write_all(int fd, const char *pacBuf, size_t nLen)
{
    size_t nSent = 0;
    while (nSent < nLen) {
        if (!g_ctx.running) { errno = ECANCELED; return -1; } /* shutdown: abort */
        ssize_t n = write(fd, pacBuf + nSent, nLen - nSent);
        if (n < 0) {
            if (errno == EINTR) continue; /* signal: recheck running above */
            LogPrintf("%s: write error: %s\n", __FUNCTION__, strerror(errno));
            return -1;
        }
        nSent += (size_t)n;
    }
    return 0;
}

/*
 * connect_to_ipp_server - Open a new TCP connection to the local ippeveprinter.
 * Returns a connected fd on success, or -1 on error.
 */
static int connect_to_ipp_server(int iIfaceNum)
{
    int iFd = socket(AF_INET, SOCK_STREAM, 0);
    if (iFd < 0) {
        LogPrintf("%s: IF%d socket(): %s\n", __FUNCTION__, iIfaceNum, strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(IPP_SERVER_PORT);
    if (inet_pton(AF_INET, IPP_SERVER_HOST, &addr.sin_addr) != 1) {
        LogPrintf("%s: IF%d inet_pton(%s): %s\n", __FUNCTION__, iIfaceNum, IPP_SERVER_HOST, strerror(errno));
        close(iFd);
        return -1;
    }

    if (connect(iFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LogPrintf("%s: IF%d connect(%s:%d): %s\n", __FUNCTION__, iIfaceNum, IPP_SERVER_HOST, IPP_SERVER_PORT, strerror(errno));
        close(iFd);
        return -1;
    }

    return iFd;
}

/*
 * ep_worker_thread - Persistent bidirectional proxy between a USB FunctionFS
 * bulk endpoint pair and the local ippeveprinter TCP server.
 *
 * Previous design: each read() from ep_out spawned a separate worker thread
 * that opened a fresh TCP connection, wrote the chunk, called shutdown(SHUT_WR),
 * and closed.  This broke large IPP requests (e.g. Send-Document) because the
 * print data arrives as many consecutive USB bulk transfers (~32 KiB each),
 * and each one was sent as a standalone, incomplete HTTP POST body.
 * ippeveprinter received no valid request body and returned 0 bytes every time.
 *
 * New design: one persistent TCP connection per interface.  The event loop
 * polls both ep_out and the TCP socket; USB data is forwarded to TCP without
 * ever calling shutdown, and TCP response data is forwarded back to ep_in.
 * The TCP connection is only closed when the server closes it (after it has
 * finished sending a complete HTTP response), at which point we transparently
 * reconnect before the next request chunk arrives.
 */
void *ep_worker_thread(void *ptr) {
    if (ptr == NULL) {
        LogPrintf("%s: invalid context\n", __FUNCTION__);
        return ptr;
    }

    ep_worker_ctx_t *ptCtx = (ep_worker_ctx_t *)ptr;

    LogPrintf("%s: IF%d worker thread started\n", __FUNCTION__, ptCtx->iIfaceNum);

    if (ptCtx->acEpInPath == NULL || ptCtx->acEpOutPath == NULL) {
        LogPrintf("%s: endpoint paths are NULL\n", __FUNCTION__);
        return ptr;
    }

    int iEpOut  = -1;
    int iEpIn   = -1;
    int iCupsFd = -1;
    char aBuf[CUPS_BUF_SIZE];

    while (g_ctx.running) {
        /* Wait for USB endpoints to be activated (ENABLE event on ep0). */
        if (!g_ctx.fds_ready) {
            if (iEpOut  >= 0) { close(iEpOut);  iEpOut  = -1; }
            if (iEpIn   >= 0) { close(iEpIn);   iEpIn   = -1; }
            if (iCupsFd >= 0) { close(iCupsFd); iCupsFd = -1; }
            usleep(100000);
            continue;
        }

        if (iEpOut < 0) {
            iEpOut = open(ptCtx->acEpOutPath, O_RDWR);
            if (iEpOut < 0) {
                LogPrintf("%s: IF%d failed to open OUT endpoint: %s\n",
                          __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                usleep(10000);
                continue;
            }
            LogPrintf("%s: IF%d ep out opened successfully\n", __FUNCTION__, ptCtx->iIfaceNum);
        }

        if (iEpIn < 0) {
            iEpIn = open(ptCtx->acEpInPath, O_RDWR);
            if (iEpIn < 0) {
                LogPrintf("%s: IF%d failed to open IN endpoint: %s\n",
                          __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                usleep(10000);
                continue;
            }
            LogPrintf("%s: IF%d ep in opened successfully\n", __FUNCTION__, ptCtx->iIfaceNum);
        }

        if (iCupsFd < 0) {
            iCupsFd = connect_to_ipp_server(ptCtx->iIfaceNum);
            if (iCupsFd < 0) {
                usleep(10000);
                continue;
            }
            LogPrintf("%s: IF%d connected to IPP server\n", __FUNCTION__, ptCtx->iIfaceNum);
        }

        /*
         * Poll both directions simultaneously:
         *   sFds[0] = ep_out  (USB host → us → IPP server)
         *   sFds[1] = iCupsFd (IPP server → us → USB host)
         */
        struct pollfd sFds[2] = {
            { .fd = iEpOut,  .events = POLLIN | POLLERR | POLLHUP | POLLNVAL },
            { .fd = iCupsFd, .events = POLLIN | POLLERR | POLLHUP },
        };

        int iRv = poll(sFds, 2, 100);
        if (iRv < 0) {
            if (errno == EINTR) continue;
            LogPrintf("%s: IF%d poll error: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
            close(iEpOut);  iEpOut  = -1;
            close(iEpIn);   iEpIn   = -1;
            close(iCupsFd); iCupsFd = -1;
            continue;
        }

        /* ep_out endpoint errors → reset everything and re-open. */
        if (sFds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LogPrintf("%s: IF%d ep_out error/hangup (revents=0x%x), closing\n",
                      __FUNCTION__, ptCtx->iIfaceNum, sFds[0].revents);
            close(iEpOut);  iEpOut  = -1;
            close(iEpIn);   iEpIn   = -1;
            close(iCupsFd); iCupsFd = -1;
            continue;
        }

        /*
         * TCP error/hangup: close and reconnect before next forward.
         * But if POLLIN is also set (server sent its last response then closed),
         * do NOT close yet — fall through to check_tcp to drain the response
         * first.  Closing here would discard the response and hang the USB host.
         */
        if ((sFds[1].revents & POLLERR) ||
            ((sFds[1].revents & POLLHUP) && !(sFds[1].revents & POLLIN))) {
            LogPrintf("%s: IF%d IPP server connection lost, will reconnect\n",
                      __FUNCTION__, ptCtx->iIfaceNum);
            close(iCupsFd); iCupsFd = -1;
            continue;
        }

        /* USB OUT → TCP: forward request data (one chunk) to IPP server.
         * Do NOT call shutdown() here — more chunks may follow for the same
         * HTTP POST request (e.g. a multi-packet Send-Document body). */
        if (sFds[0].revents & POLLIN) {
            ssize_t nRead = read(iEpOut, aBuf, sizeof(aBuf));
            if (nRead < 0) {
                if (errno == EAGAIN || errno == EINTR) goto check_tcp;
                LogPrintf("%s: IF%d read ep_out: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                close(iEpOut);  iEpOut  = -1;
                close(iEpIn);   iEpIn   = -1;
                close(iCupsFd); iCupsFd = -1;
                continue;
            }
            if (nRead == 0) goto check_tcp;

            LogPrintf("%s: IF%d read %zd bytes from ep_out\n", __FUNCTION__, ptCtx->iIfaceNum, nRead);

            /* Reconnect if TCP was closed after the previous response. */
            if (iCupsFd < 0) {
                iCupsFd = connect_to_ipp_server(ptCtx->iIfaceNum);
                if (iCupsFd < 0) {
                    LogPrintf("%s: IF%d reconnect failed, dropping %zd bytes\n",
                              __FUNCTION__, ptCtx->iIfaceNum, nRead);
                    goto check_tcp;
                }
                LogPrintf("%s: IF%d reconnected to IPP server\n", __FUNCTION__, ptCtx->iIfaceNum);
            }

            /*
             * Pre-write health check: ippeveprinter closes idle connections after
             * its keep-alive timeout (~10s).  The FIN may have arrived between our
             * last poll() and now, so the outer poll()'s TCP revents are stale.
             * A 0-ms poll catches this race without blocking.
             *
             * On Linux a CLOSE_WAIT socket (remote FIN received, no pending data)
             * sets POLLIN|POLLHUP together.  We use MSG_PEEK to distinguish EOF
             * (recv returns 0) from actual data (recv returns >0).
             */
            {
                struct pollfd sChk = { .fd = iCupsFd,
                                       .events = POLLIN | POLLERR | POLLHUP };
                int bStale = 0;
                if (poll(&sChk, 1, 0) > 0) {
                    if (sChk.revents & POLLERR) {
                        bStale = 1;
                    } else if (sChk.revents & (POLLIN | POLLHUP)) {
                        char cPeek;
                        /* recv == 0 → EOF (remote closed), -1 → EAGAIN (spurious) */
                        bStale = (recv(iCupsFd, &cPeek, 1, MSG_PEEK | MSG_DONTWAIT) == 0);
                    }
                }
                if (bStale) {
                    LogPrintf("%s: IF%d TCP stale connection detected, reconnecting\n",
                              __FUNCTION__, ptCtx->iIfaceNum);
                    close(iCupsFd);
                    iCupsFd = connect_to_ipp_server(ptCtx->iIfaceNum);
                    if (iCupsFd < 0) {
                        LogPrintf("%s: IF%d reconnect failed after stale check\n",
                                  __FUNCTION__, ptCtx->iIfaceNum);
                        goto check_tcp;
                    }
                    LogPrintf("%s: IF%d reconnected to IPP server\n",
                              __FUNCTION__, ptCtx->iIfaceNum);
                }
            }

            if (write_all(iCupsFd, aBuf, (size_t)nRead) < 0) {
                LogPrintf("%s: IF%d write to IPP server failed: %s\n",
                          __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                close(iCupsFd); iCupsFd = -1;
                goto check_tcp;
            }
            LogPrintf("%s: IF%d forwarded %zd bytes to IPP server\n", __FUNCTION__, ptCtx->iIfaceNum, nRead);

            /*
             * The outer poll() fired for ep_out BEFORE we wrote to TCP, so
             * sFds[1].revents does not yet reflect the server's response.
             * Do a short inline poll on TCP to catch responses that arrive
             * immediately (single-request operations like Get-Printer-Attributes).
             * For multi-chunk requests (Send-Document) there is no response yet,
             * so this times out quickly and the main loop handles it later.
             */
            while (iCupsFd >= 0 && iEpIn >= 0) {
                struct pollfd sRsp = { .fd = iCupsFd,
                                       .events = POLLIN | POLLERR | POLLHUP };
                if (poll(&sRsp, 1, IPP_RESP_POLL_MS) <= 0) break;
                /* POLLERR: fatal. POLLHUP alone (no POLLIN): no more data. */
                if (sRsp.revents & POLLERR) {
                    LogPrintf("%s: IF%d IPP server error (inline)\n",
                              __FUNCTION__, ptCtx->iIfaceNum);
                    close(iCupsFd); iCupsFd = -1;
                    break;
                }
                if (!(sRsp.revents & POLLIN)) {
                    /* POLLHUP without data — server closed after last response. */
                    close(iCupsFd); iCupsFd = -1;
                    break;
                }
                /* POLLIN (and possibly POLLHUP): drain the data first. */

                ssize_t nResp = read(iCupsFd, aBuf, sizeof(aBuf));
                if (nResp < 0) {
                    if (errno == EAGAIN || errno == EINTR) continue;
                    LogPrintf("%s: IF%d read IPP server (inline): %s\n",
                              __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                    close(iCupsFd); iCupsFd = -1;
                    break;
                }
                if (nResp == 0) {
                    LogPrintf("%s: IF%d IPP server closed connection\n",
                              __FUNCTION__, ptCtx->iIfaceNum);
                    close(iCupsFd); iCupsFd = -1;
                    break;
                }
                LogPrintf("%s: IF%d read %zd bytes from IPP server\n",
                          __FUNCTION__, ptCtx->iIfaceNum, nResp);
                if (write_all(iEpIn, aBuf, (size_t)nResp) < 0) {
                    LogPrintf("%s: IF%d write ep_in failed: %s\n",
                              __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                    close(iEpIn); iEpIn = -1;
                    break;
                }
                LogPrintf("%s: IF%d wrote %zd bytes to ep_in\n",
                          __FUNCTION__, ptCtx->iIfaceNum, nResp);
            }

            /*
             * The inline drain may have consumed the data that caused the
             * outer poll to set sFds[1].revents = POLLIN.  Clear it now so
             * check_tcp below doesn't call blocking read() on an empty socket.
             * Any data that actually arrived after the inline drain finished
             * will be caught by the next outer poll() iteration.
             */
            sFds[1].revents = 0;
        }

    check_tcp:
        /*
         * TCP → USB IN: forward any response data not yet handled by the
         * inline drain above (e.g. large responses, or responses that arrive
         * only after multiple ep_out chunks have been forwarded).
         */
        if (iCupsFd >= 0 && (sFds[1].revents & POLLIN)) {
            ssize_t nRead = read(iCupsFd, aBuf, sizeof(aBuf));
            if (nRead < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                LogPrintf("%s: IF%d read IPP server: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                close(iCupsFd); iCupsFd = -1;
                continue;
            }
            if (nRead == 0) {
                /* Server closed connection — full response has been received. */
                LogPrintf("%s: IF%d IPP server closed connection\n", __FUNCTION__, ptCtx->iIfaceNum);
                close(iCupsFd); iCupsFd = -1;
                continue;
            }

            LogPrintf("%s: IF%d read %zd bytes from IPP server\n", __FUNCTION__, ptCtx->iIfaceNum, nRead);

            if (iEpIn >= 0 && write_all(iEpIn, aBuf, (size_t)nRead) < 0) {
                LogPrintf("%s: IF%d write ep_in failed: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                close(iEpIn); iEpIn = -1;
                continue;
            }
            if (iEpIn >= 0)
                LogPrintf("%s: IF%d wrote %zd bytes to ep_in\n", __FUNCTION__, ptCtx->iIfaceNum, nRead);
        }
    }

    if (iEpOut  >= 0) close(iEpOut);
    if (iEpIn   >= 0) close(iEpIn);
    if (iCupsFd >= 0) close(iCupsFd);

    LogPrintf("%s: IF%d worker thread exiting\n", __FUNCTION__, ptCtx->iIfaceNum);
    return ptr;
}

/*
 * Worker thread to handle ep0 events. This allows us to process control
 * requests asynchronously without blocking the main thread.
 */
void *ep0_worker_thread(void *ptr)
{
    LogPrintf("%s: ep0 worker thread started\n", __FUNCTION__);

    if (ptr == NULL) {
        LogPrintf("%s: invalid context\n", __FUNCTION__);
        return ptr;
    }

    const ep0_worker_ctx_t *ptCtx = (ep0_worker_ctx_t *)ptr;

    if (ptCtx->acEp0Path == NULL) {
        LogPrintf("%s: ep0 path is NULL\n", __FUNCTION__);
        return ptr;
    }

    int iEp0 = open(ptCtx->acEp0Path, O_RDWR);
    if (iEp0 < 0) {
        LogPrintf("%s: failed to open ep0: %s\n", __FUNCTION__, strerror(errno));
        goto exit;
    }

    if (write(iEp0, &descriptors, sizeof(descriptors)) != sizeof(descriptors)) {
        LogPrintf("%s: failed to write descriptors: %s\n", __FUNCTION__, strerror(errno));
        goto exit;
    }
    if (write(iEp0, &strings, sizeof(strings)) != sizeof(strings)) {
        LogPrintf("%s: failed to write strings: %s\n", __FUNCTION__, strerror(errno));
        goto exit;
    }

    LogPrintf("%s: descriptors written, waiting for host...\n", __FUNCTION__);

    while (g_ctx.running) {
        struct pollfd sFds[1] = {
            { .fd = iEp0, .events = POLLIN | POLLERR | POLLHUP | POLLNVAL },
        };

        int iRv = poll(sFds, 1, 500);
        if (iRv < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LogPrintf("%s: poll error: %s\n", __FUNCTION__, strerror(errno));
            break;
        }

        if (iRv == 0)
        {
            continue;
        }

        if (sFds[0].revents & POLL_IN) {
            const ep0_event_type_t event = process_ep0_event(iEp0);
            switch(event) {
                case EP0_EVENT_SETUP:
                case EP0_EVENT_OK:
                    break;
                case EP0_EVENT_FDS_READY:
                    g_ctx.fds_ready = 1;
                    break;
                case EP0_EVENT_FDS_NOT_READY:
                    g_ctx.fds_ready = 0;
                    break;
                case EP0_EVENT_ERROR:
                    LogPrintf("%s: fatal ep0 event %d\n", __FUNCTION__, event);
                    goto exit;
                default:
                    LogPrintf("%s: unknown ep0 event type %d\n", __FUNCTION__, event);
                    goto exit;
            }
        }    
    }
    goto exit;

    exit:
        close(iEp0);
        g_ctx.running = 0;
        g_ctx.fds_ready = 0;
        LogPrintf("%s: exit ep0 worker thread\n", __FUNCTION__);
        return ptr;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    const char *dir = (argc > 1) ? argv[1] : FFS_DEFAULT;

    LogPrintf("%s: starting usb-gadget, using FunctionFS directory '%s'\n", __FUNCTION__, dir);

    /* Use sigaction() without SA_RESTART so that any blocking syscall
     * (read, poll, write, …) returns EINTR immediately when a signal is
     * delivered to that thread — allowing all worker threads to notice
     * g_ctx.running == 0 and exit without waiting for a USB timeout. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* sa.sa_flags = 0 intentionally: no SA_RESTART */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    char acEp0Path[256], ep1_path[256], ep2_path[256], ep3_path[256],
        ep4_path[256], ep5_path[256], ep6_path[256];

    snprintf(acEp0Path, sizeof(acEp0Path), "%s/ep0", dir);
    snprintf(ep1_path, sizeof(ep1_path), "%s/ep1", dir);
    snprintf(ep2_path, sizeof(ep2_path), "%s/ep2", dir);
    snprintf(ep3_path, sizeof(ep3_path), "%s/ep3", dir);
    snprintf(ep4_path, sizeof(ep4_path), "%s/ep4", dir);
    snprintf(ep5_path, sizeof(ep5_path), "%s/ep5", dir);
    snprintf(ep6_path, sizeof(ep6_path), "%s/ep6", dir);
    
    pthread_t ep0_thread, ep1_thread, ep2_thread, ep3_thread;
    
    ep0_worker_ctx_t ep0_ctx = {
        .acEp0Path = acEp0Path,
    };

    int iRv = pthread_create(&ep0_thread, NULL, ep0_worker_thread, &ep0_ctx);
    if (iRv != 0)
    {
        LogPrintf("%s: failed to create ep0 worker thread: %s\n", __FUNCTION__, strerror(iRv));
        return EXIT_FAILURE;
    }

    ep_worker_ctx_t ep1_ctx = {
        .iIfaceNum = 1,
        .acEpInPath = ep1_path,
        .acEpOutPath = ep2_path,
    };

    iRv = pthread_create(&ep1_thread, NULL, ep_worker_thread, &ep1_ctx);
    if (iRv != 0)
    {
        LogPrintf("%s: failed to create ep1 worker thread: %s\n", __FUNCTION__, strerror(iRv));
        return EXIT_FAILURE;
    }

    ep_worker_ctx_t ep2_ctx = {
        .iIfaceNum = 2,
        .acEpInPath = ep3_path,
        .acEpOutPath = ep4_path,
    };

    iRv = pthread_create(&ep2_thread, NULL, ep_worker_thread, &ep2_ctx);
    if (iRv != 0)
    {
        LogPrintf("%s: failed to create ep2 worker thread: %s\n", __FUNCTION__, strerror(iRv));
        return EXIT_FAILURE;
    }

    ep_worker_ctx_t ep3_ctx = {
        .iIfaceNum = 3,
        .acEpInPath = ep5_path,
        .acEpOutPath = ep6_path,
    };

    iRv = pthread_create(&ep3_thread, NULL, ep_worker_thread, &ep3_ctx);
    if (iRv != 0)
    {
        LogPrintf("%s: failed to create ep3 worker thread: %s\n", __FUNCTION__, strerror(iRv));
        return EXIT_FAILURE;
    }

    pthread_join(ep0_thread, NULL);

    /* ep0 exiting means g_ctx.running == 0. Worker threads may be blocked
     * in poll() on FunctionFS endpoints that never return a timeout — send
     * SIGTERM directly to each thread so poll() returns EINTR, the thread
     * re-checks g_ctx.running and exits cleanly. */
    pthread_kill(ep1_thread, SIGTERM);
    pthread_kill(ep2_thread, SIGTERM);
    pthread_kill(ep3_thread, SIGTERM);

    pthread_join(ep1_thread, NULL);
    pthread_join(ep2_thread, NULL);
    pthread_join(ep3_thread, NULL);
    
    LogPrintf("%s: stop usb-gadget\n", __FUNCTION__);
    return EXIT_SUCCESS;
}
