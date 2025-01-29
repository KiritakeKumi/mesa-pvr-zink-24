/*
 * Copyright © 2022 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef LOADER_WAYLAND_HELPER_HEADER_H
#define LOADER_WAYLAND_HELPER_HEADER_H

#include <wayland-client.h>

struct loader_wayland_buffer {
   struct wl_buffer *buffer;
   uint32_t id;
   uint64_t flow_id;
   char *name;
};

struct loader_wayland_surface {
   struct wl_surface *surface;
   uint32_t id;
};

#ifndef HAVE_WL_DISPATCH_QUEUE_TIMEOUT

#include <util/timespec.h>
int
wl_display_dispatch_queue_timeout(struct wl_display *display,
                                  struct wl_event_queue *queue,
                                  const struct timespec *deadline);
#endif

#ifndef HAVE_WL_CREATE_QUEUE_WITH_NAME
struct wl_event_queue *
wl_display_create_queue_with_name(struct wl_display *display,
                                  const char *name);
#endif

int
loader_wayland_dispatch(struct wl_display *display,
                        struct wl_event_queue *queue,
                        struct timespec *end_time);

void
loader_wayland_wrap_buffer(struct loader_wayland_buffer *lwb,
                           struct wl_buffer *wl_buffer);

void
loader_wayland_buffer_destroy(struct loader_wayland_buffer *lwb);

void
loader_wayland_buffer_set_flow(struct loader_wayland_buffer *lwb, uint64_t flow_id);

bool
loader_wayland_wrap_surface(struct loader_wayland_surface *lws,
                            struct wl_surface *wl_surface,
                            struct wl_event_queue *queue);

void
loader_wayland_surface_destroy(struct loader_wayland_surface *lws);

#endif
