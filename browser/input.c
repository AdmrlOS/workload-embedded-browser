/* SPDX-License-Identifier: MIT
 * Native input forwarding for the Admiral display development shell.
 */
#define _GNU_SOURCE
#include "input.h"
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

struct EmbeddedBrowserInput {
    struct wpe_view_backend *backend;
    struct libinput *libinput;
    struct xkb_context *xkb_context;
    struct xkb_keymap *keymap;
    struct xkb_state *key_state;
    guint source;
    guint width, height;
    double x, y;
    uint32_t buttons;
    guint devices;
};

static int open_input(const char *path, int flags, void *data)
{
    (void)data;
    int fd = open(path, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

static void close_input(int fd, void *data)
{
    (void)data;
    close(fd);
}

static uint32_t modifiers(EmbeddedBrowserInput *input)
{
    uint32_t result = input->buttons;
    if (!input->key_state)
        return result;
    if (xkb_state_mod_name_is_active(input->key_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        result |= wpe_input_keyboard_modifier_control;
    if (xkb_state_mod_name_is_active(input->key_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        result |= wpe_input_keyboard_modifier_shift;
    if (xkb_state_mod_name_is_active(input->key_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        result |= wpe_input_keyboard_modifier_alt;
    if (xkb_state_mod_name_is_active(input->key_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        result |= wpe_input_keyboard_modifier_meta;
    return result;
}

static void pointer_event(EmbeddedBrowserInput *input, struct libinput_event_pointer *event,
                          enum wpe_input_pointer_event_type type, uint32_t button, uint32_t state)
{
    input->x = CLAMP(input->x, 0, (double)input->width - 1);
    input->y = CLAMP(input->y, 0, (double)input->height - 1);
    struct wpe_input_pointer_event translated = {
        .type = type, .time = libinput_event_pointer_get_time(event),
        .x = (int)input->x, .y = (int)input->y, .button = button, .state = state,
        .modifiers = modifiers(input)
    };
    wpe_view_backend_dispatch_pointer_event(input->backend, &translated);
}

static void dispatch_events(EmbeddedBrowserInput *input)
{
    libinput_dispatch(input->libinput);
    struct libinput_event *event;
    while ((event = libinput_get_event(input->libinput))) {
        enum libinput_event_type type = libinput_event_get_type(event);
        if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
            input->devices++;
            g_message("Native input device: %s", libinput_device_get_name(libinput_event_get_device(event)));
        } else if (type == LIBINPUT_EVENT_DEVICE_REMOVED) {
            if (input->devices) input->devices--;
        } else if (type == LIBINPUT_EVENT_POINTER_MOTION) {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            input->x += libinput_event_pointer_get_dx(pointer);
            input->y += libinput_event_pointer_get_dy(pointer);
            pointer_event(input, pointer, wpe_input_pointer_event_type_motion, 0, 0);
        } else if (type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            input->x = libinput_event_pointer_get_absolute_x_transformed(pointer, input->width);
            input->y = libinput_event_pointer_get_absolute_y_transformed(pointer, input->height);
            pointer_event(input, pointer, wpe_input_pointer_event_type_motion, 0, 0);
        } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            uint32_t native = libinput_event_pointer_get_button(pointer);
            uint32_t button = native == BTN_LEFT ? 1 : native == BTN_MIDDLE ? 2 : native == BTN_RIGHT ? 3 : 0;
            uint32_t pressed = libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED;
            if (button) {
                uint32_t mask = wpe_input_pointer_modifier_button1 << (button - 1);
                if (pressed) input->buttons |= mask;
                else input->buttons &= ~mask;
                /* WPE's button field uses Linux evdev BTN_* codes. */
                pointer_event(input, pointer, wpe_input_pointer_event_type_button, native, pressed);
            }
        } else if (type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL ||
                   type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER ||
                   type == LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS) {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            gboolean wheel = type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL;
            struct wpe_input_axis_2d_event translated = {
                .base = {
                    .type = wpe_input_axis_event_type_mask_2d | (wheel ? wpe_input_axis_event_type_motion : wpe_input_axis_event_type_motion_smooth),
                    .time = libinput_event_pointer_get_time(pointer),
                    .x = (int)input->x, .y = (int)input->y,
                    .modifiers = modifiers(input)
                }
            };
            if (libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
                translated.y_axis = wheel ? -libinput_event_pointer_get_scroll_value_v120(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
                                          : libinput_event_pointer_get_scroll_value(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            if (libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
                translated.x_axis = wheel ? libinput_event_pointer_get_scroll_value_v120(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)
                                          : libinput_event_pointer_get_scroll_value(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
            wpe_view_backend_dispatch_axis_event(input->backend, &translated.base);
        } else if (type == LIBINPUT_EVENT_KEYBOARD_KEY && input->key_state) {
            struct libinput_event_keyboard *keyboard = libinput_event_get_keyboard_event(event);
            uint32_t code = libinput_event_keyboard_get_key(keyboard) + 8;
            gboolean pressed = libinput_event_keyboard_get_key_state(keyboard) == LIBINPUT_KEY_STATE_PRESSED;
            xkb_keysym_t symbol = xkb_state_key_get_one_sym(input->key_state, code);
            xkb_state_update_key(input->key_state, code, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
            struct wpe_input_keyboard_event translated = {
                .time = libinput_event_keyboard_get_time(keyboard), .key_code = symbol,
                .hardware_key_code = code, .pressed = pressed, .modifiers = modifiers(input)
            };
            wpe_view_backend_dispatch_keyboard_event(input->backend, &translated);
        }
        libinput_event_destroy(event);
    }
}

static gboolean input_ready(gint fd, GIOCondition condition, gpointer data)
{
    (void)fd;
    EmbeddedBrowserInput *input = data;
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        g_warning("Native input connection lost");
        input->source = 0;
        return G_SOURCE_REMOVE;
    }
    dispatch_events(input);
    return G_SOURCE_CONTINUE;
}

EmbeddedBrowserInput *embedded_browser_input_start(struct wpe_view_backend *backend, guint width, guint height)
{
    if (g_strcmp0(g_getenv("BROWSER_INPUT"), "0") == 0)
        return NULL;
    EmbeddedBrowserInput *input = g_new0(EmbeddedBrowserInput, 1);
    input->backend = backend;
    input->width = width;
    input->height = height;
    input->x = width / 2.0;
    input->y = height / 2.0;
    static const struct libinput_interface interface = {
        .open_restricted = open_input, .close_restricted = close_input
    };
    struct udev *udev = udev_new();
    if (udev) {
        input->libinput = libinput_udev_create_context(&interface, input, udev);
        udev_unref(udev);
    }
    const char *seat = g_getenv("BROWSER_INPUT_SEAT");
    if (!input->libinput || libinput_udev_assign_seat(input->libinput, seat && *seat ? seat : "seat0") != 0) {
        g_warning("Native input unavailable; use the WebKit remote inspector to interact with the page");
        embedded_browser_input_stop(input);
        return NULL;
    }
    input->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (input->xkb_context) {
        struct xkb_rule_names names = { .layout = g_getenv("BROWSER_KEYBOARD_LAYOUT") };
        input->keymap = xkb_keymap_new_from_names(input->xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (input->keymap) input->key_state = xkb_state_new(input->keymap);
    }
    input->source = g_unix_fd_add(libinput_get_fd(input->libinput), G_IO_IN | G_IO_ERR | G_IO_HUP, input_ready, input);
    dispatch_events(input);
    if (input->devices)
        g_message("Native input enabled on seat %s with %u devices; keyboard, pointer and scrolling supported", seat && *seat ? seat : "seat0", input->devices);
    else
        g_warning("No input devices discovered on seat %s; check /dev/input permissions and the /run/udev database. Remote inspector remains available", seat && *seat ? seat : "seat0");
    return input;
}

void embedded_browser_input_stop(EmbeddedBrowserInput *input)
{
    if (!input) return;
    if (input->source) g_source_remove(input->source);
    if (input->key_state) xkb_state_unref(input->key_state);
    if (input->keymap) xkb_keymap_unref(input->keymap);
    if (input->xkb_context) xkb_context_unref(input->xkb_context);
    if (input->libinput) libinput_unref(input->libinput);
    g_free(input);
}
