/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* 
 * Copyright (C) 2001, 2002 Havoc Pennington
 * Copyright (C) 2002, 2003 Red Hat Inc.
 * Some ICCCM manager selection code derived from fvwm2,
 * Copyright (C) 2001 Dominik Vogt, Matthias Clasen, and fvwm2 team
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
 * Copyright (C) 2013 Red Hat Inc.
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

#include "config.h"

#include <string.h>
#include <clutter/clutter.h>

#ifdef HAVE_RANDR
#include <X11/extensions/Xrandr.h>
#endif

#include <meta/main.h>
#include "monitor-private.h"
#ifdef HAVE_WAYLAND
#include "meta-wayland-private.h"
#endif

#include "meta-dbus-xrandr.h"

struct _MetaMonitorManager
{
  GObject parent_instance;

  /* XXX: this structure is very badly
     packed, but I like the logical organization
     of fields */

  unsigned int serial;

  /* Outputs refer to physical screens,
     CRTCs refer to stuff that can drive outputs
     (like encoders, but less tied to the HW),
     while monitor_infos refer to logical ones.

     See also the comment in monitor-private.h
  */
  MetaOutput *outputs;
  unsigned int n_outputs;

  MetaMonitorMode *modes;
  unsigned int n_modes;

  MetaCRTC *crtcs;
  unsigned int n_crtcs;

  MetaMonitorInfo *monitor_infos;
  unsigned int n_monitor_infos;
  int primary_monitor_index;

#ifdef HAVE_RANDR
  Display *xdisplay;
#endif

  int dbus_name_id;
  MetaDBusDisplayConfig *skeleton;
};

struct _MetaMonitorManagerClass
{
  GObjectClass parent_class;
};

enum {
  MONITORS_CHANGED,
  SIGNALS_LAST
};

static int signals[SIGNALS_LAST];

G_DEFINE_TYPE (MetaMonitorManager, meta_monitor_manager, G_TYPE_OBJECT);

static void
make_dummy_monitor_config (MetaMonitorManager *manager)
{
  manager->modes = g_new0 (MetaMonitorMode, 1);
  manager->n_modes = 1;

  manager->modes[0].mode_id = 1;
  if (manager->xdisplay)
    {
      Screen *screen = ScreenOfDisplay (manager->xdisplay,
                                        DefaultScreen (manager->xdisplay));

      manager->modes[0].width = WidthOfScreen (screen);
      manager->modes[0].height = HeightOfScreen (screen);
    }
  else
    {
      manager->modes[0].width = 1024;
      manager->modes[0].height = 768;
    }
  manager->modes[0].refresh_rate = 60.0;

  manager->crtcs = g_new0 (MetaCRTC, 1);
  manager->n_crtcs = 1;

  manager->crtcs[0].crtc_id = 2;
  manager->crtcs[0].rect.x = 0;
  manager->crtcs[0].rect.y = 0;
  manager->crtcs[0].rect.width = manager->modes[0].width;
  manager->crtcs[0].rect.height = manager->modes[0].height;
  manager->crtcs[0].current_mode = &manager->modes[0];

  manager->outputs = g_new0 (MetaOutput, 1);
  manager->n_outputs = 1;

  manager->outputs[0].crtc = &manager->crtcs[0];
  manager->outputs[0].output_id = 3;
  manager->outputs[0].name = g_strdup ("LVDS");
  manager->outputs[0].vendor = g_strdup ("unknown");
  manager->outputs[0].product = g_strdup ("unknown");
  manager->outputs[0].serial = g_strdup ("");
  manager->outputs[0].width_mm = 222;
  manager->outputs[0].height_mm = 125;
  manager->outputs[0].subpixel_order = COGL_SUBPIXEL_ORDER_UNKNOWN;
  manager->outputs[0].preferred_mode = &manager->modes[0];
  manager->outputs[0].n_modes = 1;
  manager->outputs[0].modes = g_new0 (MetaMonitorMode *, 1);
  manager->outputs[0].modes[0] = &manager->modes[0];
  manager->outputs[0].n_possible_crtcs = 1;
  manager->outputs[0].possible_crtcs = g_new0 (MetaCRTC *, 1);
  manager->outputs[0].possible_crtcs[0] = &manager->crtcs[0];
  manager->outputs[0].n_possible_clones = 0;
  manager->outputs[0].possible_clones = g_new0 (MetaOutput *, 0);
}

#ifdef HAVE_RANDR

static void
read_monitor_infos_from_xrandr (MetaMonitorManager *manager)
{
    XRRScreenResources *resources;
    RROutput primary_output;
    unsigned int i, j, k;
    unsigned int n_actual_outputs;

    resources = XRRGetScreenResourcesCurrent (manager->xdisplay,
                                              DefaultRootWindow (manager->xdisplay));
    if (!resources)
      return make_dummy_monitor_config (manager);

    manager->n_outputs = resources->noutput;
    manager->n_crtcs = resources->ncrtc;
    manager->n_modes = resources->nmode;
    manager->outputs = g_new0 (MetaOutput, manager->n_outputs);
    manager->modes = g_new0 (MetaMonitorMode, manager->n_modes);
    manager->crtcs = g_new0 (MetaCRTC, manager->n_crtcs);

    for (i = 0; i < (unsigned)resources->nmode; i++)
      {
        XRRModeInfo *xmode = &resources->modes[i];
        MetaMonitorMode *mode;

        mode = &manager->modes[i];

        mode->mode_id = xmode->id;
        mode->width = xmode->width;
        mode->height = xmode->height;
        mode->refresh_rate = (xmode->dotClock /
                              ((float)xmode->hTotal * xmode->vTotal));
      }

    for (i = 0; i < (unsigned)resources->ncrtc; i++)
      {
        XRRCrtcInfo *crtc;
        MetaCRTC *meta_crtc;

        crtc = XRRGetCrtcInfo (manager->xdisplay, resources, resources->crtcs[i]);

        meta_crtc = &manager->crtcs[i];

        meta_crtc->crtc_id = resources->crtcs[i];
        meta_crtc->rect.x = crtc->x;
        meta_crtc->rect.y = crtc->y;
        meta_crtc->rect.width = crtc->width;
        meta_crtc->rect.height = crtc->height;

        for (j = 0; j < (unsigned)resources->nmode; j++)
          {
            if (resources->modes[j].id == crtc->mode)
              {
                meta_crtc->current_mode = &manager->modes[j];
                break;
              }
          }

        XRRFreeCrtcInfo (crtc);
      }

    primary_output = XRRGetOutputPrimary (manager->xdisplay,
                                          DefaultRootWindow (manager->xdisplay));

    n_actual_outputs = 0;
    for (i = 0; i < (unsigned)resources->noutput; i++)
      {
        XRROutputInfo *output;
        MetaOutput *meta_output;

        output = XRRGetOutputInfo (manager->xdisplay, resources, resources->outputs[i]);

        meta_output = &manager->outputs[n_actual_outputs];

        if (output->connection != RR_Disconnected)
          {
            meta_output->output_id = resources->outputs[i];
            meta_output->name = g_strdup (output->name);
            meta_output->vendor = g_strdup ("unknown");
            meta_output->product = g_strdup ("unknown");
            meta_output->serial = g_strdup ("");
            meta_output->width_mm = output->mm_width;
            meta_output->height_mm = output->mm_height;
            meta_output->subpixel_order = COGL_SUBPIXEL_ORDER_UNKNOWN;

            meta_output->n_modes = output->nmode;
            meta_output->modes = g_new0 (MetaMonitorMode *, meta_output->n_modes);
            for (j = 0; j < meta_output->n_modes; j++)
              {
                for (k = 0; k < manager->n_modes; k++)
                  {
                    if (output->modes[j] == (XID)manager->modes[k].mode_id)
                      {
                        meta_output->modes[j] = &manager->modes[k];
                        break;
                      }
                  }
              }
            meta_output->preferred_mode = &meta_output->modes[0];

            meta_output->n_possible_crtcs = output->ncrtc;
            meta_output->possible_crtcs = g_new0 (MetaCRTC *, meta_output->n_possible_crtcs);
            for (j = 0; j < (unsigned)output->ncrtc; j++)
              {
                for (k = 0; k < manager->n_crtcs; k++)
                  {
                    if ((XID)manager->crtcs[k].crtc_id == output->crtcs[j])
                      {
                        meta_output->possible_crtcs[j] = &manager->crtcs[k];
                        break;
                      }
                  }
              }

            meta_output->crtc = NULL;
            for (j = 0; j < manager->n_crtcs; j++)
              {
                if ((XID)manager->crtcs[j].crtc_id == output->crtc)
                  {
                    meta_output->crtc = &manager->crtcs[j];
                    break;
                  }
              }

            meta_output->n_possible_clones = output->nclone;
            meta_output->possible_clones = g_new0 (MetaOutput *, meta_output->n_possible_clones);
            /* We can build the list of clones now, because we don't have the list of outputs
               yet, so temporarily set the pointers to the bare XIDs, and then we'll fix them
               in a second pass
            */
            for (j = 0; j < (unsigned)output->nclone; j++)
              {
                meta_output->possible_clones = GINT_TO_POINTER (output->clones[j]);
              }

            meta_output->is_primary = ((XID)meta_output->output_id == primary_output);
            meta_output->is_presentation = FALSE;

            n_actual_outputs++;
          }

        XRRFreeOutputInfo (output);
      }

    manager->n_outputs = n_actual_outputs;

    /* Now fix the clones */
    for (i = 0; i < manager->n_outputs; i++)
      {
        MetaOutput *meta_output;

        meta_output = &manager->outputs[i];

        for (j = 0; j < meta_output->n_possible_clones; j++)
          {
            RROutput clone = GPOINTER_TO_INT (meta_output->possible_clones[j]);

            for (k = 0; k < manager->n_outputs; k++)
              {
                if (clone == (XID)manager->outputs[k].output_id)
                  {
                    meta_output->possible_clones[j] = &manager->outputs[k];
                    break;
                  }
              }
          }
      }

    XRRFreeScreenResources (resources);
}

#endif

static MetaMonitorMode *
find_or_create_monitor_mode (GArray          *modes,
                             int              width,
                             int              height,
                             float            refresh)
{
  unsigned int i;
  MetaMonitorMode new;

  for (i = 0; i < modes->len; i++)
    {
      MetaMonitorMode *existing;

      existing = &g_array_index (modes, MetaMonitorMode, i);
      if (existing->width == width &&
          existing->height == height &&
          existing->refresh_rate == refresh)
        return existing;
    }

  new.mode_id = 1 + modes->len;
  new.width = width;
  new.height = height;
  new.refresh_rate = refresh;

  g_array_append_val (modes, new);
  return &g_array_index (modes, MetaMonitorMode, modes->len - 1);
}
    
static void
read_monitor_from_cogl_helper (CoglOutput *output,
                               void       *user_data)
{
  GList **closure = user_data;

  *closure = g_list_prepend (*closure, cogl_object_ref (output));
}

static void
read_monitor_infos_from_cogl (MetaMonitorManager *manager)
{
  GList *iter, *output_list;
  GArray *modes;
  GArray *crtcs;
  GArray *outputs;
  ClutterBackend *backend;
  CoglContext *cogl_context;
  CoglRenderer *cogl_renderer;
  int n;

  backend = clutter_get_default_backend ();
  cogl_context = clutter_backend_get_cogl_context (backend);
  cogl_renderer = cogl_display_get_renderer (cogl_context_get_display (cogl_context));

  output_list = NULL;
  cogl_renderer_foreach_output (cogl_renderer,
                                read_monitor_from_cogl_helper, &output_list);

  if (output_list == NULL)
    return make_dummy_monitor_config (manager);

  n = g_list_length (output_list);
  modes = g_array_new (FALSE, TRUE, sizeof (MetaMonitorMode));
  crtcs = g_array_sized_new (FALSE, TRUE, sizeof (MetaCRTC), n);
  outputs = g_array_sized_new (FALSE, TRUE, sizeof (MetaOutput), n);
  
  for (iter = output_list; iter; iter = iter->next)
    {
      /* First create all modes, so that we can reference them later
         without the risk to resize the array
      */
      find_or_create_monitor_mode (modes,
                                   cogl_output_get_width (iter->data),
                                   cogl_output_get_height (iter->data),
                                   cogl_output_get_refresh_rate (iter->data));
    }

  for (iter = output_list; iter; iter = iter->next)
    {
      CoglOutput *output;
      MetaCRTC meta_crtc;
      MetaOutput meta_output;

      output = iter->data;

      meta_crtc.crtc_id = 1 + crtcs->len;
      meta_crtc.rect.x = cogl_output_get_x (output);
      meta_crtc.rect.y = cogl_output_get_y (output);
      meta_crtc.rect.width = cogl_output_get_width (output);
      meta_crtc.rect.height = cogl_output_get_height (output);
      meta_crtc.current_mode = find_or_create_monitor_mode (modes,
                                                            cogl_output_get_width (output),
                                                            cogl_output_get_height (output),
                                                            cogl_output_get_refresh_rate (output));
      meta_crtc.logical_monitor = NULL;

      /* This will never resize the array because we preallocated with g_array_sized_new() */
      g_array_append_val (crtcs, meta_crtc);

      meta_output.output_id = 1 + n + crtcs->len;
      meta_output.crtc = &g_array_index (crtcs, MetaCRTC, crtcs->len - 1);
      meta_output.name = g_strdup ("unknown");
      meta_output.vendor = g_strdup ("unknown");
      meta_output.product = g_strdup ("unknown");
      meta_output.serial = g_strdup ("");
      meta_output.width_mm = cogl_output_get_mm_width (output);
      meta_output.height_mm = cogl_output_get_mm_height (output);
      meta_output.subpixel_order = cogl_output_get_subpixel_order (output);
      meta_output.preferred_mode = meta_crtc.current_mode;
      meta_output.n_modes = 1;
      meta_output.modes = g_new (MetaMonitorMode *, 1);
      meta_output.modes[0] = meta_output.preferred_mode;
      meta_output.n_possible_crtcs = 1;
      meta_output.possible_crtcs = g_new (MetaCRTC *, 1);
      meta_output.possible_crtcs[0] = meta_output.crtc;
      meta_output.n_possible_clones = 0;
      meta_output.possible_clones = g_new (MetaOutput *, 0);
      meta_output.is_primary = (iter == output_list);
      meta_output.is_presentation = FALSE;

      g_array_append_val (outputs, meta_output);
    }

  manager->n_modes = modes->len;
  manager->modes = (void*)g_array_free (modes, FALSE);
  manager->n_crtcs = crtcs->len;
  manager->crtcs = (void*)g_array_free (crtcs, FALSE);
  manager->n_outputs = outputs->len;
  manager->outputs = (void*)g_array_free (outputs, FALSE);

  g_list_free_full (output_list, cogl_object_unref);
}

/*
 * meta_has_dummy_output:
 *
 * Returns TRUE if the only available monitor is the dummy one
 * backing the ClutterStage window.
 */
static gboolean
has_dummy_output (void)
{
#ifdef HAVE_WAYLAND
  MetaWaylandCompositor *compositor;

  if (!meta_is_display_server ())
    return FALSE;

  /* FIXME: actually, even in EGL-KMS mode, Cogl does not
     expose the outputs through CoglOutput - yet */
  compositor = meta_wayland_compositor_get_default ();
  return !meta_wayland_compositor_is_native (compositor);
#else
  return FALSE
#endif
}

static void
meta_monitor_manager_init (MetaMonitorManager *manager)
{
}

static gboolean
make_debug_config (MetaMonitorManager *manager)
{
  const char *env;

  env = g_getenv ("META_DEBUG_MULTIMONITOR");

  if (env == NULL)
    return FALSE;

#ifdef HAVE_RANDR
  if (strcmp (env, "xrandr") == 0)
    read_monitor_infos_from_xrandr (manager);
  else
#endif
    if (strcmp (env, "cogl") == 0)
      read_monitor_infos_from_cogl (manager);
    else
      make_dummy_monitor_config (manager);

  return TRUE;
}

static void
read_current_config (MetaMonitorManager *manager)
{
  if (make_debug_config (manager))
    return;

  if (has_dummy_output ())
    return make_dummy_monitor_config (manager);

#ifdef HAVE_RANDR
  if (!meta_is_display_server ())
    return read_monitor_infos_from_xrandr (manager);
#endif

  return read_monitor_infos_from_cogl (manager);
}

/*
 * make_logical_config:
 *
 * Turn outputs and CRTCs into logical MetaMonitorInfo,
 * that will be used by the core and API layer (MetaScreen
 * and friends)
 */
static void
make_logical_config (MetaMonitorManager *manager)
{
  GArray *monitor_infos;
  unsigned int i, j;

  monitor_infos = g_array_sized_new (FALSE, TRUE, sizeof (MetaMonitorInfo),
                                     manager->n_outputs);

  /* Walk the list of MetaCRTCs, and build a MetaMonitorInfo
     for each of them, unless they reference a rectangle that
     is already there.
  */
  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCRTC *crtc = &manager->crtcs[i];

      /* Ignore CRTCs not in use */
      if (crtc->current_mode == NULL)
        continue;

      for (j = 0; j < monitor_infos->len; j++)
        {
          MetaMonitorInfo *info = &g_array_index (monitor_infos, MetaMonitorInfo, i);
          if (meta_rectangle_equal (&crtc->rect,
                                    &info->rect))
            {
              crtc->logical_monitor = info;
              break;
            }
        }

      if (crtc->logical_monitor == NULL)
        {
          MetaMonitorInfo info;

          info.number = monitor_infos->len;
          info.rect = crtc->rect;
          info.is_primary = FALSE;
          /* This starts true because we want
             is_presentation only if all outputs are
             marked as such (while for primary it's enough
             that any is marked)
          */
          info.is_presentation = TRUE;
          info.in_fullscreen = -1;
          info.output_id = 0;

          g_array_append_val (monitor_infos, info);

          crtc->logical_monitor = &g_array_index (monitor_infos, MetaMonitorInfo,
                                                  info.number);
        }
    }

  /* Now walk the list of outputs applying extended properties (primary
     and presentation)
  */
  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output;
      MetaMonitorInfo *info;

      output = &manager->outputs[i];

      /* Ignore outputs that are not active */
      if (output->crtc == NULL)
        continue;

      /* We must have a logical monitor on every CRTC at this point */
      g_assert (output->crtc->logical_monitor != NULL);

      info = output->crtc->logical_monitor;

      info->is_primary = info->is_primary || output->is_primary;
      info->is_presentation = info->is_presentation && output->is_presentation;

      if (output->is_primary || info->output_id == 0)
        info->output_id = output->output_id;

      if (info->is_primary)
        manager->primary_monitor_index = info->number;
    }

  manager->n_monitor_infos = monitor_infos->len;
  manager->monitor_infos = (void*)g_array_free (monitor_infos, FALSE);
}

static MetaMonitorManager *
meta_monitor_manager_new (Display *display)
{
  MetaMonitorManager *manager;

  manager = g_object_new (META_TYPE_MONITOR_MANAGER, NULL);

  manager->xdisplay = display;

  read_current_config (manager);
  make_logical_config (manager);
  return manager;
}

static void
free_output_array (MetaOutput *old_outputs,
                   int         n_old_outputs)
{
  int i;

  for (i = 0; i < n_old_outputs; i++)
    {
      g_free (old_outputs[i].name);
      g_free (old_outputs[i].vendor);
      g_free (old_outputs[i].product);
      g_free (old_outputs[i].serial);
      g_free (old_outputs[i].modes);
      g_free (old_outputs[i].possible_crtcs);
      g_free (old_outputs[i].possible_clones);
    }

  g_free (old_outputs);
}

static void
meta_monitor_manager_finalize (GObject *object)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (object);

  free_output_array (manager->outputs, manager->n_outputs);
  g_free (manager->monitor_infos);
  g_free (manager->modes);
  g_free (manager->crtcs);

  G_OBJECT_CLASS (meta_monitor_manager_parent_class)->finalize (object);
}

static void
meta_monitor_manager_dispose (GObject *object)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (object);

  if (manager->dbus_name_id != 0)
    {
      g_bus_unown_name (manager->dbus_name_id);
      manager->dbus_name_id = 0;
    }

  g_clear_object (&manager->skeleton);

  G_OBJECT_CLASS (meta_monitor_manager_parent_class)->dispose (object);
}

static void
meta_monitor_manager_class_init (MetaMonitorManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_monitor_manager_dispose;
  object_class->finalize = meta_monitor_manager_finalize;

  signals[MONITORS_CHANGED] =
    g_signal_new ("monitors-changed",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  0,
                  NULL, NULL, NULL,
		  G_TYPE_NONE, 0);
}

static gboolean
handle_get_resources (MetaDBusDisplayConfig *skeleton,
                      GDBusMethodInvocation *invocation,
                      MetaMonitorManager    *manager)
{
  GVariantBuilder crtc_builder, output_builder, mode_builder;
  unsigned int i, j;

  g_variant_builder_init (&crtc_builder, G_VARIANT_TYPE ("a(uxiiiiiuaua{sv})"));
  g_variant_builder_init (&output_builder, G_VARIANT_TYPE ("a(uxiausauaua{sv})"));
  g_variant_builder_init (&mode_builder, G_VARIANT_TYPE ("a(uxuud)"));

  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCRTC *crtc = &manager->crtcs[i];
      GVariantBuilder transforms;

      g_variant_builder_init (&transforms, G_VARIANT_TYPE ("au"));
      g_variant_builder_add (&transforms, "u", 0); /* 0 = WL_OUTPUT_TRANSFORM_NORMAL */

      g_variant_builder_add (&crtc_builder, "(uxiiiiiuaua{sv})",
                             i, /* ID */
                             crtc->crtc_id,
                             (int)crtc->rect.x,
                             (int)crtc->rect.y,
                             (int)crtc->rect.width,
                             (int)crtc->rect.height,
                             (int)(crtc->current_mode ? crtc->current_mode - manager->modes : -1),
                             0, /* 0 = WL_OUTPUT_TRANSFORM_NORMAL */
                             &transforms,
                             NULL /* properties */);
    }

  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output = &manager->outputs[i];
      GVariantBuilder crtcs, modes, clones, properties;

      g_variant_builder_init (&crtcs, G_VARIANT_TYPE ("au"));
      for (j = 0; j < output->n_possible_crtcs; j++)
        g_variant_builder_add (&crtcs, "u",
                               (unsigned)(output->possible_crtcs[j] - manager->crtcs));

      g_variant_builder_init (&modes, G_VARIANT_TYPE ("au"));
      for (j = 0; j < output->n_modes; j++)
        g_variant_builder_add (&modes, "u",
                               (unsigned)(output->modes[j] - manager->modes));

      g_variant_builder_init (&clones, G_VARIANT_TYPE ("au"));
      for (j = 0; j < output->n_possible_clones; j++)
        g_variant_builder_add (&clones, "u",
                               (unsigned)(output->possible_clones[j] - manager->outputs));

      g_variant_builder_init (&properties, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&properties, "{sv}", "vendor",
                             g_variant_new_string (output->vendor));
      g_variant_builder_add (&properties, "{sv}", "product",
                             g_variant_new_string (output->product));
      g_variant_builder_add (&properties, "{sv}", "serial",
                             g_variant_new_string (output->serial));
      g_variant_builder_add (&properties, "{sv}", "primary",
                             g_variant_new_boolean (output->is_primary));
      g_variant_builder_add (&properties, "{sv}", "presentation",
                             g_variant_new_boolean (output->is_presentation));

      g_variant_builder_add (&output_builder, "(uxiausauaua{sv})",
                             i, /* ID */
                             output->output_id,
                             (int)(output->crtc ? output->crtc - manager->crtcs : -1),
                             &crtcs,
                             output->name,
                             &modes,
                             &clones,
                             &properties);
    }

  for (i = 0; i < manager->n_modes; i++)
    {
      MetaMonitorMode *mode = &manager->modes[i];

      g_variant_builder_add (&mode_builder, "(uxuud)",
                             i, /* ID */
                             mode->mode_id,
                             mode->width,
                             mode->height,
                             (double)mode->refresh_rate);
    }

  meta_dbus_display_config_complete_get_resources (skeleton,
                                                   invocation,
                                                   manager->serial,
                                                   g_variant_builder_end (&crtc_builder),
                                                   g_variant_builder_end (&output_builder),
                                                   g_variant_builder_end (&mode_builder));
  return FALSE;
}                     

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  MetaMonitorManager *manager = user_data;

  manager->skeleton = META_DBUS_DISPLAY_CONFIG (meta_dbus_display_config_skeleton_new ());

  g_signal_connect_object (manager->skeleton, "handle-get-resources",
                           G_CALLBACK (handle_get_resources), manager, 0);

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (manager->skeleton),
                                    connection,
                                    "/org/gnome/Mutter/DisplayConfig",
                                    NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  meta_topic (META_DEBUG_DBUS, "Acquired name %s\n", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  meta_topic (META_DEBUG_DBUS, "Lost or failed to acquire name %s\n", name);
}

static void
initialize_dbus_interface (MetaMonitorManager *manager)
{
  manager->dbus_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                          "org.gnome.Mutter.DisplayConfig",
                                          G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                                          (meta_get_replace_current_wm () ?
                                           G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                                          on_bus_acquired,
                                          on_name_acquired,
                                          on_name_lost,
                                          g_object_ref (manager),
                                          g_object_unref);
}

static MetaMonitorManager *global_manager;

void
meta_monitor_manager_initialize (Display *display)
{
  global_manager = meta_monitor_manager_new (display);

  initialize_dbus_interface (global_manager);
}

MetaMonitorManager *
meta_monitor_manager_get (void)
{
  g_assert (global_manager != NULL);

  return global_manager;
}

MetaMonitorInfo *
meta_monitor_manager_get_monitor_infos (MetaMonitorManager *manager,
                                        int                *n_infos)
{
  *n_infos = manager->n_monitor_infos;
  return manager->monitor_infos;
}

MetaOutput *
meta_monitor_manager_get_outputs (MetaMonitorManager *manager,
                                  int                *n_outputs)
{
  *n_outputs = manager->n_outputs;
  return manager->outputs;
}

int
meta_monitor_manager_get_primary_index (MetaMonitorManager *manager)
{
  return manager->primary_monitor_index;
}

void
meta_monitor_manager_invalidate (MetaMonitorManager *manager)
{
  MetaOutput *old_outputs;
  MetaCRTC *old_crtcs;
  MetaMonitorInfo *old_monitor_infos;
  MetaMonitorMode *old_modes;
  int n_old_outputs;

  /* Save the old structures, so they stay valid during the update */
  old_outputs = manager->outputs;
  n_old_outputs = manager->n_outputs;
  old_monitor_infos = manager->monitor_infos;
  old_modes = manager->modes;
  old_crtcs = manager->crtcs;

  manager->serial ++;
  read_current_config (manager);
  make_logical_config (manager);

  g_signal_emit (manager, signals[MONITORS_CHANGED], 0);

  g_free (old_monitor_infos);
  free_output_array (old_outputs, n_old_outputs);
  g_free (old_modes);
  g_free (old_crtcs);
}

