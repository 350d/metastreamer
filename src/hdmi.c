/*
 * hdmi.c — hardware H.264 decode on Raspberry Pi (V4L2 M2M, bcm2835-codec, multi-planar API)
 * and HDMI output via a KMS (vc4) overlay plane, zero-copy: the decoder buffer is exported
 * as a dmabuf and imported into DRM as a YUV420 framebuffer. Scaling and YUV->RGB are done by the HVS.
 *
 * Requires: config.txt gpu_mem=128 (full firmware with codecs), dtoverlay=vc4-kms-v3d,cma-128;
 * the bcm2835_codec module must not be blacklisted (DietPi blacklists it by default).
 * Runs in a separate thread. Frames go to the screen with non-blocking atomic page flips, newest frame wins:
 * when the decoder delivers faster than the display refreshes, single frames are skipped at the display and the
 * decoder never waits for a vblank. Only if the input queue overflows anyway are frames dropped until the next
 * keyframe. JPEG snapshots are encoded in their own thread.
 */
#include "hdmi.h"

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NOUT 6
#define NCAP 10
#define QN 16
#define T_OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define T_CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

/* ---------------- DRM ---------------- */
static int dfd = -1;
static uint32_t conn_id, crtc_id, plane_id;
static int crtc_idx;
static struct drm_mode_modeinfo mode;
/* Wanted output mode. w=0: use the EDID preferred mode. hz=0: prefer 50 Hz (goggles stream is 50 fps), then 60. */
static unsigned want_w = 1920, want_h = 1080, want_hz = 0;

int hdmi_set_mode(const char *s) {
    if (!strcmp(s, "auto")) { want_w = 0; return 0; }
    unsigned w, h, hz = 0;
    if (sscanf(s, "%ux%u@%u", &w, &h, &hz) < 2 || !w || !h) return -1;
    want_w = w; want_h = h; want_hz = hz;
    return 0;
}

/* Many sinks (USB capture cards, small monitors) list 720p as "preferred" in EDID,
 * so we pick the mode ourselves: exact size, progressive, closest refresh. */
static struct drm_mode_modeinfo pick_mode(const struct drm_mode_modeinfo *m, uint32_t n) {
    int pref = 0, best = -1, best_score = 1 << 30;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i].type & DRM_MODE_TYPE_PREFERRED) { pref = i; break; }
    }
    if (want_w) {
        for (uint32_t i = 0; i < n; i++) {
            if (m[i].hdisplay != want_w || m[i].vdisplay != want_h || (m[i].flags & DRM_MODE_FLAG_INTERLACE)) continue;
            int hz = m[i].vrefresh, score;
            if (want_hz) score = abs(hz - (int)want_hz);
            else score = hz == 50 ? 0 : hz == 60 ? 1 : 100 + abs(hz - 50);
            if (score < best_score) { best_score = score; best = i; }
        }
        if (best < 0)
            printf("[hdmi] %ux%u not offered by the display, using its preferred mode\n", want_w, want_h);
    }
    return m[best >= 0 ? best : pref];
}

static int drm_open(void) {
    char path[32];
    for (int n = 0; n < 4; n++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", n);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct drm_mode_card_res res = {0};
        if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || res.count_connectors == 0 || res.count_crtcs == 0) {
            close(fd); continue;
        }
        uint32_t *crtcs = calloc(res.count_crtcs, 4), *conns = calloc(res.count_connectors, 4);
        uint32_t *encs = calloc(res.count_encoders ? res.count_encoders : 1, 4), *fbs = calloc(res.count_fbs ? res.count_fbs : 1, 4);
        res.crtc_id_ptr = (uintptr_t)crtcs; res.connector_id_ptr = (uintptr_t)conns;
        res.encoder_id_ptr = (uintptr_t)encs; res.fb_id_ptr = (uintptr_t)fbs;
        if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { close(fd); continue; }

        for (uint32_t i = 0; i < res.count_connectors && !conn_id; i++) {
            struct drm_mode_get_connector c = {.connector_id = conns[i]};
            if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.connection != 1 || c.count_modes == 0) continue;
            struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
            uint32_t *cenc = calloc(c.count_encoders ? c.count_encoders : 1, 4);
            uint32_t *props = calloc(c.count_props ? c.count_props : 1, 4);
            uint64_t *pvals = calloc(c.count_props ? c.count_props : 1, 8);
            c.modes_ptr = (uintptr_t)modes; c.encoders_ptr = (uintptr_t)cenc;
            c.props_ptr = (uintptr_t)props; c.prop_values_ptr = (uintptr_t)pvals;
            if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) continue;
            mode = pick_mode(modes, c.count_modes);
            struct drm_mode_get_encoder e = {.encoder_id = c.encoder_id ? c.encoder_id : cenc[0]};
            if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0) continue;
            for (uint32_t k = 0; k < res.count_crtcs; k++) {
                if (e.crtc_id ? crtcs[k] == e.crtc_id : (e.possible_crtcs & (1u << k))) {
                    crtc_id = crtcs[k]; crtc_idx = k; break;
                }
            }
            if (crtc_id) conn_id = conns[i];
        }
        if (!conn_id) { close(fd); continue; }
        dfd = fd;
        printf("[hdmi] %s: %ux%u@%u, crtc %u\n", path, mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc_id);
        return 0;
    }
    return -1;
}

/* atomic page flips on the overlay plane */
static int atomic_ok, need_full = 1;
static uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name) {
    struct drm_mode_obj_get_properties gp = {.obj_id = obj, .obj_type = type};
    if (xioctl(dfd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0 || !gp.count_props) return 0;
    uint32_t *ids = calloc(gp.count_props, 4); uint64_t *vals = calloc(gp.count_props, 8), found = 0;
    gp.props_ptr = (uintptr_t)ids; gp.prop_values_ptr = (uintptr_t)vals;
    if (xioctl(dfd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) == 0)
        for (uint32_t i = 0; i < gp.count_props && !found; i++) {
            struct drm_mode_get_property p = {.prop_id = ids[i]};
            if (xioctl(dfd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0 && !strcmp(p.name, name)) found = ids[i];
        }
    free(ids); free(vals);
    return (uint32_t)found;
}

static void atomic_setup(void) {
    struct drm_set_client_cap cap = {DRM_CLIENT_CAP_ATOMIC, 1};
    if (xioctl(dfd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) return;
    uint32_t t = DRM_MODE_OBJECT_PLANE;
    p_fb = prop_id(plane_id, t, "FB_ID"); p_crtc = prop_id(plane_id, t, "CRTC_ID");
    p_sx = prop_id(plane_id, t, "SRC_X"); p_sy = prop_id(plane_id, t, "SRC_Y");
    p_sw = prop_id(plane_id, t, "SRC_W"); p_sh = prop_id(plane_id, t, "SRC_H");
    p_cx = prop_id(plane_id, t, "CRTC_X"); p_cy = prop_id(plane_id, t, "CRTC_Y");
    p_cw = prop_id(plane_id, t, "CRTC_W"); p_ch = prop_id(plane_id, t, "CRTC_H");
    atomic_ok = p_fb && p_crtc && p_sx && p_sy && p_sw && p_sh && p_cx && p_cy && p_cw && p_ch;
    if (atomic_ok) fcntl(dfd, F_SETFL, fcntl(dfd, F_GETFL) | O_NONBLOCK);   /* flip events are read after poll() */
}

static int drm_setup_crtc_and_plane(void) {
    struct drm_mode_create_dumb cd = {.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
    if (xioctl(dfd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) { perror("[hdmi] create_dumb"); return -1; }
    struct drm_mode_fb_cmd2 f = {.width = mode.hdisplay, .height = mode.vdisplay, .pixel_format = DRM_FORMAT_XRGB8888};
    f.handles[0] = cd.handle; f.pitches[0] = cd.pitch;
    if (xioctl(dfd, DRM_IOCTL_MODE_ADDFB2, &f) < 0) { perror("[hdmi] addfb bg"); return -1; }
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (xioctl(dfd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
        void *p = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, md.offset);
        if (p != MAP_FAILED) { memset(p, 0, cd.size); munmap(p, cd.size); }
    }
    struct drm_mode_crtc sc = {.crtc_id = crtc_id, .fb_id = f.fb_id, .set_connectors_ptr = (uintptr_t)&conn_id,
                               .count_connectors = 1, .mode = mode, .mode_valid = 1};
    if (xioctl(dfd, DRM_IOCTL_MODE_SETCRTC, &sc) < 0) { perror("[hdmi] setcrtc"); return -1; }

    struct drm_set_client_cap cap = {DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1};
    xioctl(dfd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    struct drm_mode_get_plane_res pr = {0};
    if (xioctl(dfd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0 || !pr.count_planes) return -1;
    uint32_t *ids = calloc(pr.count_planes, 4);
    pr.plane_id_ptr = (uintptr_t)ids;
    xioctl(dfd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
    for (uint32_t i = 0; i < pr.count_planes && !plane_id; i++) {
        struct drm_mode_get_plane gp = {.plane_id = ids[i]};
        if (xioctl(dfd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0) continue;
        uint32_t *fm = calloc(gp.count_format_types ? gp.count_format_types : 1, 4);
        gp.format_type_ptr = (uintptr_t)fm;
        if (xioctl(dfd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0) continue;
        if (!(gp.possible_crtcs & (1u << crtc_idx)) || gp.fb_id) continue;
        for (uint32_t k = 0; k < gp.count_format_types; k++)
            if (fm[k] == DRM_FORMAT_YUV420) { plane_id = ids[i]; break; }
    }
    if (!plane_id) { fprintf(stderr, "[hdmi] no plane with YUV420\n"); return -1; }
    atomic_setup();
    printf("[hdmi] plane %u, %s\n", plane_id, atomic_ok ? "atomic page flips" : "legacy SETPLANE (blocking)");
    return 0;
}

/* ---------------- V4L2 decoder (MPLANE) ---------------- */
static int vfd = -1;
static void *out_map[NOUT]; static uint32_t out_len[NOUT]; static int out_free[NOUT]; static int nout;
static int ncap; static uint32_t cap_fb[NCAP]; static int cap_dmabuf[NCAP];
static void *cap_map[NCAP]; static uint32_t cap_len[NCAP];
static uint32_t vis_w = 1920, vis_h = 1080, buf_w, buf_h, buf_bpl;
static int display;   /* 0 = no HDMI: decode only (for /snapshot) */
static int cap_streaming, shown = -1;
static unsigned long long n_in, n_dec, n_shown, n_drop;

static int v4l2_open(void) {
    char path[32];
    for (int n = 0; n < 64; n++) {
        snprintf(path, sizeof(path), "/dev/video%d", n);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability c = {0};
        uint32_t caps = 0;
        if (xioctl(fd, VIDIOC_QUERYCAP, &c) == 0) caps = (c.capabilities & V4L2_CAP_DEVICE_CAPS) ? c.device_caps : c.capabilities;
        if (strstr((char *)c.card, "decode") && (caps & V4L2_CAP_VIDEO_M2M_MPLANE)) {
            printf("[hdmi] decoder %s (%s)\n", path, c.card);
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int qbuf(int type, int idx, uint32_t bytesused) {
    struct v4l2_plane pl[1] = {{0}};
    pl[0].bytesused = bytesused;
    struct v4l2_buffer b = {.type = type, .memory = V4L2_MEMORY_MMAP, .index = idx, .m.planes = pl, .length = 1};
    return xioctl(vfd, VIDIOC_QBUF, &b);
}

static void flips_settle(void);
static void cap_release(void) {
    if (display) flips_settle();
    if (cap_streaming) { int t = T_CAP; xioctl(vfd, VIDIOC_STREAMOFF, &t); cap_streaming = 0; }
    for (int i = 0; i < ncap; i++) {
        if (cap_fb[i]) { xioctl(dfd, DRM_IOCTL_MODE_RMFB, &cap_fb[i]); cap_fb[i] = 0; }
        if (cap_dmabuf[i] >= 0) { close(cap_dmabuf[i]); cap_dmabuf[i] = -1; }
        if (cap_map[i]) { munmap(cap_map[i], cap_len[i]); cap_map[i] = NULL; }
    }
    struct v4l2_requestbuffers rb = {.count = 0, .type = T_CAP, .memory = V4L2_MEMORY_MMAP};
    xioctl(vfd, VIDIOC_REQBUFS, &rb);
    ncap = 0; shown = -1;
}

static int cap_setup(void) {
    cap_release();
    struct v4l2_format f = {.type = T_CAP};
    if (xioctl(vfd, VIDIOC_G_FMT, &f) < 0) { perror("[hdmi] g_fmt cap"); return -1; }
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 || f.fmt.pix_mp.num_planes != 1) {
        f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420; f.fmt.pix_mp.num_planes = 1;
        if (xioctl(vfd, VIDIOC_S_FMT, &f) < 0) { perror("[hdmi] s_fmt cap"); return -1; }
    }
    uint32_t aw = f.fmt.pix_mp.width, ah = f.fmt.pix_mp.height, bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    vis_w = aw; vis_h = ah; buf_w = aw; buf_h = ah; buf_bpl = bpl;
    struct v4l2_selection sel = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .target = V4L2_SEL_TGT_COMPOSE};
    if (xioctl(vfd, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width && sel.r.height) { vis_w = sel.r.width; vis_h = sel.r.height; }

    struct v4l2_requestbuffers rb = {.count = NCAP, .type = T_CAP, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(vfd, VIDIOC_REQBUFS, &rb) < 0) { perror("[hdmi] reqbufs cap"); return -1; }
    ncap = rb.count > NCAP ? NCAP : rb.count;
    for (int i = 0; i < ncap; i++) {
        struct v4l2_plane qp[1] = {{0}};
        struct v4l2_buffer qb = {.type = T_CAP, .memory = V4L2_MEMORY_MMAP, .index = i, .m.planes = qp, .length = 1};
        if (xioctl(vfd, VIDIOC_QUERYBUF, &qb) == 0) {
            cap_map[i] = mmap(NULL, qp[0].length, PROT_READ, MAP_SHARED, vfd, qp[0].m.mem_offset);
            if (cap_map[i] == MAP_FAILED) cap_map[i] = NULL; else cap_len[i] = qp[0].length;
        }
        if (!display) { if (qbuf(T_CAP, i, 0) < 0) { perror("[hdmi] qbuf cap"); return -1; } continue; }
        struct v4l2_exportbuffer eb = {.type = T_CAP, .index = i, .plane = 0, .flags = O_CLOEXEC};
        if (xioctl(vfd, VIDIOC_EXPBUF, &eb) < 0) { perror("[hdmi] expbuf"); return -1; }
        cap_dmabuf[i] = eb.fd;
        struct drm_prime_handle ph = {.fd = eb.fd};
        if (xioctl(dfd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0) { perror("[hdmi] prime"); return -1; }
        struct drm_mode_fb_cmd2 fb = {.width = vis_w, .height = vis_h, .pixel_format = DRM_FORMAT_YUV420};
        fb.handles[0] = fb.handles[1] = fb.handles[2] = ph.handle;
        fb.pitches[0] = bpl; fb.pitches[1] = fb.pitches[2] = bpl / 2;
        fb.offsets[0] = 0; fb.offsets[1] = bpl * ah; fb.offsets[2] = fb.offsets[1] + (bpl / 2) * (ah / 2);
        if (xioctl(dfd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) { perror("[hdmi] addfb2 yuv"); return -1; }
        cap_fb[i] = fb.fb_id;
        if (qbuf(T_CAP, i, 0) < 0) { perror("[hdmi] qbuf cap"); return -1; }
    }
    int t = T_CAP;
    if (xioctl(vfd, VIDIOC_STREAMON, &t) < 0) { perror("[hdmi] streamon cap"); return -1; }
    cap_streaming = 1;
    printf("[hdmi] capture %ux%u (buffer %ux%u, stride %u), %d buffers\n", vis_w, vis_h, aw, ah, bpl, ncap);
    return 0;
}

static int v4l2_setup(void) {
    for (int i = 0; i < NCAP; i++) cap_dmabuf[i] = -1;
    struct v4l2_format f = {.type = T_OUT};
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264; f.fmt.pix_mp.width = 1920; f.fmt.pix_mp.height = 1080;
    f.fmt.pix_mp.num_planes = 1; f.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;
    if (xioctl(vfd, VIDIOC_S_FMT, &f) < 0) { perror("[hdmi] s_fmt out"); return -1; }
    struct v4l2_format fc = {.type = T_CAP};
    fc.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420; fc.fmt.pix_mp.width = 1920; fc.fmt.pix_mp.height = 1080;
    fc.fmt.pix_mp.num_planes = 1;
    xioctl(vfd, VIDIOC_S_FMT, &fc);
    struct v4l2_event_subscription sub = {.type = V4L2_EVENT_SOURCE_CHANGE};
    xioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &sub);

    struct v4l2_requestbuffers rb = {.count = NOUT, .type = T_OUT, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(vfd, VIDIOC_REQBUFS, &rb) < 0 || rb.count == 0) { perror("[hdmi] reqbufs out"); return -1; }
    nout = rb.count > NOUT ? NOUT : rb.count;
    for (int i = 0; i < nout; i++) {
        struct v4l2_plane pl[1] = {{0}};
        struct v4l2_buffer b = {.type = T_OUT, .memory = V4L2_MEMORY_MMAP, .index = i, .m.planes = pl, .length = 1};
        if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) { perror("[hdmi] querybuf out"); return -1; }
        out_map[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, pl[0].m.mem_offset);
        if (out_map[i] == MAP_FAILED) { perror("[hdmi] mmap out"); return -1; }
        out_len[i] = pl[0].length; out_free[i] = 1;
    }
    int t = T_OUT;
    if (xioctl(vfd, VIDIOC_STREAMON, &t) < 0) { perror("[hdmi] streamon out"); return -1; }
    return cap_setup();
}

static void fit(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    uint32_t mw = mode.hdisplay, mh = mode.vdisplay;
    *w = mw; *h = (uint64_t)mw * vis_h / vis_w;
    if (*h > mh) { *h = mh; *w = (uint64_t)mh * vis_w / vis_h; }
    *x = (mw - *w) / 2; *y = (mh - *h) / 2;
}

/* legacy path (no atomic): blocks until the next vblank */
static void show(int idx) {
    uint32_t x, y, w, h; fit(&x, &y, &w, &h);
    struct drm_mode_set_plane sp = {.plane_id = plane_id, .crtc_id = crtc_id, .fb_id = cap_fb[idx],
        .crtc_x = (int)x, .crtc_y = (int)y, .crtc_w = w, .crtc_h = h,
        .src_x = 0, .src_y = 0, .src_w = vis_w << 16, .src_h = vis_h << 16};
    int ok = xioctl(dfd, DRM_IOCTL_MODE_SETPLANE, &sp) == 0;
    if (ok) n_shown++;
    int back = ok ? shown : idx;
    if (ok) shown = idx;
    if (back >= 0) qbuf(T_CAP, back, 0);
}

/* atomic path: `shown` is on screen, `in_flight` waits for its vblank, `waiting` holds up to WAIT_MAX decoded
 * frames, one per vblank: radio jitter (two frames within one refresh, then none) is absorbed instead of
 * turning into a skip plus a repeat. Beyond WAIT_MAX the oldest waiting frame is skipped (bounded latency). */
#define WAIT_MAX 3
static int in_flight = -1, waiting[WAIT_MAX], nwait;
static unsigned long long n_skip;

static int flip(int idx) {
    uint32_t props[10], n = 0; uint64_t vals[10];
    props[n] = p_fb; vals[n++] = cap_fb[idx];
    if (need_full) {
        uint32_t x, y, w, h; fit(&x, &y, &w, &h);
        props[n] = p_crtc; vals[n++] = crtc_id;
        props[n] = p_sx; vals[n++] = 0; props[n] = p_sy; vals[n++] = 0;
        props[n] = p_sw; vals[n++] = (uint64_t)vis_w << 16; props[n] = p_sh; vals[n++] = (uint64_t)vis_h << 16;
        props[n] = p_cx; vals[n++] = x; props[n] = p_cy; vals[n++] = y;
        props[n] = p_cw; vals[n++] = w; props[n] = p_ch; vals[n++] = h;
    }
    uint32_t objs[1] = {plane_id}, counts[1] = {n};
    struct drm_mode_atomic a = {.flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, .count_objs = 1,
        .objs_ptr = (uintptr_t)objs, .count_props_ptr = (uintptr_t)counts, .props_ptr = (uintptr_t)props,
        .prop_values_ptr = (uintptr_t)vals, .user_data = (uint64_t)idx};
    if (xioctl(dfd, DRM_IOCTL_MODE_ATOMIC, &a) < 0) return -1;
    need_full = 0;
    return 0;
}

static void present(int idx) {
    if (!atomic_ok) { show(idx); return; }
    if (in_flight >= 0) {   /* a flip is pending: the frame waits its turn */
        if (nwait == WAIT_MAX) {
            qbuf(T_CAP, waiting[0], 0); n_skip++;
            memmove(waiting, waiting + 1, (WAIT_MAX - 1) * sizeof(int)); nwait--;
        }
        waiting[nwait++] = idx;
        return;
    }
    if (flip(idx) == 0) { in_flight = idx; return; }
    perror("[hdmi] atomic flip, falling back to SETPLANE");
    atomic_ok = 0;
    show(idx);
}

static void flip_done(void) {
    if (in_flight < 0) return;
    if (shown >= 0) qbuf(T_CAP, shown, 0);
    shown = in_flight; in_flight = -1; n_shown++;
    if (nwait) {
        int k = waiting[0];
        memmove(waiting, waiting + 1, (WAIT_MAX - 1) * sizeof(int)); nwait--;
        present(k);
    }
}

static void drm_events(void) {
    char buf[256];
    ssize_t n = read(dfd, buf, sizeof(buf));
    for (ssize_t o = 0; n > 0 && o + (ssize_t)sizeof(struct drm_event) <= n;) {
        struct drm_event *e = (struct drm_event *)(buf + o);
        if (e->length < sizeof(*e)) break;
        if (e->type == DRM_EVENT_FLIP_COMPLETE) flip_done();
        o += e->length;
    }
}

/* before the capture buffers are replaced: let a pending flip land */
static void flips_settle(void) {
    for (int i = 0; atomic_ok && in_flight >= 0 && i < 10; i++) {
        struct pollfd p = {dfd, POLLIN, 0};
        if (poll(&p, 1, 20) > 0) drm_events();
    }
    in_flight = -1; nwait = 0; need_full = 1;
}

/* ---------------- JPEG snapshots (hardware encoder bcm2835-codec-encode_image) ---------------- */
static int jfd = -1, jready, jfailed;
static void *jin_map, *jout_map; static uint32_t jin_len, jout_len;
static volatile int snap_req;
static pthread_mutex_t sm = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *snap_buf; static size_t snap_n, snap_cap; static unsigned snap_seq; static double snap_t;
static int wake_pipe[2] = {-1, -1};
static unsigned long long n_snaps;

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static int jpeg_setup(void) {
    char path[32];
    for (int n = 0; n < 64 && jfd < 0; n++) {
        snprintf(path, sizeof(path), "/dev/video%d", n);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability c = {0};
        if (xioctl(fd, VIDIOC_QUERYCAP, &c) == 0 && strstr((char *)c.card, "encode_image")) jfd = fd; else close(fd);
    }
    if (jfd < 0) { fprintf(stderr, "[snap] JPEG encoder not found\n"); return -1; }
    struct v4l2_format fo = {.type = T_OUT};
    fo.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420; fo.fmt.pix_mp.width = buf_w; fo.fmt.pix_mp.height = buf_h;
    fo.fmt.pix_mp.num_planes = 1; fo.fmt.pix_mp.plane_fmt[0].bytesperline = buf_bpl;
    fo.fmt.pix_mp.plane_fmt[0].sizeimage = buf_bpl * buf_h * 3 / 2;
    if (xioctl(jfd, VIDIOC_S_FMT, &fo) < 0) { perror("[snap] s_fmt out"); return -1; }
    struct v4l2_selection sel = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .target = V4L2_SEL_TGT_CROP,
                                 .r = {.left = 0, .top = 0, .width = vis_w, .height = vis_h}};
    xioctl(jfd, VIDIOC_S_SELECTION, &sel);   /* crop 1088 -> 1080; not critical if unsupported */
    struct v4l2_format fc = {.type = T_CAP};
    fc.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG; fc.fmt.pix_mp.width = vis_w; fc.fmt.pix_mp.height = vis_h;
    fc.fmt.pix_mp.num_planes = 1; fc.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;
    if (xioctl(jfd, VIDIOC_S_FMT, &fc) < 0) { perror("[snap] s_fmt cap"); return -1; }
    struct v4l2_control q = {.id = V4L2_CID_JPEG_COMPRESSION_QUALITY, .value = 85};
    xioctl(jfd, VIDIOC_S_CTRL, &q);
    int types[2] = {T_OUT, T_CAP}; void **maps[2] = {&jin_map, &jout_map}; uint32_t *lens[2] = {&jin_len, &jout_len};
    for (int k = 0; k < 2; k++) {
        struct v4l2_requestbuffers rb = {.count = 1, .type = types[k], .memory = V4L2_MEMORY_MMAP};
        if (xioctl(jfd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 1) { perror("[snap] reqbufs"); return -1; }
        struct v4l2_plane pl[1] = {{0}};
        struct v4l2_buffer b = {.type = types[k], .memory = V4L2_MEMORY_MMAP, .index = 0, .m.planes = pl, .length = 1};
        if (xioctl(jfd, VIDIOC_QUERYBUF, &b) < 0) { perror("[snap] querybuf"); return -1; }
        *maps[k] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, jfd, pl[0].m.mem_offset);
        if (*maps[k] == MAP_FAILED) { perror("[snap] mmap"); return -1; }
        *lens[k] = pl[0].length;
        if (xioctl(jfd, VIDIOC_STREAMON, &types[k]) < 0) { perror("[snap] streamon"); return -1; }
    }
    printf("[snap] JPEG encoder ready (%ux%u -> %ux%u)\n", buf_w, buf_h, vis_w, vis_h);
    return 0;
}

static void jpeg_encode(uint32_t n);
static void jqbuf(int type, uint32_t bytes) {
    struct v4l2_plane pl[1] = {{0}}; pl[0].bytesused = bytes;
    struct v4l2_buffer b = {.type = type, .memory = V4L2_MEMORY_MMAP, .index = 0, .m.planes = pl, .length = 1};
    xioctl(jfd, VIDIOC_QBUF, &b);
}

static pthread_mutex_t jm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jc = PTHREAD_COND_INITIALIZER;
static int jbusy; static uint32_t jbytes;

/* decoder thread: copy the frame for the encoder (a few ms); the encoding runs in jpeg_thread */
static void do_snapshot(int idx) {
    if (!jready && !jfailed) { if (jpeg_setup() == 0) jready = 1; else jfailed = 1; }
    if (!jready || !cap_map[idx]) { snap_req = 0; if (wake_pipe[1] >= 0) (void)!write(wake_pipe[1], "e", 1); return; }
    pthread_mutex_lock(&jm);
    if (jbusy) { pthread_mutex_unlock(&jm); return; }   /* try again with a later frame */
    snap_req = 0;
    uint32_t n = buf_bpl * buf_h * 3 / 2;
    if (n > cap_len[idx]) n = cap_len[idx];
    if (n > jin_len) n = jin_len;
    memcpy(jin_map, cap_map[idx], n);
    jbytes = n; jbusy = 1;
    pthread_cond_signal(&jc);
    pthread_mutex_unlock(&jm);
}

static void *jpeg_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&jm);
        while (!jbusy) pthread_cond_wait(&jc, &jm);
        uint32_t n = jbytes;
        pthread_mutex_unlock(&jm);
        jpeg_encode(n);
        pthread_mutex_lock(&jm); jbusy = 0; pthread_mutex_unlock(&jm);
    }
    return NULL;
}

static void jpeg_encode(uint32_t n) {
    jqbuf(T_CAP, 0);
    jqbuf(T_OUT, n);
    struct pollfd p = {jfd, POLLIN, 0};
    uint32_t got = 0;
    if (poll(&p, 1, 1000) > 0) {
        struct v4l2_plane pl[1] = {{0}};
        struct v4l2_buffer b = {.type = T_CAP, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1};
        if (xioctl(jfd, VIDIOC_DQBUF, &b) == 0) got = pl[0].bytesused;
    }
    struct v4l2_plane po[1] = {{0}};
    struct v4l2_buffer bo = {.type = T_OUT, .memory = V4L2_MEMORY_MMAP, .m.planes = po, .length = 1};
    xioctl(jfd, VIDIOC_DQBUF, &bo);
    if (got) {
        pthread_mutex_lock(&sm);
        if (snap_cap < got) { snap_buf = realloc(snap_buf, got); snap_cap = got; }
        memcpy(snap_buf, jout_map, got); snap_n = got; snap_seq++; snap_t = now_s(); n_snaps++;
        pthread_mutex_unlock(&sm);
    }
    if (wake_pipe[1] >= 0) (void)!write(wake_pipe[1], got ? "s" : "e", 1);
}

int hdmi_decoder_running(void);
void hdmi_snapshot_request(void) { snap_req = 1; }
int hdmi_snapshot_wake_fd(void) { return wake_pipe[0]; }
const uint8_t *hdmi_snapshot_lock(size_t *n, unsigned *seq, double *age) {
    pthread_mutex_lock(&sm);
    *n = snap_n; *seq = snap_seq; *age = snap_n ? now_s() - snap_t : 1e9;
    return snap_buf;
}
void hdmi_snapshot_unlock(void) { pthread_mutex_unlock(&sm); }

/* ---------------- frame queue from the main thread ---------------- */
static struct { uint8_t *p; size_t n, cap; int key; } q[QN];
static int qh, qt, qc, drop_until_key = 1;
static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static int running;

void hdmi_feed_au(const uint8_t *d, size_t n, int key) {
    if (!running) return;
    pthread_mutex_lock(&qm);
    n_in++;
    if (drop_until_key && !key) { n_drop++; pthread_mutex_unlock(&qm); return; }
    if (qc == QN) { drop_until_key = 1; n_drop++; pthread_mutex_unlock(&qm); return; }
    drop_until_key = 0;
    if (q[qt].cap < n) { q[qt].p = realloc(q[qt].p, n); q[qt].cap = n; }
    memcpy(q[qt].p, d, n); q[qt].n = n; q[qt].key = key;
    qt = (qt + 1) % QN; qc++;
    pthread_mutex_unlock(&qm);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd pp[2] = {{vfd, POLLIN | POLLOUT | POLLPRI, 0}, {dfd, POLLIN, 0}};
        poll(pp, display && atomic_ok ? 2 : 1, 5);
        if (display && atomic_ok && (pp[1].revents & POLLIN)) drm_events();
        struct pollfd p = pp[0];
        if (p.revents & POLLPRI) {
            struct v4l2_event ev;
            while (xioctl(vfd, VIDIOC_DQEVENT, &ev) == 0)
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE) { printf("[hdmi] format change\n"); cap_setup(); }
        }
        for (;;) {
            struct v4l2_plane pl[1] = {{0}};
            struct v4l2_buffer b = {.type = T_CAP, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1};
            if (!cap_streaming || xioctl(vfd, VIDIOC_DQBUF, &b) < 0) break;
            if (pl[0].bytesused == 0 || (b.flags & V4L2_BUF_FLAG_ERROR)) { qbuf(T_CAP, b.index, 0); continue; }
            n_dec++;
            if (snap_req) do_snapshot(b.index);
            if (display) present(b.index); else qbuf(T_CAP, b.index, 0);
        }
        for (;;) {
            struct v4l2_plane pl[1] = {{0}};
            struct v4l2_buffer b = {.type = T_OUT, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1};
            if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) break;
            if (b.index < (unsigned)nout) out_free[b.index] = 1;
        }
        for (int i = 0; i < nout; i++) {
            if (!out_free[i]) continue;
            pthread_mutex_lock(&qm);
            if (qc == 0) { pthread_mutex_unlock(&qm); break; }
            size_t n = q[qh].n;
            if (n > out_len[i]) n = out_len[i];
            memcpy(out_map[i], q[qh].p, n);
            qh = (qh + 1) % QN; qc--;
            pthread_mutex_unlock(&qm);
            if (qbuf(T_OUT, i, n) == 0) out_free[i] = 0;
        }
    }
    return NULL;
}

/* initialization in its own thread: on early boot /dev/dri and the decoder may not exist yet */
static int want_display;
static void *init_and_run(void *arg) {
    (void)arg;
    if (want_display) {
        int i;
        for (i = 0; drm_open() < 0 && i < 20; i++) { if (i == 0) printf("[hdmi] waiting for display…\n"); usleep(500000); }
        if (i < 20 && dfd >= 0 && drm_setup_crtc_and_plane() == 0) display = 1;
        else printf("[hdmi] no display — decode only (for /snapshot)\n");
    }
    for (int i = 0; (vfd = v4l2_open()) < 0; i++) {
        if (i == 0) printf("[hdmi] waiting for decoder…\n");
        if (i > 120) { printf("[hdmi] hardware decoder not found (gpu_mem, bcm2835_codec blacklisted?) — no HDMI\n"); return NULL; }
        usleep(500000);
    }
    if (v4l2_setup() < 0) return NULL;
    pthread_t jt;
    if (pthread_create(&jt, NULL, jpeg_thread, NULL) == 0) pthread_detach(jt);
    running = 1;
    return worker(NULL);
}

int hdmi_start(int with_display) {
    want_display = with_display;
    if (pipe(wake_pipe) == 0) { fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK); fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK); }
    pthread_t th;
    if (pthread_create(&th, NULL, init_and_run, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}

int hdmi_decoder_running(void) { return running; }

void hdmi_stats(char *out, size_t cap) {
    if (!running) { snprintf(out, cap, "hdmi off"); return; }
    snprintf(out, cap, "hdmi%s in=%llu dec=%llu shown=%llu skip=%llu drop=%llu snaps=%llu", display ? "" : "(no display)",
             n_in, n_dec, n_shown, n_skip, n_drop, n_snaps);
}

size_t hdmi_json(char *out, size_t cap) {
    int n = snprintf(out, cap, "{\"decoder\":%s,\"display\":%s,\"mode\":\"%ux%u@%u\",\"flips\":\"%s\",\"decoded\":%llu,\"shown\":%llu,"
                     "\"skipped\":%llu,\"dropped\":%llu,\"snapshots\":%llu}", running ? "true" : "false", display ? "true" : "false",
                     mode.hdisplay, mode.vdisplay, mode.vrefresh, atomic_ok ? "atomic" : "legacy", n_dec, n_shown, n_skip, n_drop, n_snaps);
    return n < (int)cap ? (size_t)n : cap - 1;
}
