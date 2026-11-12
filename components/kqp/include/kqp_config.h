/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_config.h
 * @brief kodeOS-level QP configuration: priorities, pools, tick rate.
 */
#ifndef KQP_CONFIG_H_
#define KQP_CONFIG_H_

#include "sdkconfig.h"   /* CONFIG_KODE_SCHEDULER, CONFIG_KODE_TEST_DOT: whether KODE_PRIO_USER_HIGH and _LOW exist */


/* ---- Priorities ---------------------------------------------------------
 * QP priorities map 1:1 onto FreeRTOS priorities in this port, so this list
 * is the whole scheduling policy of kodeOS, highest last. Three gaps are
 * left on purpose for the owners that are still to come, so adding one does
 * not renumber the rest.
 *
 *   3  Gui       rendering: by far the heaviest work here and the least
 *                urgent, so it sits at the bottom where a slow repaint can
 *                only ever delay itself. It must never make the console
 *                stutter. The one level in this map the SDK does not start:
 *                it is the consumer's (KODE_PRIO_USER_GUI) - kgui's launcher
 *                in the kodeOS image, an app's own UI in the app's.
 *   4  reader    console reader task (not an AO; blocks on the console)
 *   4  Radio     the C5 link. Shares the level with the reader on purpose:
 *                the AO itself never blocks, but the worker task it owns
 *                (kdrv_radio) does - both are blocking I/O pumps that must
 *                sit below everything with real work to do, and FreeRTOS
 *                happily round-robins the three tasks at this level. Legal
 *                for QP because the reader and the worker are not AOs.
 *   5  (Storage) the media: the longest blocking I/O in the system, so it goes
 *                as low as it can and only ever delays itself
 *   6  Log       owns the console device. Above the reader so a keystroke is
 *                echoed the moment it arrives, below everything that has real
 *                work to do - a slow terminal must not hold up the system.
 *   7  LedMgr    owns the LED
 *   8  (Power)   the PMIC and the gauge, polled every couple of seconds
 *   9  AppMgr    owns the app slot
 *  10  Autotest  walks the self-test rows; the rows it judges itself run
 *                here, under the console and under every owner with a
 *                deadline
 *  11  Console   parses command lines
 *  12  Boot      owns otadata. Rarely runs; when it does the board is about
 *                to reset anyway
 *  13  (Buttons) short periodic polls, like the touch
 *  14  Touch     short periodic polls; wants low jitter to feel responsive
 *  15  Display   short DMA kick-offs on the pixel path; must not queue behind
 *                a repaint, or the repaint it is serving stalls itself
 *  17  IoExp     the expander's interrupt: one I2C read and one publish per
 *                edge, the shortest step in the system. Above Display because
 *                a button or touch edge must not wait behind a repaint it is
 *                about to change - and it can sit up there precisely because
 *                it never does anything else
 *  18  Ir        This is the one level in the map that is not where it
 *                belongs. Every entry above is ordered by urgency; 18 is
 *                ordered by nothing but arithmetic - it is the only level a
 *                raised QF_MAX_ACTIVE (16 -> 25, see qp_config.h) opened, and
 *                in QP a higher number is a higher priority, so AO_Ir lands
 *                over AO_IoExp: above the button, the touch and the repaint
 *                it is supposed to sit well below. A block this slow has no
 *                business up here.
 *
 *                It is survivable for exactly one reason: AO_Ir's own step
 *                is short regardless of level. The RMT transmits by DMA and
 *                reports completion through an ISR, so what AO_Ir does on
 *                receiving a request is "queue the frame and return", never
 *                "wait 68 ms for a NEC frame to finish". A step that short
 *                cannot bury anything beneath it, wherever it sits.
 *
 *                One step contradicts that, and naming it is what makes the
 *                argument worth anything: the first step of the production
 *                row, `test component=ir`, busy-waits about 1.1 ms inside
 *                kdrv_ir_rx_present() while the receiver's line is driven
 *                low and left to recover. At this level that holds the
 *                buttons, the touch and a repaint for that long. It is
 *                affordable because it happens once per production test and
 *                on no other path - and the exception is the boundary: a
 *                second step of that shape, on a path the board takes in
 *                ordinary use, is the point at which this level stops being
 *                survivable and the map has to be renumbered.
 *
 *                If a future owner needs a level and its own step is NOT
 *                short, the answer is renumbering this map, not stacking
 *                another level on top of it the way this one was.
 *
 * A user app is a whole separate firmware, and that is precisely why it does
 * share this scheduler: it carries its own copy of the runtime, so inside an
 * app's image the author's Active Objects and kodeOS's are the same AOs in
 * the same QF. kode_sys.h exists to invite that. What is left for them is at
 * the bottom of this list - QF_MAX_ACTIVE is 25 (the full room FreeRTOS's
 * fixed configMAX_PRIORITIES leaves in this ESP-IDF version; see
 * qp_config.h), the owners take 4..15 and 17..19, so 1 and 16 were free below
 * the ceiling, and both were worth naming; 16 is AO_Scheduler's now, below,
 * so an app that turns CONFIG_KODE_SCHEDULER on has USER_HIGH taken. (20 to 23 are owners' now -
 * AO_Audio, AO_Imu, AO_Haptic and AO_Rfid - and 24 is AO_LowPower's, below;
 * 25 is free but unnamed: it was
 * opened for AO_Ir alone, not offered to consumers, and a level up there sits
 * above every owner in the map, which is not a place to hand out without the
 * same scrutiny 18 and 19 got. It is also not reachable: FreeRTOS takes 24 as
 * its highest priority, so a task asked for 25 is clamped to it.)
 * The GUI level is a consumer level too, and kodeOS keeps no private name
 * for it: kgui takes KODE_PRIO_USER_GUI from outside the SDK exactly as an
 * app's renderer would, because on this map kodeOS is an app like any other.
 */
/* Below the GUI, and the only thing that is: a network call is the slowest
 * I/O in the system - a TLS handshake, a four-second ping - and down here it
 * can only ever delay itself. Its worker task shares the level, the way the
 * radio's does, which is legal because a worker is not an Active Object.
 * Priority 1 is still free. */
#define KODE_PRIO_NET           2U    /**< AO_Net + its worker task.        */
/* 3 is the renderer's - a consumer level, defined with its family below as
 * KODE_PRIO_USER_GUI. */
#define KODE_PRIO_READER        4U    /**< Console reader task (blocking).  */
/* The boot task is FreeRTOS's main task, at priority 1, and it runs every
 * owner's initial transition. The renderer starts in the early row and draws
 * without a pause; where the port puts it on the boot task's core, a walk at
 * priority 1 gets only the gaps between its passes, and the rows below the
 * early one come up seconds late. One level above the renderer is enough. */
#define KODE_PRIO_BOOT_WALK     (KODE_PRIO_USER_GUI + 1U) /**< The boot task while kode_init() starts and parks the rows. */
#define KODE_PRIO_RADIO         4U    /**< AO_Radio + its worker task.      */
#define KODE_PRIO_STORAGE       5U    /**< AO_Storage + its storage_sd worker task. */
#define KODE_PRIO_LOG           6U    /**< AO_Log (the console device).     */
#define KODE_PRIO_LED           7U    /**< AO_LedMgr.                       */
#define KODE_PRIO_POWER         8U    /**< AO_Power.                        */
#define KODE_PRIO_APPMGR        9U    /**< AO_AppMgr.                       */
/* AO_Autotest, right under the console that is its first client. A row it
 * judges itself runs in its own step - the crystals' holds the core for tens
 * of milliseconds while it counts - and down here that delays the console
 * and nothing that must not wait: every owner with a deadline sits above.
 * Under the console rather than over it for a second reason: the console
 * then prints a row's record before this AO asks the next row, so the log
 * lines of one row never land above the verdict of the one before. */
#define KODE_PRIO_AUTOTEST     10U    /**< AO_Autotest.                     */
#define KODE_PRIO_CONSOLE      11U    /**< AO_Console.                      */
#define KODE_PRIO_BOOT         12U    /**< AO_Boot.                         */
#define KODE_PRIO_BUTTONS      13U    /**< AO_Buttons.                      */
#define KODE_PRIO_TOUCH        14U    /**< AO_Touch.                        */
#define KODE_PRIO_DISPLAY      15U    /**< AO_Display.                      */
#define KODE_PRIO_IOEXP        17U    /**< AO_IoExp: the expander's INT.    */
#define KODE_PRIO_IR           18U    /**< AO_Ir. See the note above.       */
/* 19 sits over every owner in the map, and it survives the same scrutiny 18
 * did, for a reason that is stronger here than there: RFAL's poller is a
 * state machine, not a blocking call. rfalNfcDiscover() arms it,
 * rfalNfcWorker() advances it one step and returns, and a data exchange
 * splits into rfalNfcDataExchangeStart() and rfalNfcDataExchangeGetStatus().
 * AO_Nfc's discovery pumps and returns; it never waits on the radio. Polling
 * it slowly costs latency and nothing else, because in poller mode this board
 * sets the tempo - which stops being true the moment the chip listens
 * instead, and is why emulation is not behind this level.
 *
 * The one wait on the radio inside AO_Nfc's step is a listen window's: each
 * step waits on the chip's own conversions, KNFC_LISTEN_BURST (16) of them,
 * each bounded by the driver's KDRV_ST25_MEASURE_TOUT_MS (10 ms), and opening
 * the window waits on the oscillator, bounded at 10 ms as well. The driver
 * logs the window's longest step at debug (`listen: <n> bursts, step max <us>
 * us`), as wall time, a higher priority's preemption included. On the bench a
 * 50 ms window holds its five bursts in five ticks (n=80) and its longest
 * step is 5.1 ms, so each step ends inside its own tick and holds this
 * core for about half of it while a window runs. */
#define KODE_PRIO_NFC          19U    /**< AO_Nfc. See the note above.      */
/* 20 is AO_Audio's, and it is arithmetic rather than urgency - said plainly
 * because the alternative is somebody later reading it as a claim. There is
 * no free level below the owners: they hold 2, 4 to 15, 17, 18 and 19, and 1
 * and 16 are consumer levels the SDK has promised not to take. So it takes
 * the next level above them.
 *
 * It survives the scrutiny 18 got, and more easily. AO_Ir had to declare one
 * step that busy-waits a millisecond; AO_Audio has none at all. Everything
 * that blocks is on the worker, and this AO's longest step is a single I2C
 * register read serving the console's `audio`. A step that short cannot bury
 * anything beneath it wherever it sits.
 *
 * The worker shares the level, as AO_Net's and AO_Radio's do, and here the
 * high level is deserved rather than tolerated: its deadline is a DMA buffer
 * at 48 kHz, and a refill that queues behind a repaint is an audible click.
 *
 * Taking KODE_PRIO_USER_LOW instead was rejected. It is the documented "start
 * here" level for an app's own AO, and the SDK spending a consumer level on
 * itself is a worse precedent than an honest 20. */
#define KODE_PRIO_AUDIO        20U    /**< AO_Audio. See the note above.    */
#define KODE_PRIO_AUDIO_WORKER 20U    /**< Its worker. Not an Active Object. */
/* 21 and 22 are the IMU's and the haptic's, and they are the same arithmetic
 * 20 was: there is no free level below the owners, 1 and 16 are consumer
 * levels the SDK has promised not to take, and renumbering the map touches
 * the startup order, several vault pages and every `ps` transcript in them.
 *
 * Both survive the scrutiny 18 got, and the figures are the chip's rather
 * than a hope. AO_Imu's longest step is one chunk of a witness window -
 * sixteen samples of six bytes each, about 2.5 ms of bus at 400 kHz - PLUS
 * the Goertzel, because the chunk that empties the FIFO runs it in the same
 * step: 128 samples in float, well under a millisecond on the P4's FPU, for
 * about 3 ms together. A whole 128-sample drain is tens of milliseconds and
 * would sit above AO_Audio's I2S refill, which is precisely why it is not
 * one step: AO_Imu reads a chunk, arms its one-shot for the next and lets
 * everything above it breathe. AO_Haptic's longest step is one SYSST
 * read and two 64-byte bursts, about 3 ms. Both are under one I2S transfer
 * (5 ms, one 240-frame descriptor at 48 kHz), so neither can bury the codec's
 * refill - or anything else beneath it - wherever it sits.
 *
 * The haptic sits above the IMU because its tick has a deadline - the RTP
 * FIFO drains in about five milliseconds and an underrun is a stutter in
 * the buzz - and the IMU has none. */
#define KODE_PRIO_IMU          21U    /**< AO_Imu. See the note above.      */
#define KODE_PRIO_HAPTIC       22U    /**< AO_Haptic. See the note above.   */
#define KODE_PRIO_RFID         23U    /**< AO_Rfid + its rfid_worker task. In
                                       *   the band above the owners: the
                                       *   T5577 downlink must not be
                                       *   preempted mid-bit. Above the
                                       *   esp_timer task (22), level with
                                       *   the ST25R3916's servicer.         */

/* 24 is AO_LowPower's, and it is the one AO that belongs above every owner:
 * a wake has to be decided before the owners it wakes run, and a sleep has
 * to be able to ask them all to rest. Its steps are short (a round is a
 * publish and a count; the sleep itself happens in the idle task), so the
 * height costs the owners below nothing. */
#define KODE_PRIO_LOWPOWER     24U    /**< AO_LowPower. See the note above. */

/* 16 is AO_Scheduler's. The map has no free level among the owners and none
 * above AO_LowPower either (24 is the highest FreeRTOS gives a task here, and
 * QF_MAX_ACTIVE cannot pass configMAX_PRIORITIES), so the scheduler takes the
 * level the SDK had kept for an app's own logic above the pixel path,
 * KODE_PRIO_USER_HIGH. It earns it: its step is a pass over sixteen entries
 * and a post, it needs the minute to arrive on time while AO_Gui draws all the
 * face's time, and nothing it does touches a device.
 *
 * It is behind CONFIG_KODE_SCHEDULER, off in the SDK, and an app that turns the
 * option on gives KODE_PRIO_USER_HIGH up: the name is not defined then, so an
 * app that still starts an AO of its own at it does not compile, where QP
 * would answer a second start at an occupied level with an assert at boot. */
#define KODE_PRIO_SCHEDULER    16U    /**< AO_Scheduler. See the note above. */

/* 1 is AO_TestDot's, the E5 final test. It owns nothing while it sleeps and,
 * awake, only the station on J3: the camera, the relays and the ADC. It
 * decides a run and waits on everybody else, so it belongs where it can only
 * ever delay itself, below AO_Net. The map has no free level, so it takes the
 * one the SDK keeps for an app's own AO at the bottom, KODE_PRIO_USER_LOW.
 *
 * It is behind CONFIG_KODE_TEST_DOT, off in the SDK, and an app that turns
 * the option on gives KODE_PRIO_USER_LOW up: the name is not defined then, as
 * KODE_PRIO_USER_HIGH is not with the scheduler. */
#define KODE_PRIO_TEST_DOT      1U    /**< AO_TestDot. See the note above. */
#define KODE_PRIO_TEST_DOT_WORKER 1U  /**< Its worker task. Not an Active Object. */

/* ---- AO_TestDot: the E5 final test --------------------------------------- */
#define KODE_TEST_DOT_QUEUE_LEN     32U     /**< AO_TestDot's queue: a run's answers, the worker's, the host's and the timers. */
#define KODE_TEST_DOT_REQ_MARGIN    4U      /**< Slots a request leaves free in AO_TestDot's queue, for the answers it waits on. */
#define KODE_TEST_DOT_POST_MARGIN   4U      /**< Slots a record leaves free in AO_Console's queue, for its own lines. */
#define KODE_TEST_DOT_POOL_MARGIN   8U      /**< Blocks a record leaves free in its pool, for everybody else's events. */
#define KODE_TEST_DOT_WORKER_STACK  16384U  /**< The worker's stack, in PSRAM: the analysis and the JPEG encoder run on it. */
#define KODE_TEST_DOT_WORKER_DEPTH  2U      /**< Jobs the worker holds besides the one running. */
#define KODE_TEST_DOT_RAIL_MS       45000U  /**< The longest the wake waits for J3's rail: a Wi-Fi sweep can hold the C5 ~40 s. */
#define KODE_TEST_DOT_PROBE_MS      2000U   /**< The camera's probe on J3: it answers in well under a second when it is there. */
#define KODE_TEST_DOT_CHECK_MARGIN_MS 3000U /**< What the fixture check adds to the rail and the probe: the station's I2C and the posts. */
/** @brief The longest the fixture check takes: the rail, the probe and the margin. */
#define KODE_TEST_DOT_CHECK_MS      (KODE_TEST_DOT_RAIL_MS + KODE_TEST_DOT_PROBE_MS + KODE_TEST_DOT_CHECK_MARGIN_MS)
#define KODE_TEST_DOT_POLL_RING     64U     /**< Records a run keeps for `test_dot_poll` after the line closed; the oldest goes. */
#define KODE_TEST_DOT_DRAIN_MS      20U     /**< The wait before a record the console's queue refused is posted again. */
#define KODE_TEST_DOT_RUN_MS        240000U /**< A run's watchdog: the operator part may take minutes; past it the run ends. */
#define KODE_TEST_DOT_OP_SETTLE_MS  150U    /**< The operator's screen is on the glass before the first prompt. */
#define KODE_TEST_DOT_SCREEN_MS     2000U   /**< The longest a screen or a pattern waits for its host; past it the step goes on. */
#define KODE_TEST_DOT_JOB_MS        20000U  /**< The longest one worker job may take before the step gives it up. */
#define KODE_TEST_DOT_JPEG_MS       2000U   /**< The longest one photo's encode waits for AO_Display: an encode takes milliseconds. */
#define KODE_TEST_DOT_ANSWER_MS     5000U   /**< The longest an owner's answer may take (the boost, the dock's pins): AO_Power answers at its next 2 s poll. */
#define KODE_TEST_DOT_RESET_MS      KODE_RESET_CONSOLE_MS /**< The longest the SHIP reset may take to answer: every step at its budget. */
#define KODE_TEST_DOT_RADIO_QUEUE_MS 25000U /**< What a radio check may wait behind AO_Radio's running job: a join takes 20 s. */
/** @brief The longest `test_dot_radio` waits for RADIO_PROBE_SIG before it answers a timeout. */
#define KODE_TEST_DOT_RADIO_MS      (KODE_RADIO_PROBE_MS + KODE_TEST_DOT_RADIO_QUEUE_MS)
/** @brief A whole Wi-Fi sweep at its bound: KDRV_WIFI_SCAN_TIMEOUT_MS, checked
 *         between steps, plus the last step's KDRV_WIFI_SCAN_STEP_TIMEOUT_MS
 *         (test_dot_ao.c asserts both). One sweep measures ~36 s. */
#define KODE_TEST_DOT_SWEEP_MS      102000U
#define KODE_TEST_DOT_DISCOVER_MARGIN_MS 5000U /**< What the discovery's bound adds to its parts: the posts and the card's RPCs. */
/**
 * @brief The longest `test_dot_radio discover=on` waits for RADIO_PROBE_SIG:
 *        the radio check's own bound (the queue and the budget before the
 *        sweep), the sweep, the budget AO_Radio counts again after it, and
 *        the margin.
 */
#define KODE_TEST_DOT_DISCOVER_MS   (KODE_TEST_DOT_RADIO_MS + KODE_TEST_DOT_SWEEP_MS + KODE_RADIO_PROBE_MS \
                                     + KODE_TEST_DOT_DISCOVER_MARGIN_MS)
/** @brief The longest `test_dot_loop` takes: the dock's pins lent, the frames' job, the pins back. */
#define KODE_TEST_DOT_LOOP_MS       (KODE_TEST_DOT_ANSWER_MS + KODE_TEST_DOT_JOB_MS + KODE_TEST_DOT_ANSWER_MS)
/**
 * @brief The longest `test_dot_end` takes before it answers: the boost off,
 *        a job still out, the dock's pins back, then the camera's close job.
 */
#define KODE_TEST_DOT_END_MS        (KODE_TEST_DOT_ANSWER_MS + KODE_TEST_DOT_JOB_MS + KODE_TEST_DOT_ANSWER_MS \
                                     + KODE_TEST_DOT_JOB_MS)
#define KODE_TEST_DOT_LINE_MARGIN_MS 5000U /**< What the console's deadline adds to the AO's longest bound: the request's post and the answer's way back. */
/**
 * @brief The console's deadline for a station verb's line: above the longest
 *        bound the AO itself keeps (the SHIP reset of `test_dot_result
 *        verdict=pass`, every step at its budget), so the AO's own answer, a
 *        timeout included, always reaches the line before the console gives
 *        it up. The station's wait sits above this one. Also above the
 *        fixture check and a run's watchdog together (test_dot_ao.c asserts it).
 */
#define KODE_TEST_DOT_LINE_MS       (KODE_TEST_DOT_RESET_MS + KODE_TEST_DOT_LINE_MARGIN_MS)
#define KODE_TEST_DOT_BOOST_LIM_CODE 2U     /**< BQ25896 BOOST_LIM for the pads delivery test: 1.2 A. */
#define KODE_TEST_DOT_LOCK_KEY      "locked" /**< The lock's key in KODE_STATION_NVS_NS. */
#define KODE_TEST_DOT_LOCK_ON       1U      /**< The value the lock's key holds. */
#define KODE_TEST_DOT_GRID_LEN      17U     /**< Characters of the automatic phase's grid: identification, twelve rows, four radio. */
#define KODE_TEST_DOT_LOOP_MAX      100U    /**< Most frames `test_dot_loop` sends at each speed. */
#define KODE_TEST_DOT_LOOP_DEF      20U     /**< Frames at each speed when `count` is absent. */
#define KODE_TEST_DOT_ADC_CHANNELS  4U      /**< The ADS1115's inputs. */
#define KODE_TEST_DOT_WHY_MAX       16U     /**< Bytes of a refusal's word, NUL included. */
#define KODE_TEST_DOT_NAME_MAX      32U     /**< Bytes of a blob's name, NUL included. */
#define KODE_TEST_DOT_OUT_MAX       96U     /**< Records, ends and loans waiting for room in the console's queue: a poll's ring and its end fit. */
#define KODE_TEST_DOT_BLOB_LINE     120U    /**< Bytes of a blob in one `d=` record: 160 base64 characters. */
#define KODE_TEST_DOT_BLOB_MS       60000U  /**< The longest a lent blob may take to come back before the line goes on without it. */
#define KODE_TEST_DOT_LED_SETTLE_MS 70U     /**< The RGB LED's colour is lit before its photo. */
#define KODE_TEST_DOT_OFF_SETTLE_MS 90U     /**< The panel's brightness change has settled before the next photo. */
#define KODE_TEST_DOT_PLACE_MARK_MS 8000U   /**< `test_dot_place`: how long the marks may take to be seen, by default. */
#define KODE_TEST_DOT_PLACE_MM10    150U    /**< `test_dot_place`: the most the screen may sit off, 0.1 mm, by default. */
#define KODE_TEST_DOT_PLACE_DEG10   50U     /**< `test_dot_place`: the most the screen may turn, 0.1 degree, by default. */
#define KODE_TEST_DOT_PLACE_ARG_MAX 60000U  /**< `test_dot_place`: the largest number its keys take. */
#define KODE_TEST_DOT_PLACE_RETRY_MS 500U  /**< `test_dot_place`: the pause between two looks for the marks. */
/* AO_TestDot's stack. The camera's records are formatted on the worker, and
 * the AO's own records straight into PSRAM slots, so its deepest path is a
 * log line under a few frames of its own: about 2 KB counted from the image's
 * frames, under 2.9 KB with every error path the count cannot rule out. */
#define KODE_TEST_DOT_STACK         KQP_AO_STACK_MEDIUM /**< AO_TestDot's stack: the medium tier. */

/* ---- AO_LowPower: rounds, windows and defaults ---------------------------
 * The timings the spec pins; AO_LowPower and its participants read them. */
#define KODE_LOWPOWER_PARTICIPANTS     24U     /**< Owners that can join a round. */
#define KODE_LOWPOWER_WHO_NONE         0xFFU   /**< lowpower_join(): the table is full. */
#define KODE_LOWPOWER_WHY_MAX          24U     /**< Bytes of a busy reason, NUL included. */
#define KODE_LOWPOWER_REFUSAL_MAX      48U     /**< Bytes of a refusal, "who: why", NUL included. */
#define KODE_LOWPOWER_SRC_APP          0xFEU   /**< lowpower_request(): asked by an app, through the facade. */
#define KODE_LOWPOWER_SRC_SCHED        0xFDU   /**< lowpower_request_rest(): asked by AO_Scheduler, its work done. */
#define KODE_LOWPOWER_ROUND_MS         2000U   /**< Longest wait for a round's answers. */
#define KODE_LOWPOWER_OFF_GRACE_MS     100U    /**< Between STATE(OFF) and deep sleep, for the log to drain. */
#define KODE_LOWPOWER_REQUEST_RETRY_MS 1000U   /**< A requested round that met a busy or silent owner begins again this long
                                                *   after it was abandoned. */
#define KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS 10000U /**< The longest a request is begun again, counted from the request;
                                                *   the requester is answered once, when it runs out. */
#define KODE_LOWPOWER_ORDER_MS         10000U  /**< The longest a request for OFF waits for a busy or silent owner;
                                                *   the board goes OFF when it runs out, done or not. */
#define KODE_LOWPOWER_QUEUE_LEN        48U     /**< AO_LowPower's queue: every participant's answer, a request and the inputs. */
#define KODE_LOWPOWER_RADIO_MAX_MS     60000U  /**< Longest a radio wake lasts before the radio owners are asked to rest, done or not. */
#define KODE_LOWPOWER_SLEEP_AFTER_DEF_S 120U   /**< Idle time before ASLEEP: 2 min. */
#define KODE_LOWPOWER_OFF_AFTER_DEF_S  1200U   /**< Asleep time before OFF: 20 min. */
#define KODE_LOWPOWER_DIM_PCT          20U     /**< The glass in ASLEEP when no glyphs were handed;
                                                *   the interface sets its own through rest_pct. */
#define KODE_LOWPOWER_MINUTE_S         60U     /**< The minute the glass is lit for. */
#define KODE_LOWPOWER_GLYPHS_WAIT_MS   1500U   /**< Longest AO_Display waits for the renderer's glyphs
                                                *   after a round opens: under KODE_LOWPOWER_ROUND_MS, so
                                                *   it can still answer busy and name the reason. */
#define KODE_LOWPOWER_MINUTE_RETRY_MS  150U    /**< AO_Display's pause before it writes a minute again that did not
                                                *   reach the panel: past a flash erase in another task. */
#define KODE_LOWPOWER_GLYPH_DIGITS     10U     /**< Digits in the glyph strip; the colon follows them. */
#define KODE_LOWPOWER_CLOCK_DIGITS     4U      /**< Digits of the time on the glass: hh and mm. */
#define KODE_LOWPOWER_STATE_RETRY_MS   20U     /**< AO_LowPower's wait before it publishes a state change again
                                                *   after the event pool had none left to carry it. */
#define KODE_LOWPOWER_LAST_SAMPLES     60U     /**< Gauge samples the summary of the last rest keeps: one a
                                                *   minute, so the last hour of it. */
#define KODE_LOWPOWER_LAST_VERSION     1U      /**< The layout of the summary in the store; another reads as none. */
#define KODE_LOWPOWER_LAST_OPEN        0xFFU   /**< The summary's end cause when the rest left none: the board
                                                *   was asleep at the last write, and a reset ended it. */
#define KODE_DISPLAY_RESTORE_RETRY_MS  200U    /**< AO_Display's wait before it puts the glass back again
                                                *   after the link or the panel refused to wake. */

/* ---- AO_Scheduler: the table, the clock and the lines it runs -------------- */
#define KODE_SCHED_MAX           16U     /**< Entries, the built-in `mqtt` included (its slot is always taken). */
#define KODE_SCHED_NAME_MAX      15U     /**< Characters in an entry's name. */
#define KODE_SCHED_WHEN_MAX      63U     /**< Characters in a time expression; five fields of lists fit with room. */
#define KODE_SCHED_CMD_MAX       KODE_CONSOLE_LINE_MAX  /**< Bytes of a console entry's line, NUL included: the buffer a ConsoleLineEvt carries. */
#define KODE_SCHED_WHY_MAX       16U     /**< Bytes of a refusal's reason, NUL included. */
#define KODE_SCHED_QUEUE_LEN     24U     /**< AO_Scheduler's queue: every entry's set at an app's boot, and the line's done. */
#define KODE_SCHED_REQ_MARGIN    4U      /**< Slots a request leaves free in the queue, for the line's done and the timer. */
#define KODE_SCHED_POST_MARGIN   2U      /**< Slots a post to another AO (a line, an event of code) leaves free in its queue, as the channel's do. */
#define KODE_SCHED_CLOCK_POLL_MS 1000U   /**< How often the wall clock is asked while it is not set. */
#define KODE_SCHED_ARM_MAX_MS    60000U  /**< The longest the minute timer waits: the ticks it counts are compared with the clock again within it. */
#define KODE_SCHED_SETTLE_MS     5000U   /**< After boot, no entry fires yet: a minute that falls in it runs when it ends, if still in that minute. Whether anything may be posted at all is sched_system_up()'s to say. */
#define KODE_SCHED_BACK_MAX_MIN  60U     /**< A clock this many minutes or more behind the last minute run is a clock set back, not a jitter: the entries run from the new time on. */
#define KODE_SCHED_END_MARGIN    1U      /**< Slots an entry of an answer leaves free in the asker's queue, for the end of the answer. */
/** @brief The longest AO_Scheduler waits for the work of a minute: the answer of a console line, or the
 *         event of an entry of code to be handled. Past it the work is given up on, the lines not yet
 *         gone out are dropped and the board is asked to rest, so a verb that never answers cannot keep a
 *         scheduled wake awake. The console answers a verb in well under a second and one that waits on
 *         the radio in a few; ten seconds is past those and far short of the minute that is the least
 *         period of an entry, so the next fire never finds the last one still out by the bound. */
#define KODE_SCHED_WORK_BOUND_MS 10000U
/** @brief How long AO_Scheduler waits before it gives a line again that the console was busy for. The bound
 *         above ends the tries: a line still refused when it runs out is dropped with a WARN. */
#define KODE_SCHED_RETRY_MS      1000U

/* ---- The consumer levels -------------------------------------------------
 * kode_init() starts nothing at these levels: they belong to whoever builds
 * on the SDK. They are named so that choosing a level is choosing rather
 * than guessing, and so that adding an owner later renumbers nothing. An app
 * starts its own AOs here and nowhere else: every other level is an owner's,
 * free in an app that does not link it but still that owner's - `ps` lists a
 * level under its owner's name only when the task there is the owner's.
 */
#if CONFIG_KODE_TEST_DOT
/* KODE_PRIO_USER_LOW is KODE_PRIO_TEST_DOT's level, and with the E5 test
 * built it is taken: the name is left undefined on purpose. */
#else
#define KODE_PRIO_USER_LOW      1U    /**< Below AO_Net: an app's own AO at
                                       *   the one level where it can only
                                       *   ever delay itself. Start here.
                                       *   The same level as
                                       *   KODE_PRIO_TEST_DOT, so it is not
                                       *   defined when CONFIG_KODE_TEST_DOT
                                       *   is on.                          */
#endif
/* The standard layer's listener task is not an Active Object, so this is a
 * task's level and no slot of QP's: it shares the number with the level an
 * app's own AO starts at, and the two never collide. It runs the user's
 * listener functions, which may take as long as they like, so it sits where
 * it can only ever delay itself: below every owner AO. */
#define KODE_PRIO_STANDARD_EVENTS 1U    /**< The standard layer's dispatch task
                                       *   (kode_event_add_listener()).      */
/* The standard layer's sound tasks: the player (kode_play) feeds the codec's
 * 200 ms ring and the recorder (kode_rec) waits on the codec. Below the
 * renderer they would starve while a screen redraws, so they sit above it,
 * at AO_Radio's level, as a task may. Not USER_HIGH: that is app logic's. */
#define KODE_PRIO_STANDARD_AUDIO  KODE_PRIO_RADIO  /**< The standard layer's sound tasks
                                       *   (kode_audio_play_start(), kode_audio_record_start()). */
/* The standard layer's background readers poll a part through the extended
 * layer and post nothing of their own; at the sound tasks' level, for their
 * reason. */
#define KODE_PRIO_STANDARD_READER KODE_PRIO_RADIO  /**< The standard layer's background
                                       *   readers (infrared capture, NFC). */
/* The renderer's level. Rendering is the heaviest work in the system and
 * the least urgent - only the network sits below - and that is true of
 * anyone's renderer, so the role gets one level and kodeOS keeps no private
 * one: kgui starts AO_Gui here, from outside the SDK, exactly as an app
 * drawing its own screen starts its renderer here. The level holds a single
 * tenant - QP answers a second QACTIVE_START at an occupied priority with
 * an assertion and a reset, not a tie - so an image that carries kgui has
 * it taken, and an image that does not has it waiting. */
#define KODE_PRIO_USER_GUI      3U    /**< The renderer: kgui's launcher in
                                       *   kodeOS, an app's UI in its own
                                       *   image.                           */
_Static_assert(KODE_PRIO_STANDARD_AUDIO > KODE_PRIO_USER_GUI, "the sound tasks outrank the renderer");
_Static_assert(KODE_PRIO_STANDARD_READER > KODE_PRIO_USER_GUI, "the background readers outrank the renderer");
#if CONFIG_KODE_SCHEDULER
/* KODE_PRIO_USER_HIGH is KODE_PRIO_SCHEDULER's level, and with the scheduler
 * built it is taken: the name is left undefined on purpose. */
#else
#define KODE_PRIO_USER_HIGH    16U    /**< Above AO_Display. Only with a
                                       *   reason you can write down - app
                                       *   logic above the pixel path is how
                                       *   a repaint starts stalling on
                                       *   somebody's state machine. The
                                       *   same level as KODE_PRIO_SCHEDULER,
                                       *   so it is not defined when
                                       *   CONFIG_KODE_SCHEDULER is on.     */
#endif

/* ---- QP tick ------------------------------------------------------------ */
/* 1 ms, the rate FreeRTOS already runs at (CONFIG_FREERTOS_HZ=1000). The
 * panel refreshes every 16.8 ms and a renderer woken a tick late loses that
 * much of the frame: at 10 ms a frame had as little as 6.8 ms left to be
 * drawn in, at 1 ms it has 15.8. Measured on the DVT, the faster tick costs
 * 2.7 % of core 0 (the esp_timer task that drives it lives there) and no
 * current the gauge can see. */
#define KQP_TICKS_PER_SEC       1000U /**< 1 ms QP tick, driven by kbsp. */

/* Milliseconds to ticks. At 1000 ticks a second this is the identity, written
 * so that it cannot overflow: (ms * 1000) / 1000 wrapped a uint32_t past 71
 * minutes. kqp.c refuses a rate that is not a whole number of kilohertz.
 *
 * Every timer is armed through this, never with a bare tick count, so a
 * duration means the same thing whatever the rate. 0 ms is 0 ticks, and QP
 * answers an arm of 0 ticks with an assertion failure and a board restart
 * (Q_REQUIRE 440 in qf_time.c): a caller computing this from a runtime value
 * - an operator's number, a field off the wire - checks it before arming. */
#define KQP_TICKS_PER_MS(ms_)   ((ms_) * (KQP_TICKS_PER_SEC / 1000U))

/* ---- LedMgr -------------------------------------------------------------- */
#define KODE_LED_BLINK_MS       500U  /**< Default heartbeat period; can be
                                       *   changed at runtime via
                                       *   LED_BLINK_PERIOD_SIG. */
#define KODE_LED_BLINK_MIN_MS   10U   /**< The shortest period accepted: each
                                       *   half of a blink is an I2C write
                                       *   to the KTD2026, and nothing on
                                       *   this glass needs a faster one.  */
/**
 * @brief Half of AO_Power's write pulse, in milliseconds: lit this long, then
 *        dark this long, for as long as a flash write reports progress.
 *
 * The LED's own cadence rather than AO_LedMgr's, which is why it lives beside
 * the heartbeat and not in the power block. One second end to end reads as
 * activity rather than a state, and costs one published event and one I2C
 * write per half, on an AO that is otherwise asleep between two-second polls.
 */
#define KODE_LED_PULSE_MS        500U

/* ---- Console (kcli) -----------------------------------------------------
 * The console is the whole kodeOS front-end in this phase: a blocking reader
 * task feeds whole lines to AO_Console over the event bus, which parses them
 * and posts the resulting commands to the owning AOs.
 */
/* The longest command line. 240 rather than a rounder number: it began as
 * the widest line any verb could legitimately produce, `mqtt_pub
 * topic="<79>" data="<127>"` at 231 bytes. The MQTT payload cap now follows
 * the line instead (KODE_MQTT_PAYLOAD_MAX is this number, since a command
 * arrives on a topic as a whole line), so `mqtt_pub` fills what the line
 * leaves it and no more.
 *
 * It was 80, and at 80 three events downstream declared fields the line could
 * not fill - WifiJoinEvt.pass is cut for a 64-byte PSK that could never arrive
 * beside a 32-byte SSID, because `wifi_join ssid="<32>" pass="<63>"` is 120 bytes.
 * The editor did not refuse those: it truncated them and ran the truncated
 * command.
 *
 * This sizes the medium event pool: ConsoleLineEvt is what KqpMediumBlock is
 * typedef'd from in kqp.c, so this number IS that tier's width. At 252 bytes a
 * block and KQP_MEDIUM_POOL_COUNT blocks it costs about 10 KB of internal
 * DRAM. (It said 6 KB for a while: that was true at a count of 24, and the
 * count was raised without the number being revisited.) The ceiling is the
 * large tier at 268 bytes: QF requires strictly increasing pool block sizes
 * and kqp.c asserts KQP_BLOCK_MEDIUM < KQP_BLOCK_LARGE at compile time. */
#define KODE_CONSOLE_LINE_MAX  240U   /**< Max command line length (bytes).  */
#define KODE_FUNCTION_REC_MAX  192U   /**< Max record a function answers with. */
#define KODE_FUNCTION_MSG_MAX  16U    /**< Max word after msg= of a function's code 5. */
#define KODE_CONFIG_KEY_MAX    15U    /**< Max chars in a setting's key (KODE_ROW_NAME_MAX). */
#define KODE_CONFIG_VALUE_MAX  32U    /**< Max chars in a setting's value (KODE_VALUE_STR_MAX). */
#define KODE_GUI_SHEET_MAX     10U    /**< Max word naming the sheet gui_info says. */
#define KODE_GUI_KODY_MAX      8U     /**< Max word naming Kody's state in gui_info. */
#define KODE_GUI_GROUP_MAX     16U    /**< Max name of the launcher's open group in gui_info, terminator included. */
#define KODE_GUI_LEVEL_ROOT    0U     /**< The launcher shows its root: the groups, then the loose apps. */
#define KODE_GUI_LEVEL_GROUP   1U     /**< The launcher shows the apps of one group. */

/* Which port a command line came in on. The console answers every verb the
 * same way whoever typed it, with two exceptions: a terminal install and its
 * kin switch the input stream to raw, and the stream they switch belongs to one
 * port; and a few verbs are for the cable alone (see src_allows() in
 * kaos/console/src/console_ao.c, which holds the whole policy).
 *
 * NET is a line that arrived on the Kode channel's cmd/in topic. It has no
 * wire of its own to hand over: the channel carries lines, not bytes.
 *
 * SCHED is a line AO_Scheduler runs on its own account at a cron time. Nobody
 * typed it and no wire carries it, so it is not use of the board: AO_LowPower
 * neither counts it as input nor lets it wake the board. */
#define KODE_CONSOLE_SRC_SERIAL   0U
#define KODE_CONSOLE_SRC_BLE      1U
#define KODE_CONSOLE_SRC_NET      2U
#define KODE_CONSOLE_SRC_SCHED    3U

/* A terminal install carries a firmware image in over the same wire. The
 * chunk is the unit the reader task reads, the event pool block, and the
 * write handed to the apps region - one number, because three different ones
 * would only mean two of them were copies. 4 KB matches what AO_AppMgr
 * already reads the mailbox in. */
#define KODE_RECV_CHUNK         4096U
/* A source string, terminator included: a file name, or a URL of up to 256
 * characters, which leaves room for a token in it. kode_sys_flash() can pass all
 * of them; a console line (KODE_CONSOLE_LINE_MAX) carries fewer. A file name
 * is still bounded by KODE_APP_FILE_MAX. */
#define KODE_FLASH_SRC_MAX       257U
/* `dst=` values 0..2 are kfw_target_t. This one is not a container target -
 * nothing declares itself "for the mailbox" - it is an instruction to park
 * whatever arrives without decoding it. */
#define KODE_FLASH_DST_PENDING     3U
/* A watchdog on the gap between chunks, not on the transfer: a 5 MB image
 * over a slow link must not time out, but a host that dies mid-image must not
 * leave the console waiting forever. AO_AppMgr rearms it on every chunk. */
#define KODE_RECV_IDLE_MS       5000U

/* How long a BLE scan listens. Long enough to catch a phone's slow
 * advertising interval (up to ~1.3 s) several times over, short enough that
 * the console command does not feel hung. */
#define KODE_BLE_SCAN_MS        5000U

/** @brief The shortest scan ble_scan_request() takes: it still hears an
 *         advertiser at a 100 ms interval several times. */
#define KODE_BLE_SCAN_MIN_MS    500U

/** @brief The longest: under the radio worker's longest job, a dual-band
 *         Wi-Fi sweep of about 38 s, so a scan never holds the link longer
 *         than one already does. */
#define KODE_BLE_SCAN_MAX_MS    30000U

/* There is no advertising window. A board with BLE on advertises until
 * somebody connects, and advertises again when they leave - see AO_Radio's
 * invariant. What used to bound the exposure was the clock; what bounds it
 * now is the pairing gate on the console service, which is a better answer
 * because it does not also make the board unreachable to its owner. */

/* ---- The network ---------------------------------------------------------
 * These size the events AO_Net publishes, and every one of them has a twin in
 * kdrv_net.h - a driver cannot include the scheduler's configuration, so the
 * two halves are written down twice and each names the other. They must agree.
 *
 * The two listing ceilings obey the same rule KODE_STORAGE_LIST_MAX states:
 * every entry of a burst reaches every subscriber, so both must stay well
 * below the shallowest subscriber queue (AO_Console and AO_Gui are 64).
 */
#define KODE_NET_HOST_MAX       64U   /**< Host name or literal address.     */
#define KODE_NET_URL_MAX        80U   /**< A URL - console-sized, and the
                                       *   sized for one, though the console's
                                       *   own line is KODE_CONSOLE_LINE_MAX,
                                       *   which is no longer 80.            */
#define KODE_NET_UDP_MAX        64U   /**< Datagram payload from an event.   */
#define KODE_NET_CTYPE_MAX      48U   /**< An ask's POST Content-Type, NUL
                                       *   included. No twin: the driver takes
                                       *   a pointer.                        */
#define KODE_NET_PING_MAX       10U   /**< Echoes one net_ping may publish.  */
#define KODE_NET_HTTP_CHUNK    192U   /**< Bytes per NET_HTTP_DATA_SIG: 191
                                       *   characters and a terminator.      */
#define KODE_NET_HTTP_CHUNKS     8U   /**< How many of those one GET emits.
                                       *   8 x 191 = 1528, which is exactly
                                       *   KDRV_NET_BODY_MAX - the driver
                                       *   keeps what can be shown, and no
                                       *   more, so `shown=` stays true.     */
/* How long the blocking facade's GET gives one fetch, headers and body together. The
 * worker's own GET uses the same number from the driver's side of the copy. */
#define KODE_HTTP_TIMEOUT_MS 15000U
/* How long AO_Net waits for a broker before calling the connect a failure.
 * The client keeps trying underneath - what expires here is the *answer*, so
 * a console command closes instead of hanging on an unreachable host. */
#define KODE_MQTT_CONNECT_MS 15000U
/** @brief How long one of kdrv_net's six jobs (ping, GET, POST, datagram,
 *         MQTT start and stop) may be with AO_Net's worker before its guard
 *         fires: past the longest honest one, a ten-echo ping or a TLS GET
 *         against a slow server. The point past which the worker is wedged
 *         rather than working. */
#define KODE_NET_GUARD_MS             60000U
/** @brief What the guard of the download's open and of each read adds to the
 *         driver's own cap on it, so the cap answers first and the guard is
 *         for a worker that never comes back. */
#define KODE_NET_STREAM_SLACK_MS       5000U
/** @brief The guard of the download's close: shutting a TLS connection waits
 *         at most one I/O. */
#define KODE_NET_CLOSE_GUARD_MS       30000U
/** @brief The least a radio wake for the scheduler's `mqtt` check lasts, counted from the wake's
 *         beginning: what the C5 or the broker holds for the P4 needs this long to flow in once the
 *         sentinel stops (a TCP retransmit, a retained message, the C5's own queue) before the radio owners
 *         rest again. A first figure from reading the paths, to be measured on the bench. */
#define KODE_NET_CHECK_DWELL_MS        3000U
_Static_assert(KODE_NET_CHECK_DWELL_MS < KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS,
               "a rest refused for the check's dwell must be begun again after the dwell ends");
#define KODE_MQTT_URI_MAX       96U
#define KODE_MQTT_USER_MAX      33U
#define KODE_MQTT_PASS_MAX      65U
#define KODE_MQTT_TOPIC_MAX     80U
#define KODE_MQTT_PAYLOAD_MAX  KODE_CONSOLE_LINE_MAX   /**< Of one message,
                                       *   in or out: a whole console line,
                                       *   because a command arrives on a
                                       *   topic. A longer one is delivered
                                       *   cut, with the flag set - never
                                       *   dropped.                          */
/* The Kode channel's queue of commands: lines that arrive on cmd/in while the
 * console is busy wait here, and go to it one at a time, in order. */
#define KODE_CHAN_QUEUE_MAX     32U   /**< Lines the channel holds for the console, the one it is running
                                       *   included; a line past it is answered `>err code=4 msg=busy`.  */
#define KODE_CHAN_LINE_BOUND_MS 30000U /**< The longest AO_Net waits for the console to answer a line before
                                       *   it gives the next one: one that never answers (a reset, `sleep`)
                                       *   cannot hold the queue for ever. A verb that runs longer (`cp`, a
                                       *   dual-band scan, `install`) is passed at the bound, and its late
                                       *   end is ignored: it carries the sequence of a line no longer out. */
/* The screen upload's two event bounds: the token an order carries and the
 * id a 201 answers with. kaos.h holds the rest of the upload's limits. */
#define KSHOT_TOKEN_MAX        128U   /**< Characters of an upload token.    */
#define KSHOT_ID_MAX            48U   /**< A capture's id, terminator
                                       *   included; a longer one is "".     */

/* ---- Logging -------------------------------------------------------------
 * Must match klog's own formatting buffer: a line longer than this is
 * truncated by the formatter before AO_Log ever sees it.
 */
#define KODE_LOG_LINE_MAX       256U

/* ---- The generic store ---------------------------------------------------
 *
 * A namespace kodeOS writes into and never interprets. It exists because an
 * app here is a whole firmware: whatever it needs handed to it - a token, an
 * identifier, whatever the next app invents - has to survive a reset and a
 * change of image, and the alternative was a file, which any console can
 * already fetch off the board with `file_get`.
 *
 * Deliberately unopinionated. The day a particular key earns a verb of its
 * own it gets one; until then naming them individually would be guessing at
 * a shape nobody has yet.
 *
 * It lives here, beside the console's own ceilings, because both `kaos` (the
 * verbs) and `kode` (the facade an app calls) have to spell it identically
 * and neither may include the other's headers.
 */
#define KODE_NVS_NAMESPACE     "kapp"
/* Buffer sizes, NUL included. The key ceiling is NVS's own - 15 characters -
 * and the facade enforces it rather than letting the backend decide, so the
 * refusal is the same wherever this is stored. The value ceiling is what a
 * console line has left after `nvs_set key="<15>" value=""`, rounded down:
 * at KODE_CONSOLE_LINE_MAX the arithmetic allows 202, and the slack is there
 * so a longer verb never has to move this number. */
#define KODE_SYS_NVS_KEY_MAX        16U
#define KODE_SYS_NVS_VALUE_MAX     192U
/* How many keys one `nvs_list` may answer with. The whole burst is in flight
 * at once - AO_Log's queue is what absorbs it - and unlike `ps`, whose record
 * count is fixed by the AO table, this one is whatever an operator has
 * stored. A capped listing says so, in a '!' line, the way AO_Storage's
 * does. */
#define KODE_NVS_LIST_MAX       24U

/** @brief The longest absolute path kodeOS will carry. Two independent limits bound
 * it, they agree, and that agreement is the only reason to trust the number.
 *
 * The console line. The widest verb is `cp src="<p>" dst="<p>"`, which
 * spends 16 bytes on its own keys and quotes before either path.
 * KODE_CONSOLE_LINE_MAX is 240, so a path may not exceed 112.
 *
 * The event pool. `mv` and `cp` carry TWO paths in one event, and every event
 * that carries a path is served from the medium pool, whose block width IS
 * ConsoleLineEvt at 252 bytes. Two paths plus the header leaves 120.
 *
 * 96 sits under both with slack - the same rounding-down KODE_SYS_NVS_VALUE_MAX
 * does - so that a longer verb never has to move this number. Note what 128
 * would have cost: `cp` with two 128-byte paths is a 272-byte line the editor
 * would truncate, and a 268-byte event that fits no pool tier at all. */
#define KODE_PATH_MAX  96U

/** @brief What one `write text=` may carry. `write path="<96>" text=""` spends 117 of
 * the line's 240 bytes, leaving 123, and this rounds that down. It is the tighter
 * of the two limits: StorageTextEvt carries a path AND a text, and at 120 it
 * comes to about 233 bytes of the medium block's 252. */
#define KODE_FS_TEXT_MAX  120U

/**
 * @name cat max= limits
 * @brief `cat`'s `max=` key: the default when it is left off, and the hard ceiling
 * a value above it is refused rather than clipped to. The ceiling is not
 * arbitrary - AO_Storage reads the file through one of its own 32 KB
 * buffers in the one dispatch step that opens it, and 32768 is exactly that
 * buffer's size. A ceiling any higher would ask this AO to read more than it
 * can hold in the one pass that read allows; any lower would just be a
 * smaller number with no buffer behind the choice. What AO_Storage does
 * with the bytes once they are in that buffer - turning them into `>|line=`
 * records - is a separate question with its own answer: see
 * STORAGE_CAT_LINES_PER_STEP in storage_ao.c for why that part is paced
 * across as many steps as the file's line count needs, rather than costing
 * one step the way the read does.
 * @{
 */
#define KODE_FS_CAT_MAX_DEFAULT   4096U
#define KODE_FS_CAT_MAX_CEIL     32768U
/** @} */

/** @brief What one `>|line=` record from `cat` may carry. `>|line=` spends 7 bytes of
 * KODE_LOG_LINE_MAX's 256, and this rounds the rest down with slack, the same
 * way KODE_FS_TEXT_MAX above does. A line the file itself made longer than
 * this is cut here, once, by AO_Storage, which is what lets it say so with a
 * `!` line - left to reply()'s own snprintf the cut would happen silently,
 * and a host cannot tell a silent cut apart from a short line that just
 * ended there. */
#define KODE_FS_CAT_LINE_MAX     200U

/* ---- Apps ---------------------------------------------------------------- */
#define KODE_APP_FILE_MAX       64U   /**< Max SD-relative .bin file name.   */
/* Twins of KAPP_NAME_MAX in kdrv's kapp_desc.h and KAPP_CATEGORY_MAX in
 * kdrv's kapp_cat.h, which this layer sits below and cannot include. Same
 * reason KODE_NET_* are twins of the kdrv_net.h numbers: an event has to
 * carry a manifest's strings, and an include reaching upward would be worse
 * than two numbers that have to agree. If you change one, change the other. */
#define KODE_APP_DISPLAY_MAX    32U   /**< == KAPP_NAME_MAX.     */
#define KODE_APP_CATEGORY_MAX   64U   /**< == KAPP_CATEGORY_MAX: the whole
                                       *   list. One item is 16 there
                                       *   (KAPP_CATEGORY_ITEM_MAX).        */
#define KODE_APP_PROJECT_MAX    32U   /**< == KDRV_SLOT_PROJECT_MAX: a
                                       *   project name, terminator
                                       *   included.                        */
/* An app's data folder (app.json and icon.k64) may reach the internal
 * partition before its app does, so nothing bounds how many there are but the
 * partition itself. A new one is refused when it would leave less than the
 * floor free: Kody's pack is staged beside the one in use when it updates. */
#define KODE_APPDATA_CLUSTER_BYTES  4096U          /**< The internal FAT's allocation unit. */
#define KODE_APPDATA_FOLDER_BYTES   ((2U + (KODE_APP_ICON_BYTES / KODE_APPDATA_CLUSTER_BYTES)) \
                                     * KODE_APPDATA_CLUSTER_BYTES)
                                      /**< What one data folder takes at most: the
                                       *   directory's cluster, app.json's, and
                                       *   the clusters of a 128x128 icon (a
                                       *   64x64 one takes a quarter of them). */
#define KODE_APPDATA_FREE_MIN_BYTES (128U * 1024U) /**< What a new data folder
                                       *   must leave free on the internal
                                       *   partition.                       */
#define KODE_APPDATA_URL_MAX    200U  /**< The base URL an app's data folder is
                                       *   fetched from (`appdata_get`),
                                       *   terminator included: what a console
                                       *   line has room for beside the verb
                                       *   and a project name.              */
#define KODE_APP_DATE_MAX       16U   /**< == KAPP_IDF_DATE_MAX: a build date,
                                       *   "Oct 24 2026".                   */
#define KODE_APP_TIME_MAX       16U   /**< == KAPP_IDF_TIME_MAX: a build
                                       *   time, "21:14:03".                */
/* The most apps one answer to "what would this install evict?" names. Every
 * name is one event posted to the asker, and one STAT of the card before it,
 * so the number bounds both the burst a queue must hold and how long the
 * answer takes. The end event still counts every victim, so an asker can tell
 * a list that was cut. */
#define KODE_APP_EVICT_MAX      8U    /**< Entries in one eviction answer.   */
/* Rows a reader keeps of each list in all, read page by page: the installed
 * apps and the scanned files. The apps region's own KAPPS_MAX_ENTRIES, so
 * the launcher shows every app the region can hold (appmgr_ao.c asserts it),
 * and as many files. Past it a reader stops asking for pages and says so. */
#define KODE_APP_LIST_MAX       64U
/* The page of a listing: how many entries one burst may carry. It exists to
 * protect the queues: a card with two hundred files sent at once would
 * overflow one - which QP answers with an assert and a reset, not a dropped
 * record.
 *
 * So a listing comes in pages. storage_list_page() and appmgr_list_page()
 * send at most this many entries, each with its `pos` in the listing, then
 * an end marker whose `more` says another page follows, all posted to the AO
 * that asked; it asks the next page with a skip once it has read the end of
 * the last, so it sets the pace and no other AO's queue sees the page. The
 * burst a queue holds is
 *
 *     KODE_STORAGE_LIST_MAX + 1   (the end marker)
 *
 * and every asker's queue must hold that: AO_Gui and AO_Console are both
 * 64. It must also stay below the medium pool count. How many entries a
 * reader keeps in all is its own ceiling (KODE_APP_LIST_MAX for the launcher).
 * The one-medium listings (`sd_ls`, `fs_ls`, `ls`) are one page and stop at
 * this number, and say so.
 *
 * The WiFi scan's own cap (KDRV_WIFI_SCAN_MAX) is this number for the same
 * reason and must move with it - though that one really is per burst, since
 * there is only ever one radio to scan with. */
#define KODE_STORAGE_LIST_MAX   24U

/** @brief Slots an ask's listing leaves free in the asker's queue, the end's
 *         among them: kode_sys.h's KODE_SYS_LIST_FREE_SLOTS, and its requests'
 *         queue rule says what else must fit in them. */
#define KODE_STORAGE_LIST_FREE_SLOTS  8U

/* ---- Display / GUI -------------------------------------------------------
 * How big the glass is is NOT here: it is a fact about the board, so it lives
 * in kbsp_board.h and everyone who needs it - the panel driver and the GUI
 * alike - reads it from there. Having it here meant a device driver had to
 * include the OS's scheduling policy to know how many pixels it was pushing.
 */
#define KODE_GUI_TICK_MS        10U   /**< The longest the renderer (kscreen)
                                       *   waits between render passes. It
                                       *   wakes when LVGL's next timer is
                                       *   due, to the tick; this bounds the
                                       *   wait when nothing is, and so how
                                       *   soon a touch is drawn.           */
/* The standard layer's screen (kode_screen): what one request to it
 * carries. kode.h's KODE_WIDGET_TEXT_MAX is this, checked where both are seen. */
#define KODE_SCREEN_TEXT_MAX    128U  /**< Bytes of a title or a widget's text
                                       *   in a request, NUL included.      */
#define KODE_SCREEN_REQ_NUMS    4U    /**< Numbers a request carries.       */
#define KODE_TOUCH_RESTORE_RETRY_MS 200U  /**< Between tries to write the controller back to its
                                       *   awake settings after a wake: a chip that went to
                                       *   sleep NAKs until a finger wakes it. */
/* A touch controller that did not answer AO_Touch's bring-up is looked for
 * again: soon, because the panel's bring-up has just pulsed the reset line the
 * two share and the controller may still be coming back; then spaced, as
 * AO_Display does for a panel; and whenever its interrupt line falls, because
 * a controller in its low-power scan answers nothing until a finger touches
 * the glass. Each look is one short probe. */
#define KODE_TOUCH_LOOK_SOON_MS  { 200U, 300U, 500U, 1000U } /**< The waits before the first looks: 0.2, 0.5, 1 and 2 s after the bring-up's try. */
#define KODE_TOUCH_LOOK_EVERY_MS 10000U  /**< Between the looks after those. */
#define KODE_TOUCH_LOOK_FOR_MS   300000U /**< How long the spaced looks go on: five minutes, AO_Display's own patience with a panel. */
#define KODE_TOUCH_LOOK_EDGE_MS  1000U   /**< The least time between two looks the interrupt line asks for: a contact pulses it many times a second. */
#define KODE_TOUCH_REST_SLEEP_S 1U    /**< The auto-sleep time written for the rest: the controller
                                       *   sleeps a second after the last touch. */
#define KODE_TOUCH_STILL_MS     60000U /**< A contact that stays within
                                       *   KODE_TOUCH_STILL_PX of where it
                                       *   settled this long is not a finger:
                                       *   a release goes out, and it no longer
                                       *   keeps the board awake. */
#define KODE_TOUCH_STILL_PX     12    /**< How far a contact may wander and
                                       *   still be the same still one: more
                                       *   than the jitter of a cloth or a
                                       *   drop, less than any drag. */
#define KODE_TOUCH_HELD_POLL_MS 1000U /**< How often a contact that stopped
                                       *   counting is looked at, for its lift
                                       *   or its moving off. */
#define KODE_TOUCH_POLL_MS      10U   /**< While a finger is down. The touch
                                       *   INT wakes AO_Touch through the
                                       *   expander's owner; from then until
                                       *   the release it polls, because
                                       *   whether the controller pulses INT
                                       *   per report or per touch differs
                                       *   between CST816S firmwares. 10, the
                                       *   period it has always run at: this
                                       *   said 15, and the 10 ms tick
                                       *   truncated it.                    */
#define KODE_SYS_BUTTON_SETTLE_MS  30U   /**< A change arms this one-shot; the
                                       *   confirming sample is asked for
                                       *   when it fires. Two equal samples
                                       *   this far apart is the debounce. */
/**
 * @brief How long the top or the bottom button must stay pressed to be a
 * hold rather than a press.
 *
 * One number for the whole board: AO_Buttons arms its hold timer with it
 * and publishes BUTTON_HOLD_SIG when it fires; kgui judges a short press on
 * the release only when the two edges are closer than this. 600 ms is well
 * above what a finger does by accident on a dome switch and well below the
 * ~1 s at which BOT+PAD_LEFT resets the P4 in hardware.
 */
#define KODE_BUTTON_HOLD_MS     600U
/**
 * @brief How soon after the first press's release the top button's second
 * press must be confirmed to make a double.
 *
 * AO_Buttons measures it from the release to the second press's confirming
 * sample, both stamped with khal_time_ms(), and publishes BUTTON_DOUBLE_SIG
 * when the gap is no longer than this. 800 ms is long enough for a thumb
 * that hesitates between the two taps and short enough that two separate
 * presses, each meaning something on its own, are not read as one gesture.
 */
#define KODE_BUTTON_DOUBLE_MS   800U
/**
 * @brief How long AO_Boot waits after announcing home before it resets.
 *
 * A held bottom button always comes home (CONFIG_KODEOS_HOME_HOLD), and the
 * hold goes out as HOME_SIG before the reset so an app can hear it. This is
 * what that hearing is worth: 200 ms (200 QP ticks), long enough for an AO one
 * step below AO_Boot to run its handler and write a file, short enough that
 * the gesture still feels like the button did it. An app that needs more says
 * so with kode_sys_home_defer().
 */
#define KODE_SYS_HOME_GRACE_MS      200U
/**
 * @brief The longest a deferral can hold the board, from the hold itself.
 *
 * The grace is for saving state, not for thinking. An app that asks for a
 * moment and never calls kode_sys_home_now() is reset anyway when this expires,
 * which is what keeps the gesture a promise rather than a request: three
 * seconds is long enough for any write a sane app makes and short enough
 * that a wedged one does not feel like a board that stopped answering.
 */
#define KODE_SYS_HOME_DEFER_MAX_MS 3000U
/**
 * @brief The console's drain before a reset, in milliseconds.
 *
 * Every deliberate restart waits this long first, so the reply to the command
 * that caused it leaves the console before the board goes, and the panel's
 * owner has the two frames it needs to stop the video in a blanking
 * (BOOT_KEEP_SIG). Every millisecond here is one the way back from an app takes.
 */
#define KODE_BOOT_CONFIRM_MS    15000U  /**< With the rows walked, how long a kodeOS update waits for its first frame before it confirms itself without one: a board with no glass has to confirm too. */
#define KODE_BOOT_STABLE_MS     60000U  /**< With the rows walked, how long kodeOS has to stay up for its crashes in a row to start again from none. */
_Static_assert(KODE_BOOT_STABLE_MS > KODE_BOOT_CONFIRM_MS, "the crashes outlast the confirmation");
#define KODE_BOOT_CRASHES_MAX   3U      /**< Crashes in a row, none of them after KODE_BOOT_STABLE_MS up, that make kodeOS give its slot up. */
#define KODE_RESTART_DRAIN_MS   45U
#define KODE_LEAVE_NET_MS       200U  /**< The channel's clean stop before a
                                       *   jump.                            */
#define KODE_LEAVE_RADIO_MS     250U  /**< BLE hung up and the C5 parked
                                       *   before a jump.                   */
/** @brief Half a second: the most the two steps of a leave may cost together. */
#define KODE_LEAVE_BUDGET_MS    500U
_Static_assert((KODE_LEAVE_NET_MS + KODE_LEAVE_RADIO_MS) < KODE_LEAVE_BUDGET_MS,
               "the two steps of a leave must stay under half a second");
#define KODE_IOEXP_RETRY_MS     500U  /**< The expander's INT fired and the
                                       *   input register did not read: look
                                       *   again this much later rather than
                                       *   re-arm a line still low. A failed
                                       *   read holds the bus for its 100 ms
                                       *   timeout, so this keeps a dead chip
                                       *   under a fifth of the bus.        */
#define KODE_BUTTONS_TEST_STEP_MS 20000U /**< The guided button test: how
                                       *   long the operator has to press
                                       *   the button a step asks for.     */
#define KODE_BUTTONS_TEST_RELEASE_MS 3000U /**< A press must be let go within
                                       *   this long of being judged, or the
                                       *   button does not spring back and
                                       *   the step fails as stuck.        */
#define KODE_BUTTONS_TEST_STEPS 6U     /**< The guided button test: how many
                                       *   buttons its script asks for.    */
#define KODE_BUTTONS_TEST_TRIES 3U     /**< The guided button test: the wrong
                                       *   press that fails a step, ending
                                       *   the run (the two before it are
                                       *   forgiven).                      */
#define KODE_BUTTONS_TEST_DOUBLE_TRIES 2U /**< The guided button test: the
                                       *   press taking two buttons down
                                       *   that fails a step (the one before
                                       *   it is forgiven).                */
#define KODE_TOUCH_TEST_STEP_MS 20000U /**< The guided touch test: how long
                                       *   the operator has to tap the
                                       *   target a step shows.           */
#define KODE_TOUCH_TEST_STEPS 2U       /**< The guided touch test: how many
                                       *   targets its script shows.      */
#define KODE_TOUCH_TEST_TRIES 3U       /**< The guided touch test: the tap off
                                       *   a target that fails its step (the
                                       *   two before it are forgiven).    */
/* What each operator row may take from start to verdict, the console's
 * budget for it: the script's worst case plus five seconds. A button step
 * is at its worst with every forgiven slip taken (two wrong presses and one
 * double) and then the right press, each attempt a whole step to press and
 * up to the release limit to let go; a touch target is at its worst with
 * two taps off the target and then the right one, each a whole step. A row
 * still silent past its budget is reported as a timeout and the run moves
 * on. */
#define KODE_BUTTONS_TEST_ATTEMPTS  ((KODE_BUTTONS_TEST_TRIES - 1U) \
                                     + (KODE_BUTTONS_TEST_DOUBLE_TRIES - 1U) + 1U)
#define KODE_BUTTONS_TEST_BUDGET_MS ((KODE_BUTTONS_TEST_STEPS * KODE_BUTTONS_TEST_ATTEMPTS \
                                      * (KODE_BUTTONS_TEST_STEP_MS + KODE_BUTTONS_TEST_RELEASE_MS)) \
                                     + 5000U)
#define KODE_TOUCH_TEST_BUDGET_MS   ((KODE_TOUCH_TEST_STEPS * KODE_TOUCH_TEST_TRIES \
                                      * KODE_TOUCH_TEST_STEP_MS) + 5000U)

/* ---- The card test -------------------------------------------------------
 * The other long row, and the only automatic one: nobody has to be at the
 * glass for it, but it formats the card and moves two megabytes through it,
 * so it cannot live inside a chip's five seconds either.
 */
#define KODE_SD_TEST_KB         1024U  /**< Both the tail probed at the far
                                        *   end of the card and the file
                                        *   written to it afterwards. One
                                        *   megabyte is long enough to time
                                        *   honestly and short enough that a
                                        *   slow card does not hold the line
                                        *   up.                              */
#define KODE_SD_TEST_FLOOR_KB_S 256U   /**< The slowest card the budget below
                                        *   allows for. It sizes the budget
                                        *   and nothing else: the rate is
                                        *   reported, never judged, because a
                                        *   card that is slow and correct is
                                        *   a card that works.               */
#define KODE_SD_TEST_MOUNT_MS   3000U  /**< Unmount, mount and identify.     */
#define KODE_SD_TEST_FORMAT_MS  30000U /**< Making a fresh filesystem. One
                                        *   blocking call with no chunked
                                        *   form, and the longest single step
                                        *   in kodeOS.                       */
/* The script's worst case plus five seconds: the mount, the format, and four
 * megabytes at the floor rate - the probe written and read back, then the
 * file written and read back. */
#define KODE_SD_TEST_BUDGET_MS  (KODE_SD_TEST_MOUNT_MS \
                                 + ((4U * KODE_SD_TEST_KB * 1000U) \
                                    / KODE_SD_TEST_FLOOR_KB_S) + 5000U)
/* `sd_verify` is the bench's, not the line's: a megabyte written to the far
 * end of the card and read back, the alias scan, then a format. */
#define KODE_SD_VERIFY_BUDGET_MS (KODE_SD_TEST_MOUNT_MS + KODE_SD_TEST_FORMAT_MS \
                                  + ((2U * KODE_SD_TEST_KB * 1000U) \
                                     / KODE_SD_TEST_FLOOR_KB_S) + 5000U)
/* ---- The C5 row ----------------------------------------------------------
 * The other automatic row that cannot live inside a chip's five seconds:
 * three waits, each bounded where it happens and added up here. The link
 * (kdrv_radio waits RADIO_CP_INIT_MS for the slave to reboot and answer;
 * kqp cannot include kdrv, so the figure is repeated under its own name and
 * kdrv_radio.c names this one back), the snapshot (a handful of RPCs), the
 * WiFi scan (kdrv_wifi sweeps the air in seven steps under a whole-sweep
 * budget of KDRV_WIFI_SCAN_TIMEOUT_MS; 38 s measured on this board), and the
 * BLE step (the controller's handshake, then KODE_BLE_SCAN_MS of listening).
 * Plus the five seconds every row gets. Long, and honestly so: a row that
 * could be timed out underneath a scan that is still running would report
 * nothing about the radio at all.
 */
/** @brief From C5_EN going high to the C5's own boot being over, its
 *         amplifier enable driven: what AO_Radio waits after letting a held
 *         C5 go before it says the C5 is running. There is no signal from a
 *         C5 with no link, so this is a time. */
#define KODE_C5_BOOT_MS         600U
#define KODE_C5_TEST_LINK_MS    10000U /**< Connect to CP_INIT: RADIO_CP_INIT_MS. */
#define KODE_C5_TEST_INFO_MS    3000U  /**< The snapshot's RPCs.                */
#define KODE_C5_TEST_SCAN_MS    90000U /**< KDRV_WIFI_SCAN_TIMEOUT_MS. The row
                                        *   must not be timed out from above
                                        *   while the sweep below it is still
                                        *   inside its own budget.          */
#define KODE_C5_TEST_BLE_MS     (5000U + KODE_BLE_SCAN_MS) /**< The controller's
                                        *   start, then the listen window.   */
#define KODE_C5_TEST_RSSI_MIN_DBM (-70) /**< The strongest network the scan
                                         *   sees must reach this. A loose or
                                         *   missing antenna costs 20-30 dB,
                                         *   and the line's own access point
                                         *   a few metres away reads well
                                         *   above -60 through a good one; a
                                         *   floor here is what tells the two
                                         *   apart. It is a presence check,
                                         *   not a sensitivity figure.      */
#define KODE_C5_TEST_BUDGET_MS  (KODE_C5_TEST_LINK_MS + KODE_C5_TEST_INFO_MS \
                                 + KODE_C5_TEST_SCAN_MS + KODE_C5_TEST_BLE_MS \
                                 + 5000U)

/* ---- The IR row -----------------------------------------------------------
 * ASYNC, unlike the chip rows above it, because it waits out a real
 * transmission and its own echo rather than answering from a register. Still
 * short once the link is up: the console places it right after `clocks` and
 * before `sd` (short automatic rows, before the long ones), even though this
 * file's own order does not track the console's. With the link down AO_Ir
 * first has it brought up, and the budget holds that wait too.
 */
/** @brief One frame and the wait for its echo. */
#define KODE_IR_TEST_ECHO_MS      500U
/** @brief Attempts before the row gives up; a fresh nonce each time. */
#define KODE_IR_TEST_TRIES          3U
/** @brief The presence probe's round trip to the C5, bounded at 500 ms, and a
 *         link that may be busy. */
#define KODE_IR_TEST_PROBE_MS    2000U
/** @brief How long AO_Ir holds a request with no budget of its own - a NEC
 *         frame, the row - for the C5 link: a bring-up's wait for the C5. */
#define KODE_IR_LINK_WAIT_MS      KODE_C5_TEST_LINK_MS
/** @brief The row with the link down: its wait for the link, then the row. */
#define KODE_IR_TEST_BUDGET_MS    (KODE_IR_LINK_WAIT_MS + KODE_IR_TEST_PROBE_MS \
                                   + (KODE_IR_TEST_TRIES * KODE_IR_TEST_ECHO_MS))
_Static_assert(KODE_IR_TEST_BUDGET_MS > (KODE_IR_LINK_WAIT_MS + (KODE_IR_TEST_TRIES * KODE_IR_TEST_ECHO_MS)),
               "the IR row's budget covers a bring-up of the link and every attempt");

/* ---- AO_Ir's calls to the C5 ----------------------------------------------
 * A frame, a line reading and an arm are requests over the link, run by
 * AO_Ir's worker so the AO's step never waits on them.
 */
/** @brief Room added to the longest a call to the C5 can legitimately take
 *         (KDRV_IR_CALL_MAX_MS) to make AO_Ir's deadline for it, so a call
 *         that uses all of its time still returns before the AO gives up on
 *         it. Giving up earlier would change answers - the line reported low,
 *         a frame logged as not sent whose own echo is then published as
 *         foreign input, a false verdict of the row. The AO still takes no
 *         other call until the worker returns. */
#define KODE_IR_JOB_MARGIN_MS      500U
/** @brief Requests AO_Ir holds back while a call to the C5 is out. A request
 *         over this many is refused as busy, the way it would be with no
 *         room to run. */
#define KODE_IR_DEFER_DEPTH          8U

/* ---- AO_Storage's calls to the card ---------------------------------------
 * Mounting, unmounting, raw sector transfers and a format wait on the card,
 * and run on AO_Storage's worker (storage_sd) so the AO's step never
 * does. The most each may take is the driver's (KDRV_SD_*_MAX_MS in
 * kdrv_sd.h); the margin below is the AO's.
 */
/** @brief How long past the driver's own bound AO_Storage waits for a call to
 *         the card before it tells whoever waits for it that it failed. */
#define KODE_STORAGE_JOB_MARGIN_MS   500U
/** @brief Requests AO_Storage holds back while a call to the card is out. A
 *         request over this many is refused the way it is when the AO has no
 *         room to run it. */
#define KODE_STORAGE_DEFER_DEPTH      8U
/** @brief The longest a request that writes or deletes waits for the transfer
 *         AO_Storage has in flight to end. It is served when the transfer
 *         does; past this it is refused as busy. */
#define KODE_STORAGE_TURN_MS         2000U

/* ---- The NFC row ----------------------------------------------------------
 * ASYNC, and it does what no other row does: it WRITES. The board's own
 * serial goes onto the fixture's tag as an NDEF Text record and is read back
 * and compared, so the row proves both directions of the radio and the tag
 * layer above them.
 *
 * The write is also what makes a pass impossible to fake. A row that only
 * looked for a tag would pass on anybody's card left on the bench; this one
 * passes only when the string that comes back is the one THIS board just put
 * there. It is the infrared row's nonce, made of the board's identity
 * instead of a random byte - and unlike a random byte it also tells an
 * operator, from the tag itself, which unit last touched it.
 *
 * One window covers a whole attempt: the tag is discovered once and then
 * written and read while it stays activated, because taking the field down
 * between the two halves would mean discovering it twice for no gain.
 */
/** @brief One discovery window, and the exchange that follows it. */
#define KODE_NFC_TEST_FIELD_MS   1500U
/** @brief Attempts before the row gives up. */
#define KODE_NFC_TEST_TRIES         3U
/** @brief The row's watchdog: every attempt's window plus two seconds. */
#define KODE_NFC_TEST_BUDGET_MS  ((KODE_NFC_TEST_TRIES                       \
                                   * KODE_NFC_TEST_FIELD_MS) + 2000U)
/** @brief The watchdog for `nfc_keys`: a full dictionary sweep is minutes. */
#define KODE_NFC_KEYS_BUDGET_MS  240000U

/* ---- The RFID row ----------------------------------------------------------
 * `test component=rfid`. One verdict, proven end to end: a stable EM4100 ID
 * decoded KODE_RFID_TEST_TRIES windows in a row is the pass, and that single
 * verdict exercises the whole discrete chain at once - the LEDC carrier, the
 * tank, the tag, the op-amp demod, the ADC and the decoder. There is no reader
 * IC to read an identity from first, so unlike `nfc` this row has one step.
 */
/** @brief One read window. */
#define KODE_RFID_TEST_WINDOW_MS  150U
/** @brief Consecutive identical decodes required to pass. */
#define KODE_RFID_TEST_TRIES        2U
/** @brief Windows attempted before the row gives up. */
#define KODE_RFID_TEST_WINDOWS     16U
/** @brief The row's watchdog: every window plus a second of slack. */
#define KODE_RFID_TEST_BUDGET_MS  ((KODE_RFID_TEST_WINDOWS       \
                                    * KODE_RFID_TEST_WINDOW_MS)  \
                                   + 1000U)

/* ---- The audio row --------------------------------------------------------
 * ASYNC, and the second row in the house that drives an actuator and then
 * confirms it worked. Infrared can do that because the emitter and the
 * receiver share an enclosure; audio can do it because the speaker and the
 * microphone share a very small board and couple hard enough that the
 * validation demos had to add monitor-disable switches to stop the squealing.
 * The defect is the instrument.
 *
 * Two steps and two separable verdicts. Step 1 judges the microphone alone
 * and emits nothing: a floor that is exactly flat is a dead ADC or a dead
 * bus, a railed input is a dead input, and a floor above the ceiling means
 * the bench is too loud for step 2's margins to mean anything - and there the
 * row refuses to judge rather than judging badly. Step 2 emits TWO tones
 * whose frequencies are drawn from the hardware RNG out of eight exact
 * detector bins, and requires each to come back in its own window, above the
 * other seven and above step 1's floor AT THAT SAME BIN. A bench humming at
 * one of our frequencies therefore raises its own bar rather than lowering
 * ours.
 *
 * Two draws rather than one because one draw in eight is a coincidence a busy
 * production line would produce - several boards under test in one room, all
 * making noise. Two, forced to differ, is one in fifty-six, and a steady
 * interferer sitting on one bin fails the second window by construction.
 *
 * The frequencies, the window and the thresholds are ktone.h's rather than
 * this file's: they are arithmetic a driver owns and the host suite proves,
 * and kqp cannot include kdrv. What is here is what AO_Audio's timers need,
 * and the two files name each other where the figures have to agree.
 */
/** @brief The tone's settle before any of it is captured: the TX and RX DMA
 *         prefill, the amplifier's start-up and the DAC's volume ramp.
 *         ES_SETTLE_BLOCKS in kdrv_es8311.c is this in blocks.            */
#define KODE_AUDIO_TEST_SETTLE_MS    32U
/** @brief One analysis window: KTONE_WIN_N frames at 48 kHz, rounded up.  */
#define KODE_AUDIO_TEST_WINDOW_MS    86U
/** @brief The tone outlives the window, so a block of slip cannot truncate
 *         what the detector was measuring. ES_TAIL_BLOCKS, in milliseconds. */
#define KODE_AUDIO_TEST_TAIL_MS      22U
/** @brief One tone of step 2, start to finish. */
#define KODE_AUDIO_TEST_PROBE_MS    (KODE_AUDIO_TEST_SETTLE_MS   \
                                     + KODE_AUDIO_TEST_WINDOW_MS \
                                     + KODE_AUDIO_TEST_TAIL_MS)
/** @brief Tones per attempt. Two, so a pass is one chance in fifty-six. */
#define KODE_AUDIO_TEST_TONES         2U
/** @brief Step 1: the settle blocks, then a window, emitting silence. */
#define KODE_AUDIO_TEST_FLOOR_MS    (KODE_AUDIO_TEST_SETTLE_MS \
                                     + KODE_AUDIO_TEST_WINDOW_MS)
/** @brief Opening the codec: about forty register writes and an I2S
 *         re-initialisation. */
#define KODE_AUDIO_TEST_OPEN_MS     200U
/** @brief The wait for the microphone's start-up step before the first
 *         window after an open. ES_FRESH_RX_BLOCKS in kdrv_es8311.c is
 *         this in blocks. */
#define KODE_AUDIO_TEST_FRESH_MS    192U
/** @brief Attempts before the row gives up; a fresh pair each time. */
#define KODE_AUDIO_TEST_TRIES         3U
/** @brief The script's worst case plus five seconds: the open and its
 *         start-up step, step 1's window, and three attempts of two tones
 *         each. */
#define KODE_AUDIO_TEST_BUDGET_MS   (KODE_AUDIO_TEST_OPEN_MS                 \
                                     + KODE_AUDIO_TEST_FRESH_MS              \
                                     + KODE_AUDIO_TEST_FLOOR_MS              \
                                     + (KODE_AUDIO_TEST_TRIES                \
                                        * KODE_AUDIO_TEST_TONES              \
                                        * KODE_AUDIO_TEST_PROBE_MS) + 5000U)

/* ---- The `power_plug` row ---------------------------------------------------
 * Guided: somebody plugs the charger in and takes it out again. The row
 * looks at the charger's own input-good bit every KODE_POWER_PLUG_LOOK_MS,
 * gives each of the two asks KODE_POWER_PLUG_STEP_MS, and lets the PMIC have
 * KODE_POWER_PLUG_SETTLE_MS between seeing the input and being judged: its
 * source detection and the first conversions take that long.
 */
#define KODE_POWER_PLUG_LOOK_MS      250U
#define KODE_POWER_PLUG_STEP_MS    10000U
#define KODE_POWER_PLUG_SETTLE_MS   2000U
#define KODE_POWER_PLUG_BUDGET_MS  ((2U * KODE_POWER_PLUG_STEP_MS) \
                                    + KODE_POWER_PLUG_SETTLE_MS + 5000U)

/* ---- The self-test rows --------------------------------------------------- */
/** @brief The longest name a self-test row may carry, terminator included. */
#define KODE_TEST_NAME_LEN        12U
/** @brief What an ASYNC row with no budget of its own gets, in ms. */
#define KODE_TEST_ROW_BUDGET_MS 5000U

/* ---- The internal pool's row (`test component=mem`) ------------------------ */

/**
 * @brief The least the internal pool may have had free since boot, in bytes,
 *        for the `mem` row to pass.
 *
 * Internal RAM is what DMA descriptors, task stacks and the kernel's own
 * objects come from, and nothing else can serve them. The board's config
 * holds 32 KB of it back for them (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL);
 * the floor is twice that, so the row fails while there is still room to
 * find out why, not at the first refused allocation. kodeOS keeps over
 * 110 KB through the whole of `test component=all`.
 */
#define KODE_MEM_TEST_FLOOR_BYTES   (64U * 1024U)

/* ---- What AO_Radio's guards are for ---------------------------------------
 */
/** @brief How long one of the radio worker's jobs may be with it before its
 *         guard fires: past the longest honest one - a dual-band scan with
 *         its RPC timeout, a 32 KB firmware chunk against a slow C5, a join
 *         and its lease. The point past which the worker is wedged rather
 *         than working. */
#define KODE_RADIO_GUARD_MS            180000U
/** @brief The radio check's budget, ms: past it the readings so far are
 *         answered with KODE_TIMEOUT. It is the bound of the whole check
 *         (the station's lock_ms). Counted between its jobs, from the first one
 *         and again after a sweep it asked for, which the job guard bounds;
 *         putting the advert back is not cut by it. */
#define KODE_RADIO_PROBE_MS             8000U
/** @brief The radio check's dwell on a 2.4 GHz channel, ms. */
#define KODE_RADIO_PROBE_DWELL_MS       120U
/** @brief Its dwell on a 5 GHz channel, ms: long enough for a passive listen
 *         on a radar channel to hear more than one beacon. */
#define KODE_RADIO_PROBE_PDWELL_MS      250U
/** @brief Passes the radio check makes over a network it has not heard. */
#define KODE_RADIO_PROBE_TRIES          2U
/** @brief How long the radio check listens for one BLE name, ms. */
#define KODE_RADIO_FIND_MS              1000U
/** @brief What a BLE listen adds to the guard: its own length is the job. */
#define KODE_RADIO_BLE_SCAN_SLACK_MS    30000U
/** @brief How long kode_block_ble_value() waits, in all, for its claim, for
 *         AO_Radio's queue to take a value and for its answer: AO_Radio
 *         answers in the step that receives it, so this covers a full queue
 *         and the calls of other tasks ahead of this one. */
#define KODE_SYS_BLE_VALUE_WAIT_MS      1000U
/** @brief How long kode_block_ble_value() sleeps between tries at a held claim or a full queue. */
#define KODE_SYS_BLE_VALUE_RETRY_MS     10U
/** @brief How long a KODE request may be with the service worker before its
 *         guard fires: one RPC, which can wait behind a scan on the C5's
 *         sequential channel. Its answer is waited for separately, from the
 *         moment the send has returned. */
#define KODE_RADIO_SVC_SEND_GUARD_MS    60000U

/* ---- The PSRAM row (`test component=psram`) ------------------------------- */

/**
 * @brief PSRAM the row leaves free for the running system, in bytes.
 *
 * The GUI, Kody and the radio go on allocating while the row runs; this is
 * what they keep. The row tests the rest.
 */
#define KODE_PSRAM_TEST_RESERVE_BYTES  (2U * 1024U * 1024U)

/** @brief One block the row takes, in bytes; the tested range is a whole
 *         number of them. */
#define KODE_PSRAM_TEST_BLOCK_BYTES    (1024U * 1024U)

/** @brief The most blocks one run holds: the whole 32 MB in blocks. */
#define KODE_PSRAM_TEST_BLOCKS_MAX     32U

/** @brief Bytes of one pass done in one step, between which lower
 *         priorities run. */
#define KODE_PSRAM_TEST_STEP_BYTES     (256U * 1024U)

/** @brief The PSRAM row's budget, in ms: several times the 2-3 s a run of
 *         29 MB is expected to take. */
#define KODE_PSRAM_TEST_BUDGET_MS      15000U

/**
 * @brief The most mismatched words the PSRAM row counts.
 *
 * A dead region would count millions; past this the count says nothing
 * more, and the first mismatch is what a diagnosis starts from.
 */
#define KODE_PSRAM_TEST_ERRORS_MAX  1000U

/* ---- What AO_Audio's own timers are for -----------------------------------
 */
/** @brief How long any one job may take before its guard fires. The longest
 *         is a whole recording, so this is not a job's typical length: it is
 *         the point past which a worker is wedged rather than working, and
 *         the guard turns that into a named failure instead of a row that
 *         goes silent until the console's budget expires. */
#define KODE_AUDIO_GUARD_MS       45000U
/** @brief Volume at boot. 100 is exactly zero decibels on this board; the
 *         user's volume is attenuation from here and never gain. */
#define KODE_AUDIO_VOL_DEFAULT      100U
/** @brief Microphone gain at boot, in decibels. A GUESS: the bench sets it
 *         from the floor it produces. */
#define KODE_AUDIO_MIC_DB_DEFAULT    24U

/* ---- The IMU under test -----------------------------------------------------
 * Every wait is the part working on its own clock - a self-test running, a
 * FIFO filling - and the AO polls or drains afterwards, so nothing here is a
 * busy step. The parts themselves rest until something asks: a request
 * wakes its part, and a part nobody has asked for KODE_SYS_IMU_REST_AFTER_MS goes
 * back to standby.
 */
/** @brief |a| must be within this of 1 g. */
#define KODE_IMU_GRAVITY_TOL_MG    150U
/** @brief |w| at rest must be below this. */
#define KODE_IMU_REST_MAX_MDPS     20000U
/** @brief The die is never colder than this in a room, in tenths of a degree. */
#define KODE_IMU_TEMP_MIN_DC         0
/** @brief Nor hotter, in tenths of a degree. */
#define KODE_IMU_TEMP_MAX_DC       600
/** @brief The two self-tests at their ceilings (300 ms and 1 s of polling),
 *         the gyroscope's turn-on after them (200 ms), a second of margin. */
#define KODE_IMU_TEST_BUDGET_MS    2500U
/** @brief An external witness capture nobody drained is disarmed after this.
 *         A paced drain of a full window is about 80 ms, so this still
 *         covers one several times over. */
#define KODE_IMU_CAPTURE_MAX_MS    1000U
/** @brief How long the blocking IMU read waits for its sample. A read that finds
 *         the part awake is one I2C burst inside the step that receives it;
 *         one that finds it resting waits out the gyroscope's turn-on
 *         first, about 200 ms. This covers both with room for a queue in
 *         front. */
#define KODE_SYS_IMU_READ_WAIT_MS      600U
/** @brief A part nobody has asked for this long goes back to standby. Long
 *         enough that a caller polling a few times a second never pays the
 *         wake twice, short enough that one read does not cost a session. */
#define KODE_SYS_IMU_REST_AFTER_MS    2000U

/* ---- The magnetometer --------------------------------------------------------
 * A part of its own on the main bus, suspended until something asks and
 * converting at 50 Hz while it is awake.
 */
/** @brief How long the blocking magnetometer read waits for one reading: one conversion
 *         period if none is ready when the request lands, plus the room a
 *         motion read gets. */
#define KODE_SYS_MAG_READ_WAIT_MS      600U
/** @brief How long kode_sys_mag_cal_*() waits: AO_Imu answers in the step that
 *         receives the request, and a set or a clear commits to NVS first. */
#define KODE_SYS_MAG_CAL_WAIT_MS      1000U
/** @brief The magnetometer row's own test budget: the identity, the
 *         self-test's 10 ms and one conversion, with room to spare. */
#define KODE_MAG_TEST_BUDGET_MS   5000U

/* ---- The haptic row -------------------------------------------------------
 * The motor is driven and the IMU is the witness; see
 * https://docs.kode.diy/sdk/kaos/haptic for where the two judged constants
 * come from.
 */
/** @brief One buzz of the row, at each gain. */
#define KODE_HAPTIC_TEST_MS        200U
/** @brief The floor window: the IMU FIFO fills in 128 ms; a little more. */
#define KODE_HAPTIC_STILL_MS       140U
/** @brief How long the row waits for AO_Imu to answer an arm or a drain. The
 *         drain is the long one: a full window comes out one chunk per QP
 *         tick, which is about 80 ms, with room for the bus. */
#define KODE_HAPTIC_REPLY_MS       500U
/** @brief Fewer witness samples than this is not a window. The QMI8658A's
 *         FIFO holds 128 entries; the floor leaves eight. */
#define KODE_HAPTIC_MIN_SAMPLES    120U
/** @brief Segments in one vibration pattern: on, off, on... */
#define KODE_HAPTIC_RETRY_MS       2000U /**< After a boot that found no AW86233, when it is looked for again: a NAK on a busy bus is not a board with no Antenna PCB. */
#define KODE_HAPTIC_SEGMENTS_MAX    16U
/* The RTP feed runs every 10 ms and offers up to two bursts each time - 128
 * bytes, 10.7 ms of playback - so the chip is never given less than it is
 * about to consume. The first feed after GO lands in time because AO_Haptic
 * pushes those two bursts in the same step that starts RTP playback, so the
 * chip holds ~10 ms before it. A 4 ms feed of one burst is what the
 * arithmetic asks for and the 1 ms tick could now keep; it stays at 10 ms
 * until it is measured on the chip. */
/** @brief The RTP feed period, in milliseconds. */
#define KODE_HAPTIC_TICK_MS          10U
/** @brief Bursts offered per feed. Two, because a feed is 10 ms. */
#define KODE_HAPTIC_BURSTS_PER_TICK  2U
/** @brief On/off ratio at 235 Hz a moving motor must clear, x10. MEASURED
 *         2026-09-11 on board e8:f6:0a:e7:6c:1d, one third of what that board
 *         delivered at gain 0x40: sqrt(246.1)/3. The row itself then read a
 *         median 21.3x at that gain on the same board, so 5.2x is still one
 *         third of the weakest buzz it sees. It is not lowered further: two
 *         still windows of the same quiet bench already differ by up to 6x,
 *         and a threshold inside that spread would pass a dead motor on
 *         noise. Spec section 0.                                          */
#define KODE_HAPTIC_TEST_RATIO_X10  52U
/** @brief Energy at 235 Hz with the motor off above which the bench, not the
 *         board, is judged, in 0.1 mg. From the same measurement: three times
 *         the 0.39 mg that run's floor carried, and the row's own floor on a
 *         quiet bench is 0.1 to 0.6 mg with a median of 0.3 mg. A hand
 *         resting on the bench reads 123.7 mg and is refused, which is what
 *         this cap is for. Spec section 0.                                */
#define KODE_HAPTIC_FLOOR_MAX_X10   12U
/** @brief Gains the row buzzes at. */
#define KODE_HAPTIC_TEST_GAINS_N     3U
/** @brief A still window, a buzz and two replies from AO_Imu, four times
 *         over - one more pass than the row takes - plus a second of margin. */
#define KODE_HAPTIC_TEST_BUDGET_MS ((4U * (KODE_HAPTIC_STILL_MS              \
                                           + KODE_HAPTIC_TEST_MS            \
                                           + (2U * KODE_HAPTIC_REPLY_MS)))  \
                                    + 1000U)

/* ---- The two rows an operator judges: `led` and `display` -----------------
 * Nothing on the board can see the LED or the glass, so somebody at the bench
 * is asked, a colour at a time, and answers on the pad: BUTTON_TOP for yes,
 * BUTTON_BOT for no.
 */
/** @brief What the panel is driven to for its own row, in %. */
#define KODE_DISPLAY_TEST_BRIGHT     100U
/** @brief Rows of the band buffer one step of the display row paints. */
#define KODE_DISPLAY_TEST_BAND_ROWS   16U
/** @brief How long the operator has to answer one field of the display row.
 *         The guided button test's figure, for its reason: long enough to
 *         look, short enough that an empty bench ends the row. */
#define KODE_DISPLAY_TEST_STEP_MS  10000U
/** @brief How long the display row's opening screen stays up before the first
 *         field, unless BUTTON_TOP starts the fields sooner: time to read
 *         what the pad will mean. */
#define KODE_DISPLAY_TEST_INTRO_MS  8000U
/** @brief How long the renderer is given, once the opening screen is done
 *         with, to finish a frame it may have started on it before the first
 *         field goes up. Two of its longest frames. */
#define KODE_DISPLAY_TEST_SETTLE_MS  200U
/** @brief The level the `led` row drives each sink at, of 255: bright enough
 *         to name the colour under a bench lamp, and well short of a sink
 *         somebody has to stare into. */
#define KODE_LED_TEST_LEVEL           96U
/** @brief How long the operator has to answer one colour of the LED row. */
#define KODE_LED_TEST_STEP_MS      10000U
/** @brief The panel's row: the opening screen, five fields each waited out
 *         in full, the paints between them, margin. */
#define KODE_DISPLAY_TEST_BUDGET_MS  (KODE_DISPLAY_TEST_INTRO_MS \
                                      + KODE_DISPLAY_TEST_SETTLE_MS \
                                      + (5U * KODE_DISPLAY_TEST_STEP_MS) + 3000U)
/** @brief The LED's row: three colours each waited out in full, margin. */
#define KODE_LED_TEST_BUDGET_MS      ((3U * KODE_LED_TEST_STEP_MS) + 2000U)

/**
 * @brief How much longer than its own window a facade call waits.
 *
 * AO_Nfc arms the window in whole QP ticks and answers inside the step that
 * follows, so its real deadline lands a little after the millisecond the
 * caller asked for. Waiting exactly @c ms would make the ordinary case a
 * timeout.
 */
#define KODE_NFC_WAIT_MARGIN_MS   500U

/* ---- Echo suppression -----------------------------------------------------
 * The receiver hears the emitter, so every frame this board sends comes back
 * a few milliseconds later. AO_Ir stays deaf for the frame's own duration -
 * which it measures from the symbols it is about to send, 52 ms for a typical
 * NEC frame and 86 ms for one of all ones - plus this tail.
 *
 * The tail is not slack. A capture is not delivered when the last mark ends:
 * the receive channel calls a frame over only after the line has been idle
 * for its inter-frame gap, which is 12 ms on this channel (IR_RX_MAX_NS in
 * kdrv_ir.c), and the interrupt's event then has to reach the AO. 30 ms is
 * that 12, plus the 10 ms the QP tick itself is worth, plus room for a step
 * that was preempted. Longer would start eating real frames from a real
 * remote; shorter publishes the board's own keypresses back to the app that
 * pressed them.
 */
/* The block is on the C5: the capture comes home behind the frame's own
 * closing space (32.8 ms, which the C5 waits out before it reports anything)
 * and two trips over the link. */
#define KODE_IR_ECHO_TAIL_MS      120U

/**
 * @brief The longest interval AO_Ir will arm one of its timers for, in ms.
 *
 * Ten minutes. Not a policy about how long a caller may wait for a frame - it
 * is where a millisecond count stops fitting the multiplication AO_Ir turns
 * it into ticks with, and a wrapped one would arm a timer for a moment
 * already past. A caller that wants longer asks again.
 *
 * It lives here rather than in ir_ao.c because a capture deadline is clamped
 * to it, so a caller that waits for its own answer has to know the figure to
 * bound its own wait by the same one.
 */
#define KODE_SYS_IR_TIMER_MS_MAX   600000U

#define KODE_TOUCH_TEST_TOL_PX  64U    /**< How far from the target a tap
                                       *   may land, per axis, and still
                                       *   pass: 5 mm on this glass, for a
                                       *   finger that lands and rolls a
                                       *   few mm off its mark. The two
                                       *   targets sit mid-glass, so a
                                       *   mirrored axis puts the tap at
                                       *   least about 70 px off and a
                                       *   broken mapping hundreds: none
                                       *   of them can pass.             */

/* ---- Power ---------------------------------------------------------------
 * Nothing about a battery changes fast. The first reading waits longer than
 * the rest because the PMIC's ADC needs ~1.2s after being started before its
 * registers mean anything - ask sooner and every scale reads at its floor,
 * which looks exactly like a flat cell.
 */
#define KODE_POWER_POLL_MS      2000U
#define KODE_POWER_FIRST_MS     1500U

/* ---- The gauge row (`test component=gauge`) ---------------------------------
 * A CW2217 in shutdown (the cell just connected) takes about 1.5 s from
 * AO_Power's wake to report ready (0xA7). A row asked sooner waits for it,
 * looking at each poll, and is judged once the chip is up or once
 * KODE_GAUGE_READY_WAIT_MS has passed since the ask, whichever comes first.
 * The poll that sees the deadline comes up to KODE_POWER_POLL_MS after it.
 */
/** @brief How long the row waits for the gauge's algorithm, from the ask. */
#define KODE_GAUGE_READY_WAIT_MS   3000U
/** @brief What the row's budget adds to the wait and the poll: the judging and the verdict's way back. */
#define KODE_GAUGE_TEST_MARGIN_MS  1000U
/** @brief The row's budget: the wait, the poll that ends it, the margin. */
#define KODE_GAUGE_TEST_BUDGET_MS  (KODE_GAUGE_READY_WAIT_MS + KODE_POWER_POLL_MS \
                                    + KODE_GAUGE_TEST_MARGIN_MS)

/**
 * @brief How long the cable stays out before AO_Power tells whoever armed it.
 *
 * The factory reset's ship end waits this long after the cable leaves so a
 * bump of the connector does not switch the board off. The edge is seen by a
 * poll, not by an interrupt, so the countdown starts at the poll that sees
 * VBUS gone and ends within one KODE_POWER_POLL_MS of the cable's real
 * departure plus this time.
 */
#define KODE_SHIP_UNPLUG_MS    15000U

#define KODE_MS_PER_S          1000U  /**< Milliseconds in a second.         */

/** @brief KODE_SHIP_UNPLUG_MS in whole seconds, for the log lines that say it. */
#define KODE_SHIP_UNPLUG_S     (KODE_SHIP_UNPLUG_MS / KODE_MS_PER_S)

/** @brief NVS namespace of the station lock; the factory reset keeps it. */
#define KODE_STATION_NVS_NS     "kstation"

/** @brief Where the card keeps the copy of an app: the path before its name. */
#define KODE_CARD_APP_PREFIX    "/sdcard/"
/** @brief The end of the path of an app's copy on the card, after its name. */
#define KODE_CARD_APP_SUFFIX    ".bin"

/** @brief The longest AO_AppMgr waits for AO_Storage or AO_Net to answer an
 *         open or a STAT of the card; then it gives that wait up. */
#define KODE_APPMGR_OPEN_MS     30000U
/** @brief How many times AO_AppMgr asks again for a fetched data folder's put
 *         that AO_Storage refused as busy, before the fetch fails busy. */
#define KODE_APPMGR_PUT_TRIES   3U
/** @brief The pause before each of those tries, in ms. */
#define KODE_APPMGR_PUT_RETRY_MS 500U

/* ---- The factory reset's budgets ----------------------------------------
 * AO_Boot walks the reset one step at a time and arms one timer per step; a
 * silent owner ends the reset at that step instead of holding the board for
 * ever. Each budget is the longest its step may take, not the usual time.
 */
#define KODE_RESET_FIND_MS      35000U  /**< AO_AppMgr names where the welcome
                                         *   app is: a directory read and, for
                                         *   an app not installed, one STAT of
                                         *   the card, which AO_AppMgr itself
                                         *   gives up on after
                                         *   KODE_APPMGR_OPEN_MS. The budget
                                         *   outlasts that, so a silent card
                                         *   is answered by AO_AppMgr, not cut
                                         *   off by the reset.                */
_Static_assert(KODE_RESET_FIND_MS > KODE_APPMGR_OPEN_MS,
               "the find step must outlast AO_AppMgr's wait for the card");
#define KODE_RESET_INSTALL_MS   120000U /**< The welcome app written from the
                                         *   card: a region of 13.75 MB written
                                         *   in 32 KB chunks is the ceiling,
                                         *   and the image is far smaller.    */
#define KODE_RESET_APPS_MS      60000U  /**< Every other app removed: one
                                         *   directory commit, then each
                                         *   image header and each data
                                         *   folder in turn.                  */
#define KODE_RESET_LINK_MS      20000U  /**< The C5 link brought up, if it was
                                         *   not: the bring-up of a board that
                                         *   booted without the radio.        */
#define KODE_RESET_WIPE_MS      15000U  /**< The C5's saved networks and BLE
                                         *   keys, then this chip's bond
                                         *   store: three steps, AO_Radio's
                                         *   own 3 s each, after up to 3 s of
                                         *   a vault request of its own.      */
#define KODE_RESET_WIPE_AGAIN_MS 250U   /**< The pause before a wipe AO_Radio
                                         *   refused at the vault is asked
                                         *   again, inside the wipe's budget:
                                         *   a link just up has its own vault
                                         *   request in flight.               */
#define KODE_RESET_NS_MAX       32U     /**< NVS namespaces erased at most; a
                                         *   store that still holds one after
                                         *   this many has a namespace that
                                         *   will not go, and the reset says
                                         *   so instead of looping.           */
#define KODE_RESET_NVS_MS       5000U   /**< The console's margin for the
                                         *   steps AO_Boot does not time: up
                                         *   to KODE_RESET_NS_MAX namespace
                                         *   erases and the boot request's
                                         *   write.                           */
#define KODE_RESET_CONSOLE_MS   (KODE_RESET_FIND_MS + KODE_RESET_INSTALL_MS \
                                 + KODE_RESET_FIND_MS + KODE_RESET_APPS_MS \
                                 + KODE_RESET_LINK_MS + KODE_RESET_WIPE_MS \
                                 + KODE_RESET_NVS_MS + KODE_RESTART_DRAIN_MS)
                                        /**< How long the console waits for
                                         *   the one answer of
                                         *   `factory_reset`: every step at
                                         *   its budget, one after another,
                                         *   the find twice (before and after
                                         *   an install) and the drain that
                                         *   precedes the restart.          */

/* ---- Event pools --------------------------------------------------------
 * QF requires pools registered in order of strictly increasing block size,
 * and serves each allocation from the smallest pool whose block still fits.
 * Four tiers:
 *   - small  : LED commands, touch samples, console bytes, storage loans
 *   - medium : pixel flushes, console lines, progress reports, app-install
 *              requests and listing entries
 *   - large  : log lines, which are by far the biggest routine traffic
 *   - jumbo  : a piece of an arriving image, and nothing else
 *
 * Only the COUNTS are here. Each tier's WIDTH is set in kqp.c, where the pool
 * is declared: a block needs a type, so each tier is typedef'd from the widest
 * event that must fit in it (KqpSmallBlock is a TouchEvt) and KQP_BLOCK_SMALL
 * is that width. The type names a size and never a tenant - an event lands in
 * a tier because of how many bytes it is, nothing else.
 *
 * The large tier is deep because logging is bursty in exactly the places it
 * matters: bring-up emits a couple of dozen lines in a few milliseconds, and
 * a slot write emits one per ten percent while the GUI is also drawing.
 */
#define KQP_SMALL_POOL_COUNT    32U
#define KQP_BYTE_POOL_MARGIN    16U   /**< Small blocks a console byte leaves free (half the tier), for every other AO's small events. */
_Static_assert((KQP_BYTE_POOL_MARGIN > 0U) && (KQP_BYTE_POOL_MARGIN < KQP_SMALL_POOL_COUNT),
               "a console byte must leave the small pool some room and still fit in it");
/* Deep enough for the pages of a rescan to sit in AO_Gui's queue at once:
 * AO_Gui asks AO_Storage and AO_AppMgr together, and it is the lowest of the
 * askers (3, against AO_Console's 11), so it can be holding a page from each.
 * It is not the lowest priority in the system - AO_Net is, at 2, but AO_Net
 * asks for none of this.
 *
 * Sized against KODE_STORAGE_LIST_MAX's own worst case, a page of
 * KODE_STORAGE_LIST_MAX + 1 events, every entry a medium block while it is in
 * flight, with room left for the other medium tenants; storage_ao.c carries a
 * _Static_assert that ties the two numbers together. */
#define KQP_MEDIUM_POOL_COUNT   64U
/* Sized by the console's largest synchronous answer, not by log traffic:
 * `help` and `ps` emit one line per row plus the terminator in a single
 * run-to-completion step at a priority above AO_Log, so every one of those
 * lines holds a block until the console yields. With 24 blocks the tail of
 * the burst - including the `>ok` a host is blocked on - was silently
 * dropped. ~31 rows today; the rest is headroom for concurrent logging.
 * AO_Log's queue in log_ao.c must stay deeper than this pool. */
#define KQP_LARGE_POOL_COUNT    48U
/* The wide tier exists only in an app whose Bluetooth values are longer than
 * the large tier carries (KQP_BLE_APP_WIDE), and holds them alone, in PSRAM.
 * Eight: a central's writes waiting for the app, and the app's own values on
 * their way to AO_Radio, which takes each in one step. */
#define KQP_WIDE_POOL_COUNT     8U
/* The jumbo tier holds one thing: a piece of an image arriving on the console
 * (a terminal install). Four blocks cover everything one 4 KB protocol chunk
 * can fragment into on its way off the wire (the reader drains the RX ring in
 * whatever pieces it finds) with room to spare; the transfer's actual flow
 * control is the `>|ack` protocol, not this pool - the wire itself has none
 * (the USB-Serial/JTAG ISR drops silently once its RX ring is full). */
#define KQP_JUMBO_POOL_COUNT    4U
/* The icon tier holds one thing: a piece of an app's icon leaving a medium
 * (`app_data`). An icon is KODE_APP_ICON_BYTES, 48 KB, and travels as
 * KODE_APP_ICON_PIECES events taken in one step or not at all, section E of
 * the app data spec; one icon is in flight at a time, so the tier is one
 * icon's pieces. Its blocks come from PSRAM when kqp_init() runs: the same
 * pieces in the internal pool would be 48 KB of the 640 KB, taken for the
 * life of every firmware whether or not it ever shows an icon. */
#define KODE_APP_ICON_BYTES     49152U  /**< == KAPP_ICON_BYTES: the 128x128 RGB565 + A8 block. */
#define KODE_APP_ICON_PIECES    3U      /**< The events one icon travels in.                    */
#define KODE_APP_ICON_PIECE     (KODE_APP_ICON_BYTES / KODE_APP_ICON_PIECES)
                                        /**< What each carries: 16 KB.                          */
#define KQP_ICON_POOL_COUNT     KODE_APP_ICON_PIECES  /**< One icon's pieces.                   */

/* ---- System AO housekeeping --------------------------------------------- */
#define KQP_AO_QUEUE_LEN        16U   /**< Events buffered per system AO.     */
/* Was 4096, and that was a cliff: AO_AppMgr's install path stacks two
 * kapps_dir_t copies (~1 KB each: region_install's working copy and
 * dir_commit's candidate) plus logging frames, and the streamed-install path
 * pushed it over - the overflow hung the AO mid-CRC with no panic to show
 * for it. 8 KB buys the whole install path comfortable margin for the price
 * of a few KB of internal RAM per AO. */
#define KQP_AO_STACK_BYTES      8192U /**< Default stack per AO.              */

/* ---- Request doors: queue depths, and the post margin each leaves free (half a queue) ---- */
/* AO_Radio's queue: a scan knock and a KODE answer, posted from other tasks,
 * leave KQP_RADIO_DROP_MARGIN slots free and are refused past it. The BLE
 * state, the pairing note, the lost link and the AO's own time events post
 * with no margin: a full queue is still an assertion for them. */
#define KQP_RADIO_QUEUE_LEN     16U   /**< Events AO_Radio buffers.           */
#define KQP_POWER_QUEUE_LEN     8U    /**< Events AO_Power buffers.           */
#define KQP_RADIO_POST_MARGIN   8U    /**< Slots a Wi-Fi door or a link ask leaves free. */
#define KQP_RADIO_DROP_MARGIN   2U    /**< Slots a post that may be refused, a scan knock or a KODE answer, leaves free. */
#define KQP_POWER_POST_MARGIN   4U    /**< Slots a power door leaves free.    */
_Static_assert((KQP_RADIO_POST_MARGIN * 2U) == KQP_RADIO_QUEUE_LEN,
               "the radio door margin is half of AO_Radio's queue");
_Static_assert((KQP_RADIO_DROP_MARGIN > 0U) && (KQP_RADIO_DROP_MARGIN < KQP_RADIO_POST_MARGIN),
               "a post that may be refused goes deeper into AO_Radio's queue than a door does, and leaves slots");
_Static_assert((KQP_POWER_POST_MARGIN * 2U) == KQP_POWER_QUEUE_LEN,
               "the power door margin is half of AO_Power's queue");
#define KQP_BOOT_QUEUE_LEN      8U    /**< Events AO_Boot buffers: an AO with few events. */
#define KQP_NET_QUEUE_LEN       24U   /**< Events AO_Net buffers: the default, and an identity answer's room. */

/* ---- What owners keep for their own inbound traffic ---- */
#define KQP_WORKER_POST_MARGIN  0U    /**< Slots a worker's done event leaves free: 0 only asks that there be one. */
#define KQP_ID_PART_MARGIN      1U    /**< Slots an identity part leaves free in its asker's queue: the end's. */
#define KQP_ID_ANSWER_MARGIN    0U    /**< Slots an identity, signature or wipe answer leaves free: 0 only asks that there be one. */
#define KQP_FRAMES_MARGIN       4U    /**< Slots a Wi-Fi frames tick or frame leaves free in its asker's queue: room for its own traffic and the answer to a stop. */
#define KQP_FRAMES_ANSWER_MARGIN 0U   /**< Slots a Wi-Fi frames answer leaves free: 0 only asks that there be one. */
#define KQP_FRAMES_POOL_MARGIN  16U   /**< Blocks a Wi-Fi frames tick (the small tier) or frame (the medium tier) leaves free in its tier, for every other AO's events; kqp.c pins each event to its tier. */
_Static_assert((KQP_FRAMES_POOL_MARGIN > 0U) && (KQP_FRAMES_POOL_MARGIN < KQP_SMALL_POOL_COUNT),
               "a tick leaves some of the small tier free, and the tier has more than that");
_Static_assert(KQP_FRAMES_POOL_MARGIN < KQP_MEDIUM_POOL_COUNT,
               "a frame leaves some of the medium tier free, and the tier has more than that");
#define KQP_NET_MQTT_MARGIN     12U   /**< Slots inbound MQTT leaves free in AO_Net's queue: an identity answer's room, KODE_ID_PARTS_MAX parts and the end. */
#define KQP_NET_LINK_MARGIN     0U    /**< Slots an MQTT link event leaves free in AO_Net's queue: it yields last of all. */
#define KQP_NET_WAKE_MARGIN     1U    /**< Slots the channel's ring wake leaves free in AO_Net's queue. */
#define KQP_APPMGR_ASK_MARGIN   0U    /**< Slots an eviction question leaves free in AO_AppMgr's queue. */

/* ---- App traffic (kode_sys.h's commands and requests, kaos.h's doors, and
 *      the `config` requests each owner registers with, kode_config.h): the
 *      slots each leaves free in its owner's queue, above all the owner keeps
 *      for its inbound traffic ---- */
#define KQP_CMD_MARGIN          4U    /**< The default: half of the smallest owner queue, 8. */
#define KQP_LEDMGR_CMD_MARGIN   KQP_CMD_MARGIN                         /**< AO_LedMgr, KQP_AO_QUEUE_LEN.    */
#define KQP_IR_CMD_MARGIN       KQP_CMD_MARGIN                         /**< AO_Ir, KQP_AO_QUEUE_LEN.        */
#define KQP_AUDIO_CMD_MARGIN    KQP_CMD_MARGIN                         /**< AO_Audio, KQP_AO_QUEUE_LEN.     */
#define KQP_IMU_CMD_MARGIN      KQP_CMD_MARGIN                         /**< AO_Imu, KQP_AO_QUEUE_LEN.       */
#define KQP_NFC_CMD_MARGIN      KQP_CMD_MARGIN                         /**< AO_Nfc, KQP_AO_QUEUE_LEN.       */
#define KQP_RFID_CMD_MARGIN     KQP_CMD_MARGIN                         /**< AO_Rfid, KQP_AO_QUEUE_LEN.      */
#define KQP_STORAGE_CMD_MARGIN  KQP_CMD_MARGIN                         /**< AO_Storage, KQP_AO_QUEUE_LEN.   */
#define KQP_APPMGR_CMD_MARGIN   KQP_CMD_MARGIN                         /**< AO_AppMgr, KQP_AO_QUEUE_LEN.    */
#define KQP_POWER_CMD_MARGIN    KQP_CMD_MARGIN                         /**< AO_Power, KQP_POWER_QUEUE_LEN.  */
#define KQP_RADIO_CMD_MARGIN    KQP_RADIO_POST_MARGIN                  /**< AO_Radio, KQP_RADIO_QUEUE_LEN: its doors' half.  */
#define KQP_BOOT_CMD_MARGIN     KQP_CMD_MARGIN                         /**< AO_Boot, KQP_BOOT_QUEUE_LEN.    */
#define KQP_DISPLAY_CMD_MARGIN  KQP_CMD_MARGIN                         /**< AO_Display, KQP_AO_QUEUE_LEN.   */
#define KQP_HAPTIC_CMD_MARGIN   KQP_CMD_MARGIN                         /**< AO_Haptic, KQP_AO_QUEUE_LEN.    */
#define KQP_LOG_CMD_MARGIN      KQP_CMD_MARGIN                         /**< AO_Log, its own queue (log_ao.c). */
#define KQP_NET_CMD_MARGIN     (KQP_NET_MQTT_MARGIN + KQP_CMD_MARGIN) /**< AO_Net, KQP_NET_QUEUE_LEN: above inbound MQTT. */
_Static_assert((KQP_LEDMGR_CMD_MARGIN > 0U) && (KQP_LEDMGR_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a command leaves AO_LedMgr's no-margin posts their room");
_Static_assert((KQP_IR_CMD_MARGIN > KQP_WORKER_POST_MARGIN) && (KQP_IR_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a command leaves AO_Ir's worker answer its room");
_Static_assert((KQP_AUDIO_CMD_MARGIN > KQP_WORKER_POST_MARGIN) && (KQP_AUDIO_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a command leaves AO_Audio's worker answer its room");
_Static_assert((KQP_IMU_CMD_MARGIN > 0U) && (KQP_IMU_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a request leaves AO_Imu's timers and no-margin posts their room");
_Static_assert((KQP_NFC_CMD_MARGIN > 0U) && (KQP_NFC_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a request leaves AO_Nfc's timers and no-margin posts their room");
_Static_assert((KQP_RFID_CMD_MARGIN > KQP_WORKER_POST_MARGIN) && (KQP_RFID_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a request leaves AO_Rfid's worker answer its room");
_Static_assert((KQP_STORAGE_CMD_MARGIN > KQP_WORKER_POST_MARGIN) && (KQP_STORAGE_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a request leaves AO_Storage's worker answer its room");
_Static_assert((KQP_APPMGR_CMD_MARGIN > KQP_APPMGR_ASK_MARGIN) && (KQP_APPMGR_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a command leaves AO_AppMgr's eviction question its room");
_Static_assert((KQP_POWER_CMD_MARGIN > 0U) && (KQP_POWER_CMD_MARGIN < KQP_POWER_QUEUE_LEN),
               "a command leaves AO_Power's no-margin posts their room");
_Static_assert((KQP_RADIO_CMD_MARGIN > KQP_WORKER_POST_MARGIN) && (KQP_RADIO_CMD_MARGIN < KQP_RADIO_QUEUE_LEN),
               "a command leaves AO_Radio's worker answer and no-margin posts their room");
_Static_assert((KQP_BOOT_CMD_MARGIN > 0U) && (KQP_BOOT_CMD_MARGIN < KQP_BOOT_QUEUE_LEN),
               "a command leaves AO_Boot's no-margin posts their room");
_Static_assert((KQP_DISPLAY_CMD_MARGIN > 0U) && (KQP_DISPLAY_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a command leaves AO_Display's no-margin posts their room");
_Static_assert((KQP_HAPTIC_CMD_MARGIN > 0U) && (KQP_HAPTIC_CMD_MARGIN < KQP_AO_QUEUE_LEN),
               "a request leaves AO_Haptic's timers and no-margin posts their room");
_Static_assert((KQP_NET_MQTT_MARGIN > KQP_NET_WAKE_MARGIN) && (KQP_NET_MQTT_MARGIN > KQP_NET_LINK_MARGIN)
               && (KQP_NET_MQTT_MARGIN > KQP_ID_PART_MARGIN) && (KQP_NET_MQTT_MARGIN > KQP_WORKER_POST_MARGIN),
               "inbound MQTT leaves AO_Net's identity answers, ring wake, link events and worker answer their room");
_Static_assert((KQP_NET_CMD_MARGIN > KQP_NET_MQTT_MARGIN) && (KQP_NET_CMD_MARGIN < KQP_NET_QUEUE_LEN)
               && ((KQP_NET_QUEUE_LEN - KQP_NET_CMD_MARGIN) == (KQP_AO_QUEUE_LEN / 2U)),
               "app traffic leaves AO_Net's inbound MQTT its room, and keeps half a default queue");
#define KQP_LOWPOWER_CMD_MARGIN (KODE_LOWPOWER_PARTICIPANTS + KQP_CMD_MARGIN) /**< AO_LowPower, KODE_LOWPOWER_QUEUE_LEN: above a whole round's answers. */
_Static_assert(KQP_LOWPOWER_CMD_MARGIN < KODE_LOWPOWER_QUEUE_LEN,
               "an app's rest requests leave AO_LowPower a round's answers and room of their own");
_Static_assert((KQP_CMD_MARGIN * 2U) <= KQP_BOOT_QUEUE_LEN,
               "the default leaves at least half of the smallest queue to its owner");
_Static_assert((KQP_RADIO_CMD_MARGIN * 2U) >= KQP_RADIO_QUEUE_LEN,
               "a command leaves at least half of AO_Radio's queue to its owner");
_Static_assert((KQP_CMD_MARGIN * 2U) <= KQP_POWER_QUEUE_LEN,
               "the default leaves at least half of the smallest queue to its owner");

/** @brief Workers the registry of kqp_worker_start() can hold: the SDK's own
 *         (about ten) and a few of an app's. A worker past it still runs and
 *         is not listed by `ps`. */
#define KQP_WORKERS_MAX         16U

/** @brief Characters of a worker's task name the registry keeps, with the
 *         terminator: FreeRTOS keeps no more of a task's name. */
#define KQP_WORKER_NAME_MAX     16U

/* Four sizes rather than one, because one was 8 KB for all fifteen system
 * AOs - 123 KB of internal .bss on a chip with 640 KB of it, most of it
 * never touched. Measured high-water use under the heaviest paths the
 * console can drive (a 2 MB `file_get`, two `screenshot`s, the `test`
 * battery) runs from 352 B to 3840 B, so each AO gets the tier that keeps
 * at least twice its worst measurement, and the ones whose deep path
 * cannot be driven from the console keep the default.
 *
 * Raising an AO's tier is cheap and safe; lowering one is not. Measure
 * with `tasks` under the AO's own heaviest path before moving it down,
 * and remember that a stack overflow here does not panic - it hangs the
 * AO mid-work, which is how the 4 KB this file used to hand out was found. */
#define KQP_AO_STACK_LARGE      6144U /**< Into a deep vendor stack.          */
#define KQP_AO_STACK_MEDIUM     4096U /**< Driver work in its own frames.     */
#define KQP_AO_STACK_SMALL      3072U /**< Dispatches events, little else.    */

/* ---- Active Object watchdog (CONFIG_KQP_AO_WATCH) ------------------------ */
#define KQP_AO_WATCH_PERIOD_MS  1000U  /**< Supervisor pass interval.         */
#define KQP_AO_WATCH_TIMEOUT_MS 30000U /**< This long in one state is a hang. */
/* The longest step worth no warning has its number in Kconfig
 * (CONFIG_KQP_AO_STEP_MAX_MS, 5 s), because the port times the steps and
 * sits below this header. A longer step logs one warning at every log level
 * except off; work that waits goes to a worker. */

#endif /* KQP_CONFIG_H_ */
