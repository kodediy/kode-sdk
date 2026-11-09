/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_ao.c
 * @brief The two places an app keeps things, and how an AO asks for files without ever waiting.
 */
#include "storage_ao.h"

#include <stdio.h>
#include <string.h>

#define TAG "storage"

/** The folder this app keeps to. The FAT is shared by every app on the board,
 *  and the volume has no notion of an owner, so a folder of one's own is the
 *  whole convention. AO_Storage makes ONE level and refuses a missing parent,
 *  which is why the folder is a direct child of the volume. */
#define DEMO_DIR         "/storage/storage_demo"

/** The file in it. */
#define DEMO_LOG         DEMO_DIR "/log.txt"

/** How many lines the file holds before the next write starts it over. */
#define DEMO_MAX_LINES   8U

/** Room for the file's text, terminator included. Eight lines of the format
 *  below are about 250 bytes; this leaves room and bounds how much a file
 *  that is not ours could send back. */
#define DEMO_READ_MAX    512U

/** Room for one line of the log, terminator included. */
#define DEMO_LINE_LEN    64U

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive  super;
    uint32_t boots;    /**< Starts of this app so far, this run included. */
    uint32_t presses;  /**< Writes asked for this run; it numbers the lines. */
    uint32_t lines;    /**< Lines this run has put in the file. Zero means the
                        *   file is from an earlier run, or absent. */
    bool     reading;  /**< A read is out: @c text is lent to AO_Storage.  */
    bool     writing;  /**< A write is out: @c line is lent to AO_Storage. */
    char     line[DEMO_LINE_LEN];  /**< The line a write sends.            */
    char     text[DEMO_READ_MAX];  /**< Where a read puts the file.        */
} StorageDemo;

/* The two arrays kodeOS declares for every AO of its own, written out here
 * rather than hidden behind a macro: they are exactly the two numbers you have
 * to think about. The queue holds a burst of button edges and the answers of
 * the requests out at once, one per kind; the stack is what the deepest state
 * function puts on the stack. The SDK's defaults are plenty for both. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static StorageDemo l_ao;
QActive * const AO_StorageDemo = &l_ao.super;

/* No step here waits: kode.h's kode_file_*() block until AO_Storage answers,
 * so they belong to a task. */
static QState StorageDemo_initial(StorageDemo * const me,
                                  void const * const par);
static QState StorageDemo_running(StorageDemo * const me,
                                  QEvt const * const e);

void StorageDemo_ctor(uint32_t boots)
{
    StorageDemo * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&StorageDemo_initial));
    me->boots   = boots;
    me->presses = 0U;
    me->lines   = 0U;
    me->reading = false;
    me->writing = false;
    (void)memset(me->line, 0, sizeof(me->line));
    (void)memset(me->text, 0, sizeof(me->text));
}

void StorageDemo_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts: a full queue
     * names the victim, never the culprit. */
    QActive_setAttr(AO_StorageDemo, TASK_NAME_ATTR, "storage");
    QACTIVE_START(AO_StorageDemo, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief Append one line to the log, or start the file over.
 *
 * Whether it appends is this AO's decision and the request's flag: with
 * @c append false the same call truncates. The first line of a run replaces
 * whatever an earlier run left, and a full file starts over, so the file never
 * grows past DEMO_MAX_LINES and one read brings all of it back.
 *
 * The folder was asked for in the initial transition, and AO_Storage serves
 * its queue in order, so it exists by the time this request is handled.
 *
 * @param me The AO.
 */
static void write_line(StorageDemo * const me)
{
    bool          append;
    kode_status_t rc;

    me->presses++;
    (void)snprintf(me->line, sizeof(me->line), "boot %lu press %lu at %lu ms\n",
                   (unsigned long)me->boots, (unsigned long)me->presses,
                   (unsigned long)kode_sys_time_ms());

    append = (0U != me->lines) && (DEMO_MAX_LINES > me->lines);

    /* LENT: AO_Storage reads me->line in place until FILE_WRITE_RSP_SIG. */
    rc = kode_sys_file_write(&me->super, DEMO_LOG, me->line,
                             (uint32_t)strlen(me->line), append);
    if ( KODE_OK == rc )
    {
        if ( !append )
        {
            me->lines = 0U;
        }
        me->lines++;
        me->writing = true;
    }
    else
    {
        KLOG_W(TAG, "write not taken (status %d)", (int)rc);
    }
}

/**
 * @brief Ask for the whole file, in one window from its start.
 *
 * One byte of @c text is kept back for the terminator, so the answer can be
 * printed as a string.
 *
 * @param me The AO.
 */
static void read_log(StorageDemo * const me)
{
    /* LENT: AO_Storage fills me->text in place just before FILE_READ_RSP_SIG. */
    kode_status_t const rc = kode_sys_file_read(&me->super, DEMO_LOG, 0U,
                                                me->text, DEMO_READ_MAX - 1U);

    if ( KODE_OK == rc )
    {
        me->reading = true;
        KLOG_I(TAG, "reading %s", DEMO_LOG);
    }
    else
    {
        KLOG_W(TAG, "read not taken (status %d)", (int)rc);
    }
}

/**
 * @brief A button did something: the top one writes, the bottom one reads.
 * @param me The AO.
 * @param be The button event.
 */
static void on_button(StorageDemo * const me, ButtonEvt const * const be)
{
    /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
    if ( me->reading || me->writing )
    {
        /* A buffer is lent: ignored until its answer. */
    }
    else if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
    {
        write_line(me);
    }
    else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
    {
        read_log(me);
    }
    else
    {
        /* A release, or another button: nothing to do. */
    }
}

/**
 * @brief The folder's answer.
 *
 * KODE_EXISTS is the ordinary answer on every start but the first, and it is
 * as good as KODE_OK: the postcondition holds.
 *
 * @param a The answer.
 */
static void on_folder(kode_sys_file_answer_t const * const a)
{
    if ( ((uint8_t)KODE_OK == a->rc) || ((uint8_t)KODE_EXISTS == a->rc) )
    {
        KLOG_I(TAG, "folder %s is there", DEMO_DIR);
    }
    else
    {
        KLOG_W(TAG, "no folder (status %u): the writes will fail",
               (unsigned)a->rc);
    }
}

/**
 * @brief The write's answer; the line is this AO's again.
 * @param me The AO.
 * @param a  The answer.
 */
static void on_write(StorageDemo * const me,
                     kode_sys_file_answer_t const * const a)
{
    me->writing = false;
    if ( (uint8_t)KODE_OK == a->rc )
    {
        KLOG_I(TAG, "line written; the bottom button reads it back");
    }
    else
    {
        KLOG_W(TAG, "write failed (status %u)", (unsigned)a->rc);
    }
}

/**
 * @brief The read's answer: print the text a line at a time.
 *
 * @c got bytes arrived in @c text, at most the DEMO_READ_MAX - 1 asked for. A
 * file that is not there ends the read with a status other than KODE_OK.
 *
 * @param me The AO.
 * @param a  The answer.
 */
static void on_read(StorageDemo * const me,
                    kode_sys_file_answer_t const * const a)
{
    me->reading = false;
    if ( (uint8_t)KODE_OK == a->rc )
    {
        uint32_t const got   = a->got;
        uint32_t       start = 0U;
        uint32_t       lines = 0U;
        uint32_t       i;

        me->text[got] = '\0';
        for ( i = 0U; i <= got; i++ )
        {
            if ( (got == i) || ('\n' == me->text[i]) )
            {
                if ( i > start )
                {
                    KLOG_I(TAG, "  %.*s", (int)(i - start), &me->text[start]);
                    lines++;
                }
                start = i + 1U;
            }
        }
        KLOG_I(TAG, "%lu lines read", (unsigned long)lines);
    }
    else
    {
        KLOG_W(TAG, "nothing to read (status %u); the top button writes",
               (unsigned)a->rc);
    }
}

/**
 * @brief The initial transition: subscribe, then ask for the folder.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to StorageDemo_running.
 */
static QState StorageDemo_initial(StorageDemo * const me,
                                  void const * const par)
{
    kode_status_t rc;

    (void)par;

    /* The one published signal this AO listens to. AO_Storage's answers are
     * posted to it directly and need no subscription. */
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "boot number %lu; the top button writes a line, the bottom one "
                "reads the file back", (unsigned long)me->boots);

    /* Asked now and answered later, on this AO's own thread. The folder may
     * already be there from an earlier run: the answer says so. */
    rc = kode_sys_file_create_folder(&me->super, DEMO_DIR);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "folder not asked for (status %d)", (int)rc);
    }
    return Q_TRAN(&StorageDemo_running);
}

/**
 * @brief The one state: buttons ask, AO_Storage answers.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState StorageDemo_running(StorageDemo * const me,
                                  QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            on_button(me, Q_EVT_CAST(ButtonEvt));
            status = Q_HANDLED();
            break;
        }
        case FILE_FOLDER_RSP_SIG: {
            on_folder(Q_EVT_CAST(kode_sys_file_answer_t));
            status = Q_HANDLED();
            break;
        }
        case FILE_WRITE_RSP_SIG: {
            on_write(me, Q_EVT_CAST(kode_sys_file_answer_t));
            status = Q_HANDLED();
            break;
        }
        case FILE_READ_RSP_SIG: {
            on_read(me, Q_EVT_CAST(kode_sys_file_answer_t));
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
