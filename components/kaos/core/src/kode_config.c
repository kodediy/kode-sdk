/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_config.c
 * @brief A setting's two doors: kode_config_post(), which takes a request to
 *        the owner, and kode_config_answer(), which brings its value back.
 */
#include <stdio.h>
#include <string.h>

#include "kaos.h"
#include "kode_config.h"
#include "kregistry.h"

bool kode_config_post(KodeConfigReqEvt *req, void const *sender)
{
    bool sent = false;

    (void)sender;
    if ( NULL != req )
    {
        uint16_t margin = 0U;
        QActive *const owner = kconfig_owner(kconfig_find(req->key), &margin);

        if ( NULL == owner )
        {
            /* No table holds the key: nobody will take the event. */
            QF_gc(&req->super);
        }
        else
        {
            /* A refused post gives the event back to its pool. */
            sent = QACTIVE_POST_X(owner, &req->super, margin, sender);
        }
    }
    return sent;
}

const kode_config_t *kode_config_find(const char *key)
{
    return kconfig_find(key);
}

bool kode_config_answer(QActive *owner, const KodeConfigReqEvt *req,
                        kode_status_t status, bool stored, const char *value)
{
    KodeConfigAnsEvt *e = NULL;
    const char *word = "";
    bool sent = false;

    (void)owner;
    if ( NULL != value )
    {
        word = value;
    }
    if ( (NULL != req) && (strlen(word) <= KODE_CONFIG_VALUE_MAX) )
    {
        e = Q_NEW_X(KodeConfigAnsEvt, 1U, CONFIG_ANS_SIG);
    }
    if ( NULL != e )
    {
        e->seq    = req->seq;
        e->status = (uint8_t)status;
        e->stored = stored;
        (void)snprintf(e->value, sizeof(e->value), "%s", word);
        (void)snprintf(e->key, sizeof(e->key), "%s", req->key);
        QACTIVE_PUBLISH(&e->super, owner);
        sent = true;
    }
    return sent;
}
