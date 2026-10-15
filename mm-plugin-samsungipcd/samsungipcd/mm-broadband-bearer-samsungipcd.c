/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * GPL-2.0-or-later, see mm-plugin-samsungipcd.h
 *
 * Bearer for samsungipcd: ordinary 3GPP packet data over the modem's own
 * network interface (rmnet0), without PPP.
 *
 *   dial:       AT+CGACT=1,<cid>   (answers OK when the session is up)
 *   ip config:  AT+CGCONTRDP=<cid> (address, DNS; static configuration)
 *   disconnect: AT+CGACT=0,<cid>
 *
 * The first activation after samsungipcd started can fail while the modem
 * re-registers, so the dial is retried a few times.
 */

#include <config.h>

#include <string.h>
#include <arpa/inet.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem-at.h"
#include "mm-broadband-bearer-samsungipcd.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"

G_DEFINE_TYPE (MMBroadbandBearerSamsungipcd, mm_broadband_bearer_samsungipcd, MM_TYPE_BROADBAND_BEARER)

#define DIAL_ATTEMPTS        3
#define CGACT_TIMEOUT_SECS   35  /* samsungipcd answers ERROR after 25 s */

/*****************************************************************************/
/* 3GPP dialing (sub-step of the 3GPP connection sequence) */

typedef struct {
    MMBaseModem    *modem;
    MMPortSerialAt *primary;
    guint           cid;
    MMPort         *data;
    guint           attempt;
} DialContext;

static void
dial_context_free (DialContext *ctx)
{
    g_clear_object (&ctx->data);
    g_clear_object (&ctx->primary);
    g_clear_object (&ctx->modem);
    g_slice_free (DialContext, ctx);
}

static MMPort *
dial_3gpp_finish (MMBroadbandBearer  *self,
                  GAsyncResult       *res,
                  GError            **error)
{
    return MM_PORT (g_task_propagate_pointer (G_TASK (res), error));
}

static void dial_activate (GTask *task);

static void
dial_activate_ready (MMBaseModem  *modem,
                     GAsyncResult *res,
                     GTask        *task)
{
    DialContext *ctx;
    GError      *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_full_finish (modem, res, &error)) {
        if (ctx->attempt < DIAL_ATTEMPTS && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            mm_obj_dbg (g_task_get_source_object (task),
                        "activation attempt %u/%u failed: %s, retrying",
                        ctx->attempt, DIAL_ATTEMPTS, error->message);
            g_error_free (error);
            dial_activate (task);
            return;
        }
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_task_return_pointer (task, g_object_ref (ctx->data), g_object_unref);
    g_object_unref (task);
}

static void
dial_activate (GTask *task)
{
    DialContext *ctx;
    gchar       *cmd;

    ctx = g_task_get_task_data (task);
    ctx->attempt++;

    cmd = g_strdup_printf ("+CGACT=1,%u", ctx->cid);
    mm_base_modem_at_command_full (ctx->modem,
                                   ctx->primary,
                                   cmd,
                                   CGACT_TIMEOUT_SECS,
                                   FALSE,
                                   FALSE, /* raw */
                                   g_task_get_cancellable (task),
                                   (GAsyncReadyCallback) dial_activate_ready,
                                   task);
    g_free (cmd);
}

static void
dial_3gpp (MMBroadbandBearer   *self,
           MMBaseModem         *modem,
           MMPortSerialAt      *primary,
           guint                cid,
           GCancellable        *cancellable,
           GAsyncReadyCallback  callback,
           gpointer             user_data)
{
    GTask       *task;
    DialContext *ctx;

    g_assert (primary != NULL);

    task = g_task_new (self, cancellable, callback, user_data);

    ctx = g_slice_new0 (DialContext);
    ctx->modem   = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->cid     = cid;
    g_task_set_task_data (task, ctx, (GDestroyNotify) dial_context_free);

    /* The data goes through the modem's network interface */
    ctx->data = mm_base_modem_get_best_data_port (modem, MM_PORT_TYPE_NET);
    if (!ctx->data) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_NOT_FOUND,
                                 "No valid data port found to launch connection");
        g_object_unref (task);
        return;
    }

    dial_activate (task);
}

/*****************************************************************************/
/* IP config retrieval (sub-step of the 3GPP connection sequence) */

typedef struct {
    MMBaseModem      *modem;
    MMPort           *data;
    MMBearerIpFamily  family;
} GetIpConfigContext;

static void
get_ip_config_context_free (GetIpConfigContext *ctx)
{
    g_clear_object (&ctx->data);
    g_clear_object (&ctx->modem);
    g_slice_free (GetIpConfigContext, ctx);
}

static gboolean
get_ip_config_3gpp_finish (MMBroadbandBearer   *self,
                           GAsyncResult        *res,
                           MMBearerIpConfig   **ipv4_config,
                           MMBearerIpConfig   **ipv6_config,
                           GError             **error)
{
    MMBearerConnectResult *configs;
    MMBearerIpConfig      *ipv4, *ipv6;

    configs = g_task_propagate_pointer (G_TASK (res), error);
    if (!configs)
        return FALSE;

    ipv4 = mm_bearer_connect_result_peek_ipv4_config (configs);
    ipv6 = mm_bearer_connect_result_peek_ipv6_config (configs);
    g_assert (ipv4 || ipv6);
    if (ipv4_config && ipv4)
        *ipv4_config = g_object_ref (ipv4);
    if (ipv6_config && ipv6)
        *ipv6_config = g_object_ref (ipv6);

    mm_bearer_connect_result_unref (configs);
    return TRUE;
}

/* "255.255.255.255" -> 32 */
static guint
netmask_to_prefix (const gchar *mask)
{
    struct in_addr addr;
    guint32        value;
    guint          bits = 0;

    if (!mask || !mask[0] || inet_pton (AF_INET, mask, &addr) != 1)
        return 32; /* point to point link: the modem gives no mask */

    for (value = ntohl (addr.s_addr); value & 0x80000000u; value <<= 1)
        bits++;
    return bits;
}

static gboolean
usable_address (const gchar *str)
{
    return str && str[0] && g_strcmp0 (str, "0.0.0.0") != 0;
}

static void
get_ip_config_ready (MMBaseModem  *modem,
                     GAsyncResult *res,
                     GTask        *task)
{
    GetIpConfigContext    *ctx;
    const gchar           *response;
    GError                *error = NULL;
    gchar                 *address = NULL;
    gchar                 *subnet = NULL;
    gchar                 *gateway = NULL;
    gchar                 *dns1 = NULL;
    gchar                 *dns2 = NULL;
    MMBearerIpConfig      *ipv4_config;
    MMBearerConnectResult *connect_result;
    const gchar           *dns[3] = { NULL, NULL, NULL };
    guint                  n_dns = 0;

    ctx = g_task_get_task_data (task);

    response = mm_base_modem_at_command_full_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!mm_3gpp_parse_cgcontrdp_response (response, NULL, NULL, NULL,
                                           &address, &subnet, &gateway, &dns1, &dns2,
                                           &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!usable_address (address)) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't get IP config: no address in '%s'",
                                 response);
        goto out;
    }

    if (!(ctx->family & (MM_BEARER_IP_FAMILY_IPV4 | MM_BEARER_IP_FAMILY_IPV4V6))) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_UNSUPPORTED,
                                 "Only IPv4 is supported");
        goto out;
    }

    ipv4_config = mm_bearer_ip_config_new ();
    mm_bearer_ip_config_set_method  (ipv4_config, MM_BEARER_IP_METHOD_STATIC);
    mm_bearer_ip_config_set_address (ipv4_config, address);
    mm_bearer_ip_config_set_prefix  (ipv4_config, netmask_to_prefix (subnet));
    if (usable_address (gateway))
        mm_bearer_ip_config_set_gateway (ipv4_config, gateway);
    if (usable_address (dns1))
        dns[n_dns++] = dns1;
    if (usable_address (dns2) && g_strcmp0 (dns2, dns1) != 0)
        dns[n_dns++] = dns2;
    if (n_dns)
        mm_bearer_ip_config_set_dns (ipv4_config, dns);

    connect_result = mm_bearer_connect_result_new (ctx->data, ipv4_config, NULL);
    g_task_return_pointer (task, connect_result, (GDestroyNotify) mm_bearer_connect_result_unref);
    g_object_unref (ipv4_config);

out:
    g_object_unref (task);
    g_free (address);
    g_free (subnet);
    g_free (gateway);
    g_free (dns1);
    g_free (dns2);
}

static void
get_ip_config_3gpp (MMBroadbandBearer   *self,
                    MMBroadbandModem    *modem,
                    MMPortSerialAt      *primary,
                    MMPortSerialAt      *secondary,
                    MMPort              *data,
                    guint                cid,
                    MMBearerIpFamily     ip_family,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    GetIpConfigContext *ctx;
    GTask              *task;
    gchar              *cmd;

    ctx = g_slice_new0 (GetIpConfigContext);
    ctx->modem  = g_object_ref (MM_BASE_MODEM (modem));
    ctx->data   = g_object_ref (data);
    ctx->family = ip_family;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) get_ip_config_context_free);

    cmd = g_strdup_printf ("+CGCONTRDP=%u", cid);
    mm_base_modem_at_command_full (MM_BASE_MODEM (modem),
                                   primary,
                                   cmd,
                                   10,
                                   FALSE,
                                   FALSE, /* raw */
                                   NULL, /* cancellable */
                                   (GAsyncReadyCallback) get_ip_config_ready,
                                   task);
    g_free (cmd);
}

/*****************************************************************************/
/* 3GPP disconnect */

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer  *self,
                        GAsyncResult       *res,
                        GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
disconnect_ready (MMBaseModem  *modem,
                  GAsyncResult *res,
                  GTask        *task)
{
    GError *error = NULL;

    /* A modem that is no longer connected answers ERROR; that is the state
     * we want, so do not make a failure out of it */
    if (!mm_base_modem_at_command_full_finish (modem, res, &error)) {
        mm_obj_dbg (g_task_get_source_object (task), "deactivation reported: %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer   *self,
                 MMBroadbandModem    *modem,
                 MMPortSerialAt      *primary,
                 MMPortSerialAt      *secondary,
                 MMPort              *data,
                 guint                cid,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
    GTask *task;
    gchar *cmd;

    g_assert (primary != NULL);

    task = g_task_new (self, NULL, callback, user_data);

    cmd = g_strdup_printf ("+CGACT=0,%u", cid);
    mm_base_modem_at_command_full (MM_BASE_MODEM (modem),
                                   primary,
                                   cmd,
                                   15,
                                   FALSE,
                                   FALSE, /* raw */
                                   NULL, /* cancellable */
                                   (GAsyncReadyCallback) disconnect_ready,
                                   task);
    g_free (cmd);
}

/*****************************************************************************/

MMBaseBearer *
mm_broadband_bearer_samsungipcd_new_finish (GAsyncResult  *res,
                                            GError       **error)
{
    GObject *bearer;
    GObject *source;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    /* Only export valid bearers */
    mm_base_bearer_export (MM_BASE_BEARER (bearer));

    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_samsungipcd_new (MMBroadbandModemSamsungipcd *modem,
                                     MMBearerProperties          *config,
                                     GCancellable                *cancellable,
                                     GAsyncReadyCallback          callback,
                                     gpointer                     user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_SAMSUNGIPCD,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BASE_BEARER_MODEM,  modem,
        MM_BASE_BEARER_CONFIG, config,
        NULL);
}

static void
mm_broadband_bearer_samsungipcd_init (MMBroadbandBearerSamsungipcd *self)
{
}

static void
mm_broadband_bearer_samsungipcd_class_init (MMBroadbandBearerSamsungipcdClass *klass)
{
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    broadband_bearer_class->dial_3gpp                 = dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish          = dial_3gpp_finish;
    broadband_bearer_class->get_ip_config_3gpp        = get_ip_config_3gpp;
    broadband_bearer_class->get_ip_config_3gpp_finish = get_ip_config_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp           = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish    = disconnect_3gpp_finish;
}
