/* SPDX-License-Identifier: MIT */
#pragma once

#include <gst/gst.h>

/* A video sink that imports decoded DMA-BUF frames as framebuffers on the
 * browser's brokered DRM descriptor and asks the browser to put them on the
 * overlay plane in its own atomic commits (see drm-broker.h). */
GType admiral_plane_sink_get_type(void);

/* Borrowed: drm_fd and socket_fd stay owned by the caller and must outlive the
 * sink's PAUSED->READY transition. mode_* is the CRTC size used for clipping. */
void admiral_plane_sink_configure(GstElement *sink, gint drm_fd, gint socket_fd,
                                  gint mode_width, gint mode_height);
