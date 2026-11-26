/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kcli_proto.h
 * @brief The console protocol's error codes, defined once.
 */
#ifndef KCLI_PROTO_H_
#define KCLI_PROTO_H_

#ifdef __cplusplus
extern "C" {
#endif

/* A failed command answers one line, `>err code=<n> msg=<token>`, and the token is one word,
 * never free text. */
/** @brief Why a command line was refused (the `code=` of an `>err` line). */
typedef enum {
    KCLI_ERR_VERB    = 1, /**< Unknown verb. msg = the verb.               */
    KCLI_ERR_MISSING = 2, /**< Missing required key, or a token without
                           *   `=`. msg = the key / the token.             */
    KCLI_ERR_VALUE   = 3, /**< Bad value: wrong type, out of range,
                           *   longer than the request that carries it,
                           *   unknown enum member, or unknown key.
                           *   msg = the key.                              */
    KCLI_ERR_BUSY    = 4, /**< A multi-record command is still in flight;
                           *   wait for its `>ok`/`>err` terminator. One
                           *   verb does not wait: `wifi_scan_stop` is
                           *   answered at once, on its own port, while
                           *   the command in flight goes on to its own
                           *   terminator.
                           *   Also answered when the owner is busy with
                           *   another request, which passes: nfc_field,
                           *   rfid_field, rfid_read and rfid_write while
                           *   the front end is held, the power controls
                           *   while AO_Power waits out a boost or a
                           *   reset, touch or tap while a tap is armed,
                           *   and net_ping, http_get and udp_send while
                           *   AO_Net's worker runs another job. msg = busy. */
    KCLI_ERR_REJECT  = 5, /**< Rejected or failed by the owning AO.
                           *   msg = the verb, or - when the verb alone
                           *   cannot say which way - one word naming the
                           *   part that failed. cd, ls's STAT phase and
                           *   rmdir's STAT phase: volume / missing / notdir.
                           *   cat's STAT phase and rm's STAT phase: volume /
                           *   missing / isdir - the wrong-type word points
                           *   the opposite way from cd's, because cat and rm
                           *   want a file and notdir would claim the path
                           *   they refused is not a directory when the
                           *   whole problem is that it is one. mkdir:
                           *   noparent. rmdir's second round trip: notempty.
                           *   kdrv_storage_mkdir() and kdrv_storage_rmdir()
                           *   fold "the volume will not mount" and "the
                           *   mutation itself was refused" (a missing
                           *   parent, a directory that is not empty) into
                           *   the same KODE_ERROR, and strictly neither word
                           *   is true of the volume story - it is picked
                           *   anyway as the far more likely of the two, and
                           *   for rmdir the volume story cannot even reach
                           *   here: its own STAT phase would already have
                           *   answered `volume` first. rm's second round
                           *   trip: still volume, unchanged - its own
                           *   KODE_ERROR is reachable only through the same
                           *   narrow race window cat's STAT already accepts.
                           *   mkdir, write and cp's own KODE_IO_ERROR:
                           *   iofail, when the parent was genuinely there
                           *   and the medium itself refused the write - the
                           *   counterfeit-card case `noparent`/`volume` used
                           *   to misname. mkdir's KODE_EXISTS: exists, the
                           *   name already taken on a volume that is
                           *   perfectly healthy. KODE_NOT_DIR and
                           *   KODE_IS_DIR from either create: notdir when a
                           *   component of the path is a file, isdir when
                           *   the path itself is a directory and the verb
                           *   wanted a file - the same two words the STAT
                           *   phases use, one level further down the path,
                           *   and neither of them the medium's fault.
                           *   mv and cp's own KODE_ERROR: src,
                           *   when AO_Storage's own stat() of the source
                           *   alone - taken after kdrv_storage_rename()/
                           *   kdrv_storage_copy_open() already failed, since
                           *   neither verb has a STAT phase of its own to
                           *   have asked first - finds nothing there. That
                           *   is what a typo'd `src=` looks like, and the
                           *   one cause of either verb's KODE_ERROR an
                           *   operator hits far more often than a volume
                           *   that will not mount or a destination that
                           *   refuses the write; those two still answer
                           *   `volume`, unchanged. write: appendline, when
                           *   `append=on` and a nonzero `line=` are typed
                           *   together - refused at the handler itself,
                           *   before either key reaches AO_Storage, because
                           *   `append` means add at the end and `line=`
                           *   means replace in the middle, and no `write`
                           *   can mean both at once. nfc_field and
                           *   rfid_field: the owning AO's kode_status_t as
                           *   one word (status_word() in console_ao.c:
                           *   error, timeout, noresources, iofail, ...),
                           *   except that nfc_field says absent for its
                           *   KODE_IO_ERROR, no reader fitted. wifi_scan:
                           *   stopped, when its sweep was ended before its
                           *   last channel, by `wifi_scan_stop` or by a
                           *   job that needed the radio - the networks
                           *   before it were heard, the rest of the air
                           *   was not swept. wifi_join: dropped, behind the
                           *   `>|wifi=` record of where Wi-Fi stands, when
                           *   its join never ran - a newer join or the
                           *   automatic round took its place, or Wi-Fi
                           *   switched off, a leave or the link's end
                           *   dropped it while it waited. ls's listing:
                           *   cut, when it stopped before the folder's
                           *   end - a page
                           *   AO_Storage could not list or be asked for,
                           *   one that brought nothing, or a folder past
                           *   what a page's 16-bit skip counts; the
                           *   entries before it were listed, the rest
                           *   were not. nvs_set: full, when the apps'
                           *   keys would pass their share of the store
                           *   (KODE_NVS_SHARE_MAX); nothing is written. */
    KCLI_ERR_TIMEOUT = 6, /**< The owning AO never answered: the command's
                           *   closing event did not arrive in time. The
                           *   console is idle again. msg = the verb.      */
} kcli_err_t;

#ifdef __cplusplus
}
#endif

#endif /* KCLI_PROTO_H_ */
