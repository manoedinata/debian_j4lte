/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * GPL-2.0-or-later, see mm-plugin-samsungipcd.h
 */

#include <config.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log-object.h"
#include "mm-plugin-samsungipcd.h"
#include "mm-broadband-modem-samsungipcd.h"

G_DEFINE_TYPE (MMPluginSamsungipcd, mm_plugin_samsungipcd, MM_TYPE_PLUGIN)

MM_PLUGIN_DEFINE_MAJOR_VERSION
MM_PLUGIN_DEFINE_MINOR_VERSION

/*****************************************************************************/

static MMBaseModem *
create_modem (MMPlugin     *self,
              const gchar  *uid,
              const gchar **drivers,
              guint16       vendor,
              guint16       product,
              GList        *probes,
              GError      **error)
{
    return MM_BASE_MODEM (mm_broadband_modem_samsungipcd_new (uid,
                                                              drivers,
                                                              mm_plugin_get_name (self),
                                                              vendor,
                                                              product));
}

/*****************************************************************************/

G_MODULE_EXPORT MMPlugin *
mm_plugin_create (void)
{
    /* The ports are not recognised by vendor or product (they are virtual
     * ttys plus the modem's rmnet interface), a udev rule has to tag them. */
    static const gchar *subsystems[] = { "tty", "net", NULL };
    static const gchar *udev_tags[] = {
        "ID_MM_SAMSUNGIPCD",
        NULL
    };

    return MM_PLUGIN (
        g_object_new (MM_TYPE_PLUGIN_SAMSUNGIPCD,
                      MM_PLUGIN_NAME,               MM_MODULE_NAME,
                      MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                      MM_PLUGIN_ALLOWED_UDEV_TAGS,  udev_tags,
                      MM_PLUGIN_ALLOWED_AT,         TRUE,
                      /* The ports are virtual and have no kernel driver. Without
                       * these two the port check would insist on finding a driver
                       * name and filter every port. Nothing here is a QMI or MBIM
                       * modem, the udev tag decides which ports reach this plugin. */
                      MM_PLUGIN_ALLOWED_QMI,        TRUE,
                      MM_PLUGIN_ALLOWED_MBIM,       TRUE,
                      NULL));
}

static void
mm_plugin_samsungipcd_init (MMPluginSamsungipcd *self)
{
}

static void
mm_plugin_samsungipcd_class_init (MMPluginSamsungipcdClass *klass)
{
    MMPluginClass *plugin_class = MM_PLUGIN_CLASS (klass);

    plugin_class->create_modem = create_modem;
}
