/* SPDX-License-Identifier: MIT
 * Admiral's small WPE WebKit shell. DMA-BUF frames are scanned out directly
 * when the display can import them. Displays without an IOMMU (the T527's
 * sun4i-drm) can only scan out physically contiguous buffers, so the shell then
 * copies each frame with GLES into a small pool of contiguous KMS buffers.
 * The copy is an exact RGBA blit (no blending), so holepunch alpha survives.
 * Written against the public libwpe, WPEBackend-fdo, WebKit, EGL/GLES and libdrm APIs.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <glib-unix.h>
#include <inttypes.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <wpe/webkit.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm.h>
#include "input.h"
#include "../native/drm-broker.h"

#define VERSION "0.1.0"

#define SCANOUT_POOL_SIZE 3

/* A contiguous KMS buffer that GPU-copied browser frames are rendered into. */
typedef struct {
    struct gbm_bo *bo;
    uint32_t fb;
    EGLImageKHR image;
    GLuint texture, framebuffer;
} ScanoutBuffer;

typedef struct {
    struct wl_resource *resource;
    uint32_t fb;
    uint32_t handles[4];
    uint8_t n_planes;
    ScanoutBuffer *scanout; /* pool buffer (GPU copy); its fb is owned by the pool */
    int fence_fd;           /* GPU copy completion (IN_FENCE_FD), or -1 */
} Frame;

/* A WPE buffer imported as a GLES texture, cached until WPE destroys the buffer. */
typedef struct {
    struct wl_listener destroy;
    void *browser;
    struct wl_resource *resource;
    EGLImageKHR image;
    GLuint texture;
    GLenum target;
} ImportedBuffer;

typedef struct {
    EGLContext context;
    GLuint program_2d, program_external;
    ScanoutBuffer pool[SCANOUT_POOL_SIZE];
    GHashTable *imports;
    struct gbm_device *gbm; /* scanout allocator on the KMS fd (may equal Browser.gbm) */
    gboolean has_modifiers, has_fence, has_native_fence, has_external;
    guint64 copied;
    gint64 copy_us, reported_at;
    guint64 reported_frames;
} GpuCopy;

/* One state of the brokered video overlay; fb == 0 means disabled. */
typedef struct {
    uint32_t fb;
    uint32_t src_x, src_y, src_w, src_h; /* 16.16 */
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
} VideoState;

/* The holepunch sink hands decoded frames to the browser instead of committing
 * them itself, so UI and video share one atomic commit per vblank. */
typedef struct {
    uint32_t plane;
    uint32_t fb, crtc, src_x, src_y, src_w, src_h, crtc_x, crtc_y, crtc_w, crtc_h;
    VideoState current, pending, next; /* on screen, in flight, waiting for the next commit */
    gboolean pending_valid, dirty;
    GHashTable *serials; /* fb -> latest SHOW serial, while the fb is in a slot */
    guint64 shown, replaced, rejected;
} VideoPlane;

/* Per reporting interval; reset with the fps line. */
typedef struct {
    guint64 commits, ebusy, video_commits;
    gint64 commit_us, flip_us, max_commit_us, max_flip_us;
    guint64 replaced, video_rejected;
    guint64 video_shown_base, video_replaced_base; /* VideoPlane counters at the interval start */
} CommitStats;

typedef struct {
    int fd;
    int render_fd;
    char *device;
    uint32_t connector, crtc, crtc_index, primary;
    drmModeModeInfo mode;
    unsigned ui_width, ui_height; /* WPE view and framebuffer size; the primary plane scales it to mode */
    uint32_t mode_blob;
    uint32_t connector_crtc, crtc_mode, crtc_active;
    uint32_t plane_fb, plane_crtc, plane_src_x, plane_src_y, plane_src_w, plane_src_h;
    uint32_t plane_x, plane_y, plane_w, plane_h;
    uint32_t plane_in_fence; /* optional IN_FENCE_FD on the primary plane */
    struct gbm_device *gbm;
    EGLDisplay egl;
    gboolean copy_mode; /* direct DMA-BUF scanout failed; frames are GPU-copied */
    GpuCopy copy;
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend *backend;
    WebKitWebView *view;
    EmbeddedBrowserInput *input;
    GMainLoop *loop;
    GHashTable *handles;
    Frame *current, *pending, *queued;
    gboolean commit_pending;
    gboolean frame_complete_early; /* frame callbacks fire on commit, not on flip */ /* a page flip is outstanding; pending may still be NULL (video-only commit) */
    VideoPlane video;
    guint retry_source;
    gint64 submitted_at;
    CommitStats stats;
    guint fd_source, watchdog_source, sigterm_source, sigint_source;
    guint64 presented;
    gboolean modeset;
    gboolean holepunch;
    gboolean broker_started;
    int exit_code;
    const char *uri;
    char *failure;
    char *holepunch_error;
} Browser;

static const char *copy_sync_name(const Browser *browser);

static unsigned mode_refresh(const drmModeModeInfo *mode)
{
    if (mode->vrefresh) return mode->vrefresh;
    if (!mode->htotal || !mode->vtotal) return 0;
    uint64_t numerator = (uint64_t)mode->clock * 1000;
    uint64_t denominator = (uint64_t)mode->htotal * mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) numerator *= 2;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN) denominator *= 2;
    if (mode->vscan > 1) denominator *= mode->vscan;
    return (unsigned)((numerator + denominator / 2) / denominator);
}

static void write_status(Browser *browser)
{
    const char *path = g_getenv("BROWSER_STATUS");
    if (!path || !*path) path = "/run/embedded-browser/browser-status.json";
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
#define JSON_STRING(key, value) do { json_builder_set_member_name(builder, key); json_builder_add_string_value(builder, (value) ? (value) : ""); } while (0)
#define JSON_INT(key, value) do { json_builder_set_member_name(builder, key); json_builder_add_int_value(builder, (gint64)(value)); } while (0)
#define JSON_BOOL(key, value) do { json_builder_set_member_name(builder, key); json_builder_add_boolean_value(builder, value); } while (0)
    JSON_STRING("browser", "embedded-browser");
    JSON_STRING("version", VERSION);
    JSON_STRING("device", browser->device);
    JSON_STRING("uri", browser->uri);
    JSON_STRING("error", browser->failure);
    JSON_STRING("holepunch_error", browser->holepunch_error);
    JSON_INT("connector_id", browser->connector);
    JSON_INT("crtc_id", browser->crtc);
    JSON_INT("primary_plane_id", browser->primary);
    JSON_INT("width", browser->mode.hdisplay);
    JSON_INT("height", browser->mode.vdisplay);
    JSON_INT("ui_width", browser->ui_width);
    JSON_INT("ui_height", browser->ui_height);
    JSON_INT("refresh", mode_refresh(&browser->mode));
    JSON_INT("presented_frames", browser->presented);
    JSON_STRING("scanout_path", browser->presented == 0 ? "pending" : browser->copy_mode ? "gpu-copy" : "direct");
    JSON_INT("copied_frames", browser->copy.copied);
    JSON_STRING("copy_sync", copy_sync_name(browser));
    JSON_BOOL("video_in_browser_commit", browser->video.plane && embedded_browser_drm_broker_video_handler_active());
    JSON_INT("video_frames_shown", browser->video.shown);
    JSON_BOOL("direct_scanout", browser->presented > 0 && !browser->copy_mode);
    JSON_BOOL("alpha_preserved", browser->presented > 0 && browser->holepunch);
    JSON_BOOL("broker_ready", browser->broker_started);
    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize length;
    char *data = json_generator_to_data(generator, &length);
    char *temporary = g_strconcat(path, ".tmp", NULL);
    if (g_file_set_contents(temporary, data, length, NULL)) {
        if (rename(temporary, path) != 0) unlink(temporary);
    }
    g_free(temporary);
    g_free(data);
    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);
}

static void fail(Browser *browser, const char *message)
{
    if (!browser->failure) browser->failure = g_strdup(message);
    browser->exit_code = 1;
    g_printerr("embedded-browser: %s\n", message);
    write_status(browser);
    if (browser->loop) g_main_loop_quit(browser->loop);
}

static uint32_t property_id(int fd, uint32_t object, uint32_t type, const char *name, uint64_t *value)
{
    drmModeObjectProperties *properties = drmModeObjectGetProperties(fd, object, type);
    uint32_t found = 0;
    if (!properties) return 0;
    for (uint32_t i = 0; i < properties->count_props; i++) {
        drmModePropertyRes *property = drmModeGetProperty(fd, properties->props[i]);
        if (property && strcmp(property->name, name) == 0) {
            found = property->prop_id;
            if (value) *value = properties->prop_values[i];
        }
        if (property) drmModeFreeProperty(property);
        if (found) break;
    }
    drmModeFreeObjectProperties(properties);
    return found;
}

static gboolean supports_format(drmModePlane *plane, uint32_t format)
{
    for (uint32_t i = 0; i < plane->count_formats; i++)
        if (plane->formats[i] == format) return TRUE;
    return FALSE;
}

static gboolean parse_mode(const char *text, unsigned *width, unsigned *height, unsigned *refresh)
{
    char trailing;
    if (!text || !*text) return TRUE;
    if (sscanf(text, "%ux%u@%u%c", width, height, refresh, &trailing) == 3)
        return *width > 0 && *height > 0 && *refresh > 0;
    *refresh = 0;
    return sscanf(text, "%ux%u%c", width, height, &trailing) == 2 && *width > 0 && *height > 0;
}

static gboolean select_output(Browser *browser, drmModeRes *resources)
{
    const char *wanted_connector = g_getenv("BROWSER_DRM_CONNECTOR");
    const char *wanted_primary = g_getenv("BROWSER_DRM_PRIMARY_PLANE");
    const char *wanted_mode = g_getenv("BROWSER_DRM_MODE");
    unsigned width = 0, height = 0, refresh = 0;
    if (!parse_mode(wanted_mode, &width, &height, &refresh)) {
        g_printerr("Invalid BROWSER_DRM_MODE; expected WIDTHxHEIGHT[@REFRESH]\n");
        return FALSE;
    }
    drmModePlaneRes *planes = drmModeGetPlaneResources(browser->fd);
    if (!planes) return FALSE;
    gboolean selected = FALSE;
    for (int ci = 0; ci < resources->count_connectors && !selected; ci++) {
        drmModeConnector *connector = drmModeGetConnector(browser->fd, resources->connectors[ci]);
        if (!connector) continue;
        char connector_name[80], connector_id[32];
        snprintf(connector_name, sizeof connector_name, "%s-%u", drmModeGetConnectorTypeName(connector->connector_type), connector->connector_type_id);
        snprintf(connector_id, sizeof connector_id, "%u", connector->connector_id);
        if (connector->connection != DRM_MODE_CONNECTED || !connector->count_modes ||
            (wanted_connector && *wanted_connector && strcmp(wanted_connector, connector_name) && strcmp(wanted_connector, connector_id))) {
            drmModeFreeConnector(connector);
            continue;
        }
        int mode_index = -1;
        for (int mi = 0; mi < connector->count_modes; mi++) {
            drmModeModeInfo *mode = &connector->modes[mi];
            if (width && (mode->hdisplay != width || mode->vdisplay != height || (refresh && (unsigned)mode_refresh(mode) != refresh))) continue;
            if (mode_index < 0 || (mode->type & DRM_MODE_TYPE_PREFERRED)) mode_index = mi;
            if (mode->type & DRM_MODE_TYPE_PREFERRED) break;
        }
        if (mode_index < 0) {
            g_printerr("Connector %s has no matching mode\n", connector_name);
            drmModeFreeConnector(connector);
            continue;
        }
        uint32_t possible_crtcs = 0;
        uint32_t preferred_crtc = 0;
        for (int ei = 0; ei < connector->count_encoders; ei++) {
            drmModeEncoder *encoder = drmModeGetEncoder(browser->fd, connector->encoders[ei]);
            if (!encoder) continue;
            possible_crtcs |= encoder->possible_crtcs;
            if (encoder->encoder_id == connector->encoder_id) preferred_crtc = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
        }
        for (int pass = 0; pass < 2 && !selected; pass++) {
            for (int ri = 0; ri < resources->count_crtcs && ri < 32 && !selected; ri++) {
                uint32_t crtc = resources->crtcs[ri];
                if (!(possible_crtcs & (1u << ri)) || (pass == 0 && crtc != preferred_crtc) || (pass == 1 && crtc == preferred_crtc)) continue;
                for (uint32_t pi = 0; pi < planes->count_planes && !selected; pi++) {
                    drmModePlane *plane = drmModeGetPlane(browser->fd, planes->planes[pi]);
                    if (!plane) continue;
                    uint64_t type = 0;
                    char id[32];
                    snprintf(id, sizeof id, "%u", plane->plane_id);
                    gboolean match = (plane->possible_crtcs & (1u << ri)) &&
                        property_id(browser->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type) &&
                        type == DRM_PLANE_TYPE_PRIMARY && supports_format(plane, DRM_FORMAT_ARGB8888) &&
                        (!wanted_primary || !*wanted_primary || !strcmp(wanted_primary, id));
                    if (match) {
                        browser->connector = connector->connector_id;
                        browser->crtc = crtc;
                        browser->crtc_index = ri;
                        browser->primary = plane->plane_id;
                        browser->mode = connector->modes[mode_index];
                        selected = TRUE;
                        g_message("Selected %s connector=%u CRTC=%u primary=%u mode=%ux%u@%u", connector_name, browser->connector, crtc, browser->primary, browser->mode.hdisplay, browser->mode.vdisplay, mode_refresh(&browser->mode));
                    }
                    drmModeFreePlane(plane);
                }
            }
        }
        drmModeFreeConnector(connector);
    }
    drmModeFreePlaneResources(planes);
    return selected;
}

static gboolean try_device(Browser *browser, const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return FALSE;
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0 ||
        (!drmIsMaster(fd) && drmSetMaster(fd) != 0)) {
        g_printerr("Cannot acquire atomic DRM master on %s: %s\n", path, g_strerror(errno));
        close(fd);
        return FALSE;
    }
    browser->fd = fd;
    drmModeRes *resources = drmModeGetResources(fd);
    gboolean selected = resources && select_output(browser, resources);
    if (resources) drmModeFreeResources(resources);
    if (!selected) {
        drmDropMaster(fd);
        close(fd);
        browser->fd = -1;
        return FALSE;
    }
    browser->device = g_strdup(path);
    return TRUE;
}

static unsigned gcd_unsigned(unsigned a, unsigned b)
{
    while (b) { unsigned t = a % b; a = b; b = t; }
    return a;
}

/* GPU rendering cost scales with pixels: modes above 1080p render the UI at the
 * largest size within 1920x1080 that keeps the mode's aspect ratio. */
static gboolean choose_ui_size(Browser *browser)
{
    unsigned mode_w = browser->mode.hdisplay, mode_h = browser->mode.vdisplay;
    browser->ui_width = mode_w;
    browser->ui_height = mode_h;
    const char *wanted = g_getenv("BROWSER_UI_SIZE");
    if (wanted && *wanted) {
        unsigned width = 0, height = 0;
        char trailing;
        if (sscanf(wanted, "%ux%u%c", &width, &height, &trailing) != 2 || !width || !height || width > mode_w || height > mode_h) {
            g_printerr("Invalid BROWSER_UI_SIZE '%s'; expected WIDTHxHEIGHT no larger than the %ux%u mode\n", wanted, mode_w, mode_h);
            return FALSE;
        }
        browser->ui_width = width;
        browser->ui_height = height;
    } else if (mode_w > 1920 || mode_h > 1080) {
        unsigned divisor = gcd_unsigned(mode_w, mode_h);
        unsigned ratio_w = mode_w / divisor, ratio_h = mode_h / divisor;
        unsigned factor = MIN(1920 / ratio_w, 1080 / ratio_h);
        if (factor) {
            browser->ui_width = ratio_w * factor;
            browser->ui_height = ratio_h * factor;
        }
    }
    g_message("Browser UI %ux%u, display mode %ux%u%s", browser->ui_width, browser->ui_height, mode_w, mode_h,
              browser->ui_width == mode_w && browser->ui_height == mode_h ? "" : " (scaled by the primary plane)");
    return TRUE;
}

static gboolean open_display(Browser *browser)
{
    const char *device = g_getenv("BROWSER_DRM_DEVICE");
    if (device && *device) {
        if (!try_device(browser, device)) return FALSE;
    } else {
        for (unsigned i = 0; i < 16 && browser->fd < 0; i++) {
            char path[64];
            snprintf(path, sizeof path, "/dev/dri/card%u", i);
            try_device(browser, path);
        }
        if (browser->fd < 0) return FALSE;
    }
#define PROPERTY(field, object, type, name) do { browser->field = property_id(browser->fd, object, type, name, NULL); if (!browser->field) { g_printerr("Required atomic property %s missing\n", name); return FALSE; } } while (0)
    PROPERTY(connector_crtc, browser->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    PROPERTY(crtc_mode, browser->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    PROPERTY(crtc_active, browser->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    PROPERTY(plane_fb, browser->primary, DRM_MODE_OBJECT_PLANE, "FB_ID");
    PROPERTY(plane_crtc, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    PROPERTY(plane_src_x, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_X");
    PROPERTY(plane_src_y, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    PROPERTY(plane_src_w, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_W");
    PROPERTY(plane_src_h, browser->primary, DRM_MODE_OBJECT_PLANE, "SRC_H");
    PROPERTY(plane_x, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    PROPERTY(plane_y, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    PROPERTY(plane_w, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    PROPERTY(plane_h, browser->primary, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    /* Optional: lets the kernel wait for the GPU copy instead of the main thread. */
    browser->plane_in_fence = property_id(browser->fd, browser->primary, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", NULL);
    if (!choose_ui_size(browser)) return FALSE;
    if (drmModeCreatePropertyBlob(browser->fd, &browser->mode, sizeof browser->mode, &browser->mode_blob) != 0) return FALSE;
    return TRUE;
}

static void handle_ref(Browser *browser, uint32_t handle)
{
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(browser->handles, GUINT_TO_POINTER(handle)));
    g_hash_table_insert(browser->handles, GUINT_TO_POINTER(handle), GUINT_TO_POINTER(count + 1));
}

static void handle_unref(Browser *browser, uint32_t handle)
{
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(browser->handles, GUINT_TO_POINTER(handle)));
    if (count > 1) g_hash_table_insert(browser->handles, GUINT_TO_POINTER(handle), GUINT_TO_POINTER(count - 1));
    else {
        g_hash_table_remove(browser->handles, GUINT_TO_POINTER(handle));
        struct drm_gem_close close_request = { .handle = handle };
        drmIoctl(browser->fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    }
}

static Frame *frame_new(void)
{
    Frame *frame = g_new0(Frame, 1);
    frame->fence_fd = -1;
    return frame;
}

static void release_frame(Browser *browser, Frame *frame)
{
    if (!frame) return;
    if (frame->fence_fd >= 0) close(frame->fence_fd);
    if (frame->fb && !frame->scanout) drmModeRmFB(browser->fd, frame->fb);
    for (unsigned i = 0; i < frame->n_planes; i++) {
        gboolean unique = TRUE;
        for (unsigned j = 0; j < i; j++) if (frame->handles[j] == frame->handles[i]) unique = FALSE;
        if (unique && frame->handles[i]) handle_unref(browser, frame->handles[i]);
    }
    if (browser->exportable && frame->resource)
        wpe_view_backend_exportable_fdo_dispatch_release_buffer(browser->exportable, frame->resource);
    g_free(frame);
}

/* The source is the UI-sized buffer; the destination is always the full mode.
 * When they differ the primary plane's scaler stretches the UI over the video. */
#define ADD(object, property, value) do { if (drmModeAtomicAddProperty(request, object, property, value) < 0) valid = FALSE; } while (0)
#define ADD_PRIMARY_STATE(fb) do { \
        ADD(browser->primary, browser->plane_fb, fb); \
        ADD(browser->primary, browser->plane_crtc, browser->crtc); \
        ADD(browser->primary, browser->plane_src_x, 0); \
        ADD(browser->primary, browser->plane_src_y, 0); \
        ADD(browser->primary, browser->plane_src_w, (uint64_t)browser->ui_width << 16); \
        ADD(browser->primary, browser->plane_src_h, (uint64_t)browser->ui_height << 16); \
        ADD(browser->primary, browser->plane_x, 0); \
        ADD(browser->primary, browser->plane_y, 0); \
        ADD(browser->primary, browser->plane_w, browser->mode.hdisplay); \
        ADD(browser->primary, browser->plane_h, browser->mode.vdisplay); \
    } while (0)

/* Tells the sink it may reuse fb once no video state slot refers to it. */
static void video_release_if_unused(Browser *browser, uint32_t fb)
{
    VideoPlane *video = &browser->video;
    if (!fb || fb == video->current.fb || (video->pending_valid && fb == video->pending.fb) || (video->dirty && fb == video->next.fb)) return;
    gpointer serial = NULL;
    if (!video->serials || !g_hash_table_lookup_extended(video->serials, GUINT_TO_POINTER(fb), NULL, &serial)) return;
    g_hash_table_remove(video->serials, GUINT_TO_POINTER(fb));
    embedded_browser_drm_broker_video_released(fb, GPOINTER_TO_UINT(serial));
}

static gboolean submit_commit(Browser *browser);

static gboolean retry_commit(gpointer data)
{
    Browser *browser = data;
    browser->retry_source = 0;
    submit_commit(browser);
    return G_SOURCE_REMOVE;
}

/* Puts the queued browser frame and/or the next video state on screen in ONE
 * nonblocking atomic commit. A CRTC takes one commit per vblank, so the UI and
 * the video plane must not commit separately: two committers on one CRTC
 * serialise and each gets every other vblank. Returns FALSE after fail(). */
static gboolean submit_commit(Browser *browser)
{
    VideoPlane *video = &browser->video;
    if (browser->commit_pending || browser->retry_source || (!browser->queued && !video->dirty)) return TRUE;
    Frame *frame = browser->queued;
    for (;;) {
        gboolean with_video = video->dirty && video->plane;
        drmModeAtomicReq *request = drmModeAtomicAlloc();
        if (!request) { fail(browser, "Cannot allocate an atomic request"); return FALSE; }
        gboolean valid = TRUE;
        if (frame) {
            ADD_PRIMARY_STATE(frame->fb);
            if (frame->fence_fd >= 0 && browser->plane_in_fence) ADD(browser->primary, browser->plane_in_fence, frame->fence_fd);
        }
        if (with_video) {
            const VideoState *next = &video->next;
            ADD(video->plane, video->fb, next->fb);
            ADD(video->plane, video->crtc, next->fb ? browser->crtc : 0);
            ADD(video->plane, video->src_x, next->src_x);
            ADD(video->plane, video->src_y, next->src_y);
            ADD(video->plane, video->src_w, next->src_w);
            ADD(video->plane, video->src_h, next->src_h);
            ADD(video->plane, video->crtc_x, (uint64_t)(int64_t)next->crtc_x);
            ADD(video->plane, video->crtc_y, (uint64_t)(int64_t)next->crtc_y);
            ADD(video->plane, video->crtc_w, next->crtc_w);
            ADD(video->plane, video->crtc_h, next->crtc_h);
        }
        gint64 started = g_get_monotonic_time();
        int saved_errno = EINVAL;
        gboolean committed = valid && drmModeAtomicCommit(browser->fd, request, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, browser) == 0;
        if (valid && !committed) saved_errno = errno;
        gint64 elapsed = g_get_monotonic_time() - started;
        drmModeAtomicFree(request);
        browser->stats.commit_us += elapsed;
        browser->stats.max_commit_us = MAX(browser->stats.max_commit_us, elapsed);
        if (committed) {
            browser->stats.commits++;
            if (frame) {
                if (frame->fence_fd >= 0) { close(frame->fence_fd); frame->fence_fd = -1; }
                browser->queued = NULL;
                browser->pending = frame;
            }
            browser->commit_pending = TRUE;
            browser->submitted_at = g_get_monotonic_time();
            if (with_video) {
                browser->stats.video_commits++;
                video->pending = video->next;
                video->pending_valid = TRUE;
                video->dirty = FALSE;
            }
            /* Let WebKit render the next frame while this one's GPU copy and
             * flip are in flight. It can then reach us before the next vblank;
             * it waits in queued until this commit flips. */
            if (frame && browser->frame_complete_early) wpe_view_backend_exportable_fdo_dispatch_frame_complete(browser->exportable);
            return TRUE;
        }
        if (saved_errno == EBUSY) {
            /* Another commit on this CRTC (cleanup, broker setup, a foreign
             * client) is still in flight: retry shortly instead of failing. */
            browser->stats.ebusy++;
            browser->retry_source = g_timeout_add(1, retry_commit, browser);
            return TRUE;
        }
        if (with_video) {
            /* A stale or unsupported video framebuffer must not take the UI down. */
            g_warning("Atomic commit with video fb %u rejected: %s; dropping that video frame", video->next.fb, g_strerror(saved_errno));
            video->rejected++;
            uint32_t rejected = video->next.fb;
            video->dirty = FALSE;
            browser->stats.video_rejected++;
            video_release_if_unused(browser, rejected);
            if (!frame) return TRUE;
            continue;
        }
        char *message = g_strdup_printf("Atomic browser plane commit failed: %s; inspect DE35 format/modifier/alpha support", g_strerror(saved_errno));
        fail(browser, message);
        g_free(message);
        return FALSE;
    }
}

/* Dumb buffers are linear, which every primary plane can scan out. */
static Frame *create_initial_frame(Browser *browser)
{
    struct drm_mode_create_dumb create = {
        .width = browser->ui_width, .height = browser->ui_height, .bpp = 32
    };
    if (drmIoctl(browser->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) return NULL;
    Frame *frame = frame_new();
    frame->n_planes = 1;
    frame->handles[0] = create.handle;
    handle_ref(browser, create.handle);
    struct drm_mode_map_dumb map = { .handle = create.handle };
    gboolean valid = drmIoctl(browser->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0;
    if (valid) {
        uint32_t *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, browser->fd, map.offset);
        valid = pixels != MAP_FAILED;
        if (valid) {
            for (uint64_t i = 0; i < create.size / 4; i++) pixels[i] = 0xff09131c;
            munmap(pixels, create.size);
        }
    }
    uint32_t strides[4] = { create.pitch, 0, 0, 0 }, offsets[4] = { 0, 0, 0, 0 };
    if (valid) valid = drmModeAddFB2(browser->fd, browser->ui_width, browser->ui_height,
                                    DRM_FORMAT_ARGB8888, frame->handles, strides, offsets, &frame->fb, 0) == 0;
    if (!valid) { release_frame(browser, frame); return NULL; }
    return frame;
}

/* Activate the selected CRTC before WebKit can preroll its video sink. */
static gboolean initial_scanout(Browser *browser)
{
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        Frame *frame = create_initial_frame(browser);
        if (!frame) return FALSE;
        drmModeAtomicReq *request = drmModeAtomicAlloc();
        gboolean valid = request != NULL;
        if (valid) {
            ADD(browser->connector, browser->connector_crtc, browser->crtc);
            ADD(browser->crtc, browser->crtc_mode, browser->mode_blob);
            ADD(browser->crtc, browser->crtc_active, 1);
            ADD_PRIMARY_STATE(frame->fb);
            /* Prove the kernel accepts the scaled configuration before WPE renders for it. */
            errno = EINVAL;
            if (valid) valid = drmModeAtomicCommit(browser->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0;
            if (valid) valid = drmModeAtomicCommit(browser->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0;
        }
        int saved_errno = errno;
        if (request) drmModeAtomicFree(request);
        if (valid) {
            browser->current = frame;
            browser->modeset = TRUE;
            g_message("Initial ARGB browser mode active before video broker starts");
            return TRUE;
        }
        release_frame(browser, frame);
        if (browser->ui_width == browser->mode.hdisplay && browser->ui_height == browser->mode.vdisplay) return FALSE;
        g_warning("Kernel rejected primary-plane scaling %ux%u -> %ux%u (%s); falling back to UI size = mode size",
                  browser->ui_width, browser->ui_height, browser->mode.hdisplay, browser->mode.vdisplay, g_strerror(saved_errno));
        browser->ui_width = browser->mode.hdisplay;
        browser->ui_height = browser->mode.vdisplay;
    }
    return FALSE;
}

static void report_stats(Browser *browser)
{
    gint64 now = g_get_monotonic_time();
    CommitStats *stats = &browser->stats;
    if (browser->copy.reported_at) {
        double seconds = (now - browser->copy.reported_at) / (double)G_USEC_PER_SEC;
        guint64 copied = browser->copy.copied - browser->copy.reported_frames;
        g_message("Presented %" G_GUINT64_FORMAT " browser frames (%.1f fps over the last %.0f s, path=%s, GPU copy avg %.2f ms)",
                  browser->presented, seconds > 0 ? 600 / seconds : 0, seconds, browser->copy_mode ? "gpu-copy" : "direct",
                  copied ? browser->copy.copy_us / 1000.0 / copied : 0.0);
        g_message("Commits: %" G_GUINT64_FORMAT " (%" G_GUINT64_FORMAT " with video, %.1f video fps), ioctl avg %.2f max %.2f ms, "
                  "submit->flip avg %.2f max %.2f ms, EBUSY %" G_GUINT64_FORMAT ", replaced UI frames %" G_GUINT64_FORMAT
                  ", replaced video frames %" G_GUINT64_FORMAT,
                  stats->commits, stats->video_commits, seconds > 0 ? (browser->video.shown - stats->video_shown_base) / seconds : 0,
                  stats->commits ? stats->commit_us / 1000.0 / stats->commits : 0.0, stats->max_commit_us / 1000.0,
                  stats->commits ? stats->flip_us / 1000.0 / stats->commits : 0.0, stats->max_flip_us / 1000.0,
                  stats->ebusy, stats->replaced, browser->video.replaced - stats->video_replaced_base);
    }
    guint64 shown = browser->video.shown, video_replaced = browser->video.replaced;
    memset(stats, 0, sizeof *stats);
    stats->video_shown_base = shown;
    stats->video_replaced_base = video_replaced;
    browser->copy.reported_at = now;
    browser->copy.reported_frames = browser->copy.copied;
    browser->copy.copy_us = 0;
}

static void frame_presented(int fd, unsigned sequence, unsigned seconds, unsigned microseconds, void *data)
{
    (void)fd; (void)sequence; (void)seconds; (void)microseconds;
    Browser *browser = data;
    if (!browser->commit_pending) return;
    browser->commit_pending = FALSE;
    gint64 latency = g_get_monotonic_time() - browser->submitted_at;
    browser->stats.flip_us += latency;
    browser->stats.max_flip_us = MAX(browser->stats.max_flip_us, latency);
    VideoPlane *video = &browser->video;
    if (video->pending_valid) {
        uint32_t retired = video->current.fb;
        video->current = video->pending;
        video->pending_valid = FALSE;
        if (video->current.fb) video->shown++;
        video_release_if_unused(browser, retired);
    }
    if (browser->pending) {
        Frame *retired = browser->current;
        browser->current = browser->pending;
        browser->pending = NULL;
        release_frame(browser, retired);
        browser->presented++;
        if (browser->presented == 1) {
            if (browser->copy_mode)
                g_message("First browser frame scanned out on plane %u via GPU copy into a contiguous KMS buffer; alpha preserved", browser->primary);
            else
                g_message("First ARGB DMA-BUF browser frame scanned out directly on plane %u; alpha preserved without GLES copy", browser->primary);
        }
        if (browser->presented % 600 == 0) report_stats(browser);
        if (browser->presented == 1 || browser->presented % 120 == 0) write_status(browser);
    }
    if (!browser->frame_complete_early) wpe_view_backend_exportable_fdo_dispatch_frame_complete(browser->exportable);
    submit_commit(browser);
}

/* A frame (or hide request) from the holepunch sink; message == NULL when the
 * sink disconnected, which hides the video. */
static void video_message(const EmbeddedBrowserVideoMessage *message, gpointer data)
{
    Browser *browser = data;
    VideoPlane *video = &browser->video;
    if (!video->plane) return;
    VideoState state = { 0 };
    if (message && message->type == BROWSER_VIDEO_SHOW && message->fb && message->src_w && message->src_h && message->crtc_w && message->crtc_h) {
        state.fb = message->fb;
        state.src_x = message->src_x; state.src_y = message->src_y;
        state.src_w = message->src_w; state.src_h = message->src_h;
        state.crtc_x = message->crtc_x; state.crtc_y = message->crtc_y;
        state.crtc_w = message->crtc_w; state.crtc_h = message->crtc_h;
    } else if (message && message->type == BROWSER_VIDEO_SHOW) {
        g_warning("Ignoring malformed video frame message for fb %u", message->fb);
        embedded_browser_drm_broker_video_released(message->fb, message->serial);
        return;
    }
    if (!message) g_hash_table_remove_all(video->serials); /* nobody left to tell */
    if (state.fb) g_hash_table_insert(video->serials, GUINT_TO_POINTER(state.fb), GUINT_TO_POINTER(message->serial));
    const VideoState *latest = video->dirty ? &video->next : video->pending_valid ? &video->pending : &video->current;
    if (!memcmp(latest, &state, sizeof state)) return;
    uint32_t replaced = video->dirty ? video->next.fb : 0;
    video->next = state;
    video->dirty = TRUE;
    if (replaced && replaced != state.fb) {
        video->replaced++;
        video_release_if_unused(browser, replaced);
    }
    submit_commit(browser);
}

static void setup_video_plane(Browser *browser)
{
    VideoPlane *video = &browser->video;
    uint32_t plane = embedded_browser_drm_broker_video_plane();
    if (!plane) return;
    struct { uint32_t *field; const char *name; } properties[] = {
        { &video->fb, "FB_ID" }, { &video->crtc, "CRTC_ID" },
        { &video->src_x, "SRC_X" }, { &video->src_y, "SRC_Y" }, { &video->src_w, "SRC_W" }, { &video->src_h, "SRC_H" },
        { &video->crtc_x, "CRTC_X" }, { &video->crtc_y, "CRTC_Y" }, { &video->crtc_w, "CRTC_W" }, { &video->crtc_h, "CRTC_H" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(properties); i++) {
        *properties[i].field = property_id(browser->fd, plane, DRM_MODE_OBJECT_PLANE, properties[i].name, NULL);
        if (!*properties[i].field) {
            g_warning("Video plane %u lacks atomic property %s; the holepunch sink will commit video itself", plane, properties[i].name);
            return;
        }
    }
    video->plane = plane;
    video->serials = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_message("Video plane %u is committed by the browser together with the UI (one atomic commit per vblank)", plane);
}

static gboolean drm_ready(gint fd, GIOCondition condition, gpointer data)
{
    Browser *browser = data;
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        fail(browser, "DRM device connection lost");
        browser->fd_source = 0;
        return G_SOURCE_REMOVE;
    }
    drmEventContext context = { .version = DRM_EVENT_CONTEXT_VERSION, .page_flip_handler = frame_presented };
    if (drmHandleEvent(fd, &context) != 0 && errno != EAGAIN && errno != EINTR) {
        fail(browser, "Cannot read DRM pageflip completion events");
        browser->fd_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Zero-copy path: import the WPE buffer into the KMS device and wrap it in a
 * framebuffer. Returns NULL with a reason when the display cannot scan it out
 * (for example a non-contiguous GPU buffer on a display without an IOMMU). */
static Frame *import_direct(Browser *browser, struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource, char **reason)
{
    Frame *frame = frame_new();
    frame->n_planes = MIN(resource->n_planes, 4);
    const char *step = "unsupported size/format";
    gboolean valid = resource->width == browser->ui_width && resource->height == browser->ui_height &&
        resource->n_planes > 0 && resource->n_planes <= 4 && resource->format == DRM_FORMAT_ARGB8888;
    int saved_errno = EINVAL;
    gboolean explicit_modifiers = FALSE, all_linear_or_implicit = TRUE;
    if (valid) step = "drmPrimeFDToHandle";
    for (unsigned i = 0; valid && i < resource->n_planes; i++) {
        if (drmPrimeFDToHandle(browser->fd, resource->fds[i], &frame->handles[i]) != 0) { saved_errno = errno; valid = FALSE; break; }
        gboolean unique = TRUE;
        for (unsigned j = 0; j < i; j++) if (frame->handles[j] == frame->handles[i]) unique = FALSE;
        if (unique) handle_ref(browser, frame->handles[i]);
        if (resource->modifiers[i] != DRM_FORMAT_MOD_INVALID) explicit_modifiers = TRUE;
        if (resource->modifiers[i] != DRM_FORMAT_MOD_INVALID && resource->modifiers[i] != DRM_FORMAT_MOD_LINEAR) all_linear_or_implicit = FALSE;
    }
    /* Only a fully imported, validated buffer may reach the linear fallback. */
    if (valid) {
        step = "drmModeAddFB2";
        if (explicit_modifiers) {
            valid = drmModeAddFB2WithModifiers(browser->fd, resource->width, resource->height, resource->format,
                frame->handles, resource->strides, resource->offsets, resource->modifiers, &frame->fb, DRM_MODE_FB_MODIFIERS) == 0;
            if (!valid && all_linear_or_implicit)
                valid = drmModeAddFB2(browser->fd, resource->width, resource->height, resource->format,
                    frame->handles, resource->strides, resource->offsets, &frame->fb, 0) == 0;
        } else {
            valid = drmModeAddFB2(browser->fd, resource->width, resource->height, resource->format,
                frame->handles, resource->strides, resource->offsets, &frame->fb, 0) == 0;
        }
        if (!valid) saved_errno = errno;
    }
    if (valid) {
        frame->resource = resource->buffer_resource;
        return frame;
    }
    *reason = g_strdup_printf("%s failed: %s; size=%ux%u (UI %ux%u) format=0x%08x planes=%u modifier=0x%016" PRIx64,
                              step, g_strerror(saved_errno), resource->width, resource->height, browser->ui_width, browser->ui_height,
                              resource->format, resource->n_planes, resource->modifiers[0]);
    /* The WPE buffer is not owned by this frame yet: the copy path still needs it. */
    release_frame(browser, frame);
    return NULL;
}

/* ---- GPU copy into contiguous KMS buffers ---- */

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNEGLCREATESYNCKHRPROC create_sync;
static PFNEGLDESTROYSYNCKHRPROC destroy_sync;
static PFNEGLCLIENTWAITSYNCKHRPROC client_wait_sync;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

typedef struct {
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw, read;
} SavedContext;

/* WebKit's UI process does not normally hold a GL context on this thread, but
 * restore whatever was current so the copy never disturbs another user. */
static gboolean copy_context_enter(Browser *browser, SavedContext *saved)
{
    saved->display = eglGetCurrentDisplay();
    saved->context = eglGetCurrentContext();
    saved->draw = eglGetCurrentSurface(EGL_DRAW);
    saved->read = eglGetCurrentSurface(EGL_READ);
    if (saved->context == browser->copy.context) return TRUE;
    return eglMakeCurrent(browser->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, browser->copy.context);
}

static void copy_context_leave(Browser *browser, const SavedContext *saved)
{
    if (saved->context == browser->copy.context) return;
    if (saved->context != EGL_NO_CONTEXT && saved->display != EGL_NO_DISPLAY)
        eglMakeCurrent(saved->display, saved->draw, saved->read, saved->context);
    else
        eglMakeCurrent(browser->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static gboolean has_extension(const char *list, const char *name)
{
    size_t length = strlen(name);
    for (const char *at = list; at && (at = strstr(at, name)); at += length)
        if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == '\0')) return TRUE;
    return FALSE;
}

static GLuint compile_program(gboolean external, char **error)
{
    /* highp: 1920-pixel texture coordinates are not exact in mediump. */
    static const char *vertex_source =
        "attribute vec2 position;\n"
        "varying highp vec2 coordinate;\n"
        "void main() { coordinate = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n";
    static const char *fragment_2d =
        "precision highp float;\n"
        "uniform sampler2D source;\n"
        "varying highp vec2 coordinate;\n"
        "void main() { gl_FragColor = texture2D(source, coordinate); }\n";
    static const char *fragment_external =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision highp float;\n"
        "uniform samplerExternalOES source;\n"
        "varying highp vec2 coordinate;\n"
        "void main() { gl_FragColor = texture2D(source, coordinate); }\n";
    const char *sources[2] = { vertex_source, external ? fragment_external : fragment_2d };
    GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLuint program = glCreateProgram();
    for (unsigned i = 0; i < 2; i++) {
        GLuint shader = glCreateShader(types[i]);
        GLint ok = 0;
        glShaderSource(shader, 1, &sources[i], NULL);
        glCompileShader(shader);
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = "";
            glGetShaderInfoLog(shader, sizeof log, NULL, log);
            *error = g_strdup_printf("shader compile failed: %s", log);
            glDeleteShader(shader);
            glDeleteProgram(program);
            return 0;
        }
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        *error = g_strdup_printf("program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "source"), 0);
    return program;
}

/* Builds an EGL_LINUX_DMA_BUF_EXT attribute list for up to four planes. */
static gboolean dmabuf_attributes(Browser *browser, EGLint *attributes, unsigned width, unsigned height, uint32_t format,
                                  unsigned n_planes, const int *fds, const uint32_t *strides, const uint32_t *offsets,
                                  const uint64_t *modifiers)
{
    static const EGLint plane_attributes[4][5] = {
        { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
        { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
    };
    unsigned n = 0;
    attributes[n++] = EGL_WIDTH; attributes[n++] = width;
    attributes[n++] = EGL_HEIGHT; attributes[n++] = height;
    attributes[n++] = EGL_LINUX_DRM_FOURCC_EXT; attributes[n++] = format;
    for (unsigned i = 0; i < n_planes && i < 4; i++) {
        attributes[n++] = plane_attributes[i][0]; attributes[n++] = fds[i];
        attributes[n++] = plane_attributes[i][1]; attributes[n++] = offsets[i];
        attributes[n++] = plane_attributes[i][2]; attributes[n++] = strides[i];
        if (modifiers[i] != DRM_FORMAT_MOD_INVALID) {
            if (!browser->copy.has_modifiers) return FALSE;
            attributes[n++] = plane_attributes[i][3]; attributes[n++] = (EGLint)(modifiers[i] & 0xffffffff);
            attributes[n++] = plane_attributes[i][4]; attributes[n++] = (EGLint)(modifiers[i] >> 32);
        }
    }
    attributes[n++] = EGL_IMAGE_PRESERVED_KHR; attributes[n++] = EGL_TRUE;
    attributes[n] = EGL_NONE;
    return TRUE;
}

static void destroy_scanout_buffer(Browser *browser, ScanoutBuffer *buffer)
{
    if (buffer->framebuffer) glDeleteFramebuffers(1, &buffer->framebuffer);
    if (buffer->texture) glDeleteTextures(1, &buffer->texture);
    if (buffer->image != EGL_NO_IMAGE_KHR && destroy_image) destroy_image(browser->egl, buffer->image);
    if (buffer->fb) drmModeRmFB(browser->fd, buffer->fb);
    if (buffer->bo) gbm_bo_destroy(buffer->bo);
    memset(buffer, 0, sizeof *buffer);
    buffer->image = EGL_NO_IMAGE_KHR;
}

static gboolean create_scanout_buffer(Browser *browser, ScanoutBuffer *buffer, char **error)
{
    buffer->image = EGL_NO_IMAGE_KHR;
    buffer->bo = gbm_bo_create(browser->copy.gbm, browser->ui_width, browser->ui_height, GBM_FORMAT_ARGB8888,
                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!buffer->bo) {
        *error = g_strdup_printf("gbm_bo_create(%ux%u ARGB8888 SCANOUT|RENDERING|LINEAR) on the KMS device failed: %s",
                                 browser->ui_width, browser->ui_height, g_strerror(errno));
        return FALSE;
    }
    uint32_t handles[4] = { gbm_bo_get_handle(buffer->bo).u32 }, strides[4] = { gbm_bo_get_stride(buffer->bo) }, offsets[4] = { 0 };
    if (drmModeAddFB2(browser->fd, browser->ui_width, browser->ui_height, DRM_FORMAT_ARGB8888, handles, strides, offsets, &buffer->fb, 0) != 0) {
        *error = g_strdup_printf("drmModeAddFB2 for the contiguous scanout buffer failed: %s", g_strerror(errno));
        return FALSE;
    }
    int fd = gbm_bo_get_fd(buffer->bo);
    if (fd < 0) { *error = g_strdup("gbm_bo_get_fd for the scanout buffer failed"); return FALSE; }
    EGLint attributes[64];
    int fds[1] = { fd };
    uint64_t modifiers[1] = { browser->copy.has_modifiers ? DRM_FORMAT_MOD_LINEAR : DRM_FORMAT_MOD_INVALID };
    dmabuf_attributes(browser, attributes, browser->ui_width, browser->ui_height, DRM_FORMAT_ARGB8888, 1, fds, strides, offsets, modifiers);
    buffer->image = create_image(browser->egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    close(fd);
    if (buffer->image == EGL_NO_IMAGE_KHR) {
        *error = g_strdup_printf("eglCreateImageKHR for the scanout buffer failed: 0x%04x", eglGetError());
        return FALSE;
    }
    glGenTextures(1, &buffer->texture);
    glBindTexture(GL_TEXTURE_2D, buffer->texture);
    image_target_texture(GL_TEXTURE_2D, buffer->image);
    glGenFramebuffers(1, &buffer->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, buffer->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer->texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        *error = g_strdup_printf("scanout framebuffer incomplete: 0x%04x (GL error 0x%04x)", status, glGetError());
        return FALSE;
    }
    /* Same opaque dark background (0xff09131c) as the initial dumb buffer. */
    glViewport(0, 0, browser->ui_width, browser->ui_height);
    glClearColor(0x09 / 255.0f, 0x13 / 255.0f, 0x1c / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return TRUE;
}

static void destroy_import(Browser *browser, ImportedBuffer *import)
{
    if (import->texture) glDeleteTextures(1, &import->texture);
    if (import->image != EGL_NO_IMAGE_KHR) destroy_image(browser->egl, import->image);
    g_free(import);
}

static void import_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    ImportedBuffer *import = wl_container_of(listener, import, destroy);
    Browser *browser = import->browser;
    wl_list_remove(&import->destroy.link);
    g_hash_table_remove(browser->copy.imports, import->resource);
    SavedContext saved;
    if (copy_context_enter(browser, &saved)) {
        destroy_import(browser, import);
        copy_context_leave(browser, &saved);
    } else g_free(import);
}

static void destroy_copy(Browser *browser)
{
    GpuCopy *copy = &browser->copy;
    if (copy->context == EGL_NO_CONTEXT) {
        if (copy->gbm && copy->gbm != browser->gbm) gbm_device_destroy(copy->gbm);
        copy->gbm = NULL;
        return;
    }
    SavedContext saved;
    gboolean current = copy_context_enter(browser, &saved);
    if (copy->imports) {
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, copy->imports);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            ImportedBuffer *import = value;
            wl_list_remove(&import->destroy.link);
            if (current) destroy_import(browser, import); else g_free(import);
            g_hash_table_iter_remove(&iter);
        }
        g_hash_table_unref(copy->imports);
        copy->imports = NULL;
    }
    for (unsigned i = 0; i < SCANOUT_POOL_SIZE; i++) {
        if (current) destroy_scanout_buffer(browser, &copy->pool[i]);
        else {
            if (copy->pool[i].fb) drmModeRmFB(browser->fd, copy->pool[i].fb);
            if (copy->pool[i].bo) gbm_bo_destroy(copy->pool[i].bo);
            memset(&copy->pool[i], 0, sizeof copy->pool[i]);
        }
    }
    if (current) {
        if (copy->program_2d) glDeleteProgram(copy->program_2d);
        if (copy->program_external) glDeleteProgram(copy->program_external);
    }
    copy->program_2d = copy->program_external = 0;
    eglMakeCurrent(browser->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(browser->egl, copy->context);
    copy->context = EGL_NO_CONTEXT;
    if (copy->gbm && copy->gbm != browser->gbm) gbm_device_destroy(copy->gbm);
    copy->gbm = NULL;
}

static const char *copy_sync_name(const Browser *browser)
{
    if (browser->copy.context == EGL_NO_CONTEXT) return "";
    return browser->copy.has_native_fence ? "in-fence" : browser->copy.has_fence ? "client-wait" : "finish";
}

static gboolean setup_copy(Browser *browser, char **error)
{
    GpuCopy *copy = &browser->copy;
    const char *extensions = eglQueryString(browser->egl, EGL_EXTENSIONS);
    if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import") || !has_extension(extensions, "EGL_KHR_image_base") ||
        !has_extension(extensions, "EGL_KHR_surfaceless_context")) {
        *error = g_strdup("EGL lacks EGL_EXT_image_dma_buf_import, EGL_KHR_image_base or EGL_KHR_surfaceless_context");
        return FALSE;
    }
    copy->has_modifiers = has_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    copy->has_fence = has_extension(extensions, "EGL_KHR_fence_sync");
    create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    client_wait_sync = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create_image || !destroy_image || !image_target_texture) {
        *error = g_strdup("EGL image entry points are unavailable");
        return FALSE;
    }
    if (!create_sync || !destroy_sync || !client_wait_sync) copy->has_fence = FALSE;
    /* With a native fence the kernel waits for the copy (primary-plane IN_FENCE_FD)
     * and the main thread only flushes; otherwise it waits on the CPU. */
    dup_native_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    copy->has_native_fence = copy->has_fence && dup_native_fence && browser->plane_in_fence &&
        has_extension(extensions, "EGL_ANDROID_native_fence_sync");
    /* Scanout buffers must come from the KMS device so they are contiguous (kmsro/CMA). */
    copy->gbm = browser->render_fd >= 0 ? gbm_create_device(browser->fd) : browser->gbm;
    if (!copy->gbm) { *error = g_strdup("Cannot create a GBM device on the KMS display for scanout buffers"); return FALSE; }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) { *error = g_strdup("eglBindAPI(EGL_OPENGL_ES_API) failed"); return FALSE; }
    EGLConfig config = EGL_NO_CONFIG_KHR;
    if (!has_extension(extensions, "EGL_KHR_no_config_context")) {
        static const EGLint config_attributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_DONT_CARE, EGL_NONE };
        EGLint count = 0;
        if (!eglChooseConfig(browser->egl, config_attributes, &config, 1, &count) || count < 1) {
            *error = g_strdup("No EGL config for an OpenGL ES 2 copy context");
            return FALSE;
        }
    }
    static const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    copy->context = eglCreateContext(browser->egl, config, EGL_NO_CONTEXT, context_attributes);
    if (copy->context == EGL_NO_CONTEXT) {
        *error = g_strdup_printf("eglCreateContext for the GPU copy failed: 0x%04x", eglGetError());
        return FALSE;
    }
    SavedContext saved;
    if (!copy_context_enter(browser, &saved)) {
        *error = g_strdup_printf("eglMakeCurrent (surfaceless) for the GPU copy failed: 0x%04x", eglGetError());
        return FALSE;
    }
    gboolean ok = FALSE;
    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    copy->has_external = has_extension(gl_extensions, "GL_OES_EGL_image_external");
    if (!has_extension(gl_extensions, "GL_OES_EGL_image")) {
        *error = g_strdup("GLES lacks GL_OES_EGL_image");
        goto out;
    }
    if (!(copy->program_2d = compile_program(FALSE, error))) goto out;
    if (copy->has_external) {
        char *ignored = NULL;
        copy->program_external = compile_program(TRUE, &ignored);
        if (!copy->program_external) { g_warning("External-texture copy program unavailable: %s", ignored); copy->has_external = FALSE; }
        g_free(ignored);
    }
    for (unsigned i = 0; i < SCANOUT_POOL_SIZE; i++)
        if (!create_scanout_buffer(browser, &copy->pool[i], error)) goto out;
    glFinish();
    copy->imports = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_message("GPU copy ready: renderer=%s, %u contiguous %ux%u ARGB8888 KMS buffers (fb %u/%u/%u), modifiers=%s sync=%s",
              (const char *)glGetString(GL_RENDERER), SCANOUT_POOL_SIZE, browser->ui_width, browser->ui_height,
              copy->pool[0].fb, copy->pool[1].fb, copy->pool[2].fb, copy->has_modifiers ? "yes" : "no", copy_sync_name(browser));
    ok = TRUE;
out:
    copy_context_leave(browser, &saved);
    return ok;
}

static ImportedBuffer *import_texture(Browser *browser, struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource, char **error)
{
    ImportedBuffer *import = g_hash_table_lookup(browser->copy.imports, resource->buffer_resource);
    if (import) return import;
    EGLint attributes[64];
    if (!dmabuf_attributes(browser, attributes, resource->width, resource->height, resource->format, resource->n_planes,
                           resource->fds, resource->strides, resource->offsets, resource->modifiers)) {
        *error = g_strdup_printf("WPE buffer has modifier 0x%016" PRIx64 " but EGL lacks EGL_EXT_image_dma_buf_import_modifiers", resource->modifiers[0]);
        return NULL;
    }
    EGLImageKHR image = create_image(browser->egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (image == EGL_NO_IMAGE_KHR) {
        *error = g_strdup_printf("eglCreateImageKHR for the WPE buffer failed: 0x%04x (size=%ux%u format=0x%08x planes=%u modifier=0x%016" PRIx64 ")",
                                 eglGetError(), resource->width, resource->height, resource->format, resource->n_planes, resource->modifiers[0]);
        return NULL;
    }
    import = g_new0(ImportedBuffer, 1);
    import->browser = browser;
    import->resource = resource->buffer_resource;
    import->image = image;
    import->target = GL_TEXTURE_2D;
    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &import->texture);
    glBindTexture(GL_TEXTURE_2D, import->texture);
    image_target_texture(GL_TEXTURE_2D, image);
    if (glGetError() != GL_NO_ERROR) {
        /* Some modifiers (YUV-like or external-only) can only be sampled externally. */
        glDeleteTextures(1, &import->texture);
        import->texture = 0;
        if (browser->copy.has_external) {
            import->target = GL_TEXTURE_EXTERNAL_OES;
            glGenTextures(1, &import->texture);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, import->texture);
            image_target_texture(GL_TEXTURE_EXTERNAL_OES, image);
        }
        GLenum gl_error = glGetError();
        if (!import->texture || gl_error != GL_NO_ERROR) {
            *error = g_strdup_printf("Cannot bind the WPE buffer as a texture (GL error 0x%04x, modifier 0x%016" PRIx64 ")", gl_error, resource->modifiers[0]);
            destroy_import(browser, import);
            return NULL;
        }
    }
    glTexParameteri(import->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(import->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(import->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(import->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    import->destroy.notify = import_destroyed;
    wl_resource_add_destroy_listener(resource->buffer_resource, &import->destroy);
    g_hash_table_insert(browser->copy.imports, import->resource, import);
    g_debug("Imported WPE buffer %p as %s texture (%ux%u modifier 0x%016" PRIx64 ", %u cached)", (void *)import->resource,
            import->target == GL_TEXTURE_2D ? "2D" : "external", resource->width, resource->height, resource->modifiers[0],
            g_hash_table_size(browser->copy.imports));
    return import;
}

static gboolean frame_uses(const Frame *frame, const ScanoutBuffer *buffer)
{
    return frame && frame->scanout == buffer;
}

/* Copies the WPE frame into a free contiguous buffer and returns the WPE buffer
 * at once. Returns NULL after fail(). */
static Frame *copy_frame(Browser *browser, struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource)
{
    GpuCopy *copy = &browser->copy;
    ScanoutBuffer *target = NULL;
    for (unsigned i = 0; i < SCANOUT_POOL_SIZE && !target; i++) {
        ScanoutBuffer *buffer = &copy->pool[i];
        if (!frame_uses(browser->current, buffer) && !frame_uses(browser->pending, buffer) && !frame_uses(browser->queued, buffer))
            target = buffer;
    }
    /* All buffers busy: the queued frame was never submitted, so overwrite it. */
    if (!target && browser->queued && browser->queued->scanout) target = browser->queued->scanout;
    char *error = NULL;
    SavedContext saved;
    int fence_fd = -1;
    gint64 started = g_get_monotonic_time();
    if (!target) error = g_strdup("no free scanout buffer");
    else if (!copy_context_enter(browser, &saved)) error = g_strdup_printf("eglMakeCurrent failed: 0x%04x", eglGetError());
    else {
        ImportedBuffer *import = import_texture(browser, resource, &error);
        if (import) {
            static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
            glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
            glViewport(0, 0, browser->ui_width, browser->ui_height);
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_DITHER);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glUseProgram(import->target == GL_TEXTURE_2D ? copy->program_2d : copy->program_external);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(import->target, import->texture);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
            glEnableVertexAttribArray(0);
            /* Memory row 0 of the source lands on memory row 0 of the target:
             * both images are top-down, so no flip is applied. */
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            GLenum gl_error = glGetError();
            if (gl_error != GL_NO_ERROR) error = g_strdup_printf("GPU copy draw failed: GL error 0x%04x", gl_error);
            /* KMS must not scan out a half-written buffer: hand the kernel a fence
             * for the copy, or wait for it here when that is unavailable. */
            if (copy->has_native_fence && !error) {
                static const EGLint fence_attributes[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE };
                EGLSyncKHR native = create_sync(browser->egl, EGL_SYNC_NATIVE_FENCE_ANDROID, fence_attributes);
                if (native != EGL_NO_SYNC_KHR) {
                    glFlush(); /* the fence fd exists only once the commands are submitted */
                    fence_fd = dup_native_fence(browser->egl, native);
                    destroy_sync(browser->egl, native);
                }
                if (fence_fd < 0) {
                    g_warning("EGL native fence unavailable (0x%04x); waiting for GPU copies on the CPU from now on", eglGetError());
                    copy->has_native_fence = FALSE;
                }
            }
            EGLSyncKHR sync = fence_fd < 0 && copy->has_fence ? create_sync(browser->egl, EGL_SYNC_FENCE_KHR, NULL) : EGL_NO_SYNC_KHR;
            if (fence_fd >= 0) {
                /* The kernel waits on the fence before the flip. */
            } else if (sync != EGL_NO_SYNC_KHR) {
                EGLint result = client_wait_sync(browser->egl, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 2000000000ull);
                destroy_sync(browser->egl, sync);
                if (result != EGL_CONDITION_SATISFIED_KHR && !error)
                    error = g_strdup_printf("GPU copy fence wait failed: 0x%04x", result == EGL_FALSE ? eglGetError() : result);
            } else glFinish();
        }
        copy_context_leave(browser, &saved);
    }
    /* The pixels are ours now (or the copy failed): hand the buffer straight back to WebKit. */
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(browser->exportable, resource->buffer_resource);
    if (error) {
        if (fence_fd >= 0) close(fence_fd);
        char *message = g_strdup_printf("GPU copy of the WPE frame into a contiguous scanout buffer failed: %s", error);
        fail(browser, message);
        g_free(message);
        g_free(error);
        return NULL;
    }
    copy->copied++;
    copy->copy_us += g_get_monotonic_time() - started;
    if (browser->queued && browser->queued->scanout == target) {
        browser->stats.replaced++;
        release_frame(browser, browser->queued);
        browser->queued = NULL;
    }
    Frame *frame = frame_new();
    frame->scanout = target;
    frame->fb = target->fb;
    frame->fence_fd = fence_fd;
    return frame;
}

static void export_dmabuf(void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource)
{
    Browser *browser = data;
    Frame *frame = NULL;
    if (!browser->copy_mode) {
        char *reason = NULL;
        frame = import_direct(browser, resource, &reason);
        if (!frame) {
            char *error = NULL;
            g_message("Direct scanout of the WPE DMA-BUF is not possible (%s); switching permanently to GPU copy into contiguous KMS buffers", reason);
            if (!setup_copy(browser, &error)) {
                char *message = g_strdup_printf("Cannot scan out WPE DMA-BUF directly (%s) and the GPU-copy fallback could not be set up: %s", reason, error);
                wpe_view_backend_exportable_fdo_dispatch_release_buffer(browser->exportable, resource->buffer_resource);
                fail(browser, message);
                g_free(message);
                g_free(reason);
                g_free(error);
                return;
            }
            g_free(reason);
            browser->copy_mode = TRUE;
            write_status(browser);
        }
    }
    if (!frame && !(frame = copy_frame(browser, resource))) return;
    if (browser->queued) {
        browser->stats.replaced++;
        release_frame(browser, browser->queued);
    }
    browser->queued = frame;
    submit_commit(browser);
}

static void export_legacy(void *data, struct wl_resource *resource)
{
    Browser *browser = data;
    wpe_view_backend_exportable_fdo_dispatch_release_buffer(browser->exportable, resource);
    fail(browser, "WPE exported a legacy EGL Wayland buffer instead of DMA-BUF; direct alpha-preserving scanout is unavailable");
}

static void export_shm(void *data, struct wpe_fdo_shm_exported_buffer *buffer)
{
    Browser *browser = data;
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(browser->exportable, buffer);
    fail(browser, "WPE exported a software SHM buffer; this GPU development workload requires DMA-BUF rendering");
}

static gboolean watchdog(gpointer data)
{
    Browser *browser = data;
    if (browser->commit_pending && g_get_monotonic_time() - browser->submitted_at > 5 * G_USEC_PER_SEC) {
        fail(browser, "No DRM pageflip completion within five seconds; buffers retained until scanout is disabled");
        browser->watchdog_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean quit_signal(gpointer data)
{
    Browser *browser = data;
    g_main_loop_quit(browser->loop);
    return G_SOURCE_CONTINUE;
}

static bool fullscreen(void *data, bool enabled)
{
    Browser *browser = data;
    if (enabled) wpe_view_backend_dispatch_did_enter_fullscreen(browser->backend);
    else wpe_view_backend_dispatch_did_exit_fullscreen(browser->backend);
    return true;
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    if (event == WEBKIT_LOAD_FINISHED) g_message("Page loaded: %s", webkit_web_view_get_uri(view));
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event, const char *uri, GError *error, gpointer data)
{
    (void)view; (void)event; (void)data;
    g_warning("Page load failed for %s: %s", uri, error->message);
    return FALSE;
}

static void process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    (void)view;
    Browser *browser = data;
    char *message = g_strdup_printf("WebKit web process terminated, reason=%d", reason);
    fail(browser, message);
    g_free(message);
}

/* WebKit's bubblewrap sandbox device-binds only /dev/video0-2 and /dev/media0,
 * and webkit_web_context_add_path_to_sandbox() rejects /dev paths (its binds
 * are nodev as well). The image therefore installs embedded-browser-bwrap as
 * /usr/bin/bwrap, which device-binds every V4L2/media node before running the
 * real bwrap. Log what the web process will see so a missing decoder is
 * diagnosable from the browser log.
 *
 * The holepunch sink also runs in the web process and needs the DRM broker
 * socket; WebKit gives the sandbox an empty XDG_RUNTIME_DIR, so the broker's
 * own directory is bound read-write. Must run before the first web process. */
static void configure_sandbox(void)
{
    if (g_file_test(BROWSER_VIDEO_RUNTIME_DIR, G_FILE_TEST_IS_DIR)) {
        webkit_web_context_add_path_to_sandbox(webkit_web_context_get_default(), BROWSER_VIDEO_RUNTIME_DIR, FALSE);
        g_message("WebKit sandbox: bound %s (DRM broker socket, video status)", BROWSER_VIDEO_RUNTIME_DIR);
    }

    GString *nodes = g_string_new(NULL);
    const char *dirs[] = { "/sys/bus/media/devices", "/sys/class/video4linux" };
    for (size_t i = 0; i < G_N_ELEMENTS(dirs); i++) {
        GDir *dir = g_dir_open(dirs[i], 0, NULL);
        if (!dir) continue;
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *label_path = g_build_filename(dirs[i], name, i ? "name" : "model", NULL);
            char *label = NULL;
            if (g_file_get_contents(label_path, &label, NULL, NULL)) g_strstrip(label);
            g_string_append_printf(nodes, " /dev/%s(%s)", name, label ? label : "?");
            g_free(label);
            g_free(label_path);
        }
        g_dir_close(dir);
    }
    gboolean shim = g_file_test("/usr/bin/bwrap.real", G_FILE_TEST_IS_EXECUTABLE);
    if (shim)
        g_message("WebKit sandbox: embedded-browser-bwrap device-binds V4L2 nodes:%s", nodes->len ? nodes->str : " none");
    else
        g_warning("WebKit sandbox: /usr/bin/bwrap is not the embedded-browser shim; only /dev/video0-2 and /dev/media0 reach the web process (present:%s)",
                  nodes->len ? nodes->str : " none");
    g_string_free(nodes, TRUE);
}

static gboolean initialize_webkit(Browser *browser)
{
    const char *render_device = g_getenv("BROWSER_RENDER_DEVICE");
    if (render_device && *render_device) {
        browser->render_fd = open(render_device, O_RDWR | O_CLOEXEC);
        if (browser->render_fd < 0) {
            fail(browser, "Cannot open BROWSER_RENDER_DEVICE; check the GPU render-node path and access");
            return FALSE;
        }
        g_message("Using GPU render device %s separately from KMS display %s", render_device, browser->device);
    }
    browser->gbm = gbm_create_device(browser->render_fd >= 0 ? browser->render_fd : browser->fd);
    if (!browser->gbm) { fail(browser, "Cannot create a GBM device for the selected DRM display"); return FALSE; }
    browser->egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, browser->gbm, NULL);
    EGLint major, minor;
    if (browser->egl == EGL_NO_DISPLAY || !eglInitialize(browser->egl, &major, &minor)) {
        fail(browser, "Cannot initialize GBM EGL; check Mesa Panfrost/kmsro and render-node access");
        return FALSE;
    }
    g_message("EGL %d.%d vendor=%s version=%s", major, minor, eglQueryString(browser->egl, EGL_VENDOR), eglQueryString(browser->egl, EGL_VERSION));
    if (!wpe_loader_init("libWPEBackend-fdo-1.0.so") || !wpe_fdo_initialize_for_egl_display(browser->egl)) {
        fail(browser, "Cannot initialize WPEBackend-fdo with the GBM EGL display");
        return FALSE;
    }
    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_buffer_resource = export_legacy, .export_dmabuf_resource = export_dmabuf, .export_shm_buffer = export_shm
    };
    browser->exportable = wpe_view_backend_exportable_fdo_create(&client, browser, browser->ui_width, browser->ui_height);
    if (!browser->exportable) { fail(browser, "Cannot create the WPE exportable backend"); return FALSE; }
    browser->backend = wpe_view_backend_exportable_fdo_get_view_backend(browser->exportable);
    configure_sandbox();
    WebKitWebViewBackend *backend = webkit_web_view_backend_new(browser->backend, NULL, NULL);
    browser->view = webkit_web_view_new(backend);
    if (!browser->view) { fail(browser, "Cannot create the WebKit web view"); return FALSE; }
    WebKitColor background = { 0, 0, 0, 0 };
    webkit_web_view_set_background_color(browser->view, &background);
    WebKitSettings *settings = webkit_web_view_get_settings(browser->view);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    webkit_settings_set_media_playback_allows_inline(settings, TRUE);
    /* A DOM cursor keeps USB-pointer controls usable without a cursor KMS plane. */
    const char *cursor_source = "(()=>{let c=document.createElement('div');c.style.cssText='position:fixed;width:12px;height:12px;border:2px solid white;border-radius:50%;background:#11374466;pointer-events:none;z-index:2147483647;display:none';document.documentElement.appendChild(c);document.addEventListener('mousemove',e=>{c.style.display='block';c.style.left=(e.clientX-6)+'px';c.style.top=(e.clientY-6)+'px'})})()";
    WebKitUserScript *cursor = webkit_user_script_new(cursor_source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
    webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(browser->view), cursor);
    webkit_user_script_unref(cursor);
    g_signal_connect(browser->view, "load-changed", G_CALLBACK(load_changed), browser);
    g_signal_connect(browser->view, "load-failed", G_CALLBACK(load_failed), browser);
    g_signal_connect(browser->view, "web-process-terminated", G_CALLBACK(process_terminated), browser);
    wpe_view_backend_set_fullscreen_handler(browser->backend, fullscreen, browser);
    wpe_view_backend_add_activity_state(browser->backend, wpe_view_activity_state_visible | wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
    wpe_view_backend_set_target_refresh_rate(browser->backend, mode_refresh(&browser->mode) * 1000);
    browser->input = embedded_browser_input_start(browser->backend, browser->ui_width, browser->ui_height);
    webkit_web_view_load_uri(browser->view, browser->uri);
    return TRUE;
}

static void cleanup(Browser *browser)
{
    if (browser->fd_source) g_source_remove(browser->fd_source);
    if (browser->watchdog_source) g_source_remove(browser->watchdog_source);
    if (browser->retry_source) g_source_remove(browser->retry_source);
    if (browser->sigterm_source) g_source_remove(browser->sigterm_source);
    if (browser->sigint_source) g_source_remove(browser->sigint_source);
    embedded_browser_input_stop(browser->input);
    embedded_browser_drm_broker_stop();
    if (browser->modeset) {
        gboolean disabled = FALSE;
        drmModeAtomicReq *request = drmModeAtomicAlloc();
        if (request) {
            drmModeAtomicAddProperty(request, browser->primary, browser->plane_fb, 0);
            drmModeAtomicAddProperty(request, browser->primary, browser->plane_crtc, 0);
            drmModeAtomicAddProperty(request, browser->connector, browser->connector_crtc, 0);
            drmModeAtomicAddProperty(request, browser->crtc, browser->crtc_active, 0);
            drmModeAtomicAddProperty(request, browser->crtc, browser->crtc_mode, 0);
            /* The WebKit video process can still own the brokered overlay. */
            drmModePlaneRes *planes = drmModeGetPlaneResources(browser->fd);
            if (planes) {
                for (uint32_t i = 0; i < planes->count_planes; i++) {
                    uint64_t attached_crtc = 0;
                    uint32_t plane = planes->planes[i];
                    uint32_t crtc_property = property_id(browser->fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &attached_crtc);
                    if (plane != browser->primary && attached_crtc == browser->crtc && crtc_property) {
                        uint32_t fb_property = property_id(browser->fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
                        if (fb_property) drmModeAtomicAddProperty(request, plane, fb_property, 0);
                        drmModeAtomicAddProperty(request, plane, crtc_property, 0);
                    }
                }
                drmModeFreePlaneResources(planes);
            }
            disabled = drmModeAtomicCommit(browser->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0;
            drmModeAtomicFree(request);
        }
        if (!disabled) {
            /* Never return a buffer to WebKit while KMS might still read it.
             * The kernel and WPE resources remain owned until process exit.
             */
            g_warning("Could not disable scanout; retaining displayed/pending DMA-BUF resources until process exit");
            return;
        }
    }
    release_frame(browser, browser->queued);
    release_frame(browser, browser->pending);
    release_frame(browser, browser->current);
    browser->queued = browser->pending = browser->current = NULL;
    /* Before the exportable: its buffers' destroy listeners point into the copy cache. */
    destroy_copy(browser);
    if (browser->view) g_object_unref(browser->view);
    if (browser->exportable) wpe_view_backend_exportable_fdo_destroy(browser->exportable);
    if (browser->egl != EGL_NO_DISPLAY) eglTerminate(browser->egl);
    if (browser->gbm) gbm_device_destroy(browser->gbm);
    if (browser->render_fd >= 0) close(browser->render_fd);
    if (browser->mode_blob) drmModeDestroyPropertyBlob(browser->fd, browser->mode_blob);
    if (browser->fd >= 0) { drmDropMaster(browser->fd); close(browser->fd); }
    if (browser->handles) g_hash_table_unref(browser->handles);
    if (browser->video.serials) g_hash_table_unref(browser->video.serials);
    if (browser->loop) g_main_loop_unref(browser->loop);
    g_free(browser->device);
    g_free(browser->failure);
    g_free(browser->holepunch_error);
}

static void help(void)
{
    puts("Usage: embedded-browser [--version | --help] [URL]\n"
         "Owned WPE WebKit DRM/GBM shell for Admiral display development.\n"
         "Default URL: http://127.0.0.1:8080/\n\n"
         "BROWSER_DRM_DEVICE          Explicit primary DRM node (otherwise connected card auto-detected)\n"
         "BROWSER_RENDER_DEVICE       Optional GPU render node for GBM/EGL, separate from KMS\n"
         "BROWSER_DRM_CONNECTOR       Connector name (HDMI-A-1) or numeric ID\n"
         "BROWSER_DRM_PRIMARY_PLANE   Numeric ARGB8888 primary-plane ID\n"
         "BROWSER_DRM_MODE            WIDTHxHEIGHT[@REFRESH] (otherwise preferred connector mode)\n"
         "BROWSER_UI_SIZE            WIDTHxHEIGHT of the web view, scaled to the mode by the primary plane\n"
         "                            (default: mode size, or the largest <=1920x1080 size of the mode's aspect ratio)\n"
         "BROWSER_VIDEO_MODE          holepunch (default) or composited\n"
         "BROWSER_VIDEO_SINK          kmssink lets kmssink commit the video plane itself (default: the browser commits it with the UI)\n"
         "BROWSER_FRAME_CALLBACK      flip releases WebKit's frame callback at the page flip (default: when the commit is accepted)\n"
         "BROWSER_INPUT               0 disables native keyboard/pointer input\n"
         "BROWSER_INPUT_SEAT          udev input seat (default seat0)\n"
         "BROWSER_KEYBOARD_LAYOUT     XKB layout (default system/us)\n"
         "BROWSER_STATUS              Native JSON status path (default /run/embedded-browser/browser-status.json)\n"
         "WEBKIT_INSPECTOR_SERVER     Remote inspector listen address\n\n"
         "Requires atomic DRM master and GBM EGL. WPE DMA-BUF frames are scanned out directly when the\n"
         "display can import them, otherwise GPU-copied into contiguous ARGB8888 KMS buffers.\n"
         "No window manager or Cog dependency. Native touchscreen input is not implemented.");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        printf("embedded-browser %s (WPE WebKit %u.%u.%u)\n", VERSION, webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) { help(); return 0; }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) { help(); return 2; }
    Browser browser = { .fd = -1, .render_fd = -1, .egl = EGL_NO_DISPLAY, .copy = { .context = EGL_NO_CONTEXT }, .uri = argc == 2 ? argv[1] : "http://127.0.0.1:8080/" };
    browser.holepunch = embedded_browser_drm_holepunch_enabled();
    browser.frame_complete_early = g_strcmp0(g_getenv("BROWSER_FRAME_CALLBACK"), "flip") != 0;
    browser.handles = g_hash_table_new(g_direct_hash, g_direct_equal);
    browser.loop = g_main_loop_new(NULL, FALSE);
    if (!open_display(&browser)) {
        fail(&browser, "No connected atomic DRM display with ARGB8888 primary plane; check /dev/dri access, DRM master, connector and DE35 support");
        cleanup(&browser);
        return 1;
    }
    if (!initial_scanout(&browser)) {
        fail(&browser, "Cannot establish the initial atomic ARGB browser scanout; check DE35 primary-plane modesetting");
        cleanup(&browser);
        return 1;
    }
    if (browser.holepunch) {
        GError *error = NULL;
        if (!embedded_browser_drm_broker_start(browser.fd, browser.connector, browser.crtc, browser.crtc_index, browser.primary,
                                     browser.ui_width, browser.ui_height,
                                     browser.mode.hdisplay, browser.mode.vdisplay, &error)) {
            browser.holepunch_error = g_strdup(error ? error->message : "DRM holepunch broker initialization failed");
            g_warning("Holepunch unavailable: %s; continuing the browser diagnostic screen", browser.holepunch_error);
            g_clear_error(&error);
        } else {
            browser.broker_started = TRUE;
            if (g_strcmp0(g_getenv("BROWSER_VIDEO_SINK"), "kmssink") != 0) {
                setup_video_plane(&browser);
                if (browser.video.plane) embedded_browser_drm_broker_set_video_handler(video_message, &browser);
            } else g_message("BROWSER_VIDEO_SINK=kmssink: the holepunch sink commits the video plane itself");
        }
    }
    browser.fd_source = g_unix_fd_add(browser.fd, G_IO_IN | G_IO_ERR | G_IO_HUP, drm_ready, &browser);
    browser.watchdog_source = g_timeout_add_seconds(1, watchdog, &browser);
    browser.sigterm_source = g_unix_signal_add(SIGTERM, quit_signal, &browser);
    browser.sigint_source = g_unix_signal_add(SIGINT, quit_signal, &browser);
    write_status(&browser);
    if (initialize_webkit(&browser) && !browser.failure) g_main_loop_run(browser.loop);
    int result = browser.exit_code;
    cleanup(&browser);
    return result;
}
