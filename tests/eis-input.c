/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Inject compositor-level input into a KWin session through libei.
 *
 * Only for the isolated test session: KWin's RemoteDesktop EIS endpoint is
 * reached over the session bus, so run this through tests/isolated-run.py.
 * Commands are read from stdin, one per line:
 *   move X Y        absolute pointer position in logical pixels
 *   button CODE 0|1 Linux input button code (272 left, 273 right), release/press
 *   key CODE 0|1    Linux input key code (125 left Meta), release/press
 *   wheel N         N mouse wheel steps (120 units each), positive scrolls down
 *   smooth DY       touchpad-style scroll of DY logical pixels, then stop
 *   scroll DY       touchpad-style scroll of DY logical pixels, fingers still down
 *   scrollstop      the fingers lift: ends a run of scroll commands
 *   sleep MS
 * Every command is sent as its own frame, as a real device would.
 *
 * Build: cc -O2 -o eis-input tests/eis-input.c $(pkg-config --cflags --libs libei-1.0 libsystemd)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libei.h>
#include <systemd/sd-bus.h>

static struct ei_device *pointer, *keyboard;

/* KWin ends the EIS session when its D-Bus caller goes away: keep it open. */
static sd_bus *bus;

static int connect_eis(void)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int fd = -1, cookie;

    if (sd_bus_open_user(&bus) < 0) return -1;
    /* All libei capabilities: pointer, absolute pointer, keyboard, touch, scroll, button. */
    if (sd_bus_call_method(bus, "org.kde.KWin", "/org/kde/KWin/EIS/RemoteDesktop",
                           "org.kde.KWin.EIS.RemoteDesktop", "connectToEIS", &error, &reply, "i", 63) >= 0 &&
        sd_bus_message_read(reply, "hi", &fd, &cookie) >= 0)
        fd = fcntl(fd, F_DUPFD_CLOEXEC, 3); /* the reply owns the received descriptor */
    else
        fprintf(stderr, "connectToEIS failed: %s\n", error.message ? error.message : "unknown");
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return fd;
}

static void dispatch(struct ei *ei, int timeout)
{
    struct pollfd fds = {.fd = ei_get_fd(ei), .events = POLLIN};
    struct ei_event *event;

    if (poll(&fds, 1, timeout) > 0) ei_dispatch(ei);
    while ((event = ei_get_event(ei)))
    {
        struct ei_device *device = ei_event_get_device(event);
        switch (ei_event_get_type(event))
        {
        case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                      EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD, NULL);
            break;
        case EI_EVENT_DEVICE_RESUMED:
            if (ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE) && !pointer)
                pointer = ei_device_ref(device);
            if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD) && !keyboard)
                keyboard = ei_device_ref(device);
            ei_device_start_emulating(device, 1);
            break;
        case EI_EVENT_DISCONNECT:
            fprintf(stderr, "EIS disconnected\n");
            exit(3);
        default:
            break;
        }
        ei_event_unref(event);
    }
}

int main(void)
{
    struct ei *ei = ei_new_sender(NULL);
    char line[256];
    int fd, i;

    /* Without an explicit bus, sd-bus falls back to the user's real session:
     * never inject input into the live desktop. */
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || !getenv("WAYLAND_DISPLAY") ||
        strncmp(getenv("WAYLAND_DISPLAY"), "tuxoffice-test-", 15))
    {
        fprintf(stderr, "Refusing to inject input outside the isolated test session\n");
        return 4;
    }
    ei_configure_name(ei, "tuxoffice-test-input");
    if ((fd = connect_eis()) < 0 || ei_setup_backend_fd(ei, fd) < 0) return 2;
    for (i = 0; i < 200 && (!pointer || !keyboard); ++i) dispatch(ei, 10);
    if (!pointer || !keyboard)
    {
        fprintf(stderr, "No EIS pointer/keyboard device\n");
        return 2;
    }
    while (fgets(line, sizeof(line), stdin))
    {
        double x, y;
        int code, value, ms;

        if (sscanf(line, "move %lf %lf", &x, &y) == 2)
        {
            ei_device_pointer_motion_absolute(pointer, x, y);
            ei_device_frame(pointer, ei_now(ei));
        }
        else if (sscanf(line, "button %d %d", &code, &value) == 2)
        {
            ei_device_button_button(pointer, code, value);
            ei_device_frame(pointer, ei_now(ei));
        }
        else if (sscanf(line, "key %d %d", &code, &value) == 2)
        {
            ei_device_keyboard_key(keyboard, code, value);
            ei_device_frame(keyboard, ei_now(ei));
        }
        else if (sscanf(line, "wheel %d", &value) == 1)
        {
            ei_device_scroll_discrete(pointer, 0, value * 120);
            ei_device_frame(pointer, ei_now(ei));
        }
        else if (sscanf(line, "smooth %lf", &y) == 1)
        {
            ei_device_scroll_delta(pointer, 0, y);
            ei_device_frame(pointer, ei_now(ei));
            ei_device_scroll_stop(pointer, false, true);
            ei_device_frame(pointer, ei_now(ei));
        }
        else if (sscanf(line, "scroll %lf", &y) == 1)
        {
            ei_device_scroll_delta(pointer, 0, y);
            ei_device_frame(pointer, ei_now(ei));
        }
        else if (!strncmp(line, "scrollstop", 10))
        {
            ei_device_scroll_stop(pointer, false, true);
            ei_device_frame(pointer, ei_now(ei));
        }
        else if (sscanf(line, "sleep %d", &ms) == 1)
        {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += (long)ms * 1000000;
            until.tv_sec += until.tv_nsec / 1000000000;
            until.tv_nsec %= 1000000000;
            for (;;)
            {
                struct timespec now;
                long left;
                clock_gettime(CLOCK_MONOTONIC, &now);
                left = (until.tv_sec - now.tv_sec) * 1000 + (until.tv_nsec - now.tv_nsec) / 1000000;
                if (left <= 0) break;
                dispatch(ei, left);
            }
        }
        dispatch(ei, 0);
    }
    dispatch(ei, 50);
    ei_device_unref(pointer);
    ei_device_unref(keyboard);
    ei_unref(ei);
    sd_bus_unref(bus);
    return 0;
}
