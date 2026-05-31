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
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/usb/functionfs.h>
#include <linux/usb/ch9.h>
#include <stdarg.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

#define FFS_DEFAULT     "/dev/ffs-loopback"
#define IFACE_STR       "IPP Printer"

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
    LogPrintf("gadget: signal received, stopping...\n");
    g_ctx.running = 0;
} 

/* ------------------------------------------------------------------ */
/* ep0 event processing                                               */
/* Returns:  1 = ENABLE,  -2 = DISABLE,  -1 = fatal,  0 = other      */
/* ------------------------------------------------------------------ */

static int process_ep0_event(int ep0)
{
    struct usb_functionfs_event ev = {};

    ssize_t n = read(ep0, &ev, sizeof(ev));
    if (n < 0) {
        LogPrintf("%s: error reading ep0 event: %s\n", __FUNCTION__, strerror(errno));
        return -1;
    }

    if ((size_t)n < sizeof(ev))
    {
        LogPrintf("%s: incomplete ep0 event read: %zd bytes\n", __FUNCTION__, n);
        return 0;
    }

    switch (ev.type) {
        case FUNCTIONFS_SETUP:
            LogPrintf("%s: SETUP\n", __FUNCTION__);
            return 0;
        case FUNCTIONFS_BIND:
            LogPrintf("%s: BIND\n", __FUNCTION__);
            return 0;
        case FUNCTIONFS_ENABLE:
            LogPrintf("%s: ENABLE\n", __FUNCTION__);
            return 1;
        case FUNCTIONFS_DISABLE:
            LogPrintf("%s: DISABLE\n", __FUNCTION__);
            return -2;
        case FUNCTIONFS_UNBIND:
            LogPrintf("%s: UNBIND\n", __FUNCTION__);
            return -3;
        default:
            LogPrintf("%s: unknown event type %d\n", __FUNCTION__, ev.type);
            return -3;
    }
}

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
 * Context shared between ep_read_sub_thread and ep_write_sub_thread.
 */
typedef struct {
    int              iIfaceNum;
    int              iEpIn;
    int              iEpOut;
    /* Fresh CUPS fd produced by reader, consumed by writer (-1 = none pending) */
    int              iCupsFd;
    pthread_mutex_t  tMutex;
    pthread_cond_t   tCondReady; /* reader → writer: new CUPS fd available */
    pthread_cond_t   tCondFree;  /* writer → reader: CUPS fd consumed      */
} ep_io_ctx_t;

/* ------------------------------------------------------------------ */
/* CUPS forwarding                                                    */
/* ------------------------------------------------------------------ */

#define CUPS_SOCKET_PATH  "/run/cups/cups.sock"
#define CUPS_BUF_SIZE     (64 * 1024)  /* 64 KiB I/O chunks */

/*
 * write_all - Write exactly nLen bytes from pBuf to fd, retrying on EINTR.
 * Returns 0 on success, -1 on error.
 */
static int write_all(int fd, const uint8_t *pBuf, size_t nLen)
{
    size_t nSent = 0;
    while (nSent < nLen) {
        ssize_t n = write(fd, pBuf + nSent, nLen - nSent);
        if (n < 0) {
            if (errno == EINTR) {
                LogPrintf("%s: write interrupted by signal, retrying\n", __FUNCTION__);
                continue;
            }
            LogPrintf("%s: write error: %s\n", __FUNCTION__, strerror(errno));
            return -1;
        }
        nSent += (size_t)n;
    }
    return 0;
}

/*
 * connect_to_cups - Open a new connection to the local CUPS Unix socket.
 * Returns a connected fd on success, or -1 on error.
 */
static int connect_to_cups(int iIfaceNum)
{
    int iFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (iFd < 0) {
        LogPrintf("%s: IF%d socket(): %s\n", __FUNCTION__, iIfaceNum, strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CUPS_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(iFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LogPrintf("%s: IF%d connect(%s): %s\n", __FUNCTION__, iIfaceNum, CUPS_SOCKET_PATH, strerror(errno));
        close(iFd);
        return -1;
    }

    return iFd;
}

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

    int iEpIn = -1, iEpOut = -1, iCupsFd = -1;

    uint8_t *pcBuf = malloc(CUPS_BUF_SIZE);
    if (!pcBuf) {
        LogPrintf("%s: IF%d OOM\n", __FUNCTION__, ptCtx->iIfaceNum);
        return ptr;
    }

    while (g_ctx.running) {

        memset(pcBuf, 0, CUPS_BUF_SIZE);

        if (!g_ctx.fds_ready)
        {
            if (iEpIn    >= 0) { close(iEpIn);    iEpIn    = -1; }
            if (iEpOut   >= 0) { close(iEpOut);   iEpOut   = -1; }
            if (iCupsFd  >= 0) { close(iCupsFd);  iCupsFd  = -1; }
            usleep(100000);
            continue;
        }

        if (iEpIn < 0) {
            iEpIn = open(ptCtx->acEpInPath, O_RDWR | O_NONBLOCK);
            if (iEpIn < 0) {
                LogPrintf("%s: IF%d failed to open IN endpoint: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                continue;
            }
            LogPrintf("%s: IF%d ep in opened successfully\n", __FUNCTION__, ptCtx->iIfaceNum);
        }

        if (iEpOut < 0) {
            iEpOut = open(ptCtx->acEpOutPath, O_RDWR | O_NONBLOCK);
            if (iEpOut < 0) {
                LogPrintf("%s: IF%d failed to open OUT endpoint: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                if (iEpIn >= 0) { close(iEpIn); iEpIn = -1; }
                continue;
            }
            LogPrintf("%s: IF%d ep out opened successfully\n", __FUNCTION__, ptCtx->iIfaceNum);
        }

        /* Poll only ep_out. CUPS response is read synchronously right after
         * each write, so no need to poll the CUPS fd. */
        struct pollfd sFd = { .fd = iEpOut, .events = POLLIN | POLLERR | POLLHUP | POLLNVAL };

        int iRv = poll(&sFd, 1, 200);
        if (iRv < 0) {
            if (iEpIn   >= 0) { close(iEpIn);   iEpIn   = -1; }
            if (iEpOut  >= 0) { close(iEpOut);  iEpOut  = -1; }
            if (iCupsFd >= 0) { close(iCupsFd); iCupsFd = -1; }
            LogPrintf("%s: IF%d poll error: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
            continue;
        }

        /* ep_out → CUPS → ep_in: full request/response cycle */
        if (sFd.revents & POLLIN) {
            ssize_t nRead = read(iEpOut, pcBuf, CUPS_BUF_SIZE);
            if (nRead < 0 && (errno == EAGAIN)) {
                /* Spurious POLLIN from FunctionFS — no real data yet, skip. */
                continue;
            } else if (nRead < 0) {
                LogPrintf("%s: IF%d read ep_out: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                if (iEpIn   >= 0) { close(iEpIn);   iEpIn   = -1; }
                if (iEpOut  >= 0) { close(iEpOut);  iEpOut  = -1; }
                if (iCupsFd >= 0) { close(iCupsFd); iCupsFd = -1; }
                continue;
            }

            LogPrintf("%s: IF%d read %zd bytes from ep_out\n", __FUNCTION__, ptCtx->iIfaceNum, nRead);

            if (iCupsFd < 0) {
                iCupsFd = connect_to_cups(ptCtx->iIfaceNum);
                if (iCupsFd < 0) {
                    LogPrintf("%s: IF%d could not connect to CUPS, dropping %zd bytes\n", __FUNCTION__, ptCtx->iIfaceNum, nRead);
                    continue;
                }
                LogPrintf("%s: IF%d connected to CUPS successfully\n", __FUNCTION__, ptCtx->iIfaceNum);
            }

            if (write_all(iCupsFd, pcBuf, (size_t)nRead) < 0) {
                LogPrintf("%s: IF%d write to CUPS failed: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                close(iCupsFd); iCupsFd = -1;
                continue;
            }
            LogPrintf("%s: IF%d forwarded %zd bytes from ep_out to CUPS\n", __FUNCTION__, ptCtx->iIfaceNum, nRead);

            struct pollfd sCupsPfd = { .fd = iCupsFd, .events = POLLIN | POLLERR | POLLHUP | POLLNVAL };
            int iRv = poll(&sCupsPfd, 1, 500);

            if (iRv > 0 && (sCupsPfd.revents & POLLIN)) {
                ssize_t nResp = read(iCupsFd, pcBuf, CUPS_BUF_SIZE);
                
                LogPrintf("%s: IF%d read %zd bytes from CUPS\n", __FUNCTION__, ptCtx->iIfaceNum, nResp);
                
                if (nResp > 0) {
                    if (write_all(iEpIn, pcBuf, (size_t)nResp) < 0) {
                        LogPrintf("%s: IF%d write ep_in: %s\n", __FUNCTION__, ptCtx->iIfaceNum, strerror(errno));
                    }
                    LogPrintf("%s: IF%d wrote %zd bytes to ep_in\n", __FUNCTION__, ptCtx->iIfaceNum, nResp);
                }
            }

            LogPrintf("%s: IF%d close CUPS connection\n", __FUNCTION__, ptCtx->iIfaceNum);
            if (iCupsFd >= 0) { close(iCupsFd); iCupsFd = -1; }
        }
    }

    LogPrintf("%s: IF%d worker thread exiting\n", __FUNCTION__, ptCtx->iIfaceNum);

    if (iEpIn   >= 0) close(iEpIn);
    if (iEpOut  >= 0) close(iEpOut);
    if (iCupsFd >= 0) close(iCupsFd);
    free(pcBuf);

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
                LogPrintf("%s: poll interrupted by signal, continuing\n", __FUNCTION__);
                continue;
            }
            LogPrintf("%s: poll error: %s\n", __FUNCTION__, strerror(errno));
            break;
        }

        if (sFds[0].revents & POLL_IN) {
            iRv = process_ep0_event(iEp0);
            if (iRv == 1)
            {
                g_ctx.fds_ready = 1;
            } 
            else if (iRv == -2)
            {
                g_ctx.fds_ready = 0;
            }
            else if (iRv < 0)
            {
                break;
            }
        }    
    }

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

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
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
    pthread_join(ep1_thread, NULL);
    pthread_join(ep2_thread, NULL);
    pthread_join(ep3_thread, NULL);
    
    LogPrintf("%s: stop usb-gadget\n", __FUNCTION__);
    return EXIT_SUCCESS;
}
