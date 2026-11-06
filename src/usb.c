/*
 * usb.c — the "phone" on the goggles' USB port: an Android Open Accessory gadget (configfs) whose one
 * vendor-class interface with a bulk pair is served from user space through FunctionFS. No tty in between:
 * no echo, no line discipline, no 0x1C -> SIGQUIT.
 *
 * FunctionFS bulk endpoints cannot be polled, so two threads move the data through pipes: ep1 (OUT, from the
 * goggles) -> rx pipe, which the main loop polls; tx pipe -> ep2 (IN). ep0 carries the gadget events.
 * The gadget leaves the bus when ep0 is closed (the process exits) and is bound again on start.
 */
#include "usb.h"

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GADGET "/sys/kernel/config/usb_gadget/metastreamer"
#define FFS_NAME "metastreamer"
#define FFS_MOUNT "/run/metastreamer-usb"
#define UDC_DIR "/sys/class/udc"

static int ep0 = -1, ep_out = -1, ep_in = -1;
static int rx_pipe[2] = {-1, -1}, tx_pipe[2] = {-1, -1};
static volatile int enabled;
static unsigned epoch;
static char udc[64];
static unsigned long long rx_bytes, tx_dropped;

/* session restart (see usb_restart_session): soft_connect off 15 s, on 2 s, off 3 s, on */
static const struct { int on; double hold; } SEQ[] = {{0, 15}, {1, 2}, {0, 3}, {1, 0}};
static int seq_step = -1;
static double seq_t;

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int put(const char *path, const char *v) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, v, strlen(v));
    close(fd);
    return n == (ssize_t)strlen(v) ? 0 : -1;
}

static int putf(const char *name, const char *v) {
    char p[256];
    snprintf(p, sizeof(p), GADGET "/%s", name);
    if (put(p, v) == 0) return 0;
    fprintf(stderr, "[usb] %s: %s\n", p, strerror(errno));
    return -1;
}

/* Whatever an earlier run (or the old serial gadget) left behind */
static void teardown(void) {
    char p[512];
    put(GADGET "/UDC", "\n");
    DIR *d = opendir(GADGET "/configs/c.1");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            snprintf(p, sizeof(p), GADGET "/configs/c.1/%s", e->d_name);
            struct stat st;
            if (lstat(p, &st) == 0 && S_ISLNK(st.st_mode)) unlink(p);
        }
        closedir(d);
    }
    umount2(FFS_MOUNT, MNT_DETACH);
    rmdir(GADGET "/configs/c.1/strings/0x409");
    rmdir(GADGET "/configs/c.1");
    if ((d = opendir(GADGET "/functions"))) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), GADGET "/functions/%s", e->d_name);
            rmdir(p);
        }
        closedir(d);
    }
    rmdir(GADGET "/strings/0x409");
    rmdir(GADGET);
}

static int create(void) {
    if (mkdir(GADGET, 0755) < 0) { perror("[usb] " GADGET); return -1; }
    if (putf("idVendor", "0x18d1") || putf("idProduct", "0x2d00") ||   /* Android Open Accessory */
        putf("bcdDevice", "0x0100") || putf("bcdUSB", "0x0200") ||
        putf("bDeviceClass", "0xff") || putf("bDeviceSubClass", "0xff") || putf("bDeviceProtocol", "0x00"))
        return -1;
    if (mkdir(GADGET "/strings/0x409", 0755) < 0 || mkdir(GADGET "/configs/c.1", 0755) < 0 ||
        mkdir(GADGET "/configs/c.1/strings/0x409", 0755) < 0 || mkdir(GADGET "/functions/ffs." FFS_NAME, 0755) < 0) {
        perror("[usb] configfs mkdir");
        return -1;
    }
    if (putf("strings/0x409/serialnumber", "0123456789ABCDEF") || putf("strings/0x409/manufacturer", "metastreamer") ||
        putf("strings/0x409/product", "metastreamer") ||
        putf("configs/c.1/strings/0x409/configuration", "Config 1: AOA") || putf("configs/c.1/MaxPower", "500"))
        return -1;
    mkdir(FFS_MOUNT, 0755);
    if (mount(FFS_NAME, FFS_MOUNT, "functionfs", 0, NULL) < 0) { perror("[usb] mount functionfs"); return -1; }
    return 0;
}

/* One interface, vendor class and subclass like any Android accessory, bulk OUT (ep1) and IN (ep2) */
static int write_descriptors(void) {
    struct ep_pair {
        struct usb_interface_descriptor intf;
        struct usb_endpoint_descriptor_no_audio out, in;
    } __attribute__((packed));
    static struct {
        struct usb_functionfs_descs_head_v2 header;
        __le32 fs_count, hs_count;
        struct ep_pair fs, hs;
    } __attribute__((packed)) d;
    d.header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    d.header.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    d.header.length = htole32(sizeof(d));
    d.fs_count = d.hs_count = htole32(3);
    struct ep_pair *s[2] = {&d.fs, &d.hs};
    for (int i = 0; i < 2; i++) {
        uint16_t mps = i ? 512 : 64;
        s[i]->intf = (struct usb_interface_descriptor){.bLength = sizeof(s[i]->intf), .bDescriptorType = USB_DT_INTERFACE,
            .bNumEndpoints = 2, .bInterfaceClass = USB_CLASS_VENDOR_SPEC, .bInterfaceSubClass = 0xff, .iInterface = 1};
        s[i]->out = (struct usb_endpoint_descriptor_no_audio){.bLength = sizeof(s[i]->out), .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 1 | USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = htole16(mps)};
        s[i]->in = (struct usb_endpoint_descriptor_no_audio){.bLength = sizeof(s[i]->in), .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 2 | USB_DIR_IN, .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = htole16(mps)};
    }
    static struct {
        struct usb_functionfs_strings_head header;
        __le16 code;
        char str[sizeof("metastreamer")];
    } __attribute__((packed)) st;
    st.header.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    st.header.length = htole32(sizeof(st));
    st.header.str_count = htole32(1);
    st.header.lang_count = htole32(1);
    st.code = htole16(0x0409);
    memcpy(st.str, "metastreamer", sizeof(st.str));

    ep0 = open(FFS_MOUNT "/ep0", O_RDWR | O_CLOEXEC);
    if (ep0 < 0) { perror("[usb] ep0"); return -1; }
    if (write(ep0, &d, sizeof(d)) != (ssize_t)sizeof(d)) { perror("[usb] descriptors"); return -1; }
    if (write(ep0, &st, sizeof(st)) != (ssize_t)sizeof(st)) { perror("[usb] strings"); return -1; }
    fcntl(ep0, F_SETFL, O_NONBLOCK);
    return 0;
}

static int bind_udc(void) {
    for (int i = 0; i < 50 && !udc[0]; i++) {
        DIR *d = opendir(UDC_DIR);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.') { snprintf(udc, sizeof(udc), "%s", e->d_name); break; }
        if (d) closedir(d);
        if (!udc[0]) usleep(200000);
    }
    if (!udc[0]) { fprintf(stderr, "[usb] no USB device controller (dtoverlay=dwc2,dr_mode=peripheral?)\n"); return -1; }
    return putf("UDC", udc);
}

static void *rx_thread(void *arg) {
    (void)arg;
    static uint8_t b[64 * 1024];
    for (;;) {
        ssize_t n = read(ep_out, b, sizeof(b));
        if (n <= 0) { usleep(20000); continue; }   /* ESHUTDOWN while the host has us disabled */
        rx_bytes += (size_t)n;
        for (ssize_t o = 0; o < n;) {
            ssize_t w = write(rx_pipe[1], b + o, (size_t)(n - o));
            if (w > 0) o += w;
            else if (errno != EINTR) break;
        }
    }
    return NULL;
}

static void *tx_thread(void *arg) {
    (void)arg;
    static uint8_t b[4096];
    for (;;) {
        ssize_t n = read(tx_pipe[0], b, sizeof(b));
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; usleep(20000); continue; }
        if (!enabled) { tx_dropped += (size_t)n; continue; }   /* a write would wait for the next session */
        if (write(ep_in, b, (size_t)n) != n) tx_dropped += (size_t)n;
    }
    return NULL;
}

int usb_start(void) {
    teardown();
    if (create() < 0 || write_descriptors() < 0) return -1;
    if (symlink(GADGET "/functions/ffs." FFS_NAME, GADGET "/configs/c.1/ffs." FFS_NAME) < 0) {
        perror("[usb] link function");
        return -1;
    }
    ep_out = open(FFS_MOUNT "/ep1", O_RDONLY | O_CLOEXEC);
    ep_in = open(FFS_MOUNT "/ep2", O_WRONLY | O_CLOEXEC);
    if (ep_out < 0 || ep_in < 0) { perror("[usb] bulk endpoints"); return -1; }
    if (pipe2(rx_pipe, O_CLOEXEC) < 0 || pipe2(tx_pipe, O_CLOEXEC) < 0) { perror("[usb] pipe"); return -1; }
    fcntl(rx_pipe[1], F_SETPIPE_SZ, 1024 * 1024);
    fcntl(rx_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(tx_pipe[1], F_SETFL, O_NONBLOCK);
    pthread_t t;
    if (pthread_create(&t, NULL, rx_thread, NULL) || pthread_create(&t, NULL, tx_thread, NULL)) return -1;
    if (bind_udc() < 0) return -1;
    printf("[usb] AOA gadget 18d1:2d00 bound to %s (FunctionFS)\n", udc);
    return 0;
}

int usb_rx_fd(void) { return rx_pipe[0]; }
int usb_event_fd(void) { return ep0; }
int usb_enabled(void) { return enabled; }
unsigned usb_epoch(void) { return epoch; }

ssize_t usb_read(void *b, size_t n) { return read(rx_pipe[0], b, n); }

void usb_write(const void *b, size_t n) {
    if (!enabled || write(tx_pipe[1], b, n) != (ssize_t)n) tx_dropped += n;
}

void usb_events(void) {
    struct usb_functionfs_event ev[4];
    ssize_t n;
    while ((n = read(ep0, ev, sizeof(ev))) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            switch (ev[i].type) {
            case FUNCTIONFS_ENABLE:
                enabled = 1; epoch++;
                printf("[usb] enabled by the goggles\n");
                break;
            case FUNCTIONFS_DISABLE:
            case FUNCTIONFS_UNBIND:
                if (enabled) printf("[usb] %s\n", ev[i].type == FUNCTIONFS_DISABLE ? "disabled (cable pulled?)" : "unbound");
                enabled = 0; epoch++;
                break;
            case FUNCTIONFS_SETUP: {
                /* nothing to answer: stall (read for an IN request, write for an OUT one) */
                const struct usb_ctrlrequest *r = &ev[i].u.setup;
                printf("[usb] control request %02x/%02x stalled\n", r->bRequestType, r->bRequest);
                if (r->bRequestType & USB_DIR_IN) { if (read(ep0, NULL, 0) < 0) {} }
                else if (write(ep0, NULL, 0) < 0) {}
                break;
            }
            default:
                break;
            }
        }
    }
}

/* The goggles keep an accessory session across a short drop off the bus and then never talk to us again;
 * this sequence makes them open a new one (Goggles 2, five tries out of five, see docs/protocol.md). */
void usb_restart_session(void) {
    if (seq_step >= 0 || !udc[0]) return;
    printf("[usb] restarting the goggles' session (about 20 s)\n");
    seq_step = 0;
    seq_t = 0;
}

int usb_restarting(void) { return seq_step >= 0; }

void usb_tick(void) {
    if (seq_step < 0) return;
    double t = mono();
    if (seq_t && t < seq_t) return;
    if (seq_step == (int)(sizeof(SEQ) / sizeof(SEQ[0]))) { seq_step = -1; return; }
    char p[160];
    snprintf(p, sizeof(p), UDC_DIR "/%s/soft_connect", udc);
    put(p, SEQ[seq_step].on ? "connect" : "disconnect");
    seq_t = t + SEQ[seq_step].hold;
    seq_step++;
}

size_t usb_stats(char *out, size_t cap) {
    int n = snprintf(out, cap, "usb %s rx=%llu tx_dropped=%llu", enabled ? "on" : "off", rx_bytes, tx_dropped);
    return n < (int)cap ? (size_t)n : cap - 1;
}
