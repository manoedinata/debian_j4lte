/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * GPL-2.0-or-later, see mm-plugin-samsungipcd.h
 */

#include <config.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-iface-modem.h"
#include "mm-broadband-modem-samsungipcd.h"
#include "mm-broadband-bearer-samsungipcd.h"
#include "mm-log-object.h"

static void iface_modem_init (MMIfaceModem *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemSamsungipcd, mm_broadband_modem_samsungipcd, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init))

/*****************************************************************************/
/* Create Bearer (Modem interface) */

static MMBaseBearer *
modem_create_bearer_finish (MMIfaceModem  *self,
                            GAsyncResult  *res,
                            GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
broadband_bearer_new_ready (GObject      *source,
                            GAsyncResult *res,
                            GTask        *task)
{
    MMBaseBearer *bearer;
    GError       *error = NULL;

    bearer = mm_broadband_bearer_samsungipcd_new_finish (res, &error);
    if (!bearer)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

static void
modem_create_bearer (MMIfaceModem        *self,
                     MMBearerProperties  *properties,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    mm_obj_dbg (self, "creating samsungipcd bearer...");
    mm_broadband_bearer_samsungipcd_new (MM_BROADBAND_MODEM_SAMSUNGIPCD (self),
                                         properties,
                                         NULL, /* cancellable */
                                         (GAsyncReadyCallback) broadband_bearer_new_ready,
                                         g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/

MMBroadbandModemSamsungipcd *
mm_broadband_modem_samsungipcd_new (const gchar  *device,
                                    const gchar **drivers,
                                    const gchar  *plugin,
                                    guint16       vendor_id,
                                    guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_SAMSUNGIPCD,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->create_bearer        = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;
}

static void
mm_broadband_modem_samsungipcd_init (MMBroadbandModemSamsungipcd *self)
{
}

static void
mm_broadband_modem_samsungipcd_class_init (MMBroadbandModemSamsungipcdClass *klass)
{
}
