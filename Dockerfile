# syntax=docker/dockerfile:1.7
ARG DEBIAN_IMAGE=debian:trixie-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
FROM ${DEBIAN_IMAGE} AS runtime-deps
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates python3 dbus dbus-x11 tini udev procps \
    libwpewebkit-2.0-1 libwpebackend-fdo-1.0-1 libwpe-1.0-1 bubblewrap \
    libdrm2 libgbm1 libegl1 libgles2 libepoxy0 libinput10 libudev1 \
    libwayland-server0 libxkbcommon0 libjson-glib-1.0-0 \
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
    gstreamer1.0-gl gstreamer1.0-alsa libgl1-mesa-dri libegl-mesa0 \
    mesa-utils drm-info libdrm-tests v4l-utils fonts-dejavu-core fonts-dejavu-extra \
    && rm -rf /var/lib/apt/lists/*

FROM runtime-deps AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config libwpewebkit-2.0-dev libwpebackend-fdo-1.0-dev libwpe-1.0-dev \
    libdrm-dev libgbm-dev libegl-dev libgles-dev libepoxy-dev libinput-dev libudev-dev \
    libwayland-dev libxkbcommon-dev libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev libjson-glib-dev \
    && rm -rf /var/lib/apt/lists/*
COPY native/ /opt/embedded-browser/src/native/
COPY browser/ /opt/embedded-browser/src/browser/
RUN make -C /opt/embedded-browser/src/native && make -C /opt/embedded-browser/src/native install && \
    make -C /opt/embedded-browser/src/browser && make -C /opt/embedded-browser/src/browser install

FROM runtime-deps AS runtime
ARG VERSION=dev
ARG REVISION=unknown
LABEL org.opencontainers.image.title="Admiral Embedded Browser" \
      org.opencontainers.image.description="Reusable WPE WebKit DRM/KMS browser with T527 GPU-copy scanout and native V4L2 video planes" \
      org.opencontainers.image.source="https://github.com/AdmrlOS/workload-embedded-browser" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.revision="${REVISION}" \
      org.opencontainers.image.licenses="MIT"
COPY --from=build /usr/local/bin/embedded-browser /usr/local/bin/embedded-browser
COPY --from=build /usr/local/lib/gstreamer-1.0/ /usr/local/lib/gstreamer-1.0/
COPY --from=build /usr/local/libexec/embedded-browser/ /usr/local/libexec/embedded-browser/
# Static PID 1 works independently of a distribution-specific ELF loader.
RUN install -m 0755 /usr/bin/tini-static /embedded-browser-init && \
    mkdir -p /tmp/init-check && cp /embedded-browser-init /tmp/init-check/embedded-browser-init && \
    chroot /tmp/init-check /embedded-browser-init --version && rm -rf /tmp/init-check && \
    test -x /usr/bin/bwrap && \
    dpkg-divert --local --rename --divert /usr/bin/bwrap.real --add /usr/bin/bwrap && \
    install -m 0755 /usr/local/libexec/embedded-browser/embedded-browser-bwrap /usr/bin/bwrap && ldconfig
COPY app/ /opt/embedded-browser/app/
COPY www/ /opt/embedded-browser/www/
COPY LICENSE NOTICE.md /opt/embedded-browser/
COPY --chmod=0755 bin/embedded-browser-entrypoint bin/embedded-browser-doctor bin/embedded-browser-smoke /usr/local/bin/
RUN mkdir -p /data && dpkg-query -W > /opt/embedded-browser/package-versions.txt
ENV PYTHONUNBUFFERED=1 \
    GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 \
    XDG_RUNTIME_DIR=/run/embedded-browser \
    BROWSER_HTTP_HOST=127.0.0.1 \
    BROWSER_HTTP_PORT=8080 \
    GST_DEBUG_NO_COLOR=1 \
    GST_DEBUG=2
WORKDIR /
STOPSIGNAL SIGTERM
ENTRYPOINT ["/embedded-browser-init", "-g", "--", "/usr/local/bin/embedded-browser-entrypoint"]
CMD ["run"]

FROM build AS development
COPY --from=runtime /embedded-browser-init /embedded-browser-init
COPY --from=runtime /opt/embedded-browser/app/ /opt/embedded-browser/app/
COPY --from=runtime /opt/embedded-browser/www/ /opt/embedded-browser/www/
COPY --from=runtime /opt/embedded-browser/LICENSE /opt/embedded-browser/NOTICE.md /opt/embedded-browser/
COPY --from=runtime /usr/bin/bwrap /usr/bin/bwrap.real /usr/bin/
COPY --chmod=0755 bin/ /usr/local/bin/
RUN apt-get update && apt-get install -y --no-install-recommends gdb strace ffmpeg git \
    && rm -rf /var/lib/apt/lists/* && mkdir -p /data && ldconfig && \
    dpkg-divert --local --no-rename --divert /usr/bin/bwrap.real --add /usr/bin/bwrap && \
    dpkg-query -W > /opt/embedded-browser/package-versions.txt
ENV PYTHONUNBUFFERED=1 \
    GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0 \
    XDG_RUNTIME_DIR=/run/embedded-browser \
    BROWSER_HTTP_HOST=127.0.0.1 \
    BROWSER_HTTP_PORT=8080 \
    GST_DEBUG_NO_COLOR=1 \
    GST_DEBUG="2,v4l2codecs:6,admiral*:6" \
    G_MESSAGES_DEBUG=all
WORKDIR /
STOPSIGNAL SIGTERM
ENTRYPOINT ["/embedded-browser-init", "-g", "--", "/usr/local/bin/embedded-browser-entrypoint"]
CMD ["run"]

# A plain `docker build .` always produces the reusable runtime.
FROM runtime AS workload
