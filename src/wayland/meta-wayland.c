/*
 * Wayland Support
 *
 * Copyright (C) 2012 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <config.h>

#include <clutter/clutter.h>
#include <clutter/wayland/clutter-wayland-compositor.h>
#include <clutter/wayland/clutter-wayland-surface.h>
#include <clutter/evdev/clutter-evdev.h>

#include <glib.h>
#include <sys/time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <wayland-server.h>

#include "xserver-server-protocol.h"

#include "meta-wayland-private.h"
#include "meta-wayland-stage.h"
#include "meta-window-actor-private.h"
#include "meta-wayland-seat.h"
#include "meta-wayland-keyboard.h"
#include "meta-wayland-pointer.h"
#include "meta-wayland-data-device.h"
#include "display-private.h"
#include "window-private.h"
#include <meta/types.h>
#include <meta/main.h>
#include "frame.h"
#include "meta-weston-launch.h"

static MetaWaylandCompositor _meta_wayland_compositor;

MetaWaylandCompositor *
meta_wayland_compositor_get_default (void)
{
  return &_meta_wayland_compositor;
}

static guint32
get_time (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
 return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static gboolean
wayland_event_source_prepare (GSource *base, int *timeout)
{
  WaylandEventSource *source = (WaylandEventSource *)base;

  *timeout = -1;

  wl_display_flush_clients (source->display);

  return FALSE;
}

static gboolean
wayland_event_source_check (GSource *base)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  return source->pfd.revents;
}

static gboolean
wayland_event_source_dispatch (GSource *base,
                               GSourceFunc callback,
                               void *data)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  struct wl_event_loop *loop = wl_display_get_event_loop (source->display);

  wl_event_loop_dispatch (loop, 0);

  return TRUE;
}

static GSourceFuncs wayland_event_source_funcs =
{
  wayland_event_source_prepare,
  wayland_event_source_check,
  wayland_event_source_dispatch,
  NULL
};

static GSource *
wayland_event_source_new (struct wl_display *display)
{
  WaylandEventSource *source;
  struct wl_event_loop *loop = wl_display_get_event_loop (display);

  source = (WaylandEventSource *) g_source_new (&wayland_event_source_funcs,
                                                sizeof (WaylandEventSource));
  source->display = display;
  source->pfd.fd = wl_event_loop_get_fd (loop);
  source->pfd.events = G_IO_IN | G_IO_ERR;
  g_source_add_poll (&source->source, &source->pfd);

  return &source->source;
}

static void
meta_wayland_buffer_destroy_handler (struct wl_listener *listener,
                                     void *data)
{
  MetaWaylandBuffer *buffer =
    wl_container_of (listener, buffer, destroy_listener);

  wl_signal_emit (&buffer->destroy_signal, buffer);
  g_slice_free (MetaWaylandBuffer, buffer);
}

static MetaWaylandBuffer *
meta_wayland_buffer_from_resource (struct wl_resource *resource)
{
  MetaWaylandBuffer *buffer;
  struct wl_listener *listener;

  listener =
    wl_resource_get_destroy_listener (resource,
                                      meta_wayland_buffer_destroy_handler);

  if (listener)
    {
      buffer = wl_container_of (listener, buffer, destroy_listener);
    }
  else
    {
      buffer = g_slice_new0 (MetaWaylandBuffer);

      buffer->resource = resource;
      wl_signal_init (&buffer->destroy_signal);
      buffer->destroy_listener.notify = meta_wayland_buffer_destroy_handler;
      wl_resource_add_destroy_listener (resource, &buffer->destroy_listener);
    }

  return buffer;
}

static void
meta_wayland_buffer_reference_handle_destroy (struct wl_listener *listener,
                                          void *data)
{
  MetaWaylandBufferReference *ref =
    wl_container_of (listener, ref, destroy_listener);

  g_assert (data == ref->buffer);

  ref->buffer = NULL;
}

static void
meta_wayland_buffer_reference (MetaWaylandBufferReference *ref,
                               MetaWaylandBuffer *buffer)
{
  if (ref->buffer && buffer != ref->buffer)
    {
      ref->buffer->busy_count--;

      if (ref->buffer->busy_count == 0)
        {
          g_assert (wl_resource_get_client (ref->buffer->resource));
          wl_resource_queue_event (ref->buffer->resource, WL_BUFFER_RELEASE);
        }

      wl_list_remove (&ref->destroy_listener.link);
    }

  if (buffer && buffer != ref->buffer)
    {
      buffer->busy_count++;
      wl_signal_add (&buffer->destroy_signal, &ref->destroy_listener);
    }

  ref->buffer = buffer;
  ref->destroy_listener.notify = meta_wayland_buffer_reference_handle_destroy;
}

static void
surface_process_damage (MetaWaylandSurface *surface,
                        cairo_region_t *region)
{
  if (surface->window &&
      surface->buffer_ref.buffer)
    {
      MetaWindowActor *window_actor =
        META_WINDOW_ACTOR (meta_window_get_compositor_private (surface->window));

      if (window_actor)
        {
          int i, n_rectangles = cairo_region_num_rectangles (region);

          for (i = 0; i < n_rectangles; i++)
            {
              cairo_rectangle_int_t rectangle;

              cairo_region_get_rectangle (region, i, &rectangle);

              meta_window_actor_process_wayland_damage (window_actor,
                                                        rectangle.x,
                                                        rectangle.y,
                                                        rectangle.width,
                                                        rectangle.height);
            }
        }
    }
}

static void
meta_wayland_surface_destroy (struct wl_client *wayland_client,
                              struct wl_resource *wayland_resource)
{
  wl_resource_destroy (wayland_resource);
}

static void
meta_wayland_surface_attach (struct wl_client *wayland_client,
                             struct wl_resource *wayland_surface_resource,
                             struct wl_resource *wayland_buffer_resource,
                             gint32 sx, gint32 sy)
{
  MetaWaylandSurface *surface =
    wl_resource_get_user_data (wayland_surface_resource);
  MetaWaylandBuffer *buffer;

  if (wayland_buffer_resource)
    buffer = meta_wayland_buffer_from_resource (wayland_buffer_resource);
  else
    buffer = NULL;

  /* Attach without commit in between does not send wl_buffer.release */
  if (surface->pending.buffer)
    wl_list_remove (&surface->pending.buffer_destroy_listener.link);

  surface->pending.sx = sx;
  surface->pending.sy = sy;
  surface->pending.buffer = buffer;
  surface->pending.newly_attached = TRUE;

  if (buffer)
    wl_signal_add (&buffer->destroy_signal,
                   &surface->pending.buffer_destroy_listener);
}

static void
meta_wayland_surface_damage (struct wl_client *client,
                             struct wl_resource *surface_resource,
                             gint32 x,
                             gint32 y,
                             gint32 width,
                             gint32 height)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };

  cairo_region_union_rectangle (surface->pending.damage, &rectangle);
}

static void
destroy_frame_callback (struct wl_resource *callback_resource)
{
  MetaWaylandFrameCallback *callback =
    wl_resource_get_user_data (callback_resource);

  wl_list_remove (&callback->link);
  g_slice_free (MetaWaylandFrameCallback, callback);
}

static void
meta_wayland_surface_frame (struct wl_client *client,
                            struct wl_resource *surface_resource,
                            guint32 callback_id)
{
  MetaWaylandFrameCallback *callback;
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);

  callback = g_slice_new0 (MetaWaylandFrameCallback);
  callback->compositor = surface->compositor;
  callback->resource = wl_resource_create (client,
					   &wl_callback_interface, 1,
					   callback_id);
  wl_resource_set_user_data (callback->resource, callback);
  wl_resource_set_destructor (callback->resource, destroy_frame_callback);

  wl_list_insert (surface->pending.frame_callback_list.prev, &callback->link);
}

static void
meta_wayland_surface_set_opaque_region (struct wl_client *client,
                                        struct wl_resource *resource,
                                        struct wl_resource *region)
{
}

static void
meta_wayland_surface_set_input_region (struct wl_client *client,
                                       struct wl_resource *resource,
                                       struct wl_resource *region)
{
}

static void
empty_region (cairo_region_t *region)
{
  cairo_rectangle_int_t rectangle = { 0, 0, 0, 0 };
  cairo_region_intersect_rectangle (region, &rectangle);
}

static void
meta_wayland_surface_commit (struct wl_client *client,
                             struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);
  MetaWaylandCompositor *compositor = surface->compositor;

  /* wl_surface.attach */
  if (surface->pending.newly_attached &&
      surface->buffer_ref.buffer != surface->pending.buffer)
    {
      /* Note: we set this before informing any window-actor since the
       * window actor will expect to find the new buffer within the
       * surface. */
      meta_wayland_buffer_reference (&surface->buffer_ref,
                                     surface->pending.buffer);

      if (surface->pending.buffer)
        {
          MetaWaylandBuffer *buffer = surface->pending.buffer;

          if (surface->window)
            {
              MetaWindow *window = surface->window;
              MetaWindowActor *window_actor =
                META_WINDOW_ACTOR (meta_window_get_compositor_private (window));
              MetaRectangle rect;

              meta_window_get_input_rect (surface->window, &rect);

              if (window_actor)
                meta_window_actor_attach_wayland_buffer (window_actor, buffer);

              /* XXX: we resize X based surfaces according to X events */
              if (surface->xid == 0 &&
                  (buffer->width != rect.width || buffer->height != rect.height))
                meta_window_resize (surface->window, FALSE, buffer->width, buffer->height);
            }
          else if (surface == compositor->seat->sprite)
            meta_wayland_seat_update_sprite (compositor->seat);
        }
    }

  if (surface->pending.buffer)
    {
      wl_list_remove (&surface->pending.buffer_destroy_listener.link);
      surface->pending.buffer = NULL;
    }
  surface->pending.sx = 0;
  surface->pending.sy = 0;
  surface->pending.newly_attached = FALSE;

  surface_process_damage (surface, surface->pending.damage);
  empty_region (surface->pending.damage);

  /* wl_surface.frame */
  wl_list_insert_list (&compositor->frame_callbacks,
                       &surface->pending.frame_callback_list);
  wl_list_init (&surface->pending.frame_callback_list);
}

static void
meta_wayland_surface_set_buffer_transform (struct wl_client *client,
                                           struct wl_resource *resource,
                                           int32_t transform)
{
}

const struct wl_surface_interface meta_wayland_surface_interface = {
  meta_wayland_surface_destroy,
  meta_wayland_surface_attach,
  meta_wayland_surface_damage,
  meta_wayland_surface_frame,
  meta_wayland_surface_set_opaque_region,
  meta_wayland_surface_set_input_region,
  meta_wayland_surface_commit,
  meta_wayland_surface_set_buffer_transform
};

void
meta_wayland_compositor_set_input_focus (MetaWaylandCompositor *compositor,
                                         MetaWindow            *window)
{
  MetaWaylandSurface *surface = window ? window->surface : NULL;

  meta_wayland_keyboard_set_focus (&compositor->seat->keyboard,
                                   surface);
  meta_wayland_data_device_set_keyboard_focus (compositor->seat);
}

static void
window_destroyed_cb (void *user_data, GObject *old_object)
{
  MetaWaylandSurface *surface = user_data;

  surface->window = NULL;
}

void
meta_wayland_compositor_repick (MetaWaylandCompositor *compositor)
{
  meta_wayland_seat_repick (compositor->seat,
                            get_time (),
                            NULL);
}

static void
meta_wayland_surface_free (MetaWaylandSurface *surface)
{
  MetaWaylandCompositor *compositor = surface->compositor;
  MetaWaylandFrameCallback *cb, *next;

  compositor->surfaces = g_list_remove (compositor->surfaces, surface);

  meta_wayland_buffer_reference (&surface->buffer_ref, NULL);

  if (surface->window)
    g_object_weak_unref (G_OBJECT (surface->window),
                         window_destroyed_cb,
                         surface);

  /* NB: If the surface corresponds to an X window then we will be
   * sure to free the MetaWindow according to some X event. */
  if (surface->window &&
      surface->window->client_type == META_WINDOW_CLIENT_TYPE_WAYLAND)
    {
      MetaDisplay *display = meta_get_display ();
      guint32 timestamp = meta_display_get_current_time_roundtrip (display);
      meta_window_unmanage (surface->window, timestamp);
    }

  if (surface->pending.buffer)
    wl_list_remove (&surface->pending.buffer_destroy_listener.link);

  cairo_region_destroy (surface->pending.damage);

  wl_list_for_each_safe (cb, next,
                         &surface->pending.frame_callback_list, link)
    wl_resource_destroy (cb->resource);

  g_slice_free (MetaWaylandSurface, surface);

  meta_wayland_compositor_repick (compositor);

 if (compositor->implicit_grab_surface == surface)
   compositor->implicit_grab_surface = compositor->seat->pointer.current;
}

static void
meta_wayland_surface_resource_destroy_cb (struct wl_resource *resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (resource);
  meta_wayland_surface_free (surface);
}

static void
surface_handle_pending_buffer_destroy (struct wl_listener *listener,
                                       void *data)
{
  MetaWaylandSurface *surface =
    wl_container_of (listener, surface, pending.buffer_destroy_listener);

  surface->pending.buffer = NULL;
}

static void
meta_wayland_compositor_create_surface (struct wl_client *wayland_client,
                                        struct wl_resource *wayland_compositor_resource,
                                        guint32 id)
{
  MetaWaylandCompositor *compositor =
    wl_resource_get_user_data (wayland_compositor_resource);
  MetaWaylandSurface *surface = g_slice_new0 (MetaWaylandSurface);

  surface->compositor = compositor;

  /* a surface inherits the version from the compositor */
  surface->resource = wl_resource_create (wayland_client,
					  &wl_surface_interface,
					  wl_resource_get_version (wayland_compositor_resource),
					  id);
  wl_resource_set_implementation (surface->resource, &meta_wayland_surface_interface, surface,
				  meta_wayland_surface_resource_destroy_cb);

  surface->pending.damage = cairo_region_create ();

  surface->pending.buffer_destroy_listener.notify =
    surface_handle_pending_buffer_destroy;
  wl_list_init (&surface->pending.frame_callback_list);

  compositor->surfaces = g_list_prepend (compositor->surfaces, surface);
}

static void
meta_wayland_region_destroy (struct wl_client *client,
                             struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
meta_wayland_region_add (struct wl_client *client,
                         struct wl_resource *resource,
                         gint32 x,
                         gint32 y,
                         gint32 width,
                         gint32 height)
{
  MetaWaylandRegion *region = wl_resource_get_user_data (resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };

  cairo_region_union_rectangle (region->region, &rectangle);
}

static void
meta_wayland_region_subtract (struct wl_client *client,
                              struct wl_resource *resource,
                              gint32 x,
                              gint32 y,
                              gint32 width,
                              gint32 height)
{
  MetaWaylandRegion *region = wl_resource_get_user_data (resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };

  cairo_region_subtract_rectangle (region->region, &rectangle);
}

const struct wl_region_interface meta_wayland_region_interface = {
  meta_wayland_region_destroy,
  meta_wayland_region_add,
  meta_wayland_region_subtract
};

static void
meta_wayland_region_resource_destroy_cb (struct wl_resource *resource)
{
  MetaWaylandRegion *region = wl_resource_get_user_data (resource);

  cairo_region_destroy (region->region);
  g_slice_free (MetaWaylandRegion, region);
}

static void
meta_wayland_compositor_create_region (struct wl_client *wayland_client,
                                       struct wl_resource *compositor_resource,
                                       uint32_t id)
{
  MetaWaylandRegion *region = g_slice_new0 (MetaWaylandRegion);

  region->resource = wl_resource_create (wayland_client,
					 &wl_region_interface, 1,
					 id);
  wl_resource_set_implementation (region->resource,
				  &meta_wayland_region_interface, region,
				  meta_wayland_region_resource_destroy_cb);

  region->region = cairo_region_create ();
}

static void
bind_output (struct wl_client *client,
             void *data,
             guint32 version,
             guint32 id)
{
  MetaOutput *output = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_output_interface, version, id);

  wl_resource_post_event (resource,
                          WL_OUTPUT_GEOMETRY,
                          (int)output->monitor->rect.x,
			  (int)output->monitor->rect.y,
			  output->width_mm,
                          output->height_mm,
			  /* Cogl values reflect XRandR values,
			     and so does wayland */
			  output->subpixel_order,
                          "unknown", /* make */
                          "unknown", /* model */
			  WL_OUTPUT_TRANSFORM_NORMAL);

  wl_resource_post_event (resource,
			  WL_OUTPUT_MODE,
			  WL_OUTPUT_MODE_PREFERRED | WL_OUTPUT_MODE_CURRENT,
			  (int)output->monitor->rect.width,
			  (int)output->monitor->rect.height,
			  (int)output->monitor->refresh_rate);

  if (version >= 2)
    wl_resource_post_event (resource,
			    WL_OUTPUT_DONE);
}

static void
meta_wayland_compositor_create_outputs (MetaWaylandCompositor *compositor,
					MetaMonitorManager    *monitors)
{
  MetaOutput *outputs;
  int i, n_outputs;

  outputs = meta_monitor_manager_get_outputs (monitors, &n_outputs);

  for (i = 0; i < n_outputs; i++)
    compositor->outputs = g_list_prepend (compositor->outputs,
					  wl_global_create (compositor->wayland_display,
							    &wl_output_interface, 2,
							    &outputs[i], bind_output));
}

const static struct wl_compositor_interface meta_wayland_compositor_interface = {
  meta_wayland_compositor_create_surface,
  meta_wayland_compositor_create_region
};

static void
paint_finished_cb (ClutterActor *self, void *user_data)
{
  MetaWaylandCompositor *compositor = user_data;

  while (!wl_list_empty (&compositor->frame_callbacks))
    {
      MetaWaylandFrameCallback *callback =
        wl_container_of (compositor->frame_callbacks.next, callback, link);

      wl_resource_post_event (callback->resource,
                              WL_CALLBACK_DONE, get_time ());
      wl_resource_destroy (callback->resource);
    }
}

static void
compositor_bind (struct wl_client *client,
		 void *data,
                 guint32 version,
                 guint32 id)
{
  MetaWaylandCompositor *compositor = data;
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_compositor_interface, version, id);
  wl_resource_set_implementation (resource, &meta_wayland_compositor_interface, compositor, NULL);
}

static void
shell_surface_pong (struct wl_client *client,
                    struct wl_resource *resource,
                    guint32 serial)
{
}

/* TODO: consider adding this to window.c */
static void
meta_window_get_surface_rect (const MetaWindow *window,
                              MetaRectangle    *rect)
{
  if (window->frame)
    {
      MetaFrameBorders borders;
      *rect = window->frame->rect;
      meta_frame_calc_borders (window->frame, &borders);
    }
  else
    *rect = window->rect;
}

typedef struct _MetaWaylandGrab
{
  MetaWaylandPointerGrab grab;
  MetaWaylandShellSurface *shell_surface;
  struct wl_listener shell_surface_destroy_listener;
  MetaWaylandPointer *pointer;
} MetaWaylandGrab;

typedef struct _MetaWaylandMoveGrab
{
  MetaWaylandGrab base;
  wl_fixed_t dx, dy;
} MetaWaylandMoveGrab;

static void
destroy_shell_surface_grab_listener (struct wl_listener *listener,
                                     void *data)
{
  MetaWaylandGrab *grab = wl_container_of (listener, grab,
                                           shell_surface_destroy_listener);
  grab->shell_surface = NULL;

  /* XXX: Could we perhaps just stop the grab here so we don't have
   * to consider grab->shell_surface becoming NULL in grab interface
   * callbacks? */
}

typedef enum _GrabCursor
{
  GRAB_CURSOR_MOVE,
} GrabCursor;

static void
grab_pointer (MetaWaylandGrab *grab,
              const MetaWaylandPointerGrabInterface *interface,
              MetaWaylandShellSurface *shell_surface,
              MetaWaylandPointer *pointer,
              GrabCursor cursor)
{
  /* TODO: popup_grab_end (pointer); */

  grab->grab.interface = interface;
  grab->shell_surface = shell_surface;
  grab->shell_surface_destroy_listener.notify =
    destroy_shell_surface_grab_listener;
  wl_resource_add_destroy_listener (shell_surface->resource,
                                    &grab->shell_surface_destroy_listener);

  grab->pointer = pointer;
  grab->grab.focus = shell_surface->surface;

  meta_wayland_pointer_start_grab (pointer, &grab->grab);

  /* TODO: send_grab_cursor (cursor); */

  /* XXX: In Weston there is a desktop shell protocol which has
   * a set_grab_surface request that's used to specify the surface
   * that's focused here.
   *
   * TODO: understand why.
   *
   * XXX: For now we just focus the surface directly associated with
   * the grab.
   */
  meta_wayland_pointer_set_focus (pointer,
                                  grab->shell_surface->surface,
                                  wl_fixed_from_int (0),
                                  wl_fixed_from_int (0));
}

static void
release_pointer (MetaWaylandGrab *grab)
{
  if (grab->shell_surface)
    wl_list_remove (&grab->shell_surface_destroy_listener.link);

  meta_wayland_pointer_end_grab (grab->pointer);
}

static void
noop_grab_focus (MetaWaylandPointerGrab *grab,
                 MetaWaylandSurface *surface,
                 wl_fixed_t x,
                 wl_fixed_t y)
{
  grab->focus = NULL;
}

static void
move_grab_motion (MetaWaylandPointerGrab *grab,
                  uint32_t time,
                  wl_fixed_t x,
                  wl_fixed_t y)
{
  MetaWaylandMoveGrab *move = (MetaWaylandMoveGrab *)grab;
  MetaWaylandPointer *pointer = move->base.pointer;
  MetaWaylandShellSurface *shell_surface = move->base.shell_surface;

  if (!shell_surface)
    return;

  meta_window_move (shell_surface->surface->window,
                    TRUE,
                    wl_fixed_to_int (pointer->x + move->dx),
                    wl_fixed_to_int (pointer->y + move->dy));
}

static void
move_grab_button (MetaWaylandPointerGrab *pointer_grab,
                  uint32_t time,
                  uint32_t button,
                  uint32_t state_w)
{
  MetaWaylandGrab *grab =
    wl_container_of (pointer_grab, grab, grab);
  MetaWaylandMoveGrab *move = (MetaWaylandMoveGrab *)grab;
  MetaWaylandPointer *pointer = grab->pointer;
  enum wl_pointer_button_state state = state_w;

  if (pointer->button_count == 0 && state == WL_POINTER_BUTTON_STATE_RELEASED)
    {
      release_pointer (grab);
      g_slice_free (MetaWaylandMoveGrab, move);
    }
}

static const MetaWaylandPointerGrabInterface move_grab_interface = {
    noop_grab_focus,
    move_grab_motion,
    move_grab_button,
};

static void
start_surface_move (MetaWaylandShellSurface *shell_surface,
                    MetaWaylandSeat *seat)
{
  MetaWaylandMoveGrab *move;
  MetaRectangle rect;

  g_return_if_fail (shell_surface != NULL);

  /* TODO: check if the surface is fullscreen when we support fullscreen */

  move = g_slice_new (MetaWaylandMoveGrab);

  meta_window_get_surface_rect (shell_surface->surface->window,
                                &rect);

  move->dx = wl_fixed_from_int (rect.x) - seat->pointer.grab_x;
  move->dy = wl_fixed_from_int (rect.y) - seat->pointer.grab_y;

  grab_pointer (&move->base, &move_grab_interface, shell_surface,
                &seat->pointer, GRAB_CURSOR_MOVE);
}

static void
shell_surface_move (struct wl_client *client,
                    struct wl_resource *resource,
                    struct wl_resource *seat_resource,
                    guint32 serial)
{
  MetaWaylandSeat *seat = wl_resource_get_user_data (seat_resource);
  MetaWaylandShellSurface *shell_surface = wl_resource_get_user_data (resource);

  if (seat->pointer.button_count == 0 ||
      seat->pointer.grab_serial != serial ||
      seat->pointer.focus != shell_surface->surface)
    return;

  start_surface_move (shell_surface, seat);
}

static void
shell_surface_resize (struct wl_client *client,
                      struct wl_resource *resource,
                      struct wl_resource *seat,
                      guint32 serial,
                      guint32 edges)
{
}

static void
ensure_surface_window (MetaWaylandSurface *surface)
{
  MetaDisplay *display = meta_get_display ();

  if (!surface->window)
    {
      int width, height;

      if (surface->buffer_ref.buffer)
        {
          MetaWaylandBuffer *buffer = surface->buffer_ref.buffer;
          width = buffer->width;
          height = buffer->width;
        }
      else
        {
          width = 0;
          height = 0;
        }

      surface->window =
        meta_window_new_for_wayland (display, width, height, surface);

      /* If the MetaWindow becomes unmanaged (surface->window will be
       * freed in this case) we need to make sure to clear our
       * ->window pointers. */
      g_object_weak_ref (G_OBJECT (surface->window),
                         window_destroyed_cb,
                         surface);

      meta_window_calc_showing (surface->window);
    }
}

static void
shell_surface_set_toplevel (struct wl_client *client,
                            struct wl_resource *resource)
{
  MetaWaylandCompositor *compositor = &_meta_wayland_compositor;
  MetaWaylandShellSurface *shell_surface = wl_resource_get_user_data (resource);
  MetaWaylandSurface *surface = shell_surface->surface;

  /* NB: Surfaces from xwayland become managed based on X events. */
  if (client == compositor->xwayland_client)
    return;

  ensure_surface_window (surface);

  meta_window_unmake_fullscreen (surface->window);
}

static void
shell_surface_set_transient (struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *parent,
                             int x,
                             int y,
                             guint32 flags)
{
  MetaWaylandCompositor *compositor = &_meta_wayland_compositor;
  MetaWaylandShellSurface *shell_surface = wl_resource_get_user_data (resource);
  MetaWaylandSurface *surface = shell_surface->surface;

  /* NB: Surfaces from xwayland become managed based on X events. */
  if (client == compositor->xwayland_client)
    return;

  ensure_surface_window (surface);
}

static void
shell_surface_set_fullscreen (struct wl_client *client,
                              struct wl_resource *resource,
                              guint32 method,
                              guint32 framerate,
                              struct wl_resource *output)
{
  MetaWaylandCompositor *compositor = &_meta_wayland_compositor;
  MetaWaylandShellSurface *shell_surface = wl_resource_get_user_data (resource);
  MetaWaylandSurface *surface = shell_surface->surface;

  /* NB: Surfaces from xwayland become managed based on X events. */
  if (client == compositor->xwayland_client)
    return;

  ensure_surface_window (surface);

  meta_window_make_fullscreen (surface->window);
}

static void
shell_surface_set_popup (struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *seat,
                         guint32 serial,
                         struct wl_resource *parent,
                         gint32 x,
                         gint32 y,
                         guint32 flags)
{
}

static void
shell_surface_set_maximized (struct wl_client *client,
                             struct wl_resource *resource,
                             struct wl_resource *output)
{
}

static void
shell_surface_set_title (struct wl_client *client,
                         struct wl_resource *resource,
                         const char *title)
{
}

static void
shell_surface_set_class (struct wl_client *client,
                         struct wl_resource *resource,
                         const char *class_)
{
}

static const struct wl_shell_surface_interface meta_wayland_shell_surface_interface =
{
  shell_surface_pong,
  shell_surface_move,
  shell_surface_resize,
  shell_surface_set_toplevel,
  shell_surface_set_transient,
  shell_surface_set_fullscreen,
  shell_surface_set_popup,
  shell_surface_set_maximized,
  shell_surface_set_title,
  shell_surface_set_class
};

static void
shell_handle_surface_destroy (struct wl_listener *listener,
                              void *data)
{
  MetaWaylandShellSurface *shell_surface =
    wl_container_of (listener, shell_surface, surface_destroy_listener);
  shell_surface->surface->has_shell_surface = FALSE;
  shell_surface->surface = NULL;
  wl_resource_destroy (shell_surface->resource);
}

static void
destroy_shell_surface (struct wl_resource *resource)
{
  MetaWaylandShellSurface *shell_surface = wl_resource_get_user_data (resource);

  /* In case cleaning up a dead client destroys shell_surface first */
  if (shell_surface->surface)
    {
      wl_list_remove (&shell_surface->surface_destroy_listener.link);
      shell_surface->surface->has_shell_surface = FALSE;
    }

  g_free (shell_surface);
}

static void
get_shell_surface (struct wl_client *client,
                   struct wl_resource *resource,
                   guint32 id,
                   struct wl_resource *surface_resource)
{
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  MetaWaylandShellSurface *shell_surface;

  if (surface->has_shell_surface)
    {
      wl_resource_post_error (surface_resource,
                              WL_DISPLAY_ERROR_INVALID_OBJECT,
                              "wl_shell::get_shell_surface already requested");
      return;
    }

  shell_surface = g_new0 (MetaWaylandShellSurface, 1);

  /* a shell surface inherits the version from the shell */
  shell_surface->resource =
    wl_resource_create (client, &wl_shell_surface_interface,
			wl_resource_get_version (resource), id);
  wl_resource_set_implementation (shell_surface->resource, &meta_wayland_shell_surface_interface,
				  shell_surface, destroy_shell_surface);

  shell_surface->surface = surface;
  shell_surface->surface_destroy_listener.notify = shell_handle_surface_destroy;
  wl_resource_add_destroy_listener (surface->resource,
                                    &shell_surface->surface_destroy_listener);
  surface->has_shell_surface = TRUE;
}

static const struct wl_shell_interface meta_wayland_shell_interface =
{
  get_shell_surface
};

static void
bind_shell (struct wl_client *client,
            void *data,
            guint32 version,
            guint32 id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_shell_interface, version, id);
  wl_resource_set_implementation (resource, &meta_wayland_shell_interface, data, NULL);
}

static char *
create_lockfile (int display, int *display_out)
{
  char *filename;
  int size;
  char pid[11];
  int fd;

  do
    {
      char *end;
      pid_t other;

      filename = g_strdup_printf ("/tmp/.X%d-lock", display);
      fd = open (filename, O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL, 0444);

      if (fd < 0 && errno == EEXIST)
        {
          fd = open (filename, O_CLOEXEC, O_RDONLY);
          if (fd < 0 || read (fd, pid, 11) != 11)
            {
              const char *msg = strerror (errno);
              g_warning ("can't read lock file %s: %s", filename, msg);
              g_free (filename);

              /* ignore error and try the next display number */
              display++;
              continue;
          }
          close (fd);

          other = strtol (pid, &end, 0);
          if (end != pid + 10)
            {
              g_warning ("can't parse lock file %s", filename);
              g_free (filename);

              /* ignore error and try the next display number */
              display++;
              continue;
          }

          if (kill (other, 0) < 0 && errno == ESRCH)
            {
              g_warning ("unlinking stale lock file %s", filename);
              if (unlink (filename) < 0)
                {
                  const char *msg = strerror (errno);
                  g_warning ("failed to unlink stale lock file: %s", msg);
                  display++;
                }
              g_free (filename);
              continue;
          }

          g_free (filename);
          display++;
          continue;
        }
      else if (fd < 0)
        {
          const char *msg = strerror (errno);
          g_warning ("failed to create lock file %s: %s", filename , msg);
          g_free (filename);
          return NULL;
        }

      break;
    }
  while (1);

  /* Subtle detail: we use the pid of the wayland compositor, not the xserver
   * in the lock file. */
  size = snprintf (pid, 11, "%10d\n", getpid ());
  if (size != 11 || write (fd, pid, 11) != 11)
    {
      unlink (filename);
      close (fd);
      g_warning ("failed to write pid to lock file %s", filename);
      g_free (filename);
      return NULL;
    }

  close (fd);

  *display_out = display;
  return filename;
}

static int
bind_to_abstract_socket (int display)
{
  struct sockaddr_un addr;
  socklen_t size, name_size;
  int fd;

  fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  addr.sun_family = AF_LOCAL;
  name_size = snprintf (addr.sun_path, sizeof addr.sun_path,
                        "%c/tmp/.X11-unix/X%d", 0, display);
  size = offsetof (struct sockaddr_un, sun_path) + name_size;
  if (bind (fd, (struct sockaddr *) &addr, size) < 0)
    {
      g_warning ("failed to bind to @%s: %s\n",
                 addr.sun_path + 1, strerror (errno));
      close (fd);
      return -1;
    }

  if (listen (fd, 1) < 0)
    {
      close (fd);
      return -1;
    }

  return fd;
}

static int
bind_to_unix_socket (int display)
{
  struct sockaddr_un addr;
  socklen_t size, name_size;
  int fd;

  fd = socket (PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  addr.sun_family = AF_LOCAL;
  name_size = snprintf (addr.sun_path, sizeof addr.sun_path,
                        "/tmp/.X11-unix/X%d", display) + 1;
  size = offsetof (struct sockaddr_un, sun_path) + name_size;
  unlink (addr.sun_path);
  if (bind (fd, (struct sockaddr *) &addr, size) < 0)
    {
      char *msg = strerror (errno);
      g_warning ("failed to bind to %s (%s)\n", addr.sun_path, msg);
      close (fd);
      return -1;
    }

  if (listen (fd, 1) < 0) {
      unlink (addr.sun_path);
      close (fd);
      return -1;
  }

  return fd;
}

static void
uncloexec (gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);

  /* Make sure the client end of the socket pair doesn't get closed
   * when we exec xwayland. */
  int flags = fcntl (fd, F_GETFD);
  if (flags != -1)
    fcntl (fd, F_SETFD, flags & ~FD_CLOEXEC);
}

static void
xserver_died (GPid     pid,
	      gint     status,
	      gpointer user_data)
{
  if (!WIFEXITED (status))
    g_error ("X Wayland crashed; aborting");
  else
    {
      /* For now we simply abort if we see the server exit.
       *
       * In the future X will only be loaded lazily for legacy X support
       * but for now it's a hard requirement. */
      g_error ("Spurious exit of X Wayland server");
    }
}

static gboolean
start_xwayland (MetaWaylandCompositor *compositor)
{
  int display = 0;
  char *lockfile = NULL;
  int sp[2];
  pid_t pid;
  char **env;
  char *fd_string;
  char *display_name;
  char *args[11];
  GError *error;

  do
    {
      lockfile = create_lockfile (display, &display);
      if (!lockfile)
        {
         g_warning ("Failed to create an X lock file");
         return FALSE;
        }

      compositor->xwayland_abstract_fd = bind_to_abstract_socket (display);
      if (compositor->xwayland_abstract_fd < 0)
        {
          if (errno == EADDRINUSE)
            {
              unlink (lockfile);
              display++;
              continue;
            }

          compositor->xwayland_unix_fd = bind_to_unix_socket (display);
          if (compositor->xwayland_abstract_fd < 0)
            {
              unlink (lockfile);

              if (errno == EADDRINUSE)
                {
                  display++;
                  continue;
                }
              else
                return FALSE;
            }
        }

      break;
    }
  while (1);

  compositor->xwayland_display_index = display;
  compositor->xwayland_lockfile = lockfile;

  /* We want xwayland to be a wayland client so we make a socketpair to setup a
   * wayland protocol connection. */
  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) < 0)
    {
      g_warning ("socketpair failed\n");
      unlink (lockfile);
      return 1;
    }

  env = g_get_environ ();
  fd_string = g_strdup_printf ("%d", sp[1]);
  env = g_environ_setenv (env, "WAYLAND_SOCKET", fd_string, TRUE);
  g_free (fd_string);

  display_name = g_strdup_printf (":%d",
				  compositor->xwayland_display_index);

  args[0] = XWAYLAND_PATH;
  args[1] = display_name;
  args[2] = "-wayland";
  args[3] = "-rootless";
  args[4] = "-retro";
  args[5] = "-noreset";
  args[6] = "-logfile";
  args[7] = g_build_filename (g_get_user_cache_dir (), "xwayland.log", NULL);
  args[8] = "-nolisten";
  args[9] = "all";
  args[10] = NULL;

  error = NULL;
  if (g_spawn_async (NULL, /* cwd */
		     args,
		     env,
		     G_SPAWN_LEAVE_DESCRIPTORS_OPEN |
		     G_SPAWN_DO_NOT_REAP_CHILD |
		     G_SPAWN_STDOUT_TO_DEV_NULL |
		     G_SPAWN_STDERR_TO_DEV_NULL,
		     uncloexec,
		     GINT_TO_POINTER (sp[1]),
		     &pid,
		     &error))
    {
      g_message ("forked X server, pid %d\n", pid);

      close (sp[1]);
      compositor->xwayland_client =
        wl_client_create (compositor->wayland_display, sp[0]);

      compositor->xwayland_pid = pid;
      g_child_watch_add (pid, xserver_died, NULL);
    }
  else
    {
      g_error ("Failed to fork for xwayland server: %s", error->message);
    }

  g_strfreev (env);
  g_free (display_name);

  return TRUE;
}

static void
stop_xwayland (MetaWaylandCompositor *compositor)
{
  char path[256];

  snprintf (path, sizeof path, "/tmp/.X%d-lock",
            compositor->xwayland_display_index);
  unlink (path);
  snprintf (path, sizeof path, "/tmp/.X11-unix/X%d",
            compositor->xwayland_display_index);
  unlink (path);

  unlink (compositor->xwayland_lockfile);
}

static void
xserver_set_window_id (struct wl_client *client,
                       struct wl_resource *compositor_resource,
                       struct wl_resource *surface_resource,
                       guint32 xid)
{
  MetaWaylandCompositor *compositor =
    wl_resource_get_user_data (compositor_resource);
  MetaWaylandSurface *surface = wl_resource_get_user_data (surface_resource);
  MetaDisplay *display = meta_get_display ();
  MetaWindow *window;

  g_return_if_fail (surface->xid == None);

  surface->xid = xid;

  g_hash_table_insert (compositor->window_surfaces, &xid, surface);

  window  = meta_display_lookup_x_window (display, xid);
  if (window)
    {
      MetaWindowActor *window_actor =
        META_WINDOW_ACTOR (meta_window_get_compositor_private (window));

      meta_window_actor_set_wayland_surface (window_actor, surface);

      surface->window = window;
      window->surface = surface;

      /* If the MetaWindow becomes unmanaged (surface->window will be
       * freed in this case) we need to make sure to clear our
       * ->window pointers in this case. */
      g_object_weak_ref (G_OBJECT (surface->window),
                         window_destroyed_cb,
                         surface);

      /* If the window is already meant to have focus then the
       * original attempt to call this in response to the FocusIn
       * event will have been lost because there was no surface
       * yet. */
      if (window->has_focus)
        meta_wayland_compositor_set_input_focus (compositor, window);

    }
#warning "FIXME: Handle surface destroy and remove window_surfaces mapping"
}

MetaWaylandSurface *
meta_wayland_lookup_surface_for_xid (guint32 xid)
{
  return g_hash_table_lookup (_meta_wayland_compositor.window_surfaces, &xid);
}

static const struct xserver_interface xserver_implementation = {
    xserver_set_window_id
};

static void
bind_xserver (struct wl_client *client,
	      void *data,
              guint32 version,
              guint32 id)
{
  MetaWaylandCompositor *compositor = data;

  /* If it's a different client than the xserver we launched,
   * don't start the wm. */
  if (client != compositor->xwayland_client)
    return;

  compositor->xserver_resource =
    wl_resource_create (client, &xserver_interface, version, id);
  wl_resource_set_implementation (compositor->xserver_resource,
				  &xserver_implementation, compositor, NULL);

  wl_resource_post_event (compositor->xserver_resource,
                          XSERVER_LISTEN_SOCKET,
                          compositor->xwayland_abstract_fd);

  wl_resource_post_event (compositor->xserver_resource,
                          XSERVER_LISTEN_SOCKET,
                          compositor->xwayland_unix_fd);

  /* Make sure xwayland will recieve the above sockets in a finite
   * time before unblocking the initialization mainloop since we are
   * then going to immediately try and connect to those as the window
   * manager. */
  wl_client_flush (client);

  /* At this point xwayland is all setup to start accepting
   * connections so we can quit the transient initialization mainloop
   * and unblock meta_wayland_init() to continue initializing mutter.
   * */
  g_main_loop_quit (compositor->init_loop);
  compositor->init_loop = NULL;
}

static void
stage_destroy_cb (void)
{
  meta_quit (META_EXIT_SUCCESS);
}

static gboolean
event_cb (ClutterActor *stage,
          const ClutterEvent *event,
          MetaWaylandCompositor *compositor)
{
  MetaWaylandSeat *seat = compositor->seat;
  MetaWaylandPointer *pointer = &seat->pointer;
  MetaWaylandSurface *surface;
  MetaDisplay *display;
  XMotionEvent xevent;

  meta_wayland_seat_handle_event (compositor->seat, event);

  /* HACK: for now, the surfaces from Wayland clients aren't
     integrated into Mutter's stacking and Mutter won't give them
     focus on mouse clicks. As a hack to work around this we can just
     give them input focus on mouse clicks so we can at least test the
     keyboard support */
  if (event->type == CLUTTER_BUTTON_PRESS)
    {
      surface = pointer->current;

      /* Only focus surfaces that wouldn't be handled by the
         corresponding X events */
      if (surface && surface->xid == 0)
        {
          meta_wayland_keyboard_set_focus (&seat->keyboard, surface);
          meta_wayland_data_device_set_keyboard_focus (seat);
        }
    }

  meta_wayland_stage_set_cursor_position (META_WAYLAND_STAGE (stage),
                                          wl_fixed_to_int (pointer->x),
                                          wl_fixed_to_int (pointer->y));

  if (pointer->current == NULL)
    meta_wayland_stage_set_default_cursor (META_WAYLAND_STAGE (stage));

  display = meta_get_display ();
  if (!display)
    return FALSE;

  /* We want to synthesize X events for mouse motion events so that we
     don't have to rely on the X server's window position being
     synched with the surface positoin. See the comment in
     event_callback() in display.c */

  switch (event->type)
    {
    case CLUTTER_BUTTON_PRESS:
      if (compositor->implicit_grab_surface == NULL)
        {
          compositor->implicit_grab_button = event->button.button;
          compositor->implicit_grab_surface = pointer->current;
        }
      return FALSE;

    case CLUTTER_BUTTON_RELEASE:
      if (event->type == CLUTTER_BUTTON_RELEASE &&
          compositor->implicit_grab_surface &&
          event->button.button == compositor->implicit_grab_button)
        compositor->implicit_grab_surface = NULL;
      return FALSE;

    case CLUTTER_MOTION:
      break;

    default:
      return FALSE;
    }

  xevent.type = MotionNotify;
  xevent.is_hint = NotifyNormal;
  xevent.same_screen = TRUE;
  xevent.serial = 0;
  xevent.send_event = False;
  xevent.display = display->xdisplay;
  xevent.root = DefaultRootWindow (display->xdisplay);

  if (compositor->implicit_grab_surface)
    surface = compositor->implicit_grab_surface;
  else
    surface = pointer->current;

  if (surface == pointer->current)
    {
      xevent.x = wl_fixed_to_int (pointer->current_x);
      xevent.y = wl_fixed_to_int (pointer->current_y);
    }
  else if (surface && surface->window)
    {
      ClutterActor *window_actor =
        CLUTTER_ACTOR (meta_window_get_compositor_private (surface->window));
      float ax, ay;

      if (window_actor)
        {
          clutter_actor_transform_stage_point (window_actor,
                                               wl_fixed_to_double (pointer->x),
                                               wl_fixed_to_double (pointer->y),
                                               &ax, &ay);

          xevent.x = ax;
          xevent.y = ay;
        }
      else
        {
          xevent.x = wl_fixed_to_int (pointer->x);
          xevent.y = wl_fixed_to_int (pointer->y);
        }
    }
  else
    {
      xevent.x = wl_fixed_to_int (pointer->x);
      xevent.y = wl_fixed_to_int (pointer->y);
    }

  if (surface && surface->xid != None)
    xevent.window = surface->xid;
  else
    xevent.window = xevent.root;

  /* Mutter doesn't really know about the sub-windows. This assumes it
     doesn't care either */
  xevent.subwindow = xevent.window;
  xevent.time = event->any.time;
  xevent.x_root = wl_fixed_to_int (pointer->x);
  xevent.y_root = wl_fixed_to_int (pointer->y);
  /* The Clutter state flags exactly match the X values */
  xevent.state = clutter_event_get_state (event);

  meta_display_handle_event (display, (XEvent *) &xevent);

  return FALSE;
}

static gboolean
event_emission_hook_cb (GSignalInvocationHint *ihint,
                        guint n_param_values,
                        const GValue *param_values,
                        gpointer data)
{
  MetaWaylandCompositor *compositor = data;
  ClutterActor *actor;
  ClutterEvent *event;

  g_return_val_if_fail (n_param_values == 2, FALSE);

  actor = g_value_get_object (param_values + 0);
  event = g_value_get_boxed (param_values + 1);

  if (actor == NULL)
    return TRUE /* stay connected */;

  /* If this event belongs to the corresponding grab for this event
   * type then the captured-event signal won't be emitted so we have
   * to manually forward it on */

  switch (event->type)
    {
      /* Pointer events */
    case CLUTTER_MOTION:
    case CLUTTER_ENTER:
    case CLUTTER_LEAVE:
    case CLUTTER_BUTTON_PRESS:
    case CLUTTER_BUTTON_RELEASE:
    case CLUTTER_SCROLL:
      if (actor == clutter_get_pointer_grab ())
        event_cb (clutter_actor_get_stage (actor),
                  event,
                  compositor);
      break;

      /* Keyboard events */
    case CLUTTER_KEY_PRESS:
    case CLUTTER_KEY_RELEASE:
      if (actor == clutter_get_keyboard_grab ())
        event_cb (clutter_actor_get_stage (actor),
                  event,
                  compositor);

    default:
      break;
    }

  return TRUE /* stay connected */;
}

static int
env_get_fd (const char *env)
{
  const char *value;

  value = g_getenv (env);

  if (value == NULL)
    return -1;
  else
    return g_ascii_strtoll (value, NULL, 10);
}

static void
on_our_vt_enter (MetaTTY               *tty,
		 MetaWaylandCompositor *compositor)
{
  GError *error;

  error = NULL;
  if (!meta_weston_launch_set_master (compositor->weston_launch,
				      compositor->drm_fd, TRUE, &error))
    {
      g_warning ("Failed to become DRM master: %s", error->message);
      g_error_free (error);
    }

  clutter_evdev_reclaim_devices ();
}

static void
on_our_vt_leave (MetaTTY               *tty,
		 MetaWaylandCompositor *compositor)
{
  GError *error;

  error = NULL;
  if (!meta_weston_launch_set_master (compositor->weston_launch,
				      compositor->drm_fd, FALSE, &error))
    {
      g_warning ("Failed to release DRM master: %s", error->message);
      g_error_free (error);
    }

  clutter_evdev_release_devices ();
}

static int
on_evdev_device_open (const char  *path,
		      int          flags,
		      gpointer     user_data,
		      GError     **error)
{
  MetaWaylandCompositor *compositor = user_data;

  return meta_weston_launch_open_input_device (compositor->weston_launch,
					       path, flags, error);
}

static void
on_monitors_changed (MetaMonitorManager    *monitors,
		     MetaWaylandCompositor *compositor)
{
  g_list_free_full (compositor->outputs, (GDestroyNotify) wl_global_destroy);
  meta_wayland_compositor_create_outputs (compositor, monitors);
}

void
meta_wayland_init (void)
{
  MetaWaylandCompositor *compositor = &_meta_wayland_compositor;
  guint event_signal;
  ClutterBackend *backend;
  CoglContext *cogl_context;
  CoglRenderer *cogl_renderer;
  int weston_launch_fd;
  MetaMonitorManager *monitors;

  memset (compositor, 0, sizeof (MetaWaylandCompositor));

  compositor->wayland_display = wl_display_create ();
  if (compositor->wayland_display == NULL)
    g_error ("failed to create wayland display");

  wl_display_init_shm (compositor->wayland_display);

  wl_list_init (&compositor->frame_callbacks);

  if (!wl_global_create (compositor->wayland_display,
			 &wl_compositor_interface, 1,
			 compositor, compositor_bind))
    g_error ("Failed to register wayland compositor object");

  compositor->wayland_loop =
    wl_display_get_event_loop (compositor->wayland_display);
  compositor->wayland_event_source =
    wayland_event_source_new (compositor->wayland_display);

  /* XXX: Here we are setting the wayland event source to have a
   * slightly lower priority than the X event source, because we are
   * much more likely to get confused being told about surface changes
   * relating to X clients when we don't know what's happened to them
   * according to the X protocol.
   *
   * At some point we could perhaps try and get the X protocol proxied
   * over the wayland protocol so that we don't have to worry about
   * synchronizing the two command streams. */
  g_source_set_priority (compositor->wayland_event_source,
                         GDK_PRIORITY_EVENTS + 1);
  g_source_attach (compositor->wayland_event_source, NULL);

  clutter_wayland_set_compositor_display (compositor->wayland_display);

  /* We need to set this before clutter_init(), so we do it unconditionally.
     It doesn't harm anyway to do it under X11 */
  weston_launch_fd = env_get_fd ("WESTON_LAUNCHER_SOCK");
  if (weston_launch_fd >= 0)
    compositor->weston_launch = g_socket_new_from_fd (weston_launch_fd, NULL);
  clutter_evdev_set_open_callback (on_evdev_device_open, compositor);

  if (clutter_init (NULL, NULL) != CLUTTER_INIT_SUCCESS)
    g_error ("Failed to initialize Clutter");

  backend = clutter_get_default_backend ();
  cogl_context = clutter_backend_get_cogl_context (backend);
  cogl_renderer = cogl_display_get_renderer (cogl_context_get_display (cogl_context));

  if (cogl_renderer_get_winsys_id (cogl_renderer) == COGL_WINSYS_ID_EGL_KMS)
    compositor->drm_fd = cogl_kms_renderer_get_kms_fd (cogl_renderer);
  else
    compositor->drm_fd = -1;

  if (compositor->drm_fd >= 0)
    {
      GError *error;

      /* Running on bare metal, let's initalize DRM master and VT handling */
      compositor->tty = meta_tty_new ();
      if (compositor->tty)
	{
	  g_signal_connect (compositor->tty, "enter", G_CALLBACK (on_our_vt_enter), compositor);
	  g_signal_connect (compositor->tty, "leave", G_CALLBACK (on_our_vt_leave), compositor);
	}

      error = NULL;
      if (!meta_weston_launch_set_master (compositor->weston_launch,
					  compositor->drm_fd, TRUE, &error))
	{
	  g_error ("Failed to become DRM master: %s", error->message);
	  g_error_free (error);
	}
    }

  compositor->stage = meta_wayland_stage_new ();
  /* FIXME */
  clutter_actor_set_size (CLUTTER_ACTOR (compositor->stage), 1024, 768);
  clutter_stage_set_user_resizable (CLUTTER_STAGE (compositor->stage), FALSE);
  g_signal_connect_after (compositor->stage, "paint",
                          G_CALLBACK (paint_finished_cb), compositor);
  g_signal_connect (compositor->stage, "destroy",
                    G_CALLBACK (stage_destroy_cb), NULL);

  meta_monitor_manager_initialize (NULL);
  monitors = meta_monitor_manager_get ();
  g_signal_connect (monitors, "monitors-changed",
		    G_CALLBACK (on_monitors_changed), compositor);
  meta_wayland_compositor_create_outputs (compositor, monitors);

  meta_wayland_data_device_manager_init (compositor->wayland_display);

  compositor->seat = meta_wayland_seat_new (compositor->wayland_display);

  g_signal_connect (compositor->stage,
                    "captured-event",
                    G_CALLBACK (event_cb),
                    compositor);
  /* If something sets a grab on an actor then the captured event
   * signal won't get emitted but we still want to see these events so
   * we can update the cursor position. To make sure we see all events
   * we also install an emission hook on the event signal */
  event_signal = g_signal_lookup ("event", CLUTTER_TYPE_STAGE);
  g_signal_add_emission_hook (event_signal,
                              0 /* detail */,
                              event_emission_hook_cb,
                              compositor, /* hook_data */
                              NULL /* data_destroy */);

  if (wl_global_create (compositor->wayland_display,
			&wl_shell_interface, 1,
			compositor, bind_shell) == NULL)
    g_error ("Failed to register a global shell object");

  clutter_actor_show (compositor->stage);

  if (wl_display_add_socket (compositor->wayland_display, "wayland-0"))
    g_error ("Failed to create socket");

  wl_global_create (compositor->wayland_display,
		    &xserver_interface, 1,
		    compositor, bind_xserver);

  /* We need a mapping from xids to wayland surfaces... */
  compositor->window_surfaces = g_hash_table_new (g_int_hash, g_int_equal);

  /* XXX: It's important that we only try and start xwayland after we
   * have initialized EGL because EGL implements the "wl_drm"
   * interface which xwayland requires to determine what drm device
   * name it should use.
   *
   * By waiting until we've shown the stage above we ensure that the
   * underlying GL resources for the surface have also been allocated
   * and so EGL must be initialized by this point.
   */

  if (!start_xwayland (compositor))
    g_error ("Failed to start X Wayland");

  putenv (g_strdup_printf ("DISPLAY=:%d", compositor->xwayland_display_index));

  /* We need to run a mainloop until we know xwayland has a binding
   * for our xserver interface at which point we can assume it's
   * ready to start accepting connections. */
  compositor->init_loop = g_main_loop_new (NULL, FALSE);

  g_main_loop_run (compositor->init_loop);
}

void
meta_wayland_finalize (void)
{
  MetaWaylandCompositor *compositor;

  compositor = meta_wayland_compositor_get_default ();

  stop_xwayland (compositor);
  g_clear_object (&compositor->tty);
}

MetaTTY *
meta_wayland_compositor_get_tty (MetaWaylandCompositor *compositor)
{
  return compositor->tty;
}

gboolean
meta_wayland_compositor_is_native (MetaWaylandCompositor *compositor)
{
  return compositor->drm_fd >= 0;
}
