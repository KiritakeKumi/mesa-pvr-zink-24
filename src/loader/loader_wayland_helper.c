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

#include <poll.h>
#include <errno.h>
#include <stdlib.h>

#include "util/perf/cpu_trace.h"

#include "loader_wayland_helper.h"

#ifndef HAVE_WL_DISPATCH_QUEUE_TIMEOUT
static int
wl_display_poll(struct wl_display *display,
                short int events,
                const struct timespec *timeout)
{
   int ret;
   struct pollfd pfd[1];
   struct timespec now;
   struct timespec deadline = {0};
   struct timespec result;
   struct timespec *remaining_timeout = NULL;

   if (timeout) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      timespec_add(&deadline, &now, timeout);
   }

   pfd[0].fd = wl_display_get_fd(display);
   pfd[0].events = events;
   do {
      if (timeout) {
         clock_gettime(CLOCK_MONOTONIC, &now);
         timespec_sub_saturate(&result, &deadline, &now);
         remaining_timeout = &result;
      }
      ret = ppoll(pfd, 1, remaining_timeout, NULL);
   } while (ret == -1 && errno == EINTR);

   return ret;
}

int
wl_display_dispatch_queue_timeout(struct wl_display *display,
                                  struct wl_event_queue *queue,
                                  const struct timespec *timeout)
{
   int ret;
   struct timespec now;
   struct timespec deadline = {0};
   struct timespec result;
   struct timespec *remaining_timeout = NULL;

   if (timeout) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      timespec_add(&deadline, &now, timeout);
   }

   if (wl_display_prepare_read_queue(display, queue) == -1)
      return wl_display_dispatch_queue_pending(display, queue);

   while (true) {
      ret = wl_display_flush(display);

      if (ret != -1 || errno != EAGAIN)
         break;

      if (timeout) {
         clock_gettime(CLOCK_MONOTONIC, &now);
         timespec_sub_saturate(&result, &deadline, &now);
         remaining_timeout = &result;
      }
      ret = wl_display_poll(display, POLLOUT, remaining_timeout);

      if (ret <= 0) {
         wl_display_cancel_read(display);
         return ret;
      }
   }

   /* Don't stop if flushing hits an EPIPE; continue so we can read any
    * protocol error that may have triggered it. */
   if (ret < 0 && errno != EPIPE) {
      wl_display_cancel_read(display);
      return -1;
   }

   while (true) {
      if (timeout) {
         clock_gettime(CLOCK_MONOTONIC, &now);
         timespec_sub_saturate(&result, &deadline, &now);
         remaining_timeout = &result;
      }

      ret = wl_display_poll(display, POLLIN, remaining_timeout);
      if (ret <= 0) {
         wl_display_cancel_read(display);
         break;
      }

      ret = wl_display_read_events(display);
      if (ret == -1)
         break;

      ret = wl_display_dispatch_queue_pending(display, queue);
      if (ret != 0)
         break;

      /* wl_display_dispatch_queue_pending can return 0 if we ended up reading
       * from WL fd, but there was no complete event to dispatch yet.
       * Try reading again. */
      if (wl_display_prepare_read_queue(display, queue) == -1)
         return wl_display_dispatch_queue_pending(display, queue);
   }

   return ret;
}
#endif

#ifndef HAVE_WL_CREATE_QUEUE_WITH_NAME
struct wl_event_queue *
wl_display_create_queue_with_name(struct wl_display *display, const char *name)
{
   return wl_display_create_queue(display);
}
#endif

int
loader_wayland_dispatch(struct wl_display *wl_display,
                        struct wl_event_queue *queue,
                        struct timespec *end_time)
{
   struct timespec current_time;
   struct timespec remaining_timeout;

   MESA_TRACE_FUNC();

   if (!end_time)
      return wl_display_dispatch_queue(wl_display, queue);

   clock_gettime(CLOCK_MONOTONIC, &current_time);
   timespec_sub_saturate(&remaining_timeout, end_time, &current_time);
   return wl_display_dispatch_queue_timeout(wl_display,
                                            queue,
                                            &remaining_timeout);
}

static char *
stringify_wayland_id(uint32_t id)
{
   char *out;

   if (asprintf(&out, "wl%d", id) < 0)
      return "Wayland buffer";

   return out;
}

void
loader_wayland_wrap_buffer(struct loader_wayland_buffer *lwb,
                           struct wl_buffer *wl_buffer)
{
   lwb->buffer = wl_buffer;
   lwb->id = wl_proxy_get_id((struct wl_proxy *)wl_buffer);
   lwb->flow_id = 0;
   lwb->name = stringify_wayland_id(lwb->id);
}

void
loader_wayland_buffer_destroy(struct loader_wayland_buffer *lwb)
{
   wl_buffer_destroy(lwb->buffer);
   lwb->buffer = NULL;
   lwb->id = 0;
   lwb->flow_id = 0;
   free(lwb->name);
   lwb->name = NULL;
}

void
loader_wayland_buffer_set_flow(struct loader_wayland_buffer *lwb, uint64_t flow_id)
{
  lwb->flow_id = flow_id;
}

bool
loader_wayland_wrap_surface(struct loader_wayland_surface *lws,
                            struct wl_surface *wl_surface,
                            struct wl_event_queue *queue)
{
   lws->surface = wl_proxy_create_wrapper(wl_surface);
   if (!lws->surface)
      return false;

   lws->id = wl_proxy_get_id((struct wl_proxy *)wl_surface);
   wl_proxy_set_queue((struct wl_proxy *)lws->surface, queue);

   return true;
}

void
loader_wayland_surface_destroy(struct loader_wayland_surface *lws)
{
   if (!lws->surface)
      return;

   wl_proxy_wrapper_destroy(lws->surface);
   lws->surface = NULL;
   lws->id = 0;
}
