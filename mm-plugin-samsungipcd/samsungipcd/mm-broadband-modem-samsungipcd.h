/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * GPL-2.0-or-later, see mm-plugin-samsungipcd.h
 */

#ifndef MM_BROADBAND_MODEM_SAMSUNGIPCD_H
#define MM_BROADBAND_MODEM_SAMSUNGIPCD_H

#include "mm-broadband-modem.h"

#define MM_TYPE_BROADBAND_MODEM_SAMSUNGIPCD            (mm_broadband_modem_samsungipcd_get_type ())
#define MM_BROADBAND_MODEM_SAMSUNGIPCD(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_SAMSUNGIPCD, MMBroadbandModemSamsungipcd))
#define MM_BROADBAND_MODEM_SAMSUNGIPCD_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_MODEM_SAMSUNGIPCD, MMBroadbandModemSamsungipcdClass))
#define MM_IS_BROADBAND_MODEM_SAMSUNGIPCD(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_SAMSUNGIPCD))
#define MM_IS_BROADBAND_MODEM_SAMSUNGIPCD_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_MODEM_SAMSUNGIPCD))
#define MM_BROADBAND_MODEM_SAMSUNGIPCD_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_MODEM_SAMSUNGIPCD, MMBroadbandModemSamsungipcdClass))

typedef struct _MMBroadbandModemSamsungipcd MMBroadbandModemSamsungipcd;
typedef struct _MMBroadbandModemSamsungipcdClass MMBroadbandModemSamsungipcdClass;

struct _MMBroadbandModemSamsungipcd {
    MMBroadbandModem parent;
};

struct _MMBroadbandModemSamsungipcdClass {
    MMBroadbandModemClass parent;
};

GType mm_broadband_modem_samsungipcd_get_type (void);

MMBroadbandModemSamsungipcd *mm_broadband_modem_samsungipcd_new (const gchar  *device,
                                                                 const gchar **drivers,
                                                                 const gchar  *plugin,
                                                                 guint16       vendor_id,
                                                                 guint16       product_id);

#endif /* MM_BROADBAND_MODEM_SAMSUNGIPCD_H */
