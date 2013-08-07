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

#include "display-private.h"
#include <meta/screen.h>
#include "stack-tracker.h"
#include "ui.h"

#include <cogl/cogl.h>

typedef struct _MetaOutput MetaOutput;
typedef struct _MetaMonitorInfo MetaMonitorInfo;

struct _MetaOutput
{
  MetaMonitorInfo *monitor;
  char *name;
  int width_mm;
  int height_mm;
  CoglSubpixelOrder subpixel_order;
};

struct _MetaMonitorInfo
{
  int number;
  int xinerama_index;
  MetaRectangle rect;
  gboolean is_primary;
  gboolean in_fullscreen;
  float refresh_rate;

  /* The primary or first output for this crtc, 0 if we can't figure out.
     This is a XID when using XRandR, otherwise a KMS id (not implemented) */
  glong output_id;
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
							    int                *n_infos);

MetaOutput         *meta_monitor_manager_get_outputs       (MetaMonitorManager *manager,
							    int                *n_outputs);

int                 meta_monitor_manager_get_primary_index (MetaMonitorManager *manager);

void                meta_monitor_manager_invalidate        (MetaMonitorManager *manager);

#endif
