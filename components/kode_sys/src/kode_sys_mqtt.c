/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_mqtt.c
 * @brief The MQTT commands, posted to AO_Net, which owns the connection; without CONFIG_KODEOS_NET they link and refuse.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kdrv_net.h"
#include "sdkconfig.h"

#include <string.h>

_Static_assert((uint32_t)KDRV_MQTT_DOWN == KODE_SYS_MQTT_STATE_DOWN, "MQTT states follow the driver's");
_Static_assert((uint32_t)KDRV_MQTT_CONNECTING == KODE_SYS_MQTT_STATE_CONNECTING, "MQTT states follow the driver's");
_Static_assert((uint32_t)KDRV_MQTT_UP == KODE_SYS_MQTT_STATE_UP, "MQTT states follow the driver's");
_Static_assert((uint32_t)KDRV_MQTT_ERR_NONE == KODE_SYS_MQTT_ERR_NONE, "MQTT errors follow the driver's");
_Static_assert((uint32_t)KDRV_MQTT_ERR_NO_NET == KODE_SYS_MQTT_ERR_NO_NET, "MQTT errors follow the driver's");
_Static_assert((uint32_t)KDRV_MQTT_ERR_REFUSED == KODE_SYS_MQTT_ERR_REFUSED, "MQTT errors follow the driver's");
_Static_assert((uint32_t)KDRV_MQTT_ERR_TIMEOUT == KODE_SYS_MQTT_ERR_TIMEOUT, "MQTT errors follow the driver's");
_Static_assert((uint32_t)KDRV_MQTT_ERR_BUSY == KODE_SYS_MQTT_ERR_BUSY, "MQTT errors follow the driver's");

#if CONFIG_KODEOS_NET

/**
 * @brief Whether @p text is a credential a connect carries whole.
 * @param text NULL, "" or a string.
 * @param cap  Bytes allowed, NUL included.
 * @return true for NULL, "" or a string shorter than @p cap.
 */
static bool mqtt_secret_fits(const char *text, size_t cap)
{
    return (NULL == text) || ('\0' == text[0]) || kode_sys_text_fits(text, cap);
}

kode_status_t kode_sys_mqtt_connect(const char *uri, const char *user,
                                const char *pass)
{
    if ( !kode_sys_text_fits(uri, KODE_SYS_MQTT_URI_MAX)
        || !mqtt_secret_fits(user, KODE_SYS_MQTT_USER_MAX)
        || !mqtt_secret_fits(pass, KODE_SYS_MQTT_PASS_MAX) )
    {
        return KODE_DENIED;
    }
    return kode_sys_cmd_post(AO_Net, mqtt_up_evt(uri, user, pass), KQP_NET_CMD_MARGIN);
}

kode_status_t kode_sys_mqtt_disconnect(void)
{
    return kode_sys_cmd_post(AO_Net, mqtt_down_evt(), KQP_NET_CMD_MARGIN);
}

kode_status_t kode_sys_mqtt_publish(const char *topic, const void *data,
                                uint16_t len)
{
    if ( !kode_sys_text_fits(topic, KODE_SYS_MQTT_TOPIC_MAX) || ((NULL == data) && (0U < len)) )
    {
        return KODE_DENIED;
    }
    return kode_sys_cmd_post(AO_Net, mqtt_pub_evt(topic, data, len), KQP_NET_CMD_MARGIN);
}

kode_status_t kode_sys_mqtt_subscribe(const char *topic)
{
    if ( !kode_sys_text_fits(topic, KODE_SYS_MQTT_TOPIC_MAX) )
    {
        return KODE_DENIED;
    }
    return kode_sys_cmd_post(AO_Net, mqtt_sub_evt(topic), KQP_NET_CMD_MARGIN);
}

kode_status_t kode_sys_mqtt_unsubscribe(const char *topic)
{
    if ( !kode_sys_text_fits(topic, KODE_SYS_MQTT_TOPIC_MAX) )
    {
        return KODE_DENIED;
    }
    return kode_sys_cmd_post(AO_Net, mqtt_unsub_evt(topic), KQP_NET_CMD_MARGIN);
}

kode_status_t kode_sys_mqtt_status(kode_sys_mqtt_status_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    (void)memset(out, 0, sizeof(*out));
    out->answers = mqtt_answers(&out->err);
    out->state   = (uint8_t)kdrv_net_mqtt_state();
    kdrv_net_mqtt_uri_copy(out->uri, sizeof(out->uri));
    out->channel = mqtt_channel_holds();
    return KODE_OK;
}

bool kode_sys_mqtt_is_connected(void)
{
    return kdrv_net_mqtt_state() == KDRV_MQTT_UP;
}

#else  /* !CONFIG_KODEOS_NET: the surface links and refuses */

kode_status_t kode_sys_mqtt_connect(const char *uri, const char *user,
                                const char *pass)
{
    (void)uri; (void)user; (void)pass;
    return KODE_DENIED;
}

kode_status_t kode_sys_mqtt_disconnect(void)          { return KODE_DENIED; }

kode_status_t kode_sys_mqtt_publish(const char *topic, const void *data,
                                uint16_t len)
{
    (void)topic; (void)data; (void)len;
    return KODE_DENIED;
}

kode_status_t kode_sys_mqtt_subscribe(const char *topic)
{
    (void)topic;
    return KODE_DENIED;
}

kode_status_t kode_sys_mqtt_unsubscribe(const char *topic)
{
    (void)topic;
    return KODE_DENIED;
}

kode_status_t kode_sys_mqtt_status(kode_sys_mqtt_status_t *out)
{
    (void)out;
    return KODE_DENIED;
}

bool kode_sys_mqtt_is_connected(void)                          { return false; }

#endif /* CONFIG_KODEOS_NET */
