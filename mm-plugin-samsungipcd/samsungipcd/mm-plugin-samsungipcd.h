/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Plugin for the modem proxy samsungipcd (Samsung IPC modems, e.g. Shannon).
 * Modelled on the MBM plugin.
 */

#ifndef MM_PLUGIN_SAMSUNGIPCD_H
#define MM_PLUGIN_SAMSUNGIPCD_H

#include "mm-plugin.h"

#define MM_TYPE_PLUGIN_SAMSUNGIPCD            (mm_plugin_samsungipcd_get_type ())
#define MM_PLUGIN_SAMSUNGIPCD(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_PLUGIN_SAMSUNGIPCD, MMPluginSamsungipcd))
#define MM_PLUGIN_SAMSUNGIPCD_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_PLUGIN_SAMSUNGIPCD, MMPluginSamsungipcdClass))
#define MM_IS_PLUGIN_SAMSUNGIPCD(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_PLUGIN_SAMSUNGIPCD))
#define MM_IS_PLUGIN_SAMSUNGIPCD_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_PLUGIN_SAMSUNGIPCD))
#define MM_PLUGIN_SAMSUNGIPCD_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_PLUGIN_SAMSUNGIPCD, MMPluginSamsungipcdClass))

typedef struct {
    MMPlugin parent;
} MMPluginSamsungipcd;

typedef struct {
    MMPluginClass parent;
} MMPluginSamsungipcdClass;

GType mm_plugin_samsungipcd_get_type (void);

G_MODULE_EXPORT MMPlugin *mm_plugin_create (void);

#endif /* MM_PLUGIN_SAMSUNGIPCD_H */
