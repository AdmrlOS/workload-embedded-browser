# syntax=docker/dockerfile:1.7
# Use the signage repository as the main context and this repository as browser-source.
ARG BROWSER_IMAGE=workload-embedded-browser:dev
FROM ${BROWSER_IMAGE}
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*
COPY app/ /opt/signage/app/
COPY www/ /opt/signage/www/
COPY media/ /opt/signage/media/
COPY --from=browser-source /examples/signage/entrypoint.py /opt/signage/browser-entrypoint.py
RUN ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 \
    -t 12 -an -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
    -preset veryfast -crf 23 -g 60 -movflags +faststart /opt/signage/media/test-h264.mp4
ENV SIGNAGE_DATA_DIR=/data \
    SIGNAGE_PAGE=player \
    SIGNAGE_HTTP_HOST=0.0.0.0 \
    SIGNAGE_HTTP_PORT=8080
LABEL org.opencontainers.image.title="Admiral Signage" \
      org.opencontainers.image.source="https://github.com/AdmrlOS/workload-admiral-signage"
EXPOSE 8080
ENTRYPOINT ["/embedded-browser-init", "-g", "--", "/usr/bin/python3", "/opt/signage/browser-entrypoint.py"]
CMD ["run"]
