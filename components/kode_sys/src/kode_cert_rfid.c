/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_cert_rfid.c
 * @brief The 125 kHz certification requests, to AO_Rfid: its carrier and its field capture.
 */
#include "kode_cert.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kdrv_rfid_lf.h"    /* the capture's ceiling */
#include "sdkconfig.h"

_Static_assert(KODE_CERT_RFID_FIELD_MS_MAX == KDRV_RFID_LF_CAPTURE_MS_MAX,
               "the facade's window is the driver's ceiling");

/* cppcheck-suppress misra-c2012-20.9 ; sdkconfig.h leaves an option that is off undefined, and #if reads it as 0 */
#if CONFIG_KODEOS_NFC_CERT
/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_cert_rfid_carrier(QActive *me, int32_t mode)
{
    if ( !kode_cert_mode_ok(mode) )
    {
        return KODE_DENIED;
    }
    return rfid_cert_ask(me, (uint8_t)mode);
}
#else  /* !CONFIG_KODEOS_NFC_CERT: the stub refuses */
/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_cert_rfid_carrier(QActive *me, int32_t mode)
{
    (void)me;
    (void)mode;
    return KODE_DENIED;
}
#endif /* CONFIG_KODEOS_NFC_CERT */

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_cert_rfid_field_read(QActive *me, uint16_t *wave, size_t cap, uint32_t ms)
{
    if ( (0U == ms) || (KODE_CERT_RFID_FIELD_MS_MAX < ms) || ((NULL != wave) && (0U == cap)) )
    {
        return KODE_DENIED;
    }
    return rfid_field_ask(me, wave, cap, ms);
}
