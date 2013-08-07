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
#include <X11/extensions/dpms.h>
#endif

#include <meta/main.h>
#include <meta/errors.h>
#include "monitor-private.h"
#ifdef HAVE_WAYLAND
#include "meta-wayland-private.h"
#endif

#include "meta-dbus-xrandr.h"

#define ALL_WL_TRANSFORMS ((1 << (WL_OUTPUT_TRANSFORM_FLIPPED_270 + 1)) - 1)

typedef enum {
  META_POWER_SAVE_UNKNOWN = -1,
  META_POWER_SAVE_ON = 0,
  META_POWER_SAVE_STANDBY,
  META_POWER_SAVE_SUSPEND,
  META_POWER_SAVE_OFF,
} MetaPowerSave;

typedef enum {
  META_BACKEND_UNSPECIFIED,
  META_BACKEND_DUMMY,
  META_BACKEND_XRANDR,
  META_BACKEND_COGL
} MetaMonitorBackend;

struct _MetaMonitorManager
{
  MetaDBusDisplayConfigSkeleton parent_instance;

  MetaMonitorBackend backend;

  /* XXX: this structure is very badly
     packed, but I like the logical organization
     of fields */

  unsigned int serial;

  MetaPowerSave power_save_mode;

  int max_screen_width;
  int max_screen_height;
  int screen_width;
  int screen_height;

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
  XRRScreenResources *resources;
  int time;
  int rr_event_base;
  int rr_error_base;
#endif

  int dbus_name_id;
};

struct _MetaMonitorManagerClass
{
  MetaDBusDisplayConfigSkeletonClass parent_class;
};

enum {
  MONITORS_CHANGED,
  SIGNALS_LAST
};

enum {
  PROP_0,
  PROP_POWER_SAVE_MODE,
  PROP_LAST
};

static int signals[SIGNALS_LAST];

static void meta_monitor_manager_display_config_init (MetaDBusDisplayConfigIface *iface);

G_DEFINE_TYPE_WITH_CODE (MetaMonitorManager, meta_monitor_manager, META_DBUS_TYPE_DISPLAY_CONFIG_SKELETON,
                         G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_DISPLAY_CONFIG, meta_monitor_manager_display_config_init));

static void invalidate_logical_config (MetaMonitorManager *manager);

static void
make_dummy_monitor_config (MetaMonitorManager *manager)
{
  /* The dummy monitor config has:
     - one enabled output, LVDS, primary, at 0x0 and 1024x768
     - one free CRTC
     - two disabled outputs
     - three modes, 1024x768, 800x600 and 640x480
     - no clones are possible (use different CRTCs)

     Low-level IDs should be assigned sequentially, to
     mimick what XRandR and KMS do
  */

  manager->backend = META_BACKEND_DUMMY;

  manager->max_screen_width = 65535;
  manager->max_screen_height = 65535;
  manager->screen_width = 1024;
  manager->screen_height = 768;

  manager->modes = g_new0 (MetaMonitorMode, 3);
  manager->n_modes = 3;

  manager->modes[0].mode_id = 1;
  manager->modes[0].width = 1024;
  manager->modes[0].height = 768;
  manager->modes[0].refresh_rate = 60.0;

  manager->modes[1].mode_id = 2;
  manager->modes[1].width = 800;
  manager->modes[1].height = 600;
  manager->modes[1].refresh_rate = 60.0;

  manager->modes[2].mode_id = 3;
  manager->modes[2].width = 640;
  manager->modes[2].height = 480;
  manager->modes[2].refresh_rate = 60.0;

  manager->crtcs = g_new0 (MetaCRTC, 2);
  manager->n_crtcs = 2;

  manager->crtcs[0].crtc_id = 4;
  manager->crtcs[0].rect.x = 0;
  manager->crtcs[0].rect.y = 0;
  manager->crtcs[0].rect.width = manager->modes[0].width;
  manager->crtcs[0].rect.height = manager->modes[0].height;
  manager->crtcs[0].current_mode = &manager->modes[0];
  manager->crtcs[0].transform = WL_OUTPUT_TRANSFORM_NORMAL;
  manager->crtcs[0].all_transforms = ALL_WL_TRANSFORMS;
  manager->crtcs[0].dirty = FALSE;
  manager->crtcs[0].logical_monitor = NULL;

  manager->crtcs[1].crtc_id = 5;
  manager->crtcs[1].rect.x = 0;
  manager->crtcs[1].rect.y = 0;
  manager->crtcs[1].rect.width = 0;
  manager->crtcs[1].rect.height = 0;
  manager->crtcs[1].current_mode = NULL;
  manager->crtcs[1].transform = WL_OUTPUT_TRANSFORM_NORMAL;
  manager->crtcs[1].all_transforms = ALL_WL_TRANSFORMS;
  manager->crtcs[1].dirty = FALSE;
  manager->crtcs[1].logical_monitor = NULL;

  manager->outputs = g_new0 (MetaOutput, 3);
  manager->n_outputs = 3;

  manager->outputs[0].crtc = &manager->crtcs[0];
  manager->outputs[0].output_id = 6;
  manager->outputs[0].name = g_strdup ("LVDS");
  manager->outputs[0].vendor = g_strdup ("unknown");
  manager->outputs[0].product = g_strdup ("unknown");
  manager->outputs[0].serial = g_strdup ("");
  manager->outputs[0].width_mm = 222;
  manager->outputs[0].height_mm = 125;
  manager->outputs[0].subpixel_order = COGL_SUBPIXEL_ORDER_UNKNOWN;
  manager->outputs[0].preferred_mode = &manager->modes[0];
  manager->outputs[0].n_modes = 3;
  manager->outputs[0].modes = g_new0 (MetaMonitorMode *, 3);
  manager->outputs[0].modes[0] = &manager->modes[0];
  manager->outputs[0].modes[1] = &manager->modes[1];
  manager->outputs[0].modes[2] = &manager->modes[2];
  manager->outputs[0].n_possible_crtcs = 2;
  manager->outputs[0].possible_crtcs = g_new0 (MetaCRTC *, 2);
  manager->outputs[0].possible_crtcs[0] = &manager->crtcs[0];
  manager->outputs[0].possible_crtcs[1] = &manager->crtcs[1];
  manager->outputs[0].n_possible_clones = 0;
  manager->outputs[0].possible_clones = g_new0 (MetaOutput *, 0);

  manager->outputs[1].crtc = NULL;
  manager->outputs[1].output_id = 7;
  manager->outputs[1].name = g_strdup ("HDMI");
  manager->outputs[1].vendor = g_strdup ("unknown");
  manager->outputs[1].product = g_strdup ("unknown");
  manager->outputs[1].serial = g_strdup ("");
  manager->outputs[1].width_mm = 510;
  manager->outputs[1].height_mm = 287;
  manager->outputs[1].subpixel_order = COGL_SUBPIXEL_ORDER_UNKNOWN;
  manager->outputs[1].preferred_mode = &manager->modes[0];
  manager->outputs[1].n_modes = 3;
  manager->outputs[1].modes = g_new0 (MetaMonitorMode *, 3);
  manager->outputs[1].modes[0] = &manager->modes[0];
  manager->outputs[1].modes[1] = &manager->modes[1];
  manager->outputs[1].modes[2] = &manager->modes[2];
  manager->outputs[1].n_possible_crtcs = 2;
  manager->outputs[1].possible_crtcs = g_new0 (MetaCRTC *, 2);
  manager->outputs[1].possible_crtcs[0] = &manager->crtcs[0];
  manager->outputs[1].possible_crtcs[1] = &manager->crtcs[1];
  manager->outputs[1].n_possible_clones = 0;
  manager->outputs[1].possible_clones = g_new0 (MetaOutput *, 0);

  manager->outputs[2].crtc = NULL;
  manager->outputs[2].output_id = 8;
  manager->outputs[2].name = g_strdup ("VGA");
  manager->outputs[2].vendor = g_strdup ("unknown");
  manager->outputs[2].product = g_strdup ("unknown");
  manager->outputs[2].serial = g_strdup ("");
  manager->outputs[2].width_mm = 309;
  manager->outputs[2].height_mm = 174;
  manager->outputs[2].subpixel_order = COGL_SUBPIXEL_ORDER_UNKNOWN;
  manager->outputs[2].preferred_mode = &manager->modes[0];
  manager->outputs[2].n_modes = 3;
  manager->outputs[2].modes = g_new0 (MetaMonitorMode *, 3);
  manager->outputs[2].modes[0] = &manager->modes[0];
  manager->outputs[2].modes[1] = &manager->modes[1];
  manager->outputs[2].modes[2] = &manager->modes[2];
  manager->outputs[2].n_possible_crtcs = 2;
  manager->outputs[2].possible_crtcs = g_new0 (MetaCRTC *, 2);
  manager->outputs[2].possible_crtcs[0] = &manager->crtcs[0];
  manager->outputs[2].possible_crtcs[1] = &manager->crtcs[1];
  manager->outputs[2].n_possible_clones = 0;
  manager->outputs[2].possible_clones = g_new0 (MetaOutput *, 0);
}

#ifdef HAVE_RANDR
static enum wl_output_transform
wl_transform_from_xrandr (Rotation rotation)
{
  static const enum wl_output_transform y_reflected_map[4] = {
    WL_OUTPUT_TRANSFORM_FLIPPED_180,
    WL_OUTPUT_TRANSFORM_FLIPPED_90,
    WL_OUTPUT_TRANSFORM_FLIPPED,
    WL_OUTPUT_TRANSFORM_FLIPPED_270
  };
  enum wl_output_transform ret;

  switch (rotation & 0x7F)
    {
    default:
    case RR_Rotate_0:
      ret = WL_OUTPUT_TRANSFORM_NORMAL;
      break;
    case RR_Rotate_90:
      ret = WL_OUTPUT_TRANSFORM_90;
      break;
    case RR_Rotate_180:
      ret = WL_OUTPUT_TRANSFORM_180;
      break;
    case RR_Rotate_270:
      ret = WL_OUTPUT_TRANSFORM_270;
      break;
    }

  if (rotation & RR_Reflect_X)
    return ret + 4;
  else if (rotation & RR_Reflect_Y)
    return y_reflected_map[ret];
  else
    return ret;
}

#define ALL_ROTATIONS (RR_Rotate_0 | RR_Rotate_90 | RR_Rotate_180 | RR_Rotate_270)

static unsigned int
wl_transform_from_xrandr_all (Rotation rotation)
{
  unsigned ret;

  /* Handle the common cases first (none or all) */
  if (rotation == 0 || rotation == RR_Rotate_0)
    return (1 << WL_OUTPUT_TRANSFORM_NORMAL);

  /* All rotations and one reflection -> all of them by composition */
  if ((rotation & ALL_ROTATIONS) &&
      ((rotation & RR_Reflect_X) || (rotation & RR_Reflect_Y)))
    return ALL_WL_TRANSFORMS;

  ret = 1 << WL_OUTPUT_TRANSFORM_NORMAL;
  if (rotation & RR_Rotate_90)
    ret |= 1 << WL_OUTPUT_TRANSFORM_90;
  if (rotation & RR_Rotate_180)
    ret |= 1 << WL_OUTPUT_TRANSFORM_180;
  if (rotation & RR_Rotate_270)
    ret |= 1 << WL_OUTPUT_TRANSFORM_270;
  if (rotation & (RR_Rotate_0 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED;
  if (rotation & (RR_Rotate_90 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED_90;
  if (rotation & (RR_Rotate_180 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED_180;
  if (rotation & (RR_Rotate_270 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED_270;

  return ret;
}

static void
read_monitor_infos_from_xrandr (MetaMonitorManager *manager)
{
    XRRScreenResources *resources;
    RROutput primary_output;
    unsigned int i, j, k;
    unsigned int n_actual_outputs;
    int min_width, min_height;
    Screen *screen;
    BOOL dpms_capable, dpms_enabled;
    CARD16 dpms_state;

    if (manager->resources)
      XRRFreeScreenResources (manager->resources);
    manager->resources = NULL;

    meta_error_trap_push (meta_get_display ());
    dpms_capable = DPMSCapable (manager->xdisplay);
    meta_error_trap_pop (meta_get_display ());

    if (dpms_capable &&
        DPMSInfo (manager->xdisplay, &dpms_state, &dpms_enabled) &&
        dpms_enabled)
      {
        switch (dpms_state)
          {
          case DPMSModeOn:
            manager->power_save_mode = META_POWER_SAVE_ON;
          case DPMSModeStandby:
            manager->power_save_mode = META_POWER_SAVE_STANDBY;
          case DPMSModeSuspend:
            manager->power_save_mode = META_POWER_SAVE_SUSPEND;
          case DPMSModeOff:
            manager->power_save_mode = META_POWER_SAVE_OFF;
          default:
            manager->power_save_mode = META_POWER_SAVE_UNKNOWN;
          }
      }
    else
      {
        manager->power_save_mode = META_POWER_SAVE_UNKNOWN;
      }

    XRRGetScreenSizeRange (manager->xdisplay, DefaultRootWindow (manager->xdisplay),
                           &min_width,
                           &min_height,
                           &manager->max_screen_width,
                           &manager->max_screen_height);

    screen = ScreenOfDisplay (manager->xdisplay,
                              DefaultScreen (manager->xdisplay));
    /* This is updated because we called RRUpdateConfiguration below */
    manager->screen_width = WidthOfScreen (screen);
    manager->screen_height = HeightOfScreen (screen);

    resources = XRRGetScreenResourcesCurrent (manager->xdisplay,
                                              DefaultRootWindow (manager->xdisplay));
    if (!resources)
      return make_dummy_monitor_config (manager);

    manager->resources = resources;
    manager->time = resources->configTimestamp;
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
        meta_crtc->dirty = FALSE;
        meta_crtc->transform = wl_transform_from_xrandr (crtc->rotation);
        meta_crtc->all_transforms = wl_transform_from_xrandr_all (crtc->rotations);

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
      meta_crtc.transform = WL_OUTPUT_TRANSFORM_NORMAL;
      meta_crtc.all_transforms = 1 << WL_OUTPUT_TRANSFORM_NORMAL;
      meta_crtc.logical_monitor = NULL;
      meta_crtc.dirty = FALSE;

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
  return FALSE;
#endif
}

static void
meta_monitor_manager_init (MetaMonitorManager *manager)
{
}

static MetaMonitorBackend
make_debug_config (MetaMonitorManager *manager)
{
  const char *env;

  env = g_getenv ("META_DEBUG_MULTIMONITOR");

  if (env == NULL)
    return META_BACKEND_UNSPECIFIED;

#ifdef HAVE_RANDR
  if (strcmp (env, "xrandr") == 0)
    return META_BACKEND_XRANDR;
  else
#endif
    if (strcmp (env, "cogl") == 0)
      return META_BACKEND_COGL;
    else
      return META_BACKEND_DUMMY;

  return TRUE;
}

static void
read_current_config (MetaMonitorManager *manager)
{
#ifdef HAVE_RANDR
  if (manager->backend == META_BACKEND_XRANDR)
    return read_monitor_infos_from_xrandr (manager);
#endif

  if (manager->backend == META_BACKEND_COGL)
    return read_monitor_infos_from_cogl (manager);
  else
    return make_dummy_monitor_config (manager);
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

  manager->backend = make_debug_config (manager);

  if (manager->backend == META_BACKEND_UNSPECIFIED)
    {
#ifdef HAVE_XRANDR
      if (display)
        manager->backend = META_BACKEND_XRANDR;
      else
#endif
        if (has_dummy_output ())
          manager->backend = META_BACKEND_DUMMY;
        else
          manager->backend = META_BACKEND_COGL;
    }

#ifdef HAVE_XRANDR
  if (manager->backend == META_BACKEND_XRANDR)
    {
      if (!XRRQueryExtension (display,
                              &manager->rr_event_base,
                              &manager->rr_error_base))
        {
          manager->backend = META_BACKEND_DUMMY;
        }
      else
        {
          /* We only use ScreenChangeNotify, but GDK uses the others,
             and we don't want to step on its toes */
          XRRSelectInput (display, DefaultRootWindow (display),
                          RRScreenChangeNotifyMask
                          | RRCrtcChangeNotifyMask
                          | RROutputPropertyNotifyMask);
        }
    }
#endif

  read_current_config (manager);
  make_logical_config (manager);
  return manager;
}

static void
meta_monitor_manager_set_power_save_mode (MetaMonitorManager *manager,
                                          MetaPowerSave       mode)
{
  if (mode == manager->power_save_mode)
    return;

  if (manager->power_save_mode == META_POWER_SAVE_UNKNOWN ||
      mode == META_POWER_SAVE_UNKNOWN)
    return;

#ifdef HAVE_RANDR
  if (manager->backend == META_BACKEND_XRANDR)
    {
      CARD16 state;

      switch (mode) {
      case META_POWER_SAVE_ON:
        state = DPMSModeOn;
        break;
      case META_POWER_SAVE_STANDBY:
        state = DPMSModeStandby;
        break;
      case META_POWER_SAVE_SUSPEND:
        state = DPMSModeSuspend;
        break;
      case META_POWER_SAVE_OFF:
        state = DPMSModeOff;
        break;
      default:
        return;
      }

      meta_error_trap_push (meta_get_display ());
      DPMSForceLevel (manager->xdisplay, state);
      DPMSSetTimeouts (manager->xdisplay, 0, 0, 0);
      meta_error_trap_pop (meta_get_display ());
    }
#endif

  manager->power_save_mode = mode;
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

  G_OBJECT_CLASS (meta_monitor_manager_parent_class)->dispose (object);
}

static void
meta_monitor_manager_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  MetaMonitorManager *self = META_MONITOR_MANAGER (object);

  switch (prop_id)
    {
    case PROP_POWER_SAVE_MODE:
      meta_monitor_manager_set_power_save_mode (self, g_value_get_int (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_monitor_manager_get_property (GObject      *object,
                                   guint         prop_id,
                                   GValue       *value,
                                   GParamSpec   *pspec)
{
  MetaMonitorManager *self = META_MONITOR_MANAGER (object);

  switch (prop_id)
    {
    case PROP_POWER_SAVE_MODE:
      g_value_set_int (value, self->power_save_mode);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_monitor_manager_class_init (MetaMonitorManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = meta_monitor_manager_get_property;
  object_class->set_property = meta_monitor_manager_set_property;
  object_class->dispose = meta_monitor_manager_dispose;
  object_class->finalize = meta_monitor_manager_finalize;

  signals[MONITORS_CHANGED] =
    g_signal_new ("monitors-changed",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  0,
                  NULL, NULL, NULL,
		  G_TYPE_NONE, 0);

  g_object_class_override_property (object_class, PROP_POWER_SAVE_MODE, "power-save-mode");
}

static gboolean
meta_monitor_manager_handle_get_resources (MetaDBusDisplayConfig *skeleton,
                                           GDBusMethodInvocation *invocation)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (skeleton);
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
      for (j = 0; j <= WL_OUTPUT_TRANSFORM_FLIPPED_270; j++)
        if (crtc->all_transforms & (1 << j))
          g_variant_builder_add (&transforms, "u", j);

      g_variant_builder_add (&crtc_builder, "(uxiiiiiuaua{sv})",
                             i, /* ID */
                             crtc->crtc_id,
                             (int)crtc->rect.x,
                             (int)crtc->rect.y,
                             (int)crtc->rect.width,
                             (int)crtc->rect.height,
                             (int)(crtc->current_mode ? crtc->current_mode - manager->modes : -1),
                             crtc->transform,
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
                                                   g_variant_builder_end (&mode_builder),
                                                   manager->max_screen_width,
                                                   manager->max_screen_height);
  return TRUE;
}

static gboolean
output_can_config (MetaOutput      *output,
                   MetaCRTC        *crtc,
                   MetaMonitorMode *mode)
{
  unsigned int i;
  gboolean ok = FALSE;

  for (i = 0; i < output->n_possible_crtcs && !ok; i++)
    ok = output->possible_crtcs[i] == crtc;

  if (!ok)
    return FALSE;

  if (mode == NULL)
    return TRUE;

  ok = FALSE;
  for (i = 0; i < output->n_modes && !ok; i++)
    ok = output->modes[i] == mode;

  return ok;
}

static gboolean
output_can_clone (MetaOutput *output,
                  MetaOutput *clone)
{
  unsigned int i;
  gboolean ok = FALSE;

  for (i = 0; i < output->n_possible_clones && !ok; i++)
    ok = output->possible_clones[i] == clone;

  return ok;
}

#ifdef HAVE_RANDR
static Rotation
wl_transform_to_xrandr (enum wl_output_transform transform)
{
  switch (transform)
    {
    case WL_OUTPUT_TRANSFORM_NORMAL:
      return RR_Rotate_0;
    case WL_OUTPUT_TRANSFORM_90:
      return RR_Rotate_90;
    case WL_OUTPUT_TRANSFORM_180:
      return RR_Rotate_180;
    case WL_OUTPUT_TRANSFORM_270:
      return RR_Rotate_270;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
      return RR_Reflect_X | RR_Rotate_0;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      return RR_Reflect_X | RR_Rotate_90;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      return RR_Reflect_X | RR_Rotate_180;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      return RR_Reflect_X | RR_Rotate_270;
    }

  g_assert_not_reached ();
}
     
static void
apply_config_xrandr (MetaMonitorManager *manager,
                     GVariantIter       *crtcs,
                     GVariantIter       *outputs)
{
  GVariant *nested_outputs, *properties;
  guint crtc_id, output_id, transform;
  int new_mode, x, y;
  unsigned i;

  while (g_variant_iter_loop (crtcs, "(uiiiu@aua{sv})",
                              &crtc_id, &new_mode, &x, &y,
                              &transform, &nested_outputs, NULL))
    {
      MetaCRTC *crtc = &manager->crtcs[crtc_id];
      crtc->dirty = TRUE;

      if (new_mode == -1)
        {
          XRRSetCrtcConfig (manager->xdisplay,
                            manager->resources,
                            (XID)crtc->crtc_id,
                            manager->time,
                            0, 0,
                            None,
                            RR_Rotate_0,
                            NULL, 0);
        }
      else
        {
          MetaMonitorMode *mode;
          XID *outputs;
          int i, n_outputs;
          guint output_id;
          Status ok;

          mode = &manager->modes[new_mode];

          n_outputs = g_variant_n_children (nested_outputs);
          outputs = g_new (XID, n_outputs);

          for (i = 0; i < n_outputs; i++)
            {
              g_variant_get_child (nested_outputs, i, "u", &output_id);

              outputs[i] = manager->outputs[output_id].output_id;
            }

          meta_error_trap_push (meta_get_display ());
          ok = XRRSetCrtcConfig (manager->xdisplay,
                                 manager->resources,
                                 (XID)crtc->crtc_id,
                                 manager->time,
                                 x, y,
                                 (XID)mode->mode_id,
                                 wl_transform_to_xrandr (transform),
                                 outputs, n_outputs);
          meta_error_trap_pop (meta_get_display ());

          if (ok != Success)
            meta_warning ("Configuring CRTC %d with mode %d (%d x %d @ %f) at position %d, %d and transfrom %u failed\n",
                          (unsigned)(crtc - manager->crtcs), (unsigned)(mode - manager->modes),
                          mode->width, mode->height, (float)mode->refresh_rate, x, y, transform);

          g_free (outputs);
        }
    }

  while (g_variant_iter_loop (outputs, "(u@a{sv})",
                              &output_id, &properties))
    {
      gboolean primary;

      if (g_variant_lookup (properties, "primary", "b", &primary) && primary)
        {
          MetaOutput *output = &manager->outputs[output_id];

          XRRSetOutputPrimary (manager->xdisplay,
                               DefaultRootWindow (manager->xdisplay),
                               (XID)output->output_id);
        }
    }

  /* Disable CRTCs not mentioned in the list */
  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCRTC *crtc = &manager->crtcs[i];

      if (crtc->dirty)
        {
          crtc->dirty = FALSE;
          continue;
        }
      if (crtc->current_mode == NULL)
        continue;

      XRRSetCrtcConfig (manager->xdisplay,
                        manager->resources,
                        (XID)crtc->crtc_id,
                        manager->time,
                        0, 0,
                        None,
                        RR_Rotate_0,
                        NULL, 0);
    }
}
#endif

static void
apply_config_dummy (MetaMonitorManager *manager,
                    GVariantIter       *crtcs,
                    GVariantIter       *outputs)
{
  GVariant *nested_outputs, *properties;
  guint crtc_id, output_id, transform;
  int new_mode, x, y;
  unsigned i;
  int screen_width = 0, screen_height = 0;

  while (g_variant_iter_loop (crtcs, "(uiiiu@aua{sv})",
                              &crtc_id, &new_mode, &x, &y,
                              &transform, &nested_outputs, NULL))
    {
      MetaCRTC *crtc = &manager->crtcs[crtc_id];
      crtc->dirty = TRUE;

      if (new_mode == -1)
        {
          crtc->rect.x = 0;
          crtc->rect.y = 0;
          crtc->rect.width = 0;
          crtc->rect.height = 0;
          crtc->current_mode = NULL;
        }
      else
        {
          MetaMonitorMode *mode;
          MetaOutput *output;
          int i, n_outputs;
          guint output_id;

          mode = &manager->modes[new_mode];

          crtc->rect.x = x;
          crtc->rect.y = y;
          crtc->rect.width = mode->width;
          crtc->rect.height = mode->height;
          crtc->current_mode = mode;
          crtc->transform = transform;

          screen_width = MAX (screen_width, x + mode->width);
          screen_height = MAX (screen_height, y + mode->height);

          n_outputs = g_variant_n_children (nested_outputs);
          for (i = 0; i < n_outputs; i++)
            {
              g_variant_get_child (nested_outputs, i, "u", &output_id);

              output = &manager->outputs[output_id];

              output->dirty = TRUE;
              output->crtc = crtc;
            }
        }
    }

  while (g_variant_iter_loop (outputs, "(u@a{sv})",
                              &output_id, &properties))
    {
      MetaOutput *output = &manager->outputs[output_id];
      gboolean primary, presentation;

      if (g_variant_lookup (properties, "primary", "b", &primary))
        output->is_primary = primary;

      if (g_variant_lookup (properties, "presentation", "b", &presentation))
        output->is_presentation = presentation;
    }

  /* Disable CRTCs not mentioned in the list */
  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCRTC *crtc = &manager->crtcs[i];

      crtc->logical_monitor = NULL;

      if (crtc->dirty)
        {
          crtc->dirty = FALSE;
          continue;
        }

      crtc->rect.x = 0;
      crtc->rect.y = 0;
      crtc->rect.width = 0;
      crtc->rect.height = 0;
      crtc->current_mode = NULL;
    }

  /* Disable outputs not mentioned in the list */
  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output = &manager->outputs[i];

      if (output->dirty)
        {
          output->dirty = FALSE;
          continue;
        }

      output->crtc = NULL;
      output->is_primary = FALSE;
    }

  manager->screen_width = screen_width;
  manager->screen_height = screen_height;

  invalidate_logical_config (manager);
}

static gboolean
meta_monitor_manager_handle_apply_configuration  (MetaDBusDisplayConfig *skeleton,
                                                  GDBusMethodInvocation *invocation,
                                                  guint                  serial,
                                                  gboolean               persistent,
                                                  GVariant              *crtcs,
                                                  GVariant              *outputs)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (skeleton);
  GVariantIter crtc_iter, output_iter, *nested_outputs;
  guint crtc_id;
  int new_mode, x, y;
  guint transform;
  guint output_id;

  if (serial != manager->serial)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "The requested configuration is based on stale information");
      return TRUE;
    }

  if (persistent)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_NOT_SUPPORTED,
                                             "Persistent configuration is not yet implemented");
      return TRUE;
    }

  /* Validate all arguments */
  g_variant_iter_init (&crtc_iter, crtcs);
  while (g_variant_iter_loop (&crtc_iter, "(uiiiuaua{sv})",
                              &crtc_id, &new_mode, &x, &y, &transform,
                              &nested_outputs, NULL))
    {
      MetaOutput *first_output;
      MetaCRTC *crtc;
      MetaMonitorMode *mode;
      guint output_id;

      if (crtc_id < 0 || crtc_id >= manager->n_crtcs)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid CRTC id");
          return TRUE;
        }
      crtc = &manager->crtcs[crtc_id];

      if (new_mode != -1 && (new_mode < 0 || (unsigned)new_mode >= manager->n_modes))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid mode id");
          return TRUE;
        }
      mode = new_mode != -1 ? &manager->modes[new_mode] : NULL;

      if (mode &&
          (x < 0 ||
           x + mode->width > manager->max_screen_width ||
           y < 0 ||
           y + mode->height > manager->max_screen_height))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid CRTC geometry");
          return TRUE;
        }

      if (transform < WL_OUTPUT_TRANSFORM_NORMAL ||
          transform > WL_OUTPUT_TRANSFORM_FLIPPED_270)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid transform");
          return TRUE;
        }

      first_output = NULL;
      while (g_variant_iter_loop (nested_outputs, "u", &output_id))
        {
          MetaOutput *output;

          if (output_id < 0 || output_id >= manager->n_outputs)
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "Invalid output id");
              return TRUE;
            }
          output = &manager->outputs[output_id];

          if (!output_can_config (output, crtc, mode))
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "Output cannot be assigned to this CRTC or mode");
              return TRUE;
            }

          if (first_output)
            {
              if (!output_can_clone (output, first_output))
                {
                  g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                         G_DBUS_ERROR_INVALID_ARGS,
                                                         "Outputs cannot be cloned");
                  return TRUE;
                }
            }
          else
            first_output = output;
        }

      if (!first_output && mode)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Mode specified without outputs?");
          return TRUE;
        }
    }

  g_variant_iter_init (&output_iter, outputs);
  while (g_variant_iter_loop (&output_iter, "(ua{sv})", &output_id, NULL))
    {
      if (output_id < 0 || output_id >= manager->n_outputs)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Invalid output id");
          return TRUE;
        }
    }

  g_variant_iter_init (&crtc_iter, crtcs);
  g_variant_iter_init (&output_iter, outputs);

  if (manager->backend == META_BACKEND_COGL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_NOT_SUPPORTED,
                                             "Changing configuration is not supported by the backend");
      return TRUE;
    }

  if (manager->backend == META_BACKEND_XRANDR)
    apply_config_xrandr (manager, &crtc_iter, &output_iter);
  else
    apply_config_dummy (manager, &crtc_iter, &output_iter);

  meta_dbus_display_config_complete_apply_configuration (skeleton, invocation);
  return TRUE;
}

static void
meta_monitor_manager_display_config_init (MetaDBusDisplayConfigIface *iface)
{
  iface->handle_get_resources = meta_monitor_manager_handle_get_resources;
  iface->handle_apply_configuration = meta_monitor_manager_handle_apply_configuration;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  MetaMonitorManager *manager = user_data;

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (manager),
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
meta_monitor_manager_get_screen_size (MetaMonitorManager *manager,
                                      int                *width,
                                      int                *height)
{
  *width = manager->screen_width;
  *height = manager->screen_height;
}

static void
invalidate_logical_config (MetaMonitorManager *manager)
{
  MetaMonitorInfo *old_monitor_infos;

  old_monitor_infos = manager->monitor_infos;

  manager->serial++;
  make_logical_config (manager);

  g_signal_emit (manager, signals[MONITORS_CHANGED], 0);

  g_free (old_monitor_infos);
}

gboolean
meta_monitor_manager_handle_xevent (MetaMonitorManager *manager,
                                    XEvent             *event)
{
  MetaOutput *old_outputs;
  MetaCRTC *old_crtcs;

  MetaMonitorMode *old_modes;
  int n_old_outputs;

  if (manager->backend != META_BACKEND_XRANDR)
    return FALSE;

#ifdef HAVE_RANDR
  if ((event->type - manager->rr_event_base) != RRScreenChangeNotify)
    return FALSE;

  XRRUpdateConfiguration (event);

  /* Save the old structures, so they stay valid during the update */
  old_outputs = manager->outputs;
  n_old_outputs = manager->n_outputs;
  old_modes = manager->modes;
  old_crtcs = manager->crtcs;

  read_current_config (manager);
  invalidate_logical_config (manager);

  free_output_array (old_outputs, n_old_outputs);
  g_free (old_modes);
  g_free (old_crtcs);

  return TRUE;
#else
  return FALSE;
#endif
}

