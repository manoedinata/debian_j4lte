/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * GPL-2.0-or-later, see mm-plugin-samsungipcd.h
 */

#ifndef MM_BROADBAND_BEARER_SAMSUNGIPCD_H
#define MM_BROADBAND_BEARER_SAMSUNGIPCD_H

#include <glib.h>
#include <glib-object.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-broadband-bearer.h"
#include "mm-broadband-modem-samsungipcd.h"

#define MM_TYPE_BROADBAND_BEARER_SAMSUNGIPCD            (mm_broadband_bearer_samsungipcd_get_type ())
#define MM_BROADBAND_BEARER_SAMSUNGIPCD(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_BEARER_SAMSUNGIPCD, MMBroadbandBearerSamsungipcd))
#define MM_BROADBAND_BEARER_SAMSUNGIPCD_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_BEARER_SAMSUNGIPCD, MMBroadbandBearerSamsungipcdClass))
#define MM_IS_BROADBAND_BEARER_SAMSUNGIPCD(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_BEARER_SAMSUNGIPCD))
#define MM_IS_BROADBAND_BEARER_SAMSUNGIPCD_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_BEARER_SAMSUNGIPCD))
#define MM_BROADBAND_BEARER_SAMSUNGIPCD_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_BEARER_SAMSUNGIPCD, MMBroadbandBearerSamsungipcdClass))

typedef struct _MMBroadbandBearerSamsungipcd MMBroadbandBearerSamsungipcd;
typedef struct _MMBroadbandBearerSamsungipcdClass MMBroadbandBearerSamsungipcdClass;

struct _MMBroadbandBearerSamsungipcd {
    MMBroadbandBearer parent;
};

struct _MMBroadbandBearerSamsungipcdClass {
    MMBroadbandBearerClass parent;
};

GType mm_broadband_bearer_samsungipcd_get_type (void);

void          mm_broadband_bearer_samsungipcd_new        (MMBroadbandModemSamsungipcd *modem,
                                                          MMBearerProperties          *config,
                                                          GCancellable                *cancellable,
                                                          GAsyncReadyCallback          callback,
                                                          gpointer                     user_data);
MMBaseBearer *mm_broadband_bearer_samsungipcd_new_finish (GAsyncResult *res,
                                                          GError      **error);

#endif /* MM_BROADBAND_BEARER_SAMSUNGIPCD_H */
