/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_cert_nfc.c
 * @brief The 13.56 MHz certification requests, to AO_Nfc: its carrier and its listen window.
 */
#include "kode_cert.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "sdkconfig.h"

_Static_assert(KODE_CERT_NFC_FIELD_MS_MAX == KODE_NFC_FIELD_MS_MAX,
               "the facade's listen window is AO_Nfc's ceiling");

/* cppcheck-suppress misra-c2012-20.9 ; sdkconfig.h leaves an option that is off undefined, and #if reads it as 0 */
#if CONFIG_KODEOS_NFC_CERT
/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_cert_nfc_carrier(QActive *me, int32_t mode)
{
    if ( !kode_cert_mode_ok(mode) )
    {
        return KODE_DENIED;
    }
    return nfc_cert_ask(me, (uint8_t)mode);
}
#else  /* !CONFIG_KODEOS_NFC_CERT: the stub refuses */
/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_cert_nfc_carrier(QActive *me, int32_t mode)
{
    (void)me;
    (void)mode;
    return KODE_DENIED;
}
#endif /* CONFIG_KODEOS_NFC_CERT */

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_cert_nfc_field_read(QActive *me, uint32_t ms)
{
    if ( (0U == ms) || (KODE_CERT_NFC_FIELD_MS_MAX < ms) )
    {
        return KODE_DENIED;
    }
    return nfc_field_ask(me, ms);
}
