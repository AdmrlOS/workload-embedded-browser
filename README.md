# Admiral embedded browser workload

A reusable ARM64 OCI base image for fullscreen browser applications on embedded
devices. It extracts the hardware-aware browser runtime from Admiral Signage so
signage, dashboards and other kiosk apps can share the same graphics/video stack.
The initial hardware target is Admiral's Allwinner T527 / Orange Pi 4A BSP.

```dockerfile
FROM ghcr.io/admrlos/workload-embedded-browser:<published-version>
COPY www/ /opt/my-app/www/
ENV BROWSER_HTTP_ROOT=/opt/my-app/www
```

Keep the inherited entrypoint and command: the image starts the app server and
fullscreen browser. For a remote dashboard, the base image needs only
`BROWSER_URL=https://your-dashboard/`; it starts no HTTP server in that mode.
Without a URL or HTTP root, it displays a small bundled welcome page.

## What the base owns

- WPE WebKit / WPEBackend-fdo with WebGL, fullscreen output and autoplay.
- Atomic DRM/KMS presentation, direct DMA-BUF scanout and the T527 GPU-copy
  fallback into contiguous linear ARGB buffers, including explicit fence sync.
- UI-size scaling independently of the physical display mode.
- The shared DRM broker and GStreamer video-plane adapter: HTML video can use
  the stateless H.264, HEVC and VP9 decoders and a separate NV12 plane beneath
  the alpha-preserving browser plane. UI and video share atomic commits.
- WebKit's enabled bubblewrap sandbox, extended to expose dynamically numbered
  V4L2/media nodes and the private DRM broker socket.
- Native keyboard/pointer input and initial private udev input metadata.
- Static init, writable runtime/cache setup, app readiness, signal handling,
  required-service supervision and hardware diagnostics.

Applications own their pages, APIs, authentication, content, media, schedules and
state. The base does not include the Signage Studio, its library or its demos.
See [NOTICE.md](NOTICE.md) for the exact extraction source and retained license.

The image supplies userspace. Admiral supplies boot, VT handoff, the kernel,
device tree, firmware and GPU/video/display drivers. It draws directly through
DRM/KMS and does not need Cog, Weston, X11 or a desktop session.

## Build and validate

```sh
docker buildx build --platform linux/arm64 --load \
  -t workload-embedded-browser:dev .
python3 -m unittest discover -s tests -v
python3 tests/check_image_startup.py workload-embedded-browser:dev
docker run --rm --workdir / --tmpfs /run --tmpfs /tmp \
  workload-embedded-browser:dev smoke
```

The default/final `workload` target contains runtime packages and compiled
binaries. Compilers and native sources stay in the build stage. For on-device
development, use the `development` target, which also includes the sources,
headers, debugger, tracing tools, FFmpeg and `embedded-browser-rebuild`:

```sh
docker buildx build --platform linux/arm64 --target development --load \
  -t workload-embedded-browser:development .
```

Exact installed versions are recorded at
`/opt/embedded-browser/package-versions.txt`. The Debian base manifest is pinned;
Debian package updates can still change installed versions between builds.
Export an OCI archive if needed:

```sh
mkdir -p output
docker buildx build --platform linux/arm64 \
  -t workload-embedded-browser:dev \
  --output type=oci,dest=output/embedded-browser-arm64.oci.tar .
```

## Build downstream applications

### Static dashboard

The included example inherits the complete browser launch contract:

```sh
docker build --builder desktop-linux -t embedded-dashboard:dev examples/dashboard
```

`--builder desktop-linux` selects Docker Desktop's local image store on this
development machine. On Linux with the default Docker context, use
`--builder default`. A separate container-based Buildx builder needs a published
base image instead of a tag that only exists in Docker's local image store.
For CI/remote builds, set `BROWSER_IMAGE` to a published tag or digest:

```sh
docker build --build-arg BROWSER_IMAGE=ghcr.io/admrlos/workload-embedded-browser:<published-version> \
  -t my-dashboard:1.0 examples/dashboard
```

The static server binds loopback by default. Set `BROWSER_HTTP_HOST=0.0.0.0` and
expose the application port only if other machines need access to the site.
`/healthz` is reserved for server readiness. Use an application server for APIs,
range-based video serving, or more advanced HTTP behavior.

### Application with its own backend

Supply a JSON argument array for the executable to supervise, with an optional
HTTP readiness URL. No shell interprets the arguments.

```dockerfile
FROM ghcr.io/admrlos/workload-embedded-browser:<published-version>
COPY app/ /opt/my-app/
ENV BROWSER_APP_COMMAND='["/usr/bin/python3","/opt/my-app/server.py"]' \
    BROWSER_APP_READY_URL=http://127.0.0.1:8080/healthz \
    BROWSER_URL=http://127.0.0.1:8080/dashboard/
```

Leave `BROWSER_HTTP_ROOT` unset when the application serves its own pages. Any
required service exiting, including with exit code zero, stops the workload with
a failure status so Admiral can restart it. Startup readiness times out and stops
all services. SIGTERM stops each service process group, allows five seconds for
cleanup and kills remaining processes. The static Tini init reaps orphaned children.

### Existing Admiral Signage

The original signage repository can consume this image without compiling its own
browser or video adapter. The included downstream Dockerfile uses the signage
repository as its main context and this repository as a named build context:

```sh
# Run from this repository, with the signage repository alongside it.
docker build --builder desktop-linux \
  --build-context browser-source=. \
  -f examples/signage/Dockerfile \
  -t admiral-signage:embedded-browser \
  ../workload-admiral-signage
```

The example copies only the signage app, pages and media from that repository;
FFmpeg remains an application dependency for upload thumbnails. The small
adapter selects the existing player/studio pages, launches the original server
and translates shared `SIGNAGE_*` settings into `BROWSER_*` settings. Explicit
`BROWSER_*` values take precedence. Existing `/data` libraries and schedules are
retained. A runtime symlink keeps the old application's hardcoded video-status
path working; the base itself uses `/run/embedded-browser` exclusively.

For a server-only preview:

```sh
docker run --rm -p 8097:8080 -v signage-preview:/data \
  admiral-signage:embedded-browser server
```

Open `http://localhost:8097/studio/`. This preview does not verify board video.
The source signage checkout is not modified by this example. Once adopted,
browser and video fixes belong here and downstream signage releases should pin
the base image version/digest instead of carrying duplicate native sources.

## Run on Admiral

Use [config/admiral.example.json](config/admiral.example.json) as the runtime
configuration shape, and replace the image placeholder with a published version
or digest. The image is intended to launch automatically as a running workload.

- Enable Interactive Mode (`options.interactive: true`) for screen/VT ownership.
- Disable Device Isolation (`options.isolateDevices: false`) for DRM, V4L2,
  input and audio devices through Admiral's normal device path.
- Leave Command empty to use the image's init and default `run` operation.
- Mount a persistent Custom Volume at `/data` for caches and diagnostic output.
- A remote dashboard or loopback app needs no inbound port. Add the app's port
  when intentionally exposing its server.

Admiral starts in `/` and mounts fresh private `/run` and `/tmp` filesystems. All
launcher paths are absolute and runtime directories are recreated on startup.
The workload does not hardcode a VT number. Admiral owns display handoff and
restart behavior; Docker HEALTHCHECK is not an Admiral liveness probe.

Use a fresh immutable image tag or digest for each rollout; Admiral may reuse an
already cached version tag. See Admiral's [launch contract and configuration
reference](https://docs.admrl.co/core-concepts/configurations) and
[hardware access guide](https://docs.admrl.co/core-concepts/hardware-access).

On a dedicated Linux development board, `docker compose up --build` provides
the display/device access needed for local testing. The Compose file grants
privileged device access and requires sole ownership of the display.

## Runtime controls

| Variable | Default | Meaning |
| --- | --- | --- |
| `BROWSER_URL` | Bundled `file://` welcome page, or local server URL when HTTP root is set | Startup URL; also settable with `run URL`. |
| `BROWSER_HTTP_ROOT` | Unset | Existing absolute directory to serve alongside the browser. |
| `BROWSER_HTTP_HOST` / `BROWSER_HTTP_PORT` | `127.0.0.1` / `8080` | Optional static server's bind address and port. |
| `BROWSER_APP_COMMAND` | Unset | JSON argument array for a required backend; executable must be absolute. |
| `BROWSER_APP_READY_URL` | Unset | Wait for a successful HTTP response before starting the browser. Requires an app command. |
| `BROWSER_START_TIMEOUT` | `30` seconds | Readiness timeout per server (signage adapter defaults to 90). |
| `BROWSER_DATA_DIR` | `/data` | Persistent cache and log directory. |
| `BROWSER_VIDEO_MODE` | `holepunch` | Native video plane, or `composited` for ordinary WebKit composition. |
| `BROWSER_DECODER` | `v4l2slh264dec,v4l2slh265dec,v4l2slvp9dec` | Allowed decoder list; software mode defaults to `avdec_h264`. |
| `BROWSER_ALLOW_SOFTWARE` | `0` | Set `1` to allow `avdec_h264` for comparison. |
| `BROWSER_DRM_DEVICE` | Auto-detect | Connected display's DRM card. |
| `BROWSER_RENDER_DEVICE` | Display GBM/kmsro | Optional separate GPU render node. |
| `BROWSER_DRM_MODE` | Preferred mode | `WIDTHxHEIGHT[@REFRESH]`. |
| `BROWSER_UI_SIZE` | Mode size, capped to an aspect-preserving 1920×1080 | Browser framebuffer size; KMS scales to the physical mode, with TEST_ONLY fallback. |
| `BROWSER_DRM_CONNECTOR` | Auto-detect | Connector name or ID. |
| `BROWSER_DRM_PRIMARY_PLANE` | Auto-detect | ARGB8888 browser plane ID. |
| `BROWSER_VIDEO_PLANE_ID` | Auto-detect | Compatible NV12 overlay plane ID. |
| `BROWSER_VIDEO_SINK` | Browser commits video | Set `kmssink` for separate-commit comparison. |
| `BROWSER_FRAME_CALLBACK` | On commit | Set `flip` to release the frame callback after the page flip. |
| `BROWSER_INPUT` | Enabled | Set `0` to disable native input and metadata preparation. |
| `BROWSER_INPUT_SEAT` / `BROWSER_KEYBOARD_LAYOUT` | `seat0` / system US | Input seat and XKB layout. |
| `BROWSER_STATUS` | `/run/embedded-browser/browser-status.json` | Native browser JSON output. |
| `WEBKIT_INSPECTOR_SERVER` | Unset | Opt-in remote inspector listen address. |
| `GST_DEBUG` | `2` | GStreamer logging categories/levels. |
| `XDG_CACHE_HOME` | `$BROWSER_DATA_DIR/cache` | Writable caches; falls back to `/tmp/embedded-browser-cache`. |
| `XDG_DATA_HOME` / `XDG_CONFIG_HOME` | `$BROWSER_DATA_DIR/browser-data` / `browser-config` | Durable browser data/configuration directories. |
| `GST_REGISTRY` | Device/kernel-keyed registry in the cache | Avoids stale decoder discovery after BSP or node changes. |

The broker socket and video evidence use the fixed
`/run/embedded-browser/video` directory shared with WebKit's sandbox.
`XDG_RUNTIME_DIR` defaults to `/run/embedded-browser`; keep that value for normal
operation so browser and diagnostic status files are collected together.

Operations: `run [URL]`, `server` (static preview only), `doctor [--quick]`,
`smoke`, `shell`, or an explicit command. Inspection with `doctor` preserves
live native status. Run `smoke` in a fresh standalone container.

## Hardware acceptance

Image builds and display-free tests validate linking, plugins, process handling
and configuration. They cannot establish T527 GPU acceleration, decoded
DMA-BUF compatibility, plane blending, audio output or frame timing.

On the target BSP, verify fullscreen boot, keyboard/pointer input, WebGL renderer,
the direct/GPU-copy path, UI scaling at 1080p/4K, H.264/HEVC/VP9 media supported by
the board, video beneath transparent page content, app failure/restart and reboot.
Native evidence is in `browser-status.json`, `drm-status.json` and
`video/video-status.json` under `/run/embedded-browser`. Collect
`embedded-browser-doctor` output alongside visible playback checks.

The extracted native adapter supports one active video sink. Touchscreen input,
multiple simultaneous video planes, arbitrary rotated/transformed video, HDR and
custom crop effects retain the original implementation's limitations. A remote
page load failure retains WebKit's normal error handling; it does not currently
have a browser-level automatic retry policy.

## Publishing

The GitHub workflow validates ARM64 builds and publishes the runtime to
`ghcr.io/<repository-owner>/workload-embedded-browser` for pushes to `main` and
version tags. Pull requests build/test without publishing. It emits commit SHA,
semantic version and main's development `latest` tags with SBOM/provenance.
The development target is built locally on demand. No image has been published
merely by creating this repository; configure the new GitHub repository/package
and use its published version or digest in downstream applications.
