# Source provenance

The browser shell, input handling, DRM broker, GStreamer video adapter,
bubblewrap shim, input metadata preparation and device diagnostics were
extracted from [AdmrlOS/workload-admiral-signage](https://github.com/AdmrlOS/workload-admiral-signage)
at commit `1f632e6ac452bdec716f0efa83655065eed22c57` (main).

The original MIT license is retained in LICENSE. Public settings and native
identifiers have been renamed for this reusable browser image. The native
presentation and decoder algorithms are preserved. The launcher and optional
static server are application-independent; the Signage Studio, schedule,
content APIs, demos and media are supplied by downstream applications.
