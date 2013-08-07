/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/**
 * \file screen-private.h  Handling of monitor configuration
 *
 * Managing multiple monitors
 * This file contains structures and functions that handle
 * multiple monitors, including reading the current configuration
 * and available hardware, and applying it.
 *
 * This interface is private to mutter, API users should look
 * at MetaScreen instead.
 */

/* 
 * Copyright (C) 2001 Havoc Pennington
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

#ifndef META_MONITOR_PRIVATE_H
#define META_MONITOR_PRIVATE_H

#include <cogl/cogl.h>

#include "display-private.h"
#include <meta/screen.h>
#include "stack-tracker.h"
#include "ui.h"
#ifdef HAVE_WAYLAND
#include <wayland-server.h>
#endif

#ifndef HAVE_WAYLAND
enum wl_output_transform {
  WL_OUTPUT_TRANSFORM_NORMAL,
  WL_OUTPUT_TRANSFORM_90,
  WL_OUTPUT_TRANSFORM_180,
  WL_OUTPUT_TRANSFORM_270,
  WL_OUTPUT_TRANSFORM_FLIPPED,
  WL_OUTPUT_TRANSFORM_FLIPPED_90,
  WL_OUTPUT_TRANSFORM_FLIPPED_180,
  WL_OUTPUT_TRANSFORM_FLIPPED_270
};
#endif

typedef struct _MetaOutput MetaOutput;
typedef struct _MetaCRTC MetaCRTC;
typedef struct _MetaMonitorMode MetaMonitorMode;
typedef struct _MetaMonitorInfo MetaMonitorInfo;
typedef struct _MetaCRTCInfo MetaCRTCInfo;
typedef struct _MetaOutputInfo MetaOutputInfo;

struct _MetaOutput
{
  /* The CRTC driving this output, NULL if the output is not enabled */
  MetaCRTC *crtc;
  /* The low-level ID of this output, used to apply back configuration */
  glong output_id;
  char *name;
  char *vendor;
  char *product;
  char *serial;
  int width_mm;
  int height_mm;
  CoglSubpixelOrder subpixel_order;

  MetaMonitorMode *preferred_mode;
  MetaMonitorMode **modes;
  unsigned int n_modes;

  MetaCRTC **possible_crtcs;
  unsigned int n_possible_crtcs;

  MetaOutput **possible_clones;
  unsigned int n_possible_clones;

  /* Used when changing configuration */
  gboolean dirty;

  /* The low-level bits used to build the high-level info
     in MetaMonitorInfo

     XXX: flags maybe?
     There is a lot of code that uses MonitorInfo->is_primary,
     but nobody uses MetaOutput yet
  */
  gboolean is_primary;
  gboolean is_presentation;
};

struct _MetaCRTC
{
  glong crtc_id;
  MetaRectangle rect;
  MetaMonitorMode *current_mode;
  enum wl_output_transform transform;
  unsigned int all_transforms;

  /* Only used to build the logical configuration
     from the HW one
  */
  MetaMonitorInfo *logical_monitor;

  /* Used when changing configuration */
  gboolean dirty;
};

struct _MetaMonitorMode
{
  /* The low-level ID of this mode, used to apply back configuration */
  glong mode_id;

  int width;
  int height;
  float refresh_rate;
};

/**
 * MetaMonitorInfo:
 *
 * A structure with high-level information about monitors.
 * This corresponds to a subset of the compositor coordinate space.
 * Clones are only reported once, irrespective of the way
 * they're implemented (two CRTCs configured for the same
 * coordinates or one CRTCs driving two outputs). Inactive CRTCs
 * are ignored, and so are disabled outputs.
 */
struct _MetaMonitorInfo
{
  int number;
  int xinerama_index;
  MetaRectangle rect;
  gboolean is_primary;
  gboolean is_presentation; /* XXX: not yet used */
  gboolean in_fullscreen;

  /* The primary or first output for this monitor, 0 if we can't figure out.
     This is a XID when using XRandR, otherwise a KMS id (not implemented).
     In any case, it can be matched to an output_id of a MetaOutput.

     This is used as an opaque token on reconfiguration when switching from
     clone to extened, to decide on what output the windows should go next
     (it's an attempt to keep windows on the same monitor, and preferably on
     the primary one).
  */
  glong output_id;
};

/*
 * MetaCRTCInfo:
 * This represents the writable part of a CRTC, as deserialized from DBus
 * or built by MetaMonitorConfig
 *
 * Note: differently from the other structures in this file, MetaCRTCInfo
 * is handled by pointer. This is to accomodate the usage in MetaMonitorConfig
 */
struct _MetaCRTCInfo {
  MetaCRTC                 *crtc;
  MetaMonitorMode          *mode;
  int                       x;
  int                       y;
  enum wl_output_transform  transform;
  GPtrArray                *outputs;
};

/*
 * MetaOutputInfo:
 * this is the same as MetaOutputInfo, but for CRTCs
 */
struct _MetaOutputInfo {
  MetaOutput  *output;
  gboolean     is_primary;
  gboolean     is_presentation;
};

#define META_TYPE_MONITOR_MANAGER            (meta_monitor_manager_get_type ())
#define META_MONITOR_MANAGER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), META_TYPE_MONITOR_MANAGER, MetaMonitorManager))
#define META_MONITOR_MANAGER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  META_TYPE_MONITOR_MANAGER, MetaMonitorManagerClass))
#define META_IS_MONITOR_MANAGER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), META_TYPE_MONITOR_MANAGER))
#define META_IS_MONITOR_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  META_TYPE_MONITOR_MANAGER))
#define META_MONITOR_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  META_TYPE_MONITOR_MANAGER, MetaMonitorManagerClass))

typedef struct _MetaMonitorManagerClass    MetaMonitorManagerClass;
typedef struct _MetaMonitorManager         MetaMonitorManager;

GType meta_monitor_manager_get_type (void);

void                meta_monitor_manager_initialize (Display *display);
MetaMonitorManager *meta_monitor_manager_get  (void);

MetaMonitorInfo    *meta_monitor_manager_get_monitor_infos (MetaMonitorManager *manager,
							    unsigned int       *n_infos);

MetaOutput         *meta_monitor_manager_get_outputs       (MetaMonitorManager *manager,
							    unsigned int       *n_outputs);

void                meta_monitor_manager_get_resources     (MetaMonitorManager  *manager,
                                                            MetaMonitorMode    **modes,
                                                            unsigned int        *n_modes,
                                                            MetaCRTC           **crtcs,
                                                            unsigned int        *n_crtcs,
                                                            MetaOutput         **outputs,
                                                            unsigned int        *n_outputs);

int                 meta_monitor_manager_get_primary_index (MetaMonitorManager *manager);

gboolean            meta_monitor_manager_handle_xevent     (MetaMonitorManager *manager,
                                                            XEvent             *event);

void                meta_monitor_manager_get_screen_size   (MetaMonitorManager *manager,
                                                            int                *width,
                                                            int                *height);

void                meta_monitor_manager_get_screen_limits (MetaMonitorManager *manager,
                                                            int                *width,
                                                            int                *height);

void                meta_monitor_manager_apply_configuration (MetaMonitorManager  *manager,
                                                              MetaCRTCInfo       **crtcs,
                                                              unsigned int         n_crtcs,
                                                              MetaOutputInfo     **outputs,
                                                              unsigned int         n_outputs);

#define META_TYPE_MONITOR_CONFIG            (meta_monitor_config_get_type ())
#define META_MONITOR_CONFIG(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), META_TYPE_MONITOR_CONFIG, MetaMonitorConfig))
#define META_MONITOR_CONFIG_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  META_TYPE_MONITOR_CONFIG, MetaMonitorConfigClass))
#define META_IS_MONITOR_CONFIG(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), META_TYPE_MONITOR_CONFIG))
#define META_IS_MONITOR_CONFIG_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  META_TYPE_MONITOR_CONFIG))
#define META_MONITOR_CONFIG_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  META_TYPE_MONITOR_CONFIG, MetaMonitorConfigClass))

typedef struct _MetaMonitorConfigClass    MetaMonitorConfigClass;
typedef struct _MetaMonitorConfig         MetaMonitorConfig;

GType meta_monitor_config_get_type (void) G_GNUC_CONST;

MetaMonitorConfig *meta_monitor_config_new (void);

gboolean           meta_monitor_config_match_current (MetaMonitorConfig  *config,
                                                      MetaMonitorManager *manager);

gboolean           meta_monitor_config_apply_stored (MetaMonitorConfig  *config,
                                                     MetaMonitorManager *manager);

void               meta_monitor_config_make_default (MetaMonitorConfig  *config,
                                                     MetaMonitorManager *manager);

void               meta_monitor_config_update_current (MetaMonitorConfig  *config,
                                                       MetaMonitorManager *manager);
void               meta_monitor_config_make_persistent (MetaMonitorConfig *config);

void               meta_crtc_info_free   (MetaCRTCInfo   *info);
void               meta_output_info_free (MetaOutputInfo *info);

#endif
