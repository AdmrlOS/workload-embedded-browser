# syntax=docker/dockerfile:1.7
ARG BROWSER_IMAGE=workload-embedded-browser:dev
FROM ${BROWSER_IMAGE}
COPY www/ /opt/dashboard/www/
ENV BROWSER_HTTP_ROOT=/opt/dashboard/www
# Inherit the browser init, launcher and default run command.
