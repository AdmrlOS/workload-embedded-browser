/* SPDX-License-Identifier: MIT
 * Experimental adapter for the WPE 2.48 Westeros holepunch ABI. This is not
 * Westeros: WebKit owns playback and geometry. Decoded frames go to the
 * browser, which commits the video plane together with its UI frames
 * (gst-admiral-videoplane.c); kmssink, committing the plane by itself, remains
 * for software decoders, older brokers and BROWSER_VIDEO_SINK=kmssink.
 */
#include "drm-broker.h"
#include "gst-admiral-videoplane.h"
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gst/allocators/gstdmabuf.h>
#include <json-glib/json-glib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

#define PACKAGE "embedded-browser"
/* The browser binds this directory into the WebKit sandbox, where this sink runs. */
#define RUN_DIR BROWSER_VIDEO_RUNTIME_DIR
#define BROKER_PATH RUN_DIR "/drm.sock"
#define STATUS_PATH RUN_DIR "/video-status.json"
/* Stateless V4L2 decoders; BROWSER_DECODER takes a comma-separated subset. */
#define DEFAULT_DECODERS "v4l2slh264dec,v4l2slh265dec,v4l2slvp9dec"
/* kmssink scans out linear NV12 only. Cedrus can also emit tiled NV12_32L32, which
 * the display shows as garbage, so force the decoder to negotiate linear NV12. */
#define LINEAR_NV12_CAPS "video/x-raw, format=(string)NV12; " \
    "video/x-raw(memory:DMABuf), format=(string)DMA_DRM, drm-format=(string)NV12"

typedef struct {
    GstBin parent;
    GstElement *kms, *filter;
    GstPad *ghost;
    GstElement *pipeline;
    gulong element_handler;
    GstBus *bus;
    gulong bus_error_handler, bus_eos_handler;
    GMutex lock;
    gint fd, lock_fd, broker_socket; /* broker_socket: kept open for browser-committed frames */
    gboolean browser_commits, broker_video_frames;
    gint crtc_width, crtc_height;
    gint connector_id, crtc_id, browser_plane_id, video_plane_id;
    gint x, y, width, height, decoded_width, decoded_height;
    gint ui_width, ui_height, mode_width, mode_height;
    gint render_x, render_y, render_width, render_height;
    gboolean blend_configured, failed, decoder_verified, dmabuf_seen;
    gboolean immediate_output;
    gfloat zorder;
    guint res_usage;
    guint64 decoded_frames, input_frames;
    gint64 last_status;
    gchar *decoder, *decoded_caps, *error, *state;
} AdmiralSink;

typedef struct { GstBinClass parent_class; } AdmiralSinkClass;
G_DEFINE_TYPE(AdmiralSink, admiral_sink, GST_TYPE_BIN)

enum { PROP_0, PROP_RECTANGLE, PROP_ZORDER, PROP_RES_USAGE, PROP_IMMEDIATE_OUTPUT, PROP_STATS };
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string){ NV12, I420, YV12, BGRA, RGBA, BGRx, RGBx }; "
                    "video/x-raw(memory:DMABuf), format=(string)DMA_DRM"));

/* Called with the sink lock held. Probe threads write at most once per second. */
static void
write_status(AdmiralSink *self, gboolean force)
{
    gint64 now = g_get_monotonic_time();
    if (!force && now - self->last_status < G_USEC_PER_SEC)
        return;
    self->last_status = now;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
#define STRING_FIELD(key, val) json_builder_set_member_name(b, key); json_builder_add_string_value(b, (val) ? (val) : "")
#define INT_FIELD(key, val) json_builder_set_member_name(b, key); json_builder_add_int_value(b, val)
#define BOOL_FIELD(key, val) json_builder_set_member_name(b, key); json_builder_add_boolean_value(b, val)
    STRING_FIELD("state", self->state);
    STRING_FIELD("decoder", self->decoder);
    STRING_FIELD("decoded_caps", self->decoded_caps);
    STRING_FIELD("error", self->error);
    INT_FIELD("position_x", self->x); INT_FIELD("position_y", self->y);
    INT_FIELD("width", self->width); INT_FIELD("height", self->height);
    INT_FIELD("ui_width", self->ui_width); INT_FIELD("ui_height", self->ui_height);
    INT_FIELD("mode_width", self->mode_width); INT_FIELD("mode_height", self->mode_height);
    INT_FIELD("render_x", self->render_x); INT_FIELD("render_y", self->render_y);
    INT_FIELD("render_width", self->render_width); INT_FIELD("render_height", self->render_height);
    INT_FIELD("decoded_width", self->decoded_width); INT_FIELD("decoded_height", self->decoded_height);
    INT_FIELD("decoded_frames", self->decoded_frames); INT_FIELD("sink_input_frames", self->input_frames);
    INT_FIELD("pid", getpid()); INT_FIELD("updated_at_unix_ms", g_get_real_time() / 1000);
    INT_FIELD("connector_id", self->connector_id); INT_FIELD("crtc_id", self->crtc_id);
    INT_FIELD("browser_plane_id", self->browser_plane_id); INT_FIELD("video_plane_id", self->video_plane_id);
    BOOL_FIELD("dmabuf_seen", self->dmabuf_seen);
    STRING_FIELD("video_sink", self->kms ? (self->browser_commits ? "browser-commit" : "kmssink") : "");
    json_builder_set_member_name(b, "hardware_decode"); json_builder_begin_object(b);
    BOOL_FIELD("verified", self->decoder_verified && !self->failed &&
               g_strcmp0(self->state, "playing") == 0 && self->decoder && g_str_has_prefix(self->decoder, "v4l2sl"));
    STRING_FIELD("evidence", "Allowed decoder output buffer observed; this does not attest display scanout");
    json_builder_end_object(b);
    json_builder_set_member_name(b, "holepunch"); json_builder_begin_object(b);
    BOOL_FIELD("configured", self->fd >= 0 && self->blend_configured && !self->failed);
    BOOL_FIELD("verified", FALSE);
    STRING_FIELD("evidence", "Shared DRM fd, overlay plane and alpha blend configured; verify visually and with DRM state");
    json_builder_end_object(b);
    json_builder_end_object(b);
    JsonNode *node = json_builder_get_root(b);
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, node);
    gsize size;
    gchar *data = json_generator_to_data(g, &size);
    g_mkdir_with_parents(RUN_DIR, 0700);
    gchar *temp = g_strdup_printf(STATUS_PATH ".%ld.%p.tmp", (long)getpid(), (void *)self);
    if (g_file_set_contents(temp, data, (gssize)size, NULL)) {
        if (rename(temp, STATUS_PATH) != 0)
            unlink(temp);
    }
    g_free(temp); g_free(data); json_node_free(node); g_object_unref(g); g_object_unref(b);
#undef STRING_FIELD
#undef INT_FIELD
#undef BOOL_FIELD
}

static void
fail_sink(AdmiralSink *self, const gchar *message)
{
    g_mutex_lock(&self->lock);
    if (self->failed) {
        g_mutex_unlock(&self->lock);
        return;
    }
    self->failed = TRUE;
    g_free(self->error); self->error = g_strdup(message);
    g_free(self->state); self->state = g_strdup("error");
    write_status(self, TRUE);
    g_mutex_unlock(&self->lock);
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", message), ("Admiral holepunch failed closed"));
}

/* Receives the browser's DRM descriptor. When keep_socket is set and the browser
 * accepts frames, the connection stays open as self->broker_socket. */
static gboolean
get_drm_fd(AdmiralSink *self, gboolean keep_socket)
{
    gint sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return FALSE;
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    g_strlcpy(address.sun_path, BROKER_PATH, sizeof(address.sun_path));
    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(sock); return FALSE;
    }
    gchar payload[4096] = {0};
    union { struct cmsghdr align; gchar bytes[CMSG_SPACE(sizeof(gint))]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = { .iov_base = payload, .iov_len = sizeof(payload) - 1 };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    ssize_t received = recvmsg(sock, &message, MSG_CMSG_CLOEXEC);
    gint received_fd = -1;
    for (struct cmsghdr *c = received > 0 ? CMSG_FIRSTHDR(&message) : NULL; c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(gint)))
            memcpy(&received_fd, CMSG_DATA(c), sizeof(received_fd));
    }
    if (received <= 0 || message.msg_flags & (MSG_CTRUNC | MSG_TRUNC) || received_fd < 0) {
        if (received_fd >= 0) close(received_fd);
        close(sock);
        return FALSE;
    }
    /* SOCK_STREAM does not preserve the broker's single write boundary. */
    while (!memchr(payload, '\n', (gsize)received) && received < (ssize_t)sizeof(payload) - 1) {
        ssize_t part = recv(sock, payload + received, sizeof(payload) - 1 - (gsize)received, 0);
        if (part < 0 && errno == EINTR) continue;
        if (part <= 0) break;
        received += part;
    }
    JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, payload, received, NULL);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj = json_node_get_object(root);
        const gchar *members[] = { "connector_id", "crtc_id", "browser_plane_id", "video_plane_id", "blend_configured" };
        for (guint i = 0; i < G_N_ELEMENTS(members); i++)
            ok = ok && json_object_has_member(obj, members[i]);
        if (ok) {
            self->connector_id = (gint)json_object_get_int_member(obj, "connector_id");
            self->crtc_id = (gint)json_object_get_int_member(obj, "crtc_id");
            self->browser_plane_id = (gint)json_object_get_int_member(obj, "browser_plane_id");
            self->video_plane_id = (gint)json_object_get_int_member(obj, "video_plane_id");
            self->blend_configured = json_object_get_boolean_member(obj, "blend_configured");
            self->broker_video_frames = json_object_has_member(obj, "video_frames") &&
                                        json_object_get_boolean_member(obj, "video_frames");
            /* Older brokers send neither size: treat the UI as mode-sized. */
            self->ui_width = self->mode_width = self->ui_height = self->mode_height = 0;
            if (json_object_has_member(obj, "ui_width") && json_object_has_member(obj, "ui_height") &&
                json_object_has_member(obj, "mode_width") && json_object_has_member(obj, "mode_height")) {
                self->ui_width = (gint)json_object_get_int_member(obj, "ui_width");
                self->ui_height = (gint)json_object_get_int_member(obj, "ui_height");
                self->mode_width = (gint)json_object_get_int_member(obj, "mode_width");
                self->mode_height = (gint)json_object_get_int_member(obj, "mode_height");
            }
            ok = self->blend_configured && self->video_plane_id > 0 && self->browser_plane_id != self->video_plane_id &&
                 ((self->ui_width > 0 && self->ui_height > 0 && self->mode_width > 0 && self->mode_height > 0) ||
                  (!self->ui_width && !self->ui_height && !self->mode_width && !self->mode_height));
        }
    } else
        ok = FALSE;
    g_object_unref(parser);
    if (ok && keep_socket && self->broker_video_frames) self->broker_socket = sock;
    else close(sock);
    if (!ok) { close(received_fd); return FALSE; }
    if (drmIsMaster(received_fd) != 1) { close(received_fd); return FALSE; }
    /* kmssink can modeset an inactive CRTC even with force-modesetting=false.
     * Only attach after the browser's synchronous primary-plane modeset.
     */
    drmModeCrtc *crtc = drmModeGetCrtc(received_fd, (guint32)self->crtc_id);
    ok = crtc && crtc->mode_valid && crtc->buffer_id != 0;
    if (ok) { self->crtc_width = crtc->mode.hdisplay; self->crtc_height = crtc->mode.vdisplay; }
    if (crtc) drmModeFreeCrtc(crtc);
    if (!ok) { close(received_fd); return FALSE; }
    self->fd = received_fd;
    return TRUE;
}

/* floor(value * numerator / denominator) for a positive denominator. */
static gint
scale_floor(gint64 value, gint64 numerator, gint64 denominator)
{
    gint64 product = value * numerator;
    gint64 result = product / denominator;
    if (product % denominator < 0) result--;
    return (gint)result;
}

/* WebKit reports the video rectangle in web-view (UI) pixels but the NV12 overlay
 * lives in CRTC (mode) pixels. Scale it, rounding outward to even pixels so the
 * subsampled NV12 plane covers the whole holepunch. Called with the lock held. */
static void
compute_render_rectangle(AdmiralSink *self)
{
    gint x = self->x, y = self->y, width = MAX(1, self->width), height = MAX(1, self->height);
    if (self->ui_width > 0 && self->ui_height > 0 && self->mode_width > 0 && self->mode_height > 0 &&
        (self->ui_width != self->mode_width || self->ui_height != self->mode_height)) {
        gint x0 = scale_floor(self->x, self->mode_width, self->ui_width);
        gint y0 = scale_floor(self->y, self->mode_height, self->ui_height);
        gint x1 = scale_floor((gint64)self->x + width, self->mode_width, self->ui_width);
        gint y1 = scale_floor((gint64)self->y + height, self->mode_height, self->ui_height);
        if ((gint64)(self->x + width) * self->mode_width % self->ui_width) x1++;
        if ((gint64)(self->y + height) * self->mode_height % self->ui_height) y1++;
        x0 &= ~1; y0 &= ~1;
        x1 = (x1 + 1) & ~1; y1 = (y1 + 1) & ~1;
        x = x0; y = y0; width = MAX(2, x1 - x0); height = MAX(2, y1 - y0);
    }
    self->render_x = x; self->render_y = y; self->render_width = width; self->render_height = height;
}

/* Called with the lock held. */
static void
apply_render_rectangle(AdmiralSink *self)
{
    compute_render_rectangle(self);
    if (self->kms)
        gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(self->kms), self->render_x, self->render_y,
                                               self->render_width, self->render_height);
}

/* Is name in the comma-separated BROWSER_DECODER list? */
static gboolean
decoder_listed(const gchar *list, const gchar *name)
{
    gchar **items = g_strsplit(list, ",", -1);
    gboolean found = FALSE;
    for (gchar **item = items; *item && !found; item++)
        found = strcmp(g_strstrip(*item), name) == 0;
    g_strfreev(items);
    return found;
}

static GstPadProbeReturn
decoder_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    AdmiralSink *self = data;
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
        return GST_PAD_PROBE_OK;
    GstElement *decoder = gst_pad_get_parent_element(pad);
    GstElementFactory *factory = decoder ? gst_element_get_factory(decoder) : NULL;
    const gchar *name = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "unknown";
    const gchar *allowed = g_getenv("BROWSER_DECODER");
    if (!allowed || !*allowed) allowed = DEFAULT_DECODERS;
    if (!decoder_listed(allowed, name)) {
        gchar *error = g_strdup_printf("Decoder %s is forbidden; expected one of %s", name, allowed);
        fail_sink(self, error); g_free(error);
        if (decoder) gst_object_unref(decoder);
        return GST_PAD_PROBE_DROP;
    }
    g_mutex_lock(&self->lock);
    gboolean failed = self->failed;
    if (!failed) {
        g_free(self->decoder); self->decoder = g_strdup(name);
        self->decoder_verified = TRUE;
        self->decoded_frames++;
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        for (guint i = 0; buffer && i < gst_buffer_n_memory(buffer); i++)
            self->dmabuf_seen |= gst_is_dmabuf_memory(gst_buffer_peek_memory(buffer, i));
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (caps && gst_caps_get_size(caps)) {
            g_free(self->decoded_caps); self->decoded_caps = gst_caps_to_string(caps);
            const GstStructure *s = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(s, "width", &self->decoded_width);
            gst_structure_get_int(s, "height", &self->decoded_height);
            const GstCapsFeatures *features = gst_caps_get_features(caps, 0);
            self->dmabuf_seen |= gst_caps_features_contains(features, "memory:DMABuf");
        }
        if (caps) gst_caps_unref(caps);
        write_status(self, FALSE);
    }
    g_mutex_unlock(&self->lock);
    if (decoder) gst_object_unref(decoder);
    return failed ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static void
observe_element(GstBin *root, GstBin *subbin, GstElement *element, gpointer data)
{
    (void)root; (void)subbin;
    AdmiralSink *self = data;
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory) return;
    const gchar *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    if (!klass || !strstr(klass, "Decoder") || !strstr(klass, "Video")) return;
    GstPad *src = gst_element_get_static_pad(element, "src");
    if (src) {
        /* Hold the sink while a decoder probe can call it during teardown. */
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, decoder_buffer,
                          gst_object_ref(self), (GDestroyNotify)gst_object_unref);
        gst_object_unref(src);
    }
}

static GstPadProbeReturn
sink_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad;
    AdmiralSink *self = data;
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    g_mutex_lock(&self->lock);
    gboolean failed = self->failed;
    if (!failed) { self->input_frames++; write_status(self, FALSE); }
    g_mutex_unlock(&self->lock);
    return failed ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static void
pipeline_message(GstBus *bus, GstMessage *message, gpointer data)
{
    (void)bus;
    AdmiralSink *self = data;
    GError *error = NULL; gchar *debug = NULL;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
        gst_message_parse_error(message, &error, &debug);
    g_mutex_lock(&self->lock);
    self->decoder_verified = FALSE;
    g_free(self->state);
    self->state = g_strdup(error ? "error" : "ended");
    if (error) {
        self->failed = TRUE;
        g_free(self->error); self->error = g_strdup(error->message);
    }
    write_status(self, TRUE);
    g_mutex_unlock(&self->lock);
    if (error && self->pipeline)
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(self->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "admiral-html-video-error");
    g_clear_error(&error); g_free(debug);
}

static void
watch_pipeline(AdmiralSink *self)
{
    GstObject *object = gst_object_ref(self);
    GstObject *parent;
    while ((parent = gst_object_get_parent(object))) { gst_object_unref(object); object = parent; }
    if (!GST_IS_BIN(object)) { gst_object_unref(object); return; }
    self->pipeline = GST_ELEMENT(object);
    self->element_handler = g_signal_connect(object, "deep-element-added", G_CALLBACK(observe_element), self);
    self->bus = gst_element_get_bus(self->pipeline);
    if (self->bus) {
        gst_bus_enable_sync_message_emission(self->bus);
        self->bus_error_handler = g_signal_connect(self->bus, "sync-message::error", G_CALLBACK(pipeline_message), self);
        self->bus_eos_handler = g_signal_connect(self->bus, "sync-message::eos", G_CALLBACK(pipeline_message), self);
    }
    GstIterator *iterator = gst_bin_iterate_recurse(GST_BIN(object));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(iterator, &item)) {
        case GST_ITERATOR_OK:
            observe_element(GST_BIN(object), NULL, g_value_get_object(&item), self);
            g_value_reset(&item); break;
        case GST_ITERATOR_RESYNC: gst_iterator_resync(iterator); break;
        default: done = TRUE; break;
        }
    }
    g_value_unset(&item); gst_iterator_free(iterator);
}

static void
release_resources(AdmiralSink *self)
{
    if (self->bus) {
        g_signal_handler_disconnect(self->bus, self->bus_error_handler);
        g_signal_handler_disconnect(self->bus, self->bus_eos_handler);
        gst_bus_disable_sync_message_emission(self->bus);
        gst_object_unref(self->bus); self->bus = NULL;
    }
    if (self->pipeline) {
        if (self->element_handler) g_signal_handler_disconnect(self->pipeline, self->element_handler);
        self->element_handler = 0;
        gst_object_unref(self->pipeline); self->pipeline = NULL;
    }
    if (self->fd >= 0) { close(self->fd); self->fd = -1; }
    if (self->broker_socket >= 0) { close(self->broker_socket); self->broker_socket = -1; }
    if (self->lock_fd >= 0) { flock(self->lock_fd, LOCK_UN); close(self->lock_fd); self->lock_fd = -1; }
}

static gboolean
create_kms_child(AdmiralSink *self, gboolean browser_commits)
{
    self->kms = browser_commits ? g_object_new(admiral_plane_sink_get_type(), "name", "admiral-video-plane", NULL)
                                : gst_element_factory_make("kmssink", "admiral-video-plane");
    self->browser_commits = browser_commits && self->kms;
    if (!self->kms) return FALSE;
    self->filter = gst_element_factory_make("capsfilter", "admiral-video-caps");
    if (!self->filter) { gst_object_unref(self->kms); self->kms = NULL; return FALSE; }
    gst_bin_add_many(GST_BIN(self), self->filter, self->kms, NULL);
    if (!gst_element_link(self->filter, self->kms)) return FALSE;
    GstPad *pad = gst_element_get_static_pad(self->kms, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, sink_buffer, self, NULL);
    gst_object_unref(pad);
    pad = gst_element_get_static_pad(self->filter, "sink");
    if (self->ghost)
        gst_ghost_pad_set_target(GST_GHOST_PAD(self->ghost), pad);
    else {
        self->ghost = gst_ghost_pad_new("sink", pad);
        gst_element_add_pad(GST_ELEMENT(self), self->ghost);
    }
    gst_object_unref(pad);
    return TRUE;
}

static void
admiral_sink_handle_message(GstBin *bin, GstMessage *message)
{
    AdmiralSink *self = (AdmiralSink *)bin;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = NULL; gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        g_mutex_lock(&self->lock);
        self->failed = TRUE;
        g_free(self->error); self->error = g_strdup(error ? error->message : "Video pipeline failed");
        g_free(self->state); self->state = g_strdup("error");
        write_status(self, TRUE);
        g_mutex_unlock(&self->lock);
        if (self->pipeline)
            GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(self->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "admiral-html-video-error");
        g_clear_error(&error); g_free(debug);
    }
    GST_BIN_CLASS(admiral_sink_parent_class)->handle_message(bin, message);
}

static GstStateChangeReturn
admiral_sink_change_state(GstElement *element, GstStateChange transition)
{
    AdmiralSink *self = (AdmiralSink *)element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
        /* WebKit may reuse this sink after a failed URL or playback attempt.
         * A stopped kmssink retains the driver name derived from its old fd,
         * so retire it before releasing the broker fd and start a fresh child.
         */
        if (self->kms) {
            gst_element_set_state(self->kms, GST_STATE_NULL);
            gst_ghost_pad_set_target(GST_GHOST_PAD(self->ghost), NULL);
            gst_element_set_state(self->filter, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(self), self->filter);
            gst_bin_remove(GST_BIN(self), self->kms); self->kms = self->filter = NULL;
        }
        release_resources(self);
        g_mutex_lock(&self->lock);
        self->failed = self->blend_configured = FALSE;
        self->decoder_verified = self->dmabuf_seen = FALSE;
        self->decoded_frames = self->input_frames = 0;
        self->connector_id = self->crtc_id = self->browser_plane_id = self->video_plane_id = 0;
        self->decoded_width = self->decoded_height = 0;
        g_clear_pointer(&self->error, g_free);
        g_clear_pointer(&self->decoder, g_free);
        g_clear_pointer(&self->decoded_caps, g_free);
        g_free(self->state); self->state = g_strdup("created");
        write_status(self, TRUE);
        g_mutex_unlock(&self->lock);
        const gchar *allowed = g_getenv("BROWSER_DECODER");
        if (!allowed || !*allowed) allowed = DEFAULT_DECODERS;
        static const gchar *const known[] = { "v4l2slh264dec", "v4l2slh265dec", "v4l2slvp9dec", "avdec_h264", NULL };
        gchar **requested = g_strsplit(allowed, ",", -1);
        gboolean valid_list = *requested != NULL, software = FALSE, hardware = FALSE;
        for (gchar **item = requested; *item; item++) {
            g_strstrip(*item);
            gboolean is_known = FALSE;
            for (const gchar *const *k = known; *k; k++) is_known |= strcmp(*item, *k) == 0;
            valid_list &= is_known;
            if (strcmp(*item, "avdec_h264") == 0) software = TRUE; else hardware = TRUE;
        }
        g_strfreev(requested);
        if (!valid_list) {
            fail_sink(self, "BROWSER_DECODER must list v4l2slh264dec, v4l2slh265dec, v4l2slvp9dec or avdec_h264"); return GST_STATE_CHANGE_FAILURE;
        }
        if (software && g_strcmp0(g_getenv("BROWSER_ALLOW_SOFTWARE"), "1") != 0) {
            fail_sink(self, "avdec_h264 comparison requires BROWSER_ALLOW_SOFTWARE=1"); return GST_STATE_CHANGE_FAILURE;
        }
        self->lock_fd = open(RUN_DIR "/video.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (self->lock_fd < 0 || flock(self->lock_fd, LOCK_EX | LOCK_NB) < 0) {
            fail_sink(self, "Only one HTML video plane is supported"); release_resources(self); return GST_STATE_CHANGE_FAILURE;
        }
        /* Browser-committed frames need DMA-BUF decoder output; software decoding
         * and an explicit override keep kmssink. */
        gboolean want_browser_commits = !software && g_strcmp0(g_getenv("BROWSER_VIDEO_SINK"), "kmssink") != 0;
        if (!get_drm_fd(self, want_browser_commits)) {
            fail_sink(self, "DRM broker unavailable, browser CRTC inactive, or valid overlay/alpha configuration missing"); release_resources(self); return GST_STATE_CHANGE_FAILURE;
        }
        if (!create_kms_child(self, self->broker_socket >= 0)) {
            fail_sink(self, "Cannot create the video plane sink for a new video playback attempt"); release_resources(self); return GST_STATE_CHANGE_FAILURE;
        }
        if (hardware) {
            GstCaps *linear = gst_caps_from_string(LINEAR_NV12_CAPS);
            g_object_set(self->filter, "caps", linear, NULL);
            gst_caps_unref(linear);
        }
        if (!self->browser_commits && !g_object_class_find_property(G_OBJECT_GET_CLASS(self->kms), "fd")) {
            fail_sink(self, "kmssink with fd support (GStreamer >= 1.22) is required"); release_resources(self); return GST_STATE_CHANGE_FAILURE;
        }
        gboolean any_available = FALSE;
        gchar **names = g_strsplit(allowed, ",", -1);
        for (gchar **name = names; *name; name++) {
            GstElementFactory *selected = gst_element_factory_find(g_strstrip(*name));
            any_available |= selected != NULL;
            if (selected) gst_object_unref(selected);
        }
        g_strfreev(names);
        if (!any_available) {
            gchar *error = g_strdup_printf("None of %s is available: expose V4L2 video/media devices before GStreamer scans", allowed);
            fail_sink(self, error); g_free(error); release_resources(self); return GST_STATE_CHANGE_FAILURE;
        }
        /* Disable all other video decoder factories, including unexpected software fallbacks. */
        GList *decoders = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER |
                          GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_NONE);
        for (GList *l = decoders; l; l = l->next) {
            GstPluginFeature *feature = l->data;
            gst_plugin_feature_set_rank(feature, decoder_listed(allowed, gst_plugin_feature_get_name(feature)) ? 512 : GST_RANK_NONE);
        }
        gst_plugin_feature_list_free(decoders);
        if (self->browser_commits) {
            gint mode_width = self->mode_width > 0 ? self->mode_width : self->crtc_width;
            gint mode_height = self->mode_height > 0 ? self->mode_height : self->crtc_height;
            admiral_plane_sink_configure(self->kms, self->fd, self->broker_socket, mode_width, mode_height);
            g_message("Holepunch video frames are committed by the browser with the UI on plane %d", self->video_plane_id);
        } else {
            g_object_set(self->kms, "fd", self->fd, "connector-id", self->connector_id,
                         "plane-id", self->video_plane_id, "force-modesetting", FALSE,
                         "restore-crtc", FALSE, "sync", TRUE, "skip-vsync", TRUE, NULL);
            gint accepted_fd = -1;
            g_object_get(self->kms, "fd", &accepted_fd, NULL);
            if (accepted_fd != self->fd) {
                fail_sink(self, "kmssink rejected the shared DRM master fd; refusing to open another card");
                release_resources(self); return GST_STATE_CHANGE_FAILURE;
            }
            g_message("Holepunch video plane %d is committed by kmssink (separately from the UI)", self->video_plane_id);
        }
        /* Only the browser consumes DRM events on this shared open file.
         * BaseSink sync still schedules frames against the media clock. */
        g_mutex_lock(&self->lock);
        apply_render_rectangle(self);
        g_mutex_unlock(&self->lock);
        watch_pipeline(self);
    }
    GstStateChangeReturn result = GST_ELEMENT_CLASS(admiral_sink_parent_class)->change_state(element, transition);
    if (result == GST_STATE_CHANGE_FAILURE) {
        fail_sink(self, "kmssink state change failed; inspect GStreamer and DRM logs");
        release_resources(self); return result;
    }
    g_mutex_lock(&self->lock);
    if (!self->failed) {
        const gchar *name = NULL;
        if (transition == GST_STATE_CHANGE_NULL_TO_READY) name = "ready";
        else if (transition == GST_STATE_CHANGE_READY_TO_PAUSED || transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED) name = "paused";
        else if (transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING) name = "playing";
        else if (transition == GST_STATE_CHANGE_READY_TO_NULL) name = "stopped";
        if (name) { g_free(self->state); self->state = g_strdup(name); }
        if (transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED || transition == GST_STATE_CHANGE_READY_TO_NULL)
            self->decoder_verified = FALSE;
    }
    write_status(self, TRUE);
    g_mutex_unlock(&self->lock);
    if (transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING && self->pipeline)
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(self->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "admiral-html-video");
    if (transition == GST_STATE_CHANGE_READY_TO_NULL) {
        release_resources(self);
    }
    return result;
}

static void
admiral_sink_set_property(GObject *object, guint property, const GValue *value, GParamSpec *spec)
{
    AdmiralSink *self = (AdmiralSink *)object;
    if (property == PROP_RECTANGLE) {
        const gchar *text = g_value_get_string(value);
        gint x, y, width, height; gchar tail;
        if (!text || sscanf(text, "%d,%d,%d,%d%c", &x, &y, &width, &height, &tail) != 4 || width < 0 || height < 0) {
            fail_sink(self, "Invalid WPE video rectangle"); return;
        }
        g_mutex_lock(&self->lock);
        self->x = x; self->y = y; self->width = width; self->height = height;
        if (width > 0 && height > 0)
            apply_render_rectangle(self);
        write_status(self, FALSE);
        g_mutex_unlock(&self->lock);
    } else if (property == PROP_ZORDER) self->zorder = g_value_get_float(value);
    else if (property == PROP_RES_USAGE) self->res_usage = g_value_get_uint(value);
    else if (property == PROP_IMMEDIATE_OUTPUT) self->immediate_output = g_value_get_boolean(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
}

static void
admiral_sink_get_property(GObject *object, guint property, GValue *value, GParamSpec *spec)
{
    AdmiralSink *self = (AdmiralSink *)object;
    if (property == PROP_RECTANGLE) {
        g_mutex_lock(&self->lock);
        g_value_take_string(value, g_strdup_printf("%d,%d,%d,%d", self->x, self->y, self->width, self->height));
        g_mutex_unlock(&self->lock);
    } else if (property == PROP_ZORDER) g_value_set_float(value, self->zorder);
    else if (property == PROP_RES_USAGE) g_value_set_uint(value, self->res_usage);
    else if (property == PROP_IMMEDIATE_OUTPUT) g_value_set_boolean(value, self->immediate_output);
    else if (property == PROP_STATS) {
        GstStructure *stats = NULL;
        if (self->kms) g_object_get(self->kms, "stats", &stats, NULL);
        g_value_take_boxed(value, stats);
    }
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
}

static void
admiral_sink_finalize(GObject *object)
{
    AdmiralSink *self = (AdmiralSink *)object;
    release_resources(self);
    g_free(self->decoder); g_free(self->decoded_caps); g_free(self->error); g_free(self->state);
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(admiral_sink_parent_class)->finalize(object);
}

static void
admiral_sink_class_init(AdmiralSinkClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = admiral_sink_set_property; object->get_property = admiral_sink_get_property;
    object->finalize = admiral_sink_finalize;
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GST_BIN_CLASS(klass)->handle_message = admiral_sink_handle_message;
    element->change_state = admiral_sink_change_state;
    gst_element_class_set_static_metadata(element, "Admiral WPE holepunch KMS adapter", "Sink/Video",
        "Experimental Westeros ABI adapter to a shared DRM kmssink", "Admiral");
    gst_element_class_add_static_pad_template(element, &sink_template);
    g_object_class_install_property(object, PROP_RECTANGLE, g_param_spec_string("rectangle", "Rectangle",
        "WPE holepunch x,y,width,height in web-view (UI) pixels; scaled to display pixels for the video plane", "0,0,1,1", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_ZORDER, g_param_spec_float("zorder", "Z order",
        "Compatibility property; validated plane order is set by the browser broker", -G_MAXFLOAT, G_MAXFLOAT, 0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_RES_USAGE, g_param_spec_uint("res-usage", "Resource usage",
        "Compatibility property; one video is supported", 0, G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_IMMEDIATE_OUTPUT, g_param_spec_boolean("immediate-output", "Immediate output",
        "Compatibility property; output remains clock synchronized", FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_STATS, g_param_spec_boxed("stats", "Statistics",
        "Underlying kmssink rendered/dropped frame statistics for WebKit playback quality",
        GST_TYPE_STRUCTURE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void
admiral_sink_init(AdmiralSink *self)
{
    g_mutex_init(&self->lock);
    self->fd = self->lock_fd = -1;
    self->width = self->height = 1;
    self->state = g_strdup("created");
    GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_SINK);
    self->broker_socket = -1;
    /* A placeholder until NULL->READY knows which sink the broker supports. */
    create_kms_child(self, FALSE);
}

static gboolean
plugin_init(GstPlugin *plugin)
{
    /* WPE 2.48 hardcodes this factory name in its Westeros holepunch quirk. */
    return gst_element_register(plugin, "westerossink", GST_RANK_NONE, admiral_sink_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, admiralholepunch,
    "Experimental Admiral WPE holepunch adapter", plugin_init, "0.1.0", "MIT", PACKAGE,
    "https://github.com/admrlos/workload-embedded-browser")
