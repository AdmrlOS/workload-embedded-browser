/* SPDX-License-Identifier: MIT */
#pragma once
#include <glib.h>
#include <wpe/wpe.h>

typedef struct EmbeddedBrowserInput EmbeddedBrowserInput;
EmbeddedBrowserInput *embedded_browser_input_start(struct wpe_view_backend *backend, guint width, guint height);
void embedded_browser_input_stop(EmbeddedBrowserInput *input);
