/* SPDX-License-Identifier: MIT
 *
 * bubblewrap shim for the WebKit sandbox.
 *
 * WebKit 2.48 starts every sandboxed subprocess as
 *   /usr/bin/bwrap --args <fd> -- <command...>
 * and only device-binds a fixed V4L2 set (/dev/v4l, /dev/video0-2,
 * /dev/media0). webkit_web_context_add_path_to_sandbox() cannot widen it: it
 * rejects every path under /dev, and its --bind-try mounts are nodev, so a
 * bound character device cannot be opened anyway.
 *
 * GStreamer v4l2codecs finds stateless decoders through the media controller
 * (/dev/mediaN) and then opens the video node it links to. Kernel numbering
 * depends on probe order (a USB webcam can take media0), so the decoder is
 * often outside WebKit's fixed list. The Dockerfile diverts the real bwrap to
 * /usr/bin/bwrap.real and installs this program as /usr/bin/bwrap. It adds
 * --dev-bind-try for every V4L2 and media-controller node the kernel exposes
 * right now, after the options WebKit passed (so after its "--dev /dev"), and
 * then executes the real bwrap. Nothing else about the sandbox changes.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BROWSER_REAL_BWRAP
#define BROWSER_REAL_BWRAP "/usr/bin/bwrap.real"
#endif

#define MAX_NODES 128

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Collect /dev/<name> for each entry of a sysfs class/bus directory whose
 * name starts with one of the prefixes and is a character device. */
static size_t collect(const char *sysfs_dir, const char *const *prefixes, char **nodes, size_t count)
{
    DIR *dir = opendir(sysfs_dir);
    if (!dir) return count;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < MAX_NODES) {
        int wanted = 0;
        for (size_t i = 0; prefixes[i]; i++)
            if (strncmp(entry->d_name, prefixes[i], strlen(prefixes[i])) == 0) wanted = 1;
        if (!wanted || strchr(entry->d_name, '/')) continue;
        char *path = NULL;
        if (asprintf(&path, "/dev/%s", entry->d_name) < 0) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISCHR(st.st_mode)) nodes[count++] = path;
        else free(path);
    }
    closedir(dir);
    return count;
}

int main(int argc, char **argv)
{
    (void)argc;
    static const char *const v4l_prefixes[] = { "video", "v4l-subdev", "radio", "swradio", "vbi", NULL };
    static const char *const media_prefixes[] = { "media", NULL };
    char *nodes[MAX_NODES];
    size_t count = 0;

    /* Only extend WebKit's own invocation shape; pass anything else through. */
    int webkit_shape = argc >= 4 && strcmp(argv[1], "--args") == 0;
    if (webkit_shape && !getenv("BROWSER_BWRAP_NO_V4L2")) {
        count = collect("/sys/bus/media/devices", media_prefixes, nodes, count);
        count = collect("/sys/class/video4linux", v4l_prefixes, nodes, count);
        qsort(nodes, count, sizeof(nodes[0]), compare_names);
    }

    char **args = calloc((size_t)argc + count * 3 + 1, sizeof(char *));
    if (!args) { perror("embedded-browser-bwrap: calloc"); return 127; }
    size_t n = 0;
    args[n++] = (char *)BROWSER_REAL_BWRAP;
    int i = 1;
    if (webkit_shape) {
        args[n++] = argv[i++];
        args[n++] = argv[i++];
        for (size_t k = 0; k < count; k++) {
            args[n++] = (char *)"--dev-bind-try";
            args[n++] = nodes[k];
            args[n++] = nodes[k];
        }
        if (count) {
            fprintf(stderr, "embedded-browser-bwrap: exposing V4L2 nodes to the WebKit sandbox:");
            for (size_t k = 0; k < count; k++) fprintf(stderr, " %s", nodes[k]);
            fputc('\n', stderr);
        }
    }
    for (; i < argc; i++) args[n++] = argv[i];
    args[n] = NULL;
    execv(BROWSER_REAL_BWRAP, args);
    fprintf(stderr, "embedded-browser-bwrap: cannot execute %s: %s\n", BROWSER_REAL_BWRAP, strerror(errno));
    return 127;
}
