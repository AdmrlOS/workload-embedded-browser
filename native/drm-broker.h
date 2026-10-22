/* SPDX-License-Identifier: MIT */
#pragma once

#include <glib.h>
#include <stdint.h>

/* Broker socket and video-status.json; shared with the sandboxed web process. */
#define BROWSER_VIDEO_RUNTIME_DIR "/run/embedded-browser/video"

/* Unset BROWSER_VIDEO_MODE defaults to the strict holepunch path. */
gboolean embedded_browser_drm_holepunch_enabled(void);

/* ui_* is the browser framebuffer size; mode_* is the CRTC size the primary plane
 * scales it to. WebKit video rectangles are in UI coordinates. */
/* The descriptor remains the browser's descriptor; no new DRM device is opened. */
gboolean embedded_browser_drm_broker_start(int drm_fd,
                                  guint connector_id,
                                  guint crtc_id,
                                  guint crtc_index,
                                  guint primary_plane_id,
                                  guint ui_width,
                                  guint ui_height,
                                  guint mode_width,
                                  guint mode_height,
                                  GError **error);
void embedded_browser_drm_broker_stop(void);

/* Video frame protocol. After the descriptor transfer the sink keeps the broker
 * connection open and sends SHOW (a framebuffer it created on the shared
 * descriptor, with plane source and CRTC rectangles) or HIDE. The browser puts
 * each SHOW into its next atomic commit together with the UI, so the CRTC sees
 * one commit per vblank, and answers RELEASED once a framebuffer is no longer
 * scanned out or queued. Both ends run from the same image build, so the
 * message is a fixed native-endian struct. The broker metadata advertises the
 * protocol with "video_frames":true; without it the sink commits by itself. */
enum {
    BROWSER_VIDEO_SHOW = 1,
    BROWSER_VIDEO_HIDE = 2,
    BROWSER_VIDEO_RELEASED = 3,
};

typedef struct {
    uint32_t type;
    uint32_t fb;
    /* SHOW: increments per message. RELEASED: the latest serial the browser had
     * for fb; the sink ignores it if it has since shown fb again. */
    uint32_t serial;
    uint32_t src_x, src_y, src_w, src_h; /* 16.16 fixed point, as KMS SRC_* */
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
} EmbeddedBrowserVideoMessage;

/* message is NULL when the sink disconnects; the browser then hides the video. */
typedef void (*EmbeddedBrowserVideoHandler)(const EmbeddedBrowserVideoMessage *message, gpointer data);

/* Overlay plane chosen and configured by embedded_browser_drm_broker_start(), or 0. */
guint embedded_browser_drm_broker_video_plane(void);
/* Enables the frame protocol for sinks connecting from now on (NULL disables). */
void embedded_browser_drm_broker_set_video_handler(EmbeddedBrowserVideoHandler handler, gpointer data);
gboolean embedded_browser_drm_broker_video_handler_active(void);
/* Tells the connected sink that fb is no longer on screen or queued. */
void embedded_browser_drm_broker_video_released(uint32_t fb, uint32_t serial);
