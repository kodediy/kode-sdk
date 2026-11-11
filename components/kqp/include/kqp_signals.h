/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_signals.h
 * @brief System-wide QP signals and event types for kodeOS.
 */
#ifndef KQP_SIGNALS_H_
#define KQP_SIGNALS_H_

#include "qpc.h"
#include "sdkconfig.h" /* CONFIG_KODE_LOWPOWER */
#include "kqp_config.h"   /* KODE_CONSOLE_LINE_MAX, KODE_APP_FILE_MAX */
#include "khal_sync.h"    /* khal_sem_t: the `done` of every blocking request */

#ifdef __cplusplus
extern "C" {
#endif

enum KodeSignals {
    /* ---- Published signals (QF pub/sub bus) ----------------------------- */
    LED_SET_SIG = Q_USER_SIG,   /**< LedMgr: drive the LED to LedEvt.on.      */
    LED_TOGGLE_SIG,             /**< LedMgr: invert the LED.                  */
    LED_BLINK_PERIOD_SIG,       /**< LedMgr: new heartbeat period (LedBlinkEvt). */
    LED_HEARTBEAT_ENABLE_SIG,   /**< LedMgr: start/stop heartbeat (LedEvt.on). */
    LED_RGB_SIG,                /**< LedMgr: set the colour (LedRgbEvt).      */
    APP_FLASH_DONE_SIG,         /**< A flash write ended well: AppMgr's app,
                                 *   or AO_Radio's C5 image, activated.
                                 *   AppFlashDoneEvt names the app installed;
                                 *   "" for the C5's image.                  */
    APP_FLASH_FAIL_SIG,         /**< A flash write failed: AppMgr's, or
                                 *   AO_Radio's after its first report.
                                 *   AppFlashFailEvt says why.               */
    APP_FLASH_PROGRESS_SIG,     /**< AppMgr or AO_Radio: AppProgressEvt, 0 %
                                 *   when a write starts, then every whole
                                 *   percent. The Opening sheet and the LED
                                 *   follow a write by this family alone.    */
    APP_ENTRY_SIG,              /**< AO_AppMgr: one installed app, in answer
                                 *   to APP_LIST_REQ_SIG (AppEntryEvt).       */
    APP_LIST_END_SIG,           /**< AO_AppMgr: that was the last entry of
                                 *   the page (ListEndEvt).                   */
    APP_EVICTED_SIG,            /**< AO_AppMgr: an install took an app's
                                 *   place in the region (AppEvictedEvt), one
                                 *   event per app, before the write starts.
                                 *   The app it replaces is not one.          */
    RECV_READY_SIG,             /**< The console's receive protocol: sent by
                                 *   whichever AO is receiving, once its
                                 *   destination is open and the console is in
                                 *   raw mode - the host may stream the
                                 *   payload now. The go signal a terminal
                                 *   install and `file_put` both turn into
                                 *   `>|ready`.
                                 *
                                 *   This one and the three below it live in
                                 *   the published range because AO_AppMgr
                                 *   and AO_Radio PUBLISH them, for
                                 *   AO_Console. AO_Storage POSTS the very same
                                 *   signals to whoever asked instead - a
                                 *   `file_put` is a plain file copy. The
                                 *   console's handlers are keyed on its own
                                 *   in-flight command and cannot tell the
                                 *   deliveries apart, which is what lets one
                                 *   protocol serve all three without a second
                                 *   set of signals or a discriminator field.
                                 *
                                 *   The protocol is the wire's, not the
                                 *   write's: the Opening sheet and the LED
                                 *   follow APP_FLASH_PROGRESS_SIG, which a
                                 *   writer publishes at 0 % with its ready.  */
    RECV_ACK_SIG,               /**< The receive protocol: one full chunk of
                                 *   the arriving payload has been consumed -
                                 *   the host may send the next (`>|ack`). The
                                 *   transfer's entire flow control: the
                                 *   USB-Serial/JTAG driver drops silently on a
                                 *   full RX ring, so the wire itself provides
                                 *   none.
                                 *
                                 *   AO_Console's alone, and that is
                                 *   load-bearing: an ack per 4 KB is some
                                 *   1250 events for a 5 MB image, which no
                                 *   low-priority subscriber's queue would
                                 *   survive.                                 */
    RECV_DONE_SIG,              /**< The receive protocol: the payload
                                 *   arrived and verified. What that MEANS
                                 *   depends on which AO posted or published
                                 *   it, the same way RECV_READY_SIG does:
                                 *   AO_AppMgr's terminal install is installed
                                 *   (booting too, if asked); AO_Storage's
                                 *   `file_put` is simply a file on FAT now -
                                 *   nothing is installed, nothing boots. Bare
                                 *   and static, as are the two endings below
                                 *   it - a static immutable event cannot fail
                                 *   to allocate, which matters here:
                                 *   AO_Console has given up its own watchdog
                                 *   for this command, so one of these three
                                 *   signals must always be able to arrive.   */
    RECV_FAIL_SIG,              /**< The receive protocol: refused, or the
                                 *   bytes did not survive the trip - kcli
                                 *   error 5.                                 */
    RECV_STALL_SIG,             /**< The receive protocol: the host stopped
                                 *   sending and never came back - kcli
                                 *   error 6.                                 */
    STORAGE_ENTRY_SIG,          /**< AO_Storage: one file found (StorageEntryEvt). */
    STORAGE_LIST_END_SIG,       /**< AO_Storage: that was the last entry of
                                 *   the page (ListEndEvt).                   */
    TEST_RESULT_SIG,            /**< Any AO: the outcome of a self-test that
                                 *   could not be answered on the spot
                                 *   (TestResultEvt).                         */
    AUTOTEST_BEGIN_SIG,         /**< AO_Autotest: a run was taken or refused
                                 *   (AutotestBeginEvt).                      */
    AUTOTEST_ROW_SIG,           /**< AO_Autotest: one row started or ended
                                 *   (AutotestRowEvt).                        */
    AUTOTEST_END_SIG,           /**< AO_Autotest: the run is over
                                 *   (AutotestEndEvt).                        */
    AUTOTEST_HOLD_SIG,          /**< AO_Autotest: the run waits for whoever
                                 *   asked for it before going on to the rows
                                 *   that need hands (AutotestHoldEvt).       */
    AUTOTEST_REPORT_SIG,        /**< AO_Autotest: one row of the report kept
                                 *   from the last `user` run, then an end
                                 *   marker (AutotestReportEvt).              */
    TEST_SKIP_SIG,              /**< Any AO: a self-test that could not be
                                 *   judged because what it needs is not
                                 *   there - no card, no cable, no tag. The
                                 *   TEST_RESULT_SIG shape; @c ok is unused.  */
    POWER_TEST_SIG,             /**< AO_Power: one step of the `power_plug`
                                 *   row (PowerTestEvt).                      */
    DISPLAY_STATE_SIG,          /**< AO_Display: whether the panel is up, and
                                 *   how bright it is driven (DisplayStateEvt:
                                 *   `up` and `bright` only, no semaphore).
                                 *   Published once from AO_Display's initial
                                 *   transition and again on every
                                 *   DISPLAY_STATE_REQ_SIG, so this is a state
                                 *   anyone can ask for rather than an edge you
                                 *   had to be early enough to catch. It
                                 *   reports a panel that did NOT come up too:
                                 *   a renderer that never hears anything
                                 *   cannot tell that apart from one that
                                 *   started late.                            */
    DISPLAY_MIRROR_INFO_SIG,    /**< AO_Display: the mirror's live state, in
                                 *   answer to DISPLAY_MIRROR_INFO_REQ_SIG
                                 *   (MirrorInfoEvt) - console_ao.c's
                                 *   mirror_info.                            */
    TOUCH_EVT_SIG,              /**< AO_Touch: a touch sample (TouchEvt).     */
    BUTTON_EVT_SIG,             /**< AO_Buttons: one button changed (ButtonEvt). */
    BUTTON_HOLD_SIG,            /**< AO_Buttons: the top or the bottom button
                                 *   has been down for KODE_BUTTON_HOLD_MS
                                 *   (ButtonHoldEvt). Once per press, while
                                 *   the button is still down. The console's
                                 *   `btn ... state=hold` publishes it on
                                 *   request.                                 */
    BUTTON_DOUBLE_SIG,          /**< AO_Buttons: the top button was pressed
                                 *   twice in quick succession
                                 *   (ButtonDoubleEvt). Published right after
                                 *   the second press's own BUTTON_EVT_SIG.
                                 *   The console's `btn id=top state=double`
                                 *   publishes it on request.                 */
    HOME_SIG,                   /**< AO_Boot: the bottom button was held, so
                                 *   the interface goes home. No payload.
                                 *   Published in BOTH roles and answered by
                                 *   whoever draws: kgui's launcher in kodeOS,
                                 *   an app's own AO in an app - where it also
                                 *   means the board is about to reset unless
                                 *   kode_sys_home_defer() says otherwise.       */
    IOEXP_INPUT_SIG,            /**< AO_IoExp: what the expander's eight pins
                                 *   read (IoexpInputEvt). Every read of the
                                 *   input register goes out here - on the
                                 *   chip's interrupt, and on request - so a
                                 *   subscriber sees each change and can ask
                                 *   for a confirming sample.                */
    IR_RX_SIG,                  /**< AO_Ir: one frame the receiver heard
                                 *   (IrRxEvt). Not published for a frame
                                 *   this board transmitted itself - see
                                 *   AO_Ir's echo suppression.             */
    NFC_TAG_SIG,                /**< AO_Nfc: one tag the poller activated
                                 *   (NfcTagEvt). Published only while a
                                 *   request is in flight - the field is off
                                 *   at rest, so silence here means nobody
                                 *   asked, not that nothing was presented. */
    NFC_EMU_SIG,                /**< AO_Nfc: one turn of an emulation window
                                 *   (NfcEmuEvt) - a reader's field arriving, a
                                 *   select, a Type-2 READ served, or the
                                 *   window stopping with its total. Published,
                                 *   so it lives in the pub/sub range; the
                                 *   emulation request and its answer are direct
                                 *   posts, below.                          */
    RFID_TAG_SIG,               /**< AO_Rfid: one EM4100 ID decoded inside a
                                 *   window (RfidTagEvt). Published only while
                                 *   a request is in flight - the field is off
                                 *   at rest, so silence here means nobody
                                 *   asked, not that no tag was presented.  */
    POWER_EVT_SIG,              /**< AO_Power: a battery reading (PowerEvt).  */
    POWER_SHIP_SIG,             /**< AO_Power: the BATFET is about to open.
                                 *   Published in the step BEFORE the write,
                                 *   so every owner above AO_Power's priority
                                 *   gets one run-to-completion step to leave
                                 *   its device safe. Design rule 6's "say it
                                 *   before the write that kills the board",
                                 *   extended from the log to the bus. No
                                 *   payload, and nobody answers it: on
                                 *   battery there is no far side. It is also
                                 *   the honest signal for the case that DOES
                                 *   have one - on USB the write fails, the
                                 *   board stays up, and the devices that put
                                 *   themselves away stay away.              */
    POWER_FIELD_SIG,            /**< AO_Power: one chip field's value or the
                                 *   outcome of writing it, in answer to
                                 *   POWER_FIELD_REQ_SIG (PowerFieldEvt) -
                                 *   console_ao.c's `pmic` and `gauge`.     */
    POWER_CTRL_SIG,             /**< AO_Power: the answer to one POWER_CTRL_REQ_SIG
                                 *   (PowerCtrlEvt), a refusal included.     */
    RADIO_STATE_SIG,            /**< AO_Radio: the C5 link's state
                                 *   (RadioStateEvt). Published on every
                                 *   change and again on RADIO_STATE_REQ_SIG
                                 *   - the DISPLAY_STATE_SIG pattern: a state
                                 *   anyone can ask for, not an edge you had
                                 *   to be early enough to catch.            */
    WIFI_STATE_SIG,             /**< AO_Radio: where the connection stands
                                 *   (WifiStateEvt).                         */
    WIFI_SCAN_ENTRY_SIG,        /**< AO_Radio: one network found
                                 *   (WifiScanEntryEvt) - one event per
                                 *   entry, like every other listing.        */
    WIFI_SCAN_END_SIG,          /**< AO_Radio: that was the last one
                                 *   (WifiScanEndEvt: how many, and whether
                                 *   the ceiling cut it short).              */
    WIFI_SAVED_ENTRY_SIG,       /**< AO_Radio: one remembered network, name
                                 *   only (WifiSavedEvt). Passwords go into
                                 *   the store and do not come out.          */
    WIFI_SAVED_END_SIG,         /**< AO_Radio: that was the last remembered
                                 *   one (WifiScanEndEvt, same shape - a
                                 *   count and whether it worked).           */
    BLE_STATE_SIG,              /**< AO_Radio: what the BLE half is doing
                                 *   (BleStateEvt). Published on every
                                 *   change - a phone connecting is news
                                 *   nobody asked for - and on request.      */
    BLE_PAIR_SIG,               /**< AO_Radio: a pairing wants a code shown,
                                 *   or has ended (BlePairEvt). One signal
                                 *   with a phase rather than two, because
                                 *   both halves are the same exchange told
                                 *   at two moments.                         */
    BLE_SCAN_ENTRY_SIG,         /**< AO_Radio: one device seen (BleScanEvt). */
    BLE_SCAN_END_SIG,           /**< AO_Radio: that was the last one
                                 *   (WifiScanEndEvt: a count and whether
                                 *   the scan itself worked).                */
    BLE_APP_WRITE_SIG,          /**< AO_Radio: a central wrote to the app's
                                 *   own service (BleAppWriteEvt, a copy of
                                 *   the bytes).                             */
    BLE_APP_LINK_SIG,           /**< AO_Radio: a central connected or left
                                 *   (BleAppLinkEvt), one edge each.         */
    NET_PING_ENTRY_SIG,         /**< AO_Net: one ICMP echo's outcome
                                 *   (NetPingEntryEvt). One event per echo,
                                 *   a timeout included - a row missing is a
                                 *   loss the reader has to count for
                                 *   itself.                                 */
    NET_PING_END_SIG,           /**< AO_Net: the session's summary
                                 *   (NetPingEndEvt), or the refusal when
                                 *   there was no session (ok=false).        */
    NET_HTTP_DATA_SIG,          /**< AO_Net: a slice of a response body
                                 *   (NetHttpDataEvt).                       */
    NET_HTTP_DONE_SIG,          /**< AO_Net: status, lengths, and whether the
                                 *   body was cut (NetHttpDoneEvt).          */
    NET_UDP_DONE_SIG,           /**< AO_Net: the datagram is away, or is not
                                 *   (NetUdpDoneEvt).                        */
    NET_INFO_SIG,               /**< AO_Net: this board's addresses and the
                                 *   MQTT client's state (NetInfoEvt).       */
    MQTT_STATE_SIG,             /**< AO_Net: where the MQTT client is
                                 *   (MqttStateEvt). Every change, and on
                                 *   request - the RADIO_STATE_SIG pattern.  */
    MQTT_MSG_SIG,               /**< AO_Net: a message the broker sent us
                                 *   (MqttMsgEvt). The only published event
                                 *   in the system whose rate a stranger
                                 *   controls, which is why AO_Net posts the
                                 *   inbound ones to *itself* with a margin
                                 *   rather than asserting on a full queue.
                                 *
                                 *   That margin protects AO_Net and stops
                                 *   there: publishing has no margin in QP,
                                 *   so a subscriber whose queue fills is an
                                 *   assert. What keeps that from being a
                                 *   reset button held by any broker is the
                                 *   priority ladder - AO_Net is the lowest
                                 *   AO in the system, so every subscriber
                                 *   preempts it and drains each message
                                 *   before the next one is published. A
                                 *   subscriber that BLOCKS (AO_Gui waiting
                                 *   on a panel flush, say) breaks that, so
                                 *   an AO that subscribes to this must
                                 *   either not block or carry a queue deep
                                 *   enough for the bursts it expects.
                                 *
                                 *   The Kode channel's cmd/in topic is not
                                 *   republished here: a message on it is a
                                 *   command and becomes a CONSOLE_LINE_SIG
                                 *   (src NET) instead; a payload that is not
                                 *   one whole line is logged and dropped.   */
    CHANNEL_STATE_SIG,          /**< AO_Net: where the Kode channel stands
                                 *   (ChannelStateEvt). Every change of state,
                                 *   and on request - the MQTT_STATE_SIG
                                 *   pattern.                                */
    CONSOLE_LINE_SIG,           /**< AO_Log: a finished command line. Published
                                 *   rather than posted to AO_Console: AO_Log
                                 *   owns the device, not the policy, and must
                                 *   not have to know whether this firmware
                                 *   even has a command line.                 */

    /* Query responses. Console queries close with data (the terminator
     * follows the answer, README.md "The console"), so the owning AOs
     * answer them with these rather than with log lines alone. Published,
     * not posted, for the same reason the listings are: the asker should
     * not have to be AO_Console. */
    BTN_STATE_SIG,              /**< AO_Buttons: all six buttons right now, in
                                 *   answer to BUTTONS_DUMP_REQ_SIG
                                 *   (ButtonsStateEvt).                       */
    BTN_TEST_SIG,               /**< AO_Buttons: one step of the guided button
                                 *   test (ButtonsTestEvt) - a prompt, or how
                                 *   the step ended. The console prints it,
                                 *   the GUI shows it.                        */
    TOUCH_TEST_SIG,             /**< AO_Touch: one step of the guided touch
                                 *   test (TouchTestEvt) - a target to tap,
                                 *   or how the step ended. The console
                                 *   prints it, the GUI draws it.             */
    BOOT_STATE_SIG,             /**< AO_Boot: the boot state, in answer to
                                 *   BOOT_INFO_REQ_SIG (BootStateEvt).        */
    GUI_STATE_SIG,              /**< AO_Gui: what state the interface is in,
                                 *   in answer to GUI_DUMP_REQ_SIG
                                 *   (GuiStateEvt).                           */
    STORAGE_INFO_SIG,           /**< AO_Storage: one medium's mount state and
                                 *   usage, in answer to STORAGE_OP_INFO
                                 *   (StorageInfoEvt).                        */
    STORAGE_TEST_SIG,           /**< AO_Storage: one phase of the card test
                                 *   (StorageTestEvt) - what it did and what
                                 *   it measured. The console prints it.      */
    PSRAM_TEST_SIG,             /**< AO_Autotest: what the PSRAM row tested
                                 *   and found (PsramTestEvt), once a run,
                                 *   before its verdict.                      */
    AUDIO_TEST_SIG,             /**< AO_Audio: one step of the audio row
                                 *   (AudioTestEvt) - a floor, a bin, a tone
                                 *   about to play or the window it came back
                                 *   in. The console prints it.             */
    AUDIO_FILE_RSP_SIG,         /**< AO_Audio: a recording reached a file, or
                                 *   did not (AudioFileRspEvt). Published, so
                                 *   the console can close on it without
                                 *   AO_Audio knowing who asked; posted
                                 *   instead to the AO that asked through
                                 *   kode_sys_audio_record_file().          */
    AUDIO_FIELD_SIG,            /**< AO_Audio: one codec field's value or the
                                 *   outcome of writing it, in answer to
                                 *   AUDIO_FIELD_REQ_SIG (AudioFieldEvt) -
                                 *   console_ao.c's `audio`.                */
    IMU_TEST_SIG,               /**< AO_Imu: one step of the IMU row
                                 *   (ImuTestEvt). The console prints it.   */
    MAG_TEST_SIG,               /**< AO_Imu: one step of the magnetometer row
                                 *   (MagTestEvt). The console prints it.   */
    HAPTIC_TEST_SIG,           /**< AO_Haptic: one step of the haptic row
                                 *   (HapticTestEvt). The console prints it. */
    HAPTIC_FIELD_SIG,           /**< AO_Haptic: one field's value or the
                                 *   outcome of writing it, in answer to
                                 *   HAPTIC_FIELD_REQ_SIG (HapticFieldEvt). */
    HAPTIC_DONE_SIG,            /**< AO_Haptic: a buzz ended (HapticDoneEvt).
                                 *   Published, so whoever asked can close on
                                 *   it without AO_Haptic knowing who did.  */
    APP_CAT_RSP_SIG,            /**< AO_AppMgr: the outcome of APP_CAT_REQ_SIG
                                 *   (AppCatRspEvt). Published, not posted
                                 *   back, so any subscriber - AO_Console
                                 *   today - can close on it without AO_AppMgr
                                 *   knowing who asked.                       */
    APP_ERASE_DONE_SIG,         /**< AO_AppMgr: an erase ended
                                 *   (AppEraseDoneEvt): what went, what stayed
                                 *   and where. Published, so the console and
                                 *   the launcher both close on it; a request
                                 *   refused while AO_AppMgr is busy ends the
                                 *   same way.                                */

    /* The one *request* that is published rather than posted. AO_Gui lives in
     * kgui, which requires the SDK; the console lives in kaos, underneath it.
     * Posting would mean kaos referencing a symbol from a component above it,
     * so the console asks the bus instead and kgui answers if it is there.
     * When it is not, nobody subscribes, nobody answers, and AO_Console's
     * 5s watchdog says so - which is the documented diagnostic already. */
    GUI_DUMP_REQ_SIG,           /**< AO_Gui: say what state the interface is
                                 *   in (answered with GUI_STATE_SIG).        */

    /* The two rows an operator judges. Published rather than posted back
     * because the console is what prints a row and whoever draws the glass
     * has to keep the pad from meaning anything else while one runs, and
     * neither owner is told who is listening. */
    DISPLAY_TEST_SIG,           /**< AO_Display: one step of the display row
                                 *   (DisplayTestEvt). The console prints
                                 *   it.                                    */
    LED_TEST_SIG,               /**< AO_LedMgr: one step of the LED row
                                 *   (LedTestEvt). The console prints it.   */
    DISPLAY_REPAINT_SIG,        /**< AO_Display: the row is over; whoever
                                 *   draws the glass must invalidate all of
                                 *   it, because every flush it issued
                                 *   during the row was completed and never
                                 *   painted. No payload.                   */
    C5_RUN_REQ_SIG,             /**< radio_amp_request(true): the speaker
                                 *   amplifier is wanted, and the C5 whose IO4
                                 *   enables it. AO_Radio drives IO4 high over
                                 *   a link, and with KODEOS_C5_HOLD lets a
                                 *   held C5 go and answers C5_RUNNING_SIG. No
                                 *   payload.                               */
    C5_RUNNING_SIG,             /**< Published by AO_Radio: the C5 has been
                                 *   out of reset for KODE_C5_BOOT_MS, so its
                                 *   own boot is over and the amplifier is
                                 *   enabled. The answer to C5_RUN_REQ_SIG,
                                 *   given at once when it is already true.
                                 *   No payload.                            */
    EXP_POWER_SIG,              /**< Published by AO_Radio: a switch of J3's
                                 *   +3.3V_EXT is over (ExpPowerDoneEvt).
                                 *   Its rc: KODE_OK when the rail is as
                                 *   asked - the C5 drove IO5, a reset of the
                                 *   C5 with no link put it on, or a request
                                 *   found it so; KODE_IO_ERROR when the C5
                                 *   would not drive the pin (the wish is
                                 *   dropped); KODE_DENIED when the link
                                 *   would not come up or went with the
                                 *   switch, so the C5 was not asked and the
                                 *   next link-up carries the wish out. An
                                 *   "off" with no link brings it up first.
                                 *   Subscribers only: whoever does not wait
                                 *   hears nothing.                         */
    C5_AMP_OFF_SIG,             /**< radio_amp_request(false): the amplifier
                                 *   is not wanted. AO_Radio drives the C5's
                                 *   IO4 low over the link, now or at the
                                 *   next link-up. No payload.              */
    CONFIG_REQ_SIG,             /**< Read or set one setting
                                 *   (KodeConfigReqEvt), posted to the
                                 *   owner its table was registered with
                                 *   (kode_config_post()). The owner answers
                                 *   with kode_config_answer().             */
    CONFIG_ANS_SIG,             /**< A setting's owner: its value, or why it
                                 *   refused (KodeConfigAnsEvt), published.
                                 *   AO_Console prints the one it asked for. */
    TEST_DOT_SCREEN_REQ_SIG,    /**< Published by AO_TestDot: a screen or a
                                 *   pattern for the glass (TestDotScreenEvt).
                                 *   The firmware's LVGL owner builds it and
                                 *   answers test_dot_screen_done() once it
                                 *   is on the panel.                       */
#if CONFIG_KODE_LOWPOWER
    LOWPOWER_ROUND_SIG,         /**< Published by AO_LowPower: a round of
                                 *   readiness is open for a target state,
                                 *   ASLEEP or OFF (LowPowerRoundEvt). A
                                 *   participant rests its device on it, from
                                 *   AWAKE or, for an OFF round, from the
                                 *   ASLEEP rest, and answers exactly once
                                 *   with lowpower_ready().                 */
    LOWPOWER_STATE_SIG,         /**< Published by AO_LowPower on every change
                                 *   of the board's state (LowPowerStateEvt).
                                 *   A participant restores its device on
                                 *   state AWAKE, which also arrives with
                                 *   prev == AWAKE when a round was abandoned
                                 *   (so the restore must be idempotent), and
                                 *   starts no QTimeEvt on state ASLEEP: the
                                 *   tick is stopped by then.               */
    LOWPOWER_REFUSED_SIG,       /**< Published by AO_LowPower when a request
                                 *   or a round did not go through: a cable,
                                 *   a busy owner, a person at the board
                                 *   (LowPowerRefusedEvt). What the console
                                 *   closes `sleep` and `power_off` with.   */
#endif
    BOOT_KEEP_SIG,              /**< AO_Boot: a restart into kodeOS is a moment
                                 *   away. The panel's owner leaves the frame on
                                 *   the glass and says so to the next boot
                                 *   (kdrv_display_keep_for_restart()); nobody
                                 *   answers, and a refusal is the old way.   */
    /* Published signals end here: QF sizes subscriber lists by it; a publish above asserts. */
    MAX_PUB_SIG,

    /* ---- Direct-post signals (AO-private protocols) -------------------- */
    LED_HEARTBEAT_SIG = MAX_PUB_SIG, /**< LedMgr private heartbeat QTimeEvt.  */
    APP_FLASH_CHUNK_SIG,        /**< AO_AppMgr private: pace the next chunk.  */
    APP_ADOPT_SIG,              /**< AO_AppMgr private: the app_pending
                                 *   mailbox holds a verified image at boot -
                                 *   adopt it. Posted to self, never by
                                 *   anything external.                       */
    APP_ERASE_REQ_SIG,          /**< AO_AppMgr: remove the app named
                                 *   AppFlashEvt.file from the region.        */
    APP_CAT_REQ_SIG,            /**< AO_AppMgr: assign or drop an app's
                                 *   categories (AppCatReqEvt). Answered with
                                 *   APP_CAT_RSP_SIG - AO_AppMgr is the
                                 *   table's only writer.                     */
    APP_CANCEL_REQ_SIG,         /**< AO_AppMgr: leave the install in hand,
                                 *   as a failure: the person went back.      */
    APP_RUN_REQ_SIG,            /**< AO_AppMgr: boot the installed app named
                                 *   AppFlashEvt.file. The board resets.      */
    APP_LIST_REQ_SIG,           /**< AO_AppMgr: post APP_ENTRY_SIG for a page
                                 *   of the installed apps to the asker, then
                                 *   APP_LIST_END_SIG (AppListReqEvt).        */
    APP_EVICT_REQ_SIG,          /**< AO_AppMgr: which installed apps would
                                 *   an install of this many bytes, under this
                                 *   project name, evict (AppEvictReqEvt)? A
                                 *   question and nothing more: the directory
                                 *   is not touched and no copy is started.
                                 *   Answered to the asker alone, posted and
                                 *   not published: one APP_EVICT_ENTRY_SIG per
                                 *   victim, then APP_EVICT_END_SIG. Busy
                                 *   AO_AppMgr answers the end alone, and the
                                 *   asker asks again.                        */
    APP_EVICT_ENTRY_SIG,        /**< To the asker: one app the install would
                                 *   evict (AppEvictEntryEvt), in the order
                                 *   they would go.                           */
    APP_EVICT_END_SIG,          /**< To the asker: that was the last entry
                                 *   (AppEvictEndEvt).                        */
    APP_FIND_REQ_SIG,           /**< AO_AppMgr: where is the app named
                                 *   AppFindReqEvt.project - its extent in the
                                 *   apps region, or whether the card has it.
                                 *   Answered to the asker alone, posted and
                                 *   not published: one APP_FIND_RSP_SIG.      */
    APP_FIND_RSP_SIG,           /**< To the asker: the answer to
                                 *   APP_FIND_REQ_SIG (AppFindRspEvt).         */
    APP_KEEP_ONLY_REQ_SIG,      /**< AO_AppMgr: remove every installed app but
                                 *   the one named (AppKeepOnlyReqEvt), their
                                 *   data folders with them. Answered to the
                                 *   asker alone, posted and not published:
                                 *   one APP_KEEP_ONLY_RSP_SIG, once the last
                                 *   folder is dealt with.                    */
    APP_KEEP_ONLY_RSP_SIG,      /**< To the asker: the answer to
                                 *   APP_KEEP_ONLY_REQ_SIG (AppKeepOnlyRspEvt). */
    APP_KEEP_ONLY_STEP_SIG,     /**< AO_AppMgr private: erase the next removed
                                 *   app's image header. Posted to self, one
                                 *   per header, never by anything external.   */
    APP_RECV_REQ_SIG,           /**< AO_AppMgr: an image is about to arrive on
                                 *   the console (AppRecvReqEvt): the region
                                 *   for `dst=app`, the inactive kodeOS slot
                                 *   for `dst=os`, the mailbox for
                                 *   `dst=pending`. Open it, then ask AO_Log
                                 *   for raw mode.                            */
    RECV_DATA_SIG,              /**< One chunk of the payload the receive
                                 *   protocol is carrying (RecvDataEvt) - a
                                 *   terminal install's image for AO_AppMgr,
                                 *   or for AO_Radio when it is the C5's, or
                                 *   a `file_put` payload for AO_Storage.
                                 *   Posted by the console reader task, not
                                 *   by an AO, straight to whichever of the
                                 *   three owns the transfer.                 */
    APP_RECV_IDLE_SIG,          /**< AO_AppMgr private QTimeEvt: no chunk for
                                 *   KODE_RECV_IDLE_MS - the host is gone.    */
    APP_RECV_LAUNCH_SIG,        /**< AO_AppMgr private QTimeEvt: the drain
                                 *   delay after a successful install is
                                 *   over - boot it (AppMgr_launching).       */
    APP_TIDY_TIMEOUT_SIG,       /**< AO_AppMgr private QTimeEvt: AO_Storage
                                 *   did not answer a folder request in
                                 *   APPMGR_TIDY_MS (AppMgr_tidying).         */
    FLASH_REQ_SIG,              /**< AO_AppMgr: one source-to-sink transfer
                                 *   (FlashReqEvt). The only way to ask for
                                 *   an image to be written.                 */
    STORAGE_CMD_SIG,            /**< AO_Storage: do StorageCmdEvt.op.         */
    STORAGE_LIST_PAGE_SIG,      /**< AO_Storage: post a page of the bootable
                                 *   images on every medium to the asker
                                 *   (StorageListPageEvt).                    */
    STORAGE_CMD_RSP_SIG,        /**< To StorageCmdEvt.who, when it named one:
                                 *   that command has run. No payload - it
                                 *   says when, and the medium's state says
                                 *   what.                                    */
    STORAGE_OPEN_REQ_SIG,       /**< AO_Storage: open StorageOpenEvt.name.    */
    STORAGE_READ_REQ_SIG,       /**< AO_Storage: fill a free buffer and send it. */
    STORAGE_NEXT_SIG,           /**< AO_Storage: here is StorageBufEvt.idx
                                 *   back - refill it and send it again.      */
    STORAGE_CLOSE_REQ_SIG,      /**< AO_Storage: close the open file.         */
    STORAGE_OPEN_RSP_SIG,       /**< To the opener (StorageOpenRspEvt).       */
    STORAGE_DATA_SIG,           /**< To the reader (StorageDataEvt).          */
    STORAGE_WRITE_REQ_SIG,      /**< AO_Storage: put StorageWriteEvt.data in a
                                 *   file. One event and not a conversation
                                 *   because the payload is already in memory
                                 *   and stays put - see storage_write_all_
                                 *   request().                              */
    STORAGE_WRITE_NEXT_SIG,     /**< AO_Storage private self-post: the next
                                 *   chunk of the write in progress.         */
    STORAGE_WRITE_RSP_SIG,      /**< To the writer (StorageWriteRspEvt).      */
    STORAGE_DESC_REQ_SIG,       /**< AO_Storage: read the manifest out of the
                                 *   front of one file (StorageDescReqEvt).
                                 *   Posted directly, never published: the
                                 *   launcher asks this up to 24 times in a
                                 *   row and only the asker wants the answer. */
    STORAGE_DESC_RSP_SIG,       /**< AO_Storage: that file's identity
                                 *   (StorageDescRspEvt), back to `who`.      */
    APPDATA_REQ_SIG,            /**< AO_Storage: resolve an app's folder
                                 *   (AppDataReqEvt). Posted directly, never
                                 *   published: only the asker wants the
                                 *   answer, and the asker is one of two, the
                                 *   launcher or the console.                 */
    APPDATA_FIELDS_SIG,         /**< To the asker: the folder's fields
                                 *   (AppDataFieldsEvt).                      */
    APPDATA_ICON_SIG,           /**< To the asker: one third of the icon
                                 *   (AppDataIconEvt).                        */
    APPDATA_END_SIG,            /**< To the asker: the conversation is
                                 *   over.                                    */
    APPDATA_RM_REQ_SIG,         /**< AO_Storage: delete one app's data folder
                                 *   on the INTERNAL partition (AppDataReqEvt
                                 *   again - the request is the same two
                                 *   fields). The card is never touched:
                                 *   https://docs.kode.diy/sdk/kdrv/apps.  */
    APPDATA_RM_RSP_SIG,         /**< To the asker: how that delete ended
                                 *   (AppDataRmRspEvt).                       */
    APPDATA_PUT_REQ_SIG,        /**< AO_Storage: write one app's data folder
                                 *   on the INTERNAL partition (AppDataPutEvt:
                                 *   app.json, then the icon when there is
                                 *   one). Posted directly, never published.  */
    APPDATA_PUT_NEXT_SIG,       /**< AO_Storage private self-post: the icon's
                                 *   step of a put in progress.               */
    APPDATA_PUT_RSP_SIG,        /**< To the asker: how that put ended
                                 *   (AppDataPutRspEvt).                      */
    STORAGE_APP_WIPE_REQ_SIG,   /**< AO_Storage: take every file of one app
                                 *   off both media (AppDataReqEvt again): the
                                 *   images at a medium's root whose descriptor
                                 *   names the project, and its data folder.
                                 *   Posted directly, never published.        */
    STORAGE_APP_WIPE_NEXT_SIG,  /**< AO_Storage private QTimeEvt: the next
                                 *   file of the wipe in progress.            */
    STORAGE_APP_WIPE_RSP_SIG,   /**< To the asker: what went and what stayed
                                 *   on each medium (StorageAppWipeRspEvt).   */
    STORAGE_FIELD_REQ_SIG,      /**< AO_Storage: read, write or dump one field
                                 *   of the card (StorageFieldReqEvt).
                                 *   Answered to StorageFieldReqEvt.who with
                                 *   STORAGE_FIELD_RSP_SIG - posted, not
                                 *   published, the expander's shape: the
                                 *   console asks and nobody else wants its
                                 *   answers.                                 */
    STORAGE_FIELD_RSP_SIG,      /**< To the requester (StorageFieldEvt).      */
    STORAGE_PATH_REQ_SIG,       /**< AO_Storage: stat, list, cat, mkdir,
                                 *   unlink or rmdir one path (StoragePathEvt).
                                 *   STAT, MKDIR, UNLINK and RMDIR are all
                                 *   answered once to StoragePathEvt.who with
                                 *   STORAGE_PATH_RSP_SIG - posted, not
                                 *   published, storage_field_request()'s
                                 *   shape again; the three mutations carry no
                                 *   `exists`/`is_dir`/`size` of their own,
                                 *   `rc` is the whole story. LIST is answered
                                 *   as the same STORAGE_ENTRY_SIG /
                                 *   STORAGE_LIST_END_SIG burst sd_ls already
                                 *   drives, `who` unused. CAT is answered as a
                                 *   STORAGE_LINE_SIG burst, `who`'s own
                                 *   private conversation - nobody else wants
                                 *   a file's text - and `max` is meaningful
                                 *   for it alone.                            */
    STORAGE_PATH_RSP_SIG,       /**< To the requester: STAT, MKDIR, UNLINK,
                                 *   RMDIR and STORAGE_PAIR_REQ_SIG's MOVE all
                                 *   answer here (StoragePathRspEvt) - one
                                 *   shape for "what did this path operation
                                 *   do", whether it asked about a path or
                                 *   changed one.                             */
    STORAGE_LINE_SIG,           /**< To the requester, CAT only
                                 *   (StorageLineEvt): one line of a file's
                                 *   text, or the terminator that closes the
                                 *   round when StorageLineEvt.eof is set.    */
    STORAGE_PAIR_REQ_SIG,       /**< AO_Storage: move or copy (StoragePairEvt
                                 *   .op) src to dst. Answered once to
                                 *   StoragePairEvt.who, the same
                                 *   STORAGE_PATH_RSP_SIG a STAT gets - a pair
                                 *   op is still "what happened", not a second
                                 *   shape of answer. MOVE refuses a crossing
                                 *   with KODE_DENIED before touching either
                                 *   path; COPY is how a file crosses, and its
                                 *   one answer comes at the END of a paced
                                 *   transfer rather than at its start.       */
    STORAGE_TEXT_REQ_SIG,       /**< AO_Storage: put text in a file
                                 *   (StorageTextEvt). Its own event and its
                                 *   own signal rather than one more member of
                                 *   storage_path_op_t - `write` carries a
                                 *   second string a STAT/MKDIR/UNLINK/RMDIR
                                 *   request has no use for, the same reason
                                 *   STORAGE_PAIR_REQ_SIG above is not one of
                                 *   them either. Truncates unless `append` is
                                 *   set, in which the bytes go on the end of
                                 *   what is already there. Answered once to
                                 *   StorageTextEvt.who with the same
                                 *   STORAGE_PATH_RSP_SIG a STAT or a mutation
                                 *   gets - `rc` alone says whether it landed,
                                 *   `exists`/`is_dir`/`size` carry nothing,
                                 *   the mkdir/rm/rmdir/mv shape again.        */
    FILE_STATUS_RSP_SIG,        /**< Answers kode_sys_file_status(), to the
                                 *   asking AO (FileRspEvt).                   */
    FILE_LIST_END_SIG,          /**< Ends kode_sys_file_list()'s page of
                                 *   STORAGE_ENTRY_SIG, to the asking AO
                                 *   (FileRspEvt).                             */
    FILE_FOLDER_RSP_SIG,        /**< Answers kode_sys_file_create_folder(), to
                                 *   the asking AO (FileRspEvt).               */
    FILE_DELETE_RSP_SIG,        /**< Answers kode_sys_file_delete(), to the
                                 *   asking AO (FileRspEvt).                   */
    FILE_RENAME_RSP_SIG,        /**< Answers kode_sys_file_rename(), to the
                                 *   asking AO (FileRspEvt).                   */
    FILE_READ_RSP_SIG,          /**< Answers kode_sys_file_read(), to the
                                 *   asking AO (FileRspEvt).                   */
    FILE_WRITE_RSP_SIG,         /**< Answers kode_sys_file_write(), to the
                                 *   asking AO (FileRspEvt).                   */
    STORAGE_RECV_REQ_SIG,       /**< AO_Storage: a file is about to arrive on
                                 *   the console (StorageRecvEvt). Open the
                                 *   destination, then ask AO_Log for raw mode
                                 *   - the same conversation APP_RECV_REQ_SIG
                                 *   starts for the apps region, with a path
                                 *   where that one has a region. Answered as
                                 *   the RECV_* protocol, POSTED to
                                 *   StorageRecvEvt.who rather than published;
                                 *   see RECV_READY_SIG for why the two
                                 *   receivers differ there.                   */
    STORAGE_TEST_NEXT_SIG,      /**< AO_Storage private QTimeEvt: the next
                                 *   step of the card test. One chunk per
                                 *   step, for the reason STORAGE_WRITE_NEXT_
                                 *   SIG exists.                              */
    STORAGE_CAT_NEXT_SIG,       /**< AO_Storage private QTimeEvt: the next
                                 *   batch of lines out of the buffer a `cat`
                                 *   already read. One batch of at most
                                 *   STORAGE_CAT_LINES_PER_STEP per step, for
                                 *   the same reason STORAGE_WRITE_NEXT_SIG
                                 *   exists.                                  */
    STORAGE_COPY_NEXT_SIG,      /**< AO_Storage private QTimeEvt: the next
                                 *   chunk of the `cp` in progress. One 32 KB
                                 *   chunk per step, for the same reason
                                 *   STORAGE_WRITE_NEXT_SIG exists - and here
                                 *   the reason bites hardest, since a copy
                                 *   moves the bytes twice and a whole file
                                 *   inside one step would hold AO_Gui,
                                 *   AO_Radio and AO_Net off for all of it.   */
    STORAGE_RECV_IDLE_SIG,      /**< AO_Storage private QTimeEvt: no chunk of
                                 *   the arriving file for KODE_RECV_IDLE_MS -
                                 *   the host is gone. AO_AppMgr's
                                 *   APP_RECV_IDLE_SIG, one receiver over.
                                 *   Without it a host that stops mid-transfer
                                 *   leaves the console wedged in raw mode
                                 *   holding a half-written file.             */
    STORAGE_SDJOB_DONE_SIG,     /**< storage_sd -> AO_Storage: a call to the
                                 *   card returned (KqpWorkerDoneEvt: its
                                 *   sequence number and status; the value is
                                 *   what the call measured or found - the
                                 *   microseconds a sector transfer took, the
                                 *   megabytes of a new volume). The AO drops
                                 *   the guard, goes on with whatever the
                                 *   call was for and takes the requests it
                                 *   held back.                               */
    STORAGE_SDJOB_TO_SIG,       /**< AO_Storage private QTimeEvt: a call to
                                 *   the card has not returned in time, so
                                 *   whoever waits for it is told it failed.
                                 *   The guard stays up until the call does
                                 *   return.                                  */

    BOOT_OS_SWITCH_SIG,         /**< AO_Boot: point otadata at the inactive
                                 *   kodeOS slot, reset.                      */
    BOOT_APP_RUN_SIG,           /**< AO_Boot: kdrv_apps_req_set(offset, size,
                                 *   NEW or ALWAYS as BootRunEvt.mode says),
                                 *   reset, so the bootloader jumps there.    */
    BOOT_INFO_REQ_SIG,          /**< AO_Boot: log the boot state.             */
    BOOT_EXIT_REQ_SIG,          /**< AO_Boot: reset. In an app this is the way
                                 *   home; otadata is deliberately untouched. */
    BOOT_HOME_DEFER_SIG,        /**< AO_Boot: the app asking for a moment
                                 *   before the hold resets it. Posted by
                                 *   kode_sys_home_defer() and honoured only while
                                 *   the grace is running.                    */
    BOOT_HOME_NOW_SIG,          /**< AO_Boot: the app is ready - go home now.
                                 *   Posted by kode_sys_home_now().              */
    BOOT_HOME_GRACE_SIG,        /**< AO_Boot private QTimeEvt: the grace is
                                 *   over (or, once extended, the cap is).    */
    BOOT_RADIO_PARKED_SIG,      /**< AO_Boot private (direct post, from
                                 *   boot_radio_parked()): AO_Radio has done
                                 *   with the C5 for the jump - parked, or
                                 *   given up on it. One per
                                 *   RADIO_PARK_REQ_SIG.                      */
    BOOT_NET_LEFT_SIG,          /**< AO_Boot private (direct post, from
                                 *   boot_net_left()): AO_Net has left the
                                 *   Kode channel for the jump - the client
                                 *   stopped cleanly, or there was none. One
                                 *   per NET_LEAVE_REQ_SIG.                   */
    BOOT_LEAVE_TMO_SIG,         /**< AO_Boot private QTimeEvt: the step of the
                                 *   leave that is running ran out of time.   */
    FACTORY_RESET_REQ_SIG,      /**< AO_Boot: forget everything the board
                                 *   remembers but its identity and the station
                                 *   lock (FactoryResetReqEvt). Posted by
                                 *   boot_factory_reset_request(); one
                                 *   FACTORY_RESET_RSP_SIG to who, always.    */
    FACTORY_RESET_RSP_SIG,      /**< To the asker: the step that stopped the
                                 *   reset, or that it is done
                                 *   (FactoryResetRspEvt).                    */
    BOOT_RESET_STEP_SIG,        /**< AO_Boot private (self-post): erase the
                                 *   next NVS namespace of the reset.         */
    BOOT_RESET_TMO_SIG,         /**< AO_Boot private QTimeEvt: the step of the
                                 *   reset that is running ran out of time.   */
    BOOT_FIRST_FRAME_SIG,       /**< AO_Boot: the interface's first frame has
                                 *   reached the glass (boot_first_frame()).  */
    BOOT_HEALTHY_TMO_SIG,       /**< AO_Boot private QTimeEvt: the board has
                                 *   been up long enough to confirm itself, and
                                 *   then long enough to forget its crashes.  */
    BOOT_RESET_AGAIN_SIG,       /**< AO_Boot private QTimeEvt: ask AO_Radio
                                 *   again for the wipe it refused as busy.   */
    LOG_LINE_SIG,               /**< AO_Log: put LogEvt.line on the console.  */
    CONSOLE_TIMEOUT_SIG,        /**< AO_Console private QTimeEvt: the command
                                 *   in flight never got its closing answer.  */
    CONSOLE_SLICE_SIG,          /**< AO_Console private QTimeEvt: emit the
                                 *   next slice of whatever is producing
                                 *   records - the `help` listing, or the
                                 *   base64 payload of `screenshot` and
                                 *   `file_get`. It is a timer and not a
                                 *   self-post because a self-post would keep
                                 *   this AO's queue non-empty, and
                                 *   AO_Console outranks AO_Log - the
                                 *   listing would outrun the writer exactly
                                 *   as it did before.                       */
    CONSOLE_WATCH_SIG,          /**< AO_Console private QTimeEvt: a followed
                                 *   variable's period ran out - read the row
                                 *   and put its record on the wire. One
                                 *   signal for KODE_VARIABLE_WATCH_MAX timers;
                                 *   the handler tells them apart by which
                                 *   timer arrived, since a QTimeEvt is
                                 *   delivered as itself.                    */
    CONSOLE_BYTE_SIG,           /**< AO_Log: a byte arrived (ConsoleByteEvt). */
    CONSOLE_RAW_REQ_SIG,        /**< AO_Log: switch the input stream to raw
                                 *   and deliver it to ConsoleRawReqEvt.who
                                 *   (ConsoleRawReqEvt). AO_Log owns the
                                 *   console device; nobody else arms this.   */
    CONSOLE_RAW_RSP_SIG,        /**< To the requester: whether raw mode is
                                 *   armed (ConsoleRawRspEvt).                */
    CONSOLE_RAW_END_SIG,        /**< AO_Log: abandon raw mode now - the
                                 *   transfer died before its last byte. A
                                 *   completed one needs no such event: the
                                 *   reader stops itself at the count.        */
    LOG_LEVEL_SET_SIG,          /**< AO_Log: apply a log level, and store it
                                 *   when it is the global default
                                 *   (LogLevelEvt). AO_Log owns the console
                                 *   device and the `log` key; `log_level`
                                 *   asks it rather than calling klog.        */
    DISPLAY_FLUSH_SIG,          /**< AO_Display: push FlushEvt to the panel.  */
    DISPLAY_BRIGHTNESS_SIG,     /**< AO_Display: set BrightnessEvt.percent.   */
    DISPLAY_STATE_REQ_SIG,      /**< AO_Display: publish DISPLAY_STATE_SIG
                                 *   again, for whoever started too late to
                                 *   have heard the first one.                */
    DISPLAY_CAPTURE_REQ_SIG,    /**< AO_Display: copy the frame on the glass
                                 *   and say how big it is
                                 *   (DisplayCaptureReqEvt).                  */
    DISPLAY_CAPTURE_RSP_SIG,    /**< To the requester (DisplayCaptureRspEvt).  */
    DISPLAY_CAPTURE_END_SIG,    /**< AO_Display: the capture is done with the
                                 *   buffer; let the flush path write it
                                 *   again.                                   */
    DISPLAY_MIRROR_START_SIG,   /**< AO_Display: begin streaming the panel
                                 *   (MirrorStartEvt). The next frame is
                                 *   always the whole panel.                  */
    DISPLAY_MIRROR_STOP_SIG,    /**< AO_Display: stop streaming. Disarms the
                                 *   mirror's own tick; a mirror nobody is
                                 *   watching should not hold the codec.      */
    DISPLAY_MIRROR_ACK_SIG,     /**< AO_Display: the host has received up to
                                 *   MirrorAckEvt.seq; releases credit. Also
                                 *   what revives a paused mirror - see
                                 *   display_ao.c's mirror_pause().           */
    DISPLAY_MIRROR_INFO_REQ_SIG, /**< AO_Display: publish DISPLAY_MIRROR_INFO_
                                 *   SIG with the mirror's current state -
                                 *   the DISPLAY_STATE_REQ_SIG pattern.       */
    TOUCH_POLL_SIG,             /**< AO_Touch private polling QTimeEvt.       */
    TOUCH_LOOK_SIG,             /**< AO_Touch private QTimeEvt, one-shot:
                                 *   look again for a controller that did
                                 *   not answer the bring-up.               */
    TOUCH_FIELD_REQ_SIG,        /**< AO_Touch: read, write or dump one field
                                 *   of the CST820 (TouchFieldReqEvt).
                                 *   Answered to TouchFieldReqEvt.who with
                                 *   TOUCH_FIELD_RSP_SIG - posted, not
                                 *   published, the expander's shape: the
                                 *   console asks and nobody else wants its
                                 *   answers.                                 */
    TOUCH_FIELD_RSP_SIG,        /**< To the requester (TouchFieldEvt).         */
    TOUCH_TEST_REQ_SIG,         /**< AO_Touch: run the guided touch test -
                                 *   `test component=tp`. Answered step by
                                 *   step with published TOUCH_TEST_SIG and
                                 *   at the end with TEST_RESULT_SIG.        */
    TOUCH_TEST_TMO_SIG,         /**< AO_Touch private one-shot QTimeEvt: the
                                 *   operator did not tap the target a step
                                 *   showed in time.                          */
    DISPLAY_MIRROR_TICK_SIG,    /**< AO_Display private: one-shot, re-armed
                                 *   after every pass - the mirror's own
                                 *   pacer, paired with the credit window
                                 *   rather than a mechanism of its own. See
                                 *   display_ao.c's mirror_pass().            */
    KCLI_INJ_TMO_SIG,           /**< AO_Console private: an injected gesture
                                 *   has a sample due - a tap's release, a
                                 *   swipe's next step. A tap cannot publish
                                 *   both samples in one step - see h_tap. */
    BUTTONS_DUMP_REQ_SIG,       /**< AO_Buttons: log every button's state now. */
    BUTTONS_SETTLE_SIG,         /**< AO_Buttons private one-shot QTimeEvt: a
                                 *   change was seen; take the confirming
                                 *   sample now.                              */
    BUTTONS_TOP_INT_SIG,        /**< AO_Buttons private: GPIO35 moved. Posted
                                 *   from the pin's interrupt through
                                 *   kqp_post_from_isr(), never by an AO.     */
    BUTTONS_TEST_REQ_SIG,       /**< AO_Buttons: run the guided button test -
                                 *   `test component=buttons`. Answered step
                                 *   by step with published BTN_TEST_SIG and
                                 *   at the end with TEST_RESULT_SIG.        */
    BUTTONS_TEST_TMO_SIG,       /**< AO_Buttons private one-shot QTimeEvt: the
                                 *   operator did not press the button a step
                                 *   asked for in time, or did not let it go
                                 *   within KODE_BUTTONS_TEST_RELEASE_MS.     */
    BUTTONS_HOLD_TMO_SIG,       /**< AO_Buttons private one-shot QTimeEvt: the
                                 *   top or the bottom button has been down
                                 *   for KODE_BUTTON_HOLD_MS.                */
    BUTTONS_ROTATE_SIG,         /**< AO_Buttons: turn the pad by a number of
                                 *   quarter turns (ButtonsRotateEvt), from
                                 *   buttons_rotation_request().              */
    BUTTONS_PAD_IDLE_SIG,       /**< AO_Buttons private: an injected release
                                 *   left the pad idle; a turn that waited for
                                 *   it may apply now.                        */
    IOEXP_INT_SIG,              /**< AO_IoExp private: the TCA6408A's INT line
                                 *   went low - an input changed since the
                                 *   register was last read. Posted from the
                                 *   GPIO6 interrupt through
                                 *   kqp_post_from_isr(); reading the input
                                 *   register is what clears it.              */
    IOEXP_RETRY_SIG,            /**< AO_IoExp private one-shot QTimeEvt: INT
                                 *   fired and the input register did not
                                 *   read, so the line was not re-armed on a
                                 *   line still low; read again now
                                 *   (KODE_IOEXP_RETRY_MS).                   */
    IOEXP_READ_REQ_SIG,         /**< AO_IoExp: read the input register now and
                                 *   publish IOEXP_INPUT_SIG, changed or not.  */
    IOEXP_FIELD_REQ_SIG,        /**< AO_IoExp: read, write or dump one field
                                 *   of the expander (IoexpFieldReqEvt).
                                 *   Answered to IoexpFieldReqEvt.who with
                                 *   IOEXP_FIELD_RSP_SIG - posted, not
                                 *   published, because AO_Display and
                                 *   AO_Console both ask and neither wants
                                 *   the other's answers.                     */
    IOEXP_FIELD_RSP_SIG,        /**< To the requester (IoexpFieldEvt).         */
    IOEXP_TEST_REQ_SIG,         /**< AO_IoExp: `test component=ioexp`;
                                 *   answers a published TEST_RESULT_SIG.     */
    IR_SEND_REQ_SIG,            /**< AO_Ir: transmit one NEC frame (IrSendEvt).
                                 *   A frame of raw symbols is
                                 *   IR_SEND_RAW_REQ_SIG.                    */
    IR_SEND_RAW_REQ_SIG,        /**< AO_Ir: transmit the frame the requester
                                 *   lends (IrSendRawEvt), on its carrier,
                                 *   and answer with the driver's status.
                                 *   Posted, the IR_CAPTURE_REQ_SIG shape.   */
    IR_CAPTURE_REQ_SIG,         /**< AO_Ir: fill the requester's buffer with
                                 *   the next frame heard (IrCaptureEvt).
                                 *   Posted, not published: only the asker
                                 *   wants the answer, the IOEXP_FIELD_REQ_SIG
                                 *   shape.                                  */
    IR_TEST_REQ_SIG,            /**< AO_Ir: `test component=ir`; answers a
                                 *   published TEST_RESULT_SIG, the IOEXP_
                                 *   TEST_REQ_SIG shape. No payload.         */
    IR_INFO_REQ_SIG,            /**< AO_Ir: the console's `ir` - the block's
                                 *   own status (IrInfoReqEvt). Answered to
                                 *   IrInfoReqEvt.who with IR_INFO_RSP_SIG -
                                 *   posted, not published, the
                                 *   IOEXP_FIELD_REQ_SIG shape.               */
    IR_INFO_RSP_SIG,            /**< To the requester (IrInfoEvt).           */
    IR_LAST_REQ_SIG,            /**< AO_Ir: the last frame it heard, written
                                 *   into the requester's memory and
                                 *   signalled on a semaphore (IrLastEvt).
                                 *   The IR_CAPTURE_REQ_SIG shape rather than
                                 *   IR_INFO_REQ_SIG's, because the caller is
                                 *   a blocked task and not an Active Object
                                 *   with a queue to answer into.            */
    IR_SEND_RAW_RSP_SIG,        /**< Answers kode_sys_ir_send_raw(), to the
                                 *   asking AO (StatusRspEvt).              */
    IR_CAPTURE_RSP_SIG,         /**< Answers kode_sys_ir_capture(), to the
                                 *   asking AO (IrCaptureRspEvt).           */
    IR_LAST_RSP_SIG,            /**< Answers kode_sys_ir_last(), to the
                                 *   asking AO (IrLastRspEvt).              */
    IR_RX_RDY_SIG,              /**< AO_Ir private: a capture is waiting in
                                 *   the driver. Posted from the receive
                                 *   interrupt, which does this and nothing
                                 *   else. Distinct from IR_RX_SIG on
                                 *   purpose: that one is what AO_Ir tells
                                 *   the world about a frame, this one is
                                 *   how it hears of one.                    */
    IR_ECHO_SIG,                /**< AO_Ir private QTimeEvt: the echo
                                 *   suppression window closed, so what the
                                 *   receiver hears next belongs to somebody
                                 *   else again.                             */
    IR_CAPTURE_TO_SIG,          /**< AO_Ir private QTimeEvt: the pending
                                 *   capture request reached the deadline its
                                 *   requester asked for, so it is answered
                                 *   with zero symbols. Without it AO_Ir would
                                 *   hold the requester's pointers after the
                                 *   requester stopped waiting - see
                                 *   IrCaptureEvt.                           */
    IR_TEST_TO_SIG,             /**< AO_Ir private QTimeEvt: one attempt of
                                 *   the production row waited its
                                 *   KODE_IR_TEST_ECHO_MS and heard nothing.
                                 *   Distinct from IR_ECHO_SIG because the
                                 *   two run at once and mean opposite
                                 *   things: that one ends a silence this AO
                                 *   imposed, this one ends a silence it was
                                 *   waiting to be broken.                   */
    IR_JOB_DONE_SIG,            /**< ir_worker -> AO_Ir: a call to the C5
                                 *   returned (KqpWorkerDoneEvt: its sequence
                                 *   number and status; for a line reading,
                                 *   the value is the level). The AO drops
                                 *   the guard, finishes whatever the call
                                 *   was for and takes the requests it held
                                 *   back.                                   */
    IR_JOB_TO_SIG,              /**< AO_Ir private QTimeEvt: a call to the C5
                                 *   has not returned in time, so whoever
                                 *   waits for it is told it failed. The
                                 *   guard stays up until the call does
                                 *   return.                                 */
    IR_LINK_TO_SIG,             /**< AO_Ir private one-shot QTimeEvt, armed
                                 *   again by each one taken while requests
                                 *   wait for the C5 link: each request past
                                 *   its own budget is answered
                                 *   KODE_TIMEOUT.                           */
    NFC_READ_REQ_SIG,           /**< AO_Nfc: energise, and answer with the
                                 *   first tag activated inside the window
                                 *   (NfcReadEvt): to a blocked task on its
                                 *   semaphore, or to an asking AO
                                 *   (kode_sys_nfc_read_tag()) with
                                 *   NFC_READ_RSP_SIG.                      */
    NFC_NDEF_REQ_SIG,           /**< AO_Nfc: read or write an NDEF message on
                                 *   the first tag activated (NfcNdefEvt).
                                 *   One signal for both directions, and
                                 *   NfcNdefEvt.write says which: they differ
                                 *   in one field, not in how AO_Nfc
                                 *   discovers, waits or gives up.          */
    NFC_NDEF_RSP_SIG,           /**< To the requester (NfcNdefRspEvt): the
                                 *   answer to an NfcNdefEvt that named a
                                 *   @c who, the console or an asking AO
                                 *   (kode_sys_nfc_ndef_read() and the
                                 *   writes). A blocked task uses the buffer
                                 *   and the semaphore instead.             */
    NFC_INFO_REQ_SIG,           /**< AO_Nfc: the console's `nfc` - the
                                 *   reader's own status (NfcInfoReqEvt).
                                 *   Answered to NfcInfoReqEvt.who with
                                 *   NFC_INFO_RSP_SIG, the IR_INFO_REQ_SIG
                                 *   shape.                                 */
    NFC_INFO_RSP_SIG,           /**< To the requester (NfcInfoEvt).          */
    NFC_KEYS_REQ_SIG,           /**< AO_Nfc: sweep the public Mifare key
                                 *   dictionary against one block and report
                                 *   which key opens it. NfcKeysReqEvt. A
                                 *   deliberate, paced operation - the batches
                                 *   run one per NFC_TICK so the AO's step
                                 *   stays short.                           */
    NFC_KEYS_RSP_SIG,           /**< To the requester (NfcKeysRspEvt).        */
    NFC_DUMP_REQ_SIG,           /**< AO_Nfc: read every sector a Mifare
                                 *   Classic will open and stream it. Paced
                                 *   one sector per tick, like the sweep.
                                 *   NfcDumpReqEvt.                          */
    NFC_DUMP_RSP_SIG,           /**< To the requester (NfcDumpRspEvt): the
                                 *   dump is finished.                       */
    NFC_TEST_REQ_SIG,           /**< AO_Nfc: `test component=nfc`; answers a
                                 *   published TEST_RESULT_SIG, the IR_TEST_
                                 *   REQ_SIG shape. No payload.             */
    NFC_TICK_SIG,               /**< AO_Nfc private one-shot QTimeEvt: pump
                                 *   rfalNfcWorker() once and arm again.
                                 *   One-shot rather than periodic, for
                                 *   AO_Gui's reason: a periodic timer would
                                 *   keep delivering ticks into an AO with
                                 *   nothing left to pump.                  */
    NFC_DEADLINE_SIG,           /**< AO_Nfc private one-shot QTimeEvt: the
                                 *   request in flight reached the window its
                                 *   requester asked for, so it is answered
                                 *   empty and the field goes down. Distinct
                                 *   from NFC_TICK_SIG because the two run at
                                 *   once and mean opposite things: that one
                                 *   advances the request, this one ends it. */
    NFC_CERT_REQ_SIG,           /**< AO_Nfc: raise, switch or drop the held
                                 *   maximum-drive certification field
                                 *   (NfcCertEvt). Only posted in a build with
                                 *   CONFIG_KODEOS_NFC_CERT - the FCC 15.225
                                 *   bench path, off in production. The field
                                 *   is held with no window and no tick until a
                                 *   mode-0 request drops it.               */
    NFC_FIELD_REQ_SIG,          /**< AO_Nfc: listen for a window with the
                                 *   reader's own field off, and describe the
                                 *   field somebody else put on the antenna
                                 *   (NfcFieldEvt). Stepped one burst per
                                 *   NFC_TICK, like discovery. Answered on the
                                 *   requester's semaphore, or posted to
                                 *   NfcFieldEvt.to as NFC_FIELD_RSP_SIG, or
                                 *   both.                                   */
    NFC_FIELD_RSP_SIG,          /**< To the requester (NfcFieldRspEvt): the
                                 *   window's statistics by value, the
                                 *   console's shape - an AO cannot wait on a
                                 *   semaphore, so it is told instead.      */
    NFC_EMU_REQ_SIG,            /**< AO_Nfc: present the Dot as a passive NFC-A
                                 *   card, or stop (NfcEmuReqEvt). Held until a
                                 *   stop, like the certification carrier, and
                                 *   answered with the outcome on the
                                 *   requester's semaphore or posted to its
                                 *   @c who as NFC_EMU_RSP_SIG.              */
    NFC_EMU_RSP_SIG,            /**< To the console (NfcEmuRspEvt): a start or
                                 *   stop it asked for finished, and how - the
                                 *   NFC_NDEF_RSP_SIG shape, so the console AO
                                 *   never blocks. (The published edge is
                                 *   NFC_EMU_SIG, up in the pub/sub range.)
                                 *   Also what kode_sys_nfc_emulate_start()
                                 *   and _stop() are answered with.            */
    NFC_READ_RSP_SIG,           /**< Answers kode_sys_nfc_read_tag(), to the
                                 *   asking AO (NfcReadRspEvt).              */
    NFC_CERT_RSP_SIG,           /**< Answers kode_cert_nfc_carrier(), to the
                                 *   asking AO (StatusRspEvt).               */
    RFID_READ_REQ_SIG,          /**< AO_Rfid: energise and report the first
                                 *   EM4100 ID decoded inside the window
                                 *   (RfidReqEvt): to a blocked task on its
                                 *   semaphore, or with RFID_OP_RSP_SIG to the
                                 *   console or an asking AO.               */
    RFID_WRITE_REQ_SIG,         /**< AO_Rfid: clone a 5-byte ID onto a T5577
                                 *   (RfidWriteReqEvt).                      */
    RFID_INFO_REQ_SIG,          /**< AO_Rfid: the console's `rfid`. Answered
                                 *   to RfidInfoReqEvt.who with
                                 *   RFID_INFO_RSP_SIG.                      */
    RFID_INFO_RSP_SIG,          /**< To the requester (RfidInfoEvt).         */
    RFID_OP_RSP_SIG,            /**< AO_Rfid -> the asker: a read or a write
                                 *   it asked for finished (RfidOpRspEvt). The
                                 *   console's `rfid_read` and `rfid_write`,
                                 *   and kode_sys_rfid_read() and _write(),
                                 *   are answered with it, the NFC_NDEF_RSP_SIG
                                 *   shape, so an AO never blocks on a field
                                 *   window. A blocked task's read takes the
                                 *   semaphore instead.                      */
    RFID_TEST_REQ_SIG,          /**< AO_Rfid: `test component=rfid`; answers
                                 *   a published TEST_RESULT_SIG. No payload. */
    RFID_WORKER_DONE_SIG,       /**< rfid_worker -> AO_Rfid: the job finished
                                 *   (KqpWorkerDoneEvt: its sequence number
                                 *   and status). The AO drops the field,
                                 *   takes requests again, and answers the
                                 *   requester unless a deadline already has;
                                 *   what a read decoded is in the AO's own
                                 *   job.                                  */
    RFID_DEADLINE_SIG,          /**< AO_Rfid private one-shot QTimeEvt: the
                                 *   request's window is over.              */
    RFID_CERT_REQ_SIG,          /**< AO_Rfid: raise, switch or drop the held
                                 *   maximum-drive 125 kHz certification
                                 *   carrier (RfidCertEvt). Only posted in a
                                 *   build with CONFIG_KODEOS_NFC_CERT - the
                                 *   FCC 15.209 bench path, off in production.
                                 *   The field is held with no window until a
                                 *   mode-0 request drops it.               */
    RFID_FIELD_REQ_SIG,         /**< AO_Rfid: read the demod line for a window
                                 *   with the carrier OFF, and describe it and
                                 *   optionally copy the waveform out
                                 *   (RfidFieldEvt). The worker does the
                                 *   reading, as it does a tag read's.
                                 *   Answered on the requester's semaphore, or
                                 *   posted to RfidFieldEvt.to as
                                 *   RFID_FIELD_RSP_SIG, or both.           */
    RFID_FIELD_RSP_SIG,         /**< To the requester (RfidFieldRspEvt): the
                                 *   window's statistics by value. The
                                 *   waveform never travels on this event; it
                                 *   goes only into the requester's own
                                 *   buffer.                                */
    RFID_EMU_REQ_SIG,           /**< AO_Rfid: play an EM4100 ID as a tag
                                 *   (RfidEmuEvt).                           */
    RFID_EMU_STOP_REQ_SIG,      /**< AO_Rfid: end the emulation (RfidEmuEvt). */
    RFID_EMU_RSP_SIG,           /**< Answers kode_sys_rfid_emulate_start() and
                                 *   _stop(), to the asking AO (RfidEmuRspEvt). */
    RFID_CERT_RSP_SIG,          /**< Answers kode_cert_rfid_carrier(), to the
                                 *   asking AO (StatusRspEvt).               */
    AUDIO_TONE_REQ_SIG,        /**< AO_Audio: emit one tone (AudioToneEvt).
                                 *   The frequency is snapped to the nearest
                                 *   detector bin, and the snapped value is
                                 *   what is reported back.                 */
    AUDIO_FIELD_REQ_SIG,        /**< AO_Audio: read, write or dump one field
                                 *   of the codec (AudioFieldReqEvt). The
                                 *   FIELD_OP_* verbs, like every other field
                                 *   table. Answered with AUDIO_FIELD_SIG.  */
    AUDIO_INFO_REQ_SIG,         /**< AO_Audio: what the audio path is and
                                 *   what it is set to (AudioInfoReqEvt).
                                 *   Answered inside the step that receives
                                 *   it, through the requester's own memory -
                                 *   the IR_LAST_REQ_SIG arrangement.      */
    AUDIO_INFO_RSP_SIG,         /**< Answers kode_sys_audio_info_read(), to
                                 *   the asking AO (AudioInfoRspEvt).       */
    AUDIO_TEST_REQ_SIG,         /**< AO_Audio: `test component=audio`;
                                 *   answers a published TEST_RESULT_SIG,
                                 *   the IR_TEST_REQ_SIG shape. No payload. */
    AUDIO_REC_FILE_REQ_SIG,     /**< AO_Audio: record for a while and put the
                                 *   result in a file (AudioFileReqEvt). The
                                 *   path arrives ALREADY RESOLVED - the
                                 *   console owns resolution and AO_Audio
                                 *   owns neither the filesystem nor the
                                 *   current directory.                    */
    AUDIO_PLAY_REQ_SIG,         /**< AO_Audio: play the frames in an
                                 *   AudioBufEvt, then wake the blocked
                                 *   waiter or answer the asking AO.        */
    AUDIO_DONE_SIG,             /**< Answers kode_sys_audio_play(), to the
                                 *   asking AO once the sound was heard or
                                 *   refused (AudioDoneEvt) - the only end
                                 *   of a play an Active Object can wait
                                 *   for, since it may not sleep on the
                                 *   semaphore AudioBufEvt carries.         */
    AUDIO_REC_REQ_SIG,          /**< AO_Audio: fill an AudioBufEvt's buffer
                                 *   and wake the waiter. Same contract.    */
    AUDIO_REC_RSP_SIG,          /**< Answers kode_sys_audio_record(), to the
                                 *   asking AO (AudioDoneEvt, @c frames
                                 *   recorded).                             */
    AUDIO_STREAM_OPEN_REQ_SIG,  /**< AO_Audio: open the stream and wake the
                                 *   waiter once it is playing (AudioBufEvt,
                                 *   no buffer). Same contract.             */
    AUDIO_STREAM_OPEN_RSP_SIG,  /**< Answers kode_sys_audio_stream_open(), to
                                 *   the asking AO (AudioDoneEvt).          */
    AUDIO_STREAM_CLOSE_REQ_SIG, /**< AO_Audio: end the stream and wake the
                                 *   waiter once what it held has been
                                 *   heard (AudioBufEvt, no buffer). Same
                                 *   contract.                              */
    AUDIO_STREAM_CLOSE_RSP_SIG, /**< Answers kode_sys_audio_stream_close(), to
                                 *   the asking AO (AudioDoneEvt).          */
    AUDIO_REC_STOP_REQ_SIG,     /**< AO_Audio: end the samples recording in
                                 *   flight at its next block, or at the
                                 *   first one if its job is not submitted
                                 *   yet. Ignored when none runs, and for a
                                 *   file recording. A static event, one
                                 *   queued at a time, no payload.          */
    AUDIO_SETTING_REQ_SIG,      /**< AO_Audio: write the latest volume, mic
                                 *   gain or mute value (AudioSettingReqEvt).
                                 *   One queued per setting. Answered with
                                 *   AUDIO_FIELD_SIG.                       */
    AUDIO_JOB_SIG,              /**< AO_Audio private: the codec worker
                                 *   finished a job (KqpWorkerDoneEvt: its
                                 *   sequence number and status). Posted
                                 *   from the worker's thread, which is the
                                 *   whole reason the worker is allowed to
                                 *   share this AO's priority.              */
    AUDIO_GUARD_SIG,            /**< AO_Audio private one-shot QTimeEvt: a job
                                 *   handed to the worker never answered. The
                                 *   only timer this AO has left: there was an
                                 *   AUDIO_IDLE_SIG beside it that closed the
                                 *   codec after a quiet spell, and the close
                                 *   it existed for was audible (2026-09-16 -
                                 *   see AO_Audio.md).                      */
    IMU_READ_REQ_SIG,           /**< AO_Imu: one sample into the caller's own
                                 *   struct (ImuReadEvt), answered inside the
                                 *   step that receives it; the blocking facade's. */
    IMU_SAMPLE_RSP_SIG,         /**< Answers kode_sys_imu_read(), to the
                                 *   asking AO (ImuSampleRspEvt).           */
    IMU_CAPTURE_REQ_SIG,        /**< AO_Imu: arm the FIFO for the witness
                                 *   (ImuCaptureReqEvt: who). Answered to
                                 *   @c who with IMU_ARMED_SIG.             */
    IMU_ARMED_SIG,              /**< To whoever asked for the capture
                                 *   (ImuArmedEvt).                         */
    IMU_DRAIN_REQ_SIG,          /**< AO_Imu: drain what the FIFO holds and
                                 *   measure it at @c hz (ImuDrainReqEvt).
                                 *   Answered to @c who with IMU_ENERGY_SIG. */
    IMU_ENERGY_SIG,             /**< To whoever asked for the drain
                                 *   (ImuEnergyEvt).                        */
    IMU_TEST_REQ_SIG,           /**< AO_Imu: `test component=imu`; answers a
                                 *   published TEST_RESULT_SIG, the
                                 *   AUDIO_TEST_REQ_SIG shape. No payload.  */
    IMU_WAIT_SIG,               /**< AO_Imu private one-shot QTimeEvt: a
                                 *   self-test's poll, a chunk of a witness
                                 *   window, the gyroscope's turn-on after
                                 *   the row, or a capture nobody drained. A
                                 *   timer rather than a self-post between
                                 *   chunks, so the priorities below AO_Imu
                                 *   get the CPU while a window comes out.  */
    IMU_REST_SIG,               /**< AO_Imu private one-shot QTimeEvt: a
                                 *   part that woke for a request has had
                                 *   KODE_SYS_IMU_REST_AFTER_MS to be asked
                                 *   again, and goes back to standby if it
                                 *   was not. This one is the QMI8658A's.   */
    MAG_REST_SIG,               /**< AO_Imu private one-shot QTimeEvt: the
                                 *   same, for the QMC5883P.                */
    MAG_READ_REQ_SIG,           /**< AO_Imu: one magnetometer sample into the
                                 *   caller's own int32_t[3] (MagReadEvt),
                                 *   answered inside the step that receives
                                 *   it; the blocking facade's.             */
    MAG_SAMPLE_RSP_SIG,         /**< Answers kode_sys_mag_read(), to the
                                 *   asking AO (MagSampleRspEvt).           */
    MAG_TEST_REQ_SIG,           /**< AO_Imu: `test component=mag`; answers a
                                 *   published TEST_RESULT_SIG named "mag",
                                 *   the IMU_TEST_REQ_SIG shape. No payload. */
    MAG_CAL_REQ_SIG,            /**< AO_Imu: read, store or clear the
                                 *   magnetometer's calibration (MagCalEvt),
                                 *   answered inside the step that receives
                                 *   it; the blocking facade's.             */
    MAG_CAL_RSP_SIG,            /**< Answers kode_sys_mag_cal_get(), _set()
                                 *   and _clear(), to the asking AO
                                 *   (MagCalRspEvt).                        */
    HAPTIC_PLAY_REQ_SIG,        /**< AO_Haptic: a pattern of on/off segments,
                                 *   or a hold (HapticPlayEvt).             */
    HAPTIC_STOP_REQ_SIG,        /**< AO_Haptic: end the pattern or the hold
                                 *   early. A static event, no payload.     */
    HAPTIC_FIELD_REQ_SIG,       /**< AO_Haptic: read, write or dump one field
                                 *   of the driver (HapticFieldReqEvt).
                                 *   Answered with HAPTIC_FIELD_SIG.        */
    HAPTIC_TEST_REQ_SIG,        /**< AO_Haptic: `test component=haptic`;
                                 *   answers a published TEST_RESULT_SIG. No
                                 *   payload.                               */
    HAPTIC_TICK_SIG,            /**< AO_Haptic private periodic QTimeEvt:
                                 *   feed the RTP FIFO.                     */
    HAPTIC_TAIL_SIG,            /**< AO_Haptic private one-shot QTimeEvt: the
                                 *   chip has played what it was left with.
                                 *   Distinct from HAPTIC_TICK_SIG because
                                 *   that one keeps the buzz alive and this
                                 *   one ends it.                           */
    HAPTIC_RETRY_SIG,           /**< AO_Haptic private one-shot QTimeEvt: look
                                 *   again for a chip that did not answer at
                                 *   boot.                                    */
    HAPTIC_WAIT_SIG,            /**< AO_Haptic private one-shot QTimeEvt: the
                                 *   row's stillness window, or the deadline
                                 *   on a reply from AO_Imu.                */
    POWER_POLL_SIG,             /**< AO_Power private one-shot QTimeEvt, armed
                                 *   again by each poll taken.               */
    POWER_PULSE_SIG,            /**< AO_Power private one-shot QTimeEvt: the
                                 *   next half of the write pulse, armed
                                 *   again by each half taken. Armed only
                                 *   while a flash write is reporting
                                 *   progress.                              */
    POWER_DUMP_REQ_SIG,         /**< AO_Power: log everything both chips say. */
    POWER_PEEK_REQ_SIG,         /**< AO_Power: take one reading, publish it and keep
                                 *   the wall clock; log nothing and dump no chip.
                                 *   AO_LowPower asks it once a minute asleep. */
    POWER_SHIP_REQ_SIG,         /**< AO_Power: ship mode - open the BATFET and
                                 *   cut the battery from VSYS. On battery the
                                 *   board dies handling this event; waking is
                                 *   BUTTON_BOT (QON) or USB.                  */
    POWER_FIELD_REQ_SIG,        /**< AO_Power: read, write or dump one field
                                 *   of the PMIC or the gauge (PowerFieldReqEvt).
                                 *   Answered with POWER_FIELD_SIG.          */
    POWER_TEST_REQ_SIG,         /**< AO_Power: `test component=pmic|gauge`
                                 *   (PowerTestReqEvt). Answered with a
                                 *   published TEST_RESULT_SIG; the detail
                                 *   goes out as log lines.                  */
    POWER_CTRL_REQ_SIG,         /**< AO_Power: one owner-level operation on
                                 *   the PMIC (PowerCtrlReqEvt): boost, charge,
                                 *   charge limit, reset. Answered with
                                 *   POWER_CTRL_SIG; one in flight at a time. */
    POWER_CTRL_TICK_SIG,        /**< AO_Power private: the one-shot wait of
                                 *   Power_waiting is over.                  */
    POWER_SHIP_ARM_SIG,         /**< AO_Power: count KODE_SHIP_UNPLUG_MS from
                                 *   the cable leaving, then tell the asker
                                 *   (PowerShipArmEvt). Posted by
                                 *   power_ship_on_unplug_request().         */
    POWER_SHIP_DUE_SIG,         /**< The asker of POWER_SHIP_ARM_SIG: the
                                 *   cable has been out for KODE_SHIP_UNPLUG_MS.
                                 *   Posted once per arming, to the one that
                                 *   asked. No payload.                      */
    POWER_SHIP_TMO_SIG,         /**< AO_Power private one-shot QTimeEvt: the
                                 *   countdown after the cable left is over. */
    RADIO_UP_REQ_SIG,           /**< AO_Radio: bring the C5 link up. The
                                 *   outcome is the next RADIO_STATE_SIG.     */
    RADIO_DOWN_REQ_SIG,         /**< AO_Radio: tear the link down. The C5
                                 *   keeps running; only the conversation and
                                 *   its hold on the SDMMC controller end.    */
    RADIO_STATE_REQ_SIG,        /**< AO_Radio: publish RADIO_STATE_SIG again,
                                 *   for whoever started too late (or asked -
                                 *   the console's `radio_info`).             */
    RADIO_JOB_SIG,              /**< AO_Radio private: a job came back from
                                 *   the radio worker (KqpWorkerDoneEvt: its
                                 *   sequence number and status).             */
    RADIO_JOB_GUARD_SIG,        /**< AO_Radio private QTimeEvt: the job with
                                 *   the radio worker never came back.        */
    RADIO_LINK_LOST_SIG,        /**< AO_Radio private: the link to the C5 is
                                 *   gone (RadioLostEvt). Posted by the
                                 *   driver's notification, from esp-hosted's
                                 *   event task or from the worker, never by
                                 *   an AO.                                  */
    RADIO_SVC_JOB_SIG,          /**< AO_Radio private: a KODE request came
                                 *   back from the service worker
                                 *   (KqpWorkerDoneEvt). The sends have a
                                 *   worker of their own, so that a scan
                                 *   cannot hold an answer up.               */
    KODE_SVC_RSP_SIG,           /**< AO_Radio private: a KODE service on the
                                 *   C5 answered (KodeSvcEvt); the payload is
                                 *   in AO_Radio's buffer for that service.  */
    KODE_SVC_TMO_SIG,           /**< AO_Radio private: a KODE request's
                                 *   answer did not come (one QTimeEvt per
                                 *   service; the event names which).        */
    ID_SIGN_REQ_SIG,            /**< AO_Radio: sign the cloud's challenge with
                                 *   the unit's key on the C5 (IdSignReqEvt).
                                 *   Answered by one ID_SIGN_RSP_SIG to who,
                                 *   always - a refusal included.            */
    ID_SIGN_RSP_SIG,            /**< To the asker: the signature, or why not
                                 *   (IdSignRspEvt).                         */
    ID_REQ_SIG,                 /**< AO_Radio: one identity operation that
                                 *   is not a signature (IdReqEvt, ID_OP_*).
                                 *   Answered by ID_PART_SIG records, if the
                                 *   operation reads something, then one
                                 *   ID_END_SIG to who, always.              */
    ID_PART_SIG,                /**< To the asker: one piece of DER, in order
                                 *   (IdPartEvt).                            */
    ID_END_SIG,                 /**< To the asker: the operation is over
                                 *   (IdEndEvt).                             */
    C5_WIPE_REQ_SIG,            /**< AO_Radio: forget the user's secrets -
                                 *   the C5's vault and BLE keys, then this
                                 *   chip's bond store; the identity stays
                                 *   (C5WipeReqEvt). One C5_WIPE_RSP_SIG to
                                 *   who, always.                            */
    C5_WIPE_RSP_SIG,            /**< To the asker: how far the wipe got
                                 *   (C5WipeRspEvt).                         */
    RADIO_RELINK_SIG,           /**< AO_Radio private: the wait before the next
                                 *   bring-up of a link that was lost is over,
                                 *   or the board woke and one is owed.      */
    WIFI_LOST_SIG,              /**< AO_Radio private: the network in use went
                                 *   away by itself (kdrv_wifi_on_lost()). A
                                 *   plain QEvt: the driver's state is the news. */
    WIFI_RETRY_SIG,             /**< AO_Radio private: the wait before the next
                                 *   try at getting back on a network is over. */
    RADIO_SCAN_MORE_SIG,        /**< AO_Radio private: a scan has finished a
                                 *   channel and the driver's table has grown.
                                 *   Posted by kdrv_wifi's progress hook from
                                 *   the worker, never by an AO, and carries
                                 *   nothing - the table is the payload and
                                 *   AO_Radio knows how far it has published.
                                 *   A plain QEvt, so it costs no pool block. */
    RADIO_SCAN_OWED_SIG,        /**< AO_Radio private: an asked scan waits
                                 *   for the link to be ready. Posted by
                                 *   radio_reconcile_next() at the end of the
                                 *   reconcile chain, which may run from an
                                 *   entry action; a plain QEvt.             */
    RADIO_EXP_OWED_SIG,         /**< AO_Radio private: J3's supply waits on
                                 *   a rail the link-up did not switch.
                                 *   Posted by Radio_up's entry, which must
                                 *   not start a job, ahead of the reconcile
                                 *   chain's steps; only Radio_up applies it.
                                 *   A plain QEvt.                          */
    RADIO_TEST_REQ_SIG,         /**< AO_Radio: `test component=c5`, the
                                 *   coprocessor's production row. Brings the
                                 *   link up if it is down, takes a fresh
                                 *   kdrv_c5 snapshot, scans the air, and
                                 *   publishes a TEST_RESULT_SIG named "c5"
                                 *   with every reading as a log line above
                                 *   the verdict.                             */
    RADIO_TEST_STEP_SIG,        /**< AO_Radio private: the link is up, take
                                 *   the row's next step. Posted to self from
                                 *   Radio_up's entry, because an entry
                                 *   action must not start a job.             */
    RADIO_FIELD_REQ_SIG,        /**< AO_Radio: read, write or dump one field
                                 *   of the coprocessor (RadioFieldReqEvt).
                                 *   Answered to RadioFieldReqEvt.who with
                                 *   RADIO_FIELD_RSP_SIG - posted, not
                                 *   published, the expander's shape.         */
    RADIO_FIELD_RSP_SIG,        /**< To the requester (RadioFieldEvt).        */
    RADIO_RECV_REQ_SIG,         /**< AO_Radio: a coprocessor image is about to
                                 *   arrive on the console (AppRecvReqEvt,
                                 *   dst KFW_TARGET_C5) - `install
                                 *   src="terminal" dst=c5`. Ask AO_Log for
                                 *   raw mode, stage the image in PSRAM,
                                 *   verify it, then push it over the RPC the
                                 *   way any `install dst=c5` does, bringing
                                 *   the link up first if it is down.        */
    RADIO_UNUSED_SIG,           /**< AO_Radio to itself: its settle found
                                 *   nothing left that uses the link to the
                                 *   C5 - no service wanted, nobody who asked
                                 *   for the link by itself, nothing in
                                 *   flight. Radio_up looks again and takes
                                 *   the link down. No payload.             */
    C5_BOOT_SIG,                /**< AO_Radio private one-shot QTimeEvt: the
                                 *   C5 it just let go has had its boot
                                 *   time.                                  */
    EXP_PULSE_SIG,              /**< AO_Radio private one-shot QTimeEvt: the
                                 *   C5 reset that switches the expansion
                                 *   header's supply on has lasted long
                                 *   enough; the C5 is let go.              */
    RADIO_RECV_IDLE_SIG,        /**< AO_Radio private QTimeEvt: no chunk for
                                 *   KODE_RECV_IDLE_MS - the host is gone.    */
    RADIO_UNMOUNT_RETRY_SIG,    /**< AO_Radio private one-shot QTimeEvt: the
                                 *   card still answered an unmount as
                                 *   mounted; ask for it again.             */
    RADIO_INSTALL_GO_SIG,       /**< AO_Radio private: the staged image is
                                 *   verified and the link is up - start the
                                 *   push. Posted to self, because an entry
                                 *   action must not start a job and a settle
                                 *   must not change state.                  */
    RADIO_PARK_REQ_SIG,         /**< AO_Radio private (direct post, from
                                 *   radio_park_request()): a jump is about
                                 *   to restart the board - hang up BLE, ask
                                 *   the C5 to park, leave the handover note
                                 *   when it says yes. RadioParkEvt. Answered
                                 *   exactly once with BOOT_RADIO_PARKED_SIG,
                                 *   in every state.                         */
    WIFI_SCAN_REQ_SIG,          /**< AO_Radio: scan. Entries follow on the
                                 *   bus as the sweep finds them, channel by
                                 *   channel, then WIFI_SCAN_END_SIG.        */
    WIFI_SCAN_STOP_REQ_SIG,     /**< AO_Radio: end the listing in flight
                                 *   after its step on the air
                                 *   (wifi_scan_stop_request()). No payload
                                 *   and no answer of its own: the listing's
                                 *   WIFI_SCAN_END_SIG says it was cut.      */
    RADIO_SCAN_ASK_SIG,         /**< AO_Radio: a scan asked by an AO or a
                                 *   blocked task (ScanAskEvt), the sweep
                                 *   WIFI_SCAN_REQ_SIG runs or the listen
                                 *   BLE_SCAN_REQ_SIG does, as its @c kind
                                 *   says. An AO's entries are posted to it,
                                 *   never published, then its end
                                 *   (WIFI_SCAN_RSP_SIG, BLE_SCAN_RSP_SIG); a
                                 *   task's rows are written into its waiter. */
    RADIO_PROBE_REQ_SIG,        /**< AO_Radio: the radio check (RadioProbeReqEvt),
                                 *   radio_probe_request(): the C5's card,
                                 *   a Wi-Fi network on a channel or a sweep
                                 *   for it, a BLE name. Answered exactly
                                 *   once with RADIO_PROBE_SIG.               */
    RADIO_PROBE_SIG,            /**< The radio check's answer, posted to the
                                 *   asking AO (RadioProbeEvt).               */
    WIFI_SCAN_RSP_SIG,          /**< Answers kode_sys_wifi_scan(), to the
                                 *   asking AO (ScanRspEvt).                 */
    WIFI_JOIN_REQ_SIG,          /**< AO_Radio: join WifiJoinEvt's network
                                 *   and remember it if it works.            */
    WIFI_LEAVE_REQ_SIG,         /**< AO_Radio: leave, and forget the stored
                                 *   credentials - "leave" that reconnects
                                 *   at the next boot would be a lie.        */
    WIFI_STATE_REQ_SIG,         /**< AO_Radio: publish WIFI_STATE_SIG now.
                                 *   Answerable in every state, link or no
                                 *   link: "not connected" is an answer.     */
    WIFI_RSSI_REQ_SIG,          /**< AO_Radio: ask the worker to refresh the
                                 *   association's signal strength. The
                                 *   answer is the next WIFI_STATE_SIG -
                                 *   wifi_info itself reads the cache and
                                 *   never waits on this.                    */
    WIFI_FAST_REQ_SIG,          /**< AO_Radio: WifiFastEvt - no power save
                                 *   on the station (on) or the default back
                                 *   (off). Posted by AO_Net's channel; the
                                 *   wish is kept, applied while the station
                                 *   is connected and dropped when it is not
                                 *   there any more.                         */
    EXP_POWER_REQ_SIG,          /**< AO_Radio: ExpPowerEvt - the expansion
                                 *   header's +3.3V_EXT on or off. Posted by
                                 *   kode_sys_exp_power() and `exp_power`; the
                                 *   wish is kept for the boot and applied
                                 *   as soon as the C5 can carry it out.     */
    WIFI_AUTO_REQ_SIG,          /**< AO_Radio: scan, and join the best
                                 *   remembered network that is actually in
                                 *   range. Posted by the console's
                                 *   `wifi_auto` and by AO_Radio itself when
                                 *   the link comes up.                      */
    WIFI_LIST_REQ_SIG,          /**< AO_Radio: publish the remembered
                                 *   networks (names only).                  */
    WIFI_FORGET_REQ_SIG,        /**< AO_Radio: remove WifiJoinEvt.ssid from
                                 *   the store. The password field is unused
                                 *   and must be left empty.                 */
    WIFI_START_REQ_SIG,         /**< AO_Radio: enable the WiFi service - set
                                 *   the stored flag, bring the link up if it
                                 *   is down, autojoin as always.            */
    WIFI_STOP_REQ_SIG,          /**< AO_Radio: disable it - stop the station,
                                 *   clear the flag. The link stays up: the
                                 *   transport cannot re-init in one boot.   */
    BLE_START_REQ_SIG,          /**< AO_Radio: bring the BLE stack up. There
                                 *   is no advertise request beside it: being
                                 *   up IS being on the air.                 */
    BLE_SCAN_REQ_SIG,           /**< AO_Radio: listen for advertisers, for
                                 *   as long as it asks (BleScanReqEvt).     */
    BLE_SCAN_RSP_SIG,           /**< Answers kode_sys_ble_scan(), to the
                                 *   asking AO (ScanRspEvt).                 */
    BLE_RSP_REQ_SIG,            /**< AO_Radio: the name and the maker data
                                 *   the scan response carries from now on
                                 *   (BleRspReqEvt). Never stored.           */
    BLE_APP_VALUE_REQ_SIG,      /**< AO_Radio: set or notify a value of the
                                 *   app's own service (BleAppValueReqEvt). */
    BLE_STOP_REQ_SIG,           /**< AO_Radio: take the BLE stack down.      */
    BLE_STATE_REQ_SIG,          /**< AO_Radio: publish BLE_STATE_SIG now. A
                                 *   question, with no effect of its own.    */
    BLE_LINK_SIG,               /**< AO_Radio, from the driver's callback: a
                                 *   central arrived or left. Publishes like
                                 *   the question above AND restores the
                                 *   advertising invariant, which is why the
                                 *   two are not one signal.                 */
    BLE_PAIR_TAKE_SIG,          /**< AO_Radio to itself, from the driver's
                                 *   callback: collect the pairing record and
                                 *   publish it. Never leaves that AO, and
                                 *   exists for the same reason BLE_LINK_SIG
                                 *   does - the nudge arrives on NimBLE's
                                 *   host task, and the record is read back
                                 *   on AO_Radio's own thread.               */
    BLE_PAIR_REJECT_REQ_SIG,    /**< AO_Radio: refuse the pairing in flight.
                                 *   Posted by whoever is showing the code
                                 *   when a human closes the card.           */
    BLE_UNPAIR_REQ_SIG,         /**< AO_Radio: forget every bonded peer.
                                 *   Answered by BLE_STATE_SIG, like the
                                 *   rest of the ble_* family.              */
    BLE_PAIR_OPEN_REQ_SIG,      /**< AO_Radio: show the board for five
                                 *   minutes, so a new device can pair.
                                 *   Answered by BLE_STATE_SIG.             */
    NET_PING_REQ_SIG,           /**< AO_Net: ping NetPingReqEvt.host. Entries
                                 *   follow on the bus, then NET_PING_END.   */
    NET_HTTP_REQ_SIG,           /**< AO_Net: GET NetHttpReqEvt.url. Body
                                 *   slices follow, then NET_HTTP_DONE.      */
    NET_UDP_REQ_SIG,            /**< AO_Net: send NetUdpReqEvt's datagram.   */
    NET_HOST_ASK_SIG,           /**< AO_Net: an ask's lookup or ping of a host
                                 *   (NetHostAskEvt).                         */
    NET_HTTP_ASK_SIG,           /**< AO_Net: an ask's GET or POST, its buffers
                                 *   lent (NetHttpAskEvt).                    */
    NET_UDP_ASK_SIG,            /**< AO_Net: an ask's datagram, lent
                                 *   (NetUdpAskEvt).                          */
    NET_LOOKUP_RSP_SIG,         /**< Answers kode_sys_net_lookup(), to the
                                 *   asking AO (NetHostRspEvt).               */
    NET_PING_RSP_SIG,           /**< Answers kode_sys_net_ping(), to the
                                 *   asking AO (NetHostRspEvt).               */
    NET_HTTP_RSP_SIG,           /**< Answers kode_sys_http_get() and
                                 *   kode_sys_http_post(), to the asking AO
                                 *   (NetHttpRspEvt).                         */
    NET_UDP_RSP_SIG,            /**< Answers kode_sys_udp_send(), to the
                                 *   asking AO (NetUdpDoneEvt).               */
    NET_INFO_REQ_SIG,           /**< AO_Net: publish NET_INFO_SIG now.
                                 *   Answerable with no network at all: an
                                 *   address of 0.0.0.0 is an answer.        */
    NET_STREAM_OPEN_REQ_SIG,    /**< AO_Net: open NetStreamOpenEvt.url.       */
    NET_STREAM_READ_REQ_SIG,    /**< AO_Net: fill a free buffer and send it.  */
    NET_STREAM_NEXT_SIG,        /**< AO_Net: a borrowed buffer, handed back
                                 *   (StorageBufEvt).                        */
    NET_STREAM_CLOSE_REQ_SIG,   /**< AO_Net: close the open transfer.         */
    NET_STREAM_OPEN_RSP_SIG,    /**< To the opener (StorageOpenRspEvt).       */
    NET_STREAM_DATA_SIG,        /**< To the reader (StorageDataEvt) - the
                                 *   same conversation AO_Storage has, because
                                 *   over here a URL is just another medium.  */
    NET_TEST_REQ_SIG,           /**< AO_Net: run the `test component=net`
                                 *   self-test; answers TEST_RESULT_SIG.     */
    NET_JOB_SIG,                /**< AO_Net private: a job came back from
                                 *   its worker (KqpWorkerDoneEvt: the job's
                                 *   sequence number and status). Posted by
                                 *   the worker, never by an AO.             */
    NET_TMO_SIG,                /**< AO_Net private QTimeEvt: the broker
                                 *   never answered the connect.             */
    NET_JOB_GUARD_SIG,          /**< AO_Net private QTimeEvt: the job with
                                 *   the worker never came back.             */
    NET_STREAM_JOB_SIG,         /**< AO_Net private: a job came back from
                                 *   the download's worker (KqpWorkerDoneEvt).
                                 *   The download has a worker of its own, so
                                 *   that a long POST cannot hold a transfer
                                 *   up.                                     */
    NET_STREAM_GUARD_SIG,       /**< AO_Net private QTimeEvt: the download's
                                 *   job never came back.                    */
    MQTT_UP_REQ_SIG,            /**< AO_Net: connect to MqttUpEvt.uri.       */
    MQTT_DOWN_REQ_SIG,          /**< AO_Net: stop the client.                */
    MQTT_PUB_REQ_SIG,           /**< AO_Net: publish MqttPubEvt.             */
    MQTT_SUB_REQ_SIG,           /**< AO_Net: subscribe to MqttTopicEvt.topic. */
    MQTT_UNSUB_REQ_SIG,         /**< AO_Net: unsubscribe from it.            */
    MQTT_STATE_REQ_SIG,         /**< AO_Net: publish MQTT_STATE_SIG now.     */
    NET_MQTT_LINK_SIG,          /**< AO_Net private: the client connected,
                                 *   dropped or errored (NetMqttLinkEvt),
                                 *   marshalled off esp-mqtt's own task.     */
    NET_MQTT_RX_SIG,            /**< AO_Net private, static event posted from
                                 *   esp-mqtt's task: the driver's receive ring
                                 *   holds messages. AO_Net takes them with
                                 *   kdrv_net_mqtt_rx_take() and republishes
                                 *   each as MQTT_MSG_SIG from its own thread,
                                 *   so nothing foreign touches the bus.     */

    CHANNEL_STATE_REQ_SIG,      /**< AO_Net: publish CHANNEL_STATE_SIG now.  */
    NET_CHAN_TMO_SIG,           /**< AO_Net private QTimeEvt: the channel's
                                 *   one timer - the wait before a retry, the
                                 *   renewal 23 h after a connect, and the
                                 *   guard on an answer the C5 owes.         */
    NET_CHAN_OUT_SIG,           /**< AO_Net private, no payload: reply lines wait
                                 *   in the channel's ring. Posted by AO_Log
                                 *   when the ring stops being empty and by
                                 *   AO_Net to itself until it is.           */
    NET_CHAN_FAST_SIG,          /**< AO_Net private QTimeEvt, one-shot: no
                                 *   cmd/in line for CHAN_FAST_HOLD_MS, so the
                                 *   fast mode ends.                         */
    NET_CHAN_WATCH_SIG,         /**< AO_Net private QTimeEvt, one-shot and
                                 *   re-armed while the channel is up: is the
                                 *   client the channel believes in still
                                 *   there?                                  */
    NET_LINE_DONE_SIG,          /**< AO_Net (direct post, from
                                 *   net_line_done()): the console has
                                 *   answered the line AO_Net gave it from
                                 *   cmd/in (`>ok` or `>err`): the next one
                                 *   may go. NetLineDoneEvt, carrying the
                                 *   sequence of the line it ended.          */
    NET_LINE_BUSY_SIG,          /**< AO_Net (direct post, from
                                 *   net_line_busy()): the console was busy
                                 *   with another command and did not take
                                 *   the line; offer it again shortly. Static
                                 *   event.                                  */
    NET_CHAN_LINE_SIG,          /**< AO_Net private QTimeEvt, one-shot: the
                                 *   console has had KODE_CHAN_LINE_BOUND_MS
                                 *   for the line it holds, or the post of
                                 *   the next line was refused and is tried
                                 *   again.                                  */
    NET_LEAVE_REQ_SIG,          /**< AO_Net (private, direct post, from
                                 *   net_leave_request()): leave the Kode
                                 *   channel cleanly and start no new session
                                 *   for the rest of the boot. Answered with
                                 *   BOOT_NET_LEFT_SIG.                       */
    NET_CHECK_AT_SIG,           /**< AO_Net (direct post, from
                                 *   net_check_at()): the next time the
                                 *   scheduler's `mqtt` entry falls due
                                 *   (NetCheckAtEvt).                         */
    NET_CHECK_TMO_SIG,          /**< AO_Net private QTimeEvt, one-shot: the
                                 *   dwell of a radio wake for that check has
                                 *   run out.                                 */

    DISPLAY_TEST_REQ_SIG,       /**< AO_Display: `test component=display`;
                                 *   answers a published TEST_RESULT_SIG. No
                                 *   payload.                               */
    DISPLAY_TEST_STEP_SIG,      /**< AO_Display private, self-posted: paint
                                 *   the next band of the field.            */
    DISPLAY_TEST_WAIT_SIG,      /**< AO_Display private one-shot QTimeEvt:
                                 *   nobody answered the field in time.     */
    LED_TEST_REQ_SIG,           /**< AO_LedMgr: `test component=led`;
                                 *   answers a published TEST_RESULT_SIG. No
                                 *   payload.                               */
    LED_TEST_WAIT_SIG,          /**< AO_LedMgr private one-shot QTimeEvt:
                                 *   nobody answered the colour in time.    */
    AUTOTEST_RUN_REQ_SIG,       /**< AO_Autotest: run a group of rows, or one
                                 *   by name (AutotestRunReqEvt).             */
    AUTOTEST_PSRAM_STEP_SIG,    /**< AO_Autotest private one-shot QTimeEvt:
                                 *   the PSRAM row's next step is due.        */
    AUTOTEST_BUDGET_SIG,        /**< AO_Autotest private one-shot QTimeEvt:
                                 *   the row in flight ran out of budget, or
                                 *   nobody answered a hold.                  */
    AUTOTEST_CANCEL_REQ_SIG,    /**< AO_Autotest: end the run in flight after
                                 *   the row in flight (AutotestClientEvt).   */
    AUTOTEST_CONTINUE_SIG,      /**< AO_Autotest: the answer to a hold
                                 *   (AutotestClientEvt).                     */
    AUTOTEST_REPORT_REQ_SIG,    /**< AO_Autotest: publish the kept report
                                 *   (AutotestClientEvt).                     */
    POWER_TEST_STEP_SIG,        /**< AO_Power private one-shot QTimeEvt, armed
                                 *   again by each look taken: the
                                 *   `power_plug` row looks at the cable
                                 *   again.                                   */

    DISPLAY_SHOT_REQ_SIG,       /**< AO_Display: encode the front framebuffer
                                 *   whole as one JPEG and lend it
                                 *   (DisplayShotReqEvt).                     */
    DISPLAY_SHOT_RSP_SIG,       /**< To the requester (DisplayShotRspEvt).    */
    DISPLAY_SHOT_END_SIG,       /**< AO_Display: the requester is done with
                                 *   the shot; take the buffer back.          */
    DISPLAY_JPEG_REQ_SIG,       /**< AO_Display: encode the requester's
                                 *   RGB565 and copy the JPEG into its buffer
                                 *   (DisplayJpegReqEvt),
                                 *   display_jpeg_request().                  */

    NET_SHOT_REQ_SIG,           /**< AO_Net (direct post, from
                                 *   net_shot_request()): upload the glass to
                                 *   the Kode channel's `shots` link
                                 *   (NetShotReqEvt).                         */
    NET_SHOT_RSP_SIG,           /**< To the requester: how the upload ended
                                 *   (NetShotRspEvt).                         */
    NET_SHOT_TICK_SIG,          /**< AO_Net private QTimeEvt: the pace of a
                                 *   stream of uploads.                       */
    NET_SHOT_WAIT_SIG,          /**< AO_Net private QTimeEvt, one-shot: the
                                 *   display did not answer a shot in time.   */

    NET_SHOT_STOP_SIG,          /**< AO_Net (direct post, from
                                 *   net_shot_stop_request()): end the
                                 *   running stream of uploads
                                 *   (NetShotStopEvt).                        */

    FUNCTION_ANS_SIG,           /**< Posted to AO_Console by the owner of a
                                 *   function that answers (FunctionAnsEvt):
                                 *   one record of the answer, or its end.    */

    KODE_SCREEN_ATTACH_SIG,     /**< The standard layer's AO (direct post, from
                                 *   kode_standard_ao_screen_attach()): take
                                 *   the screen's table of functions. Private
                                 *   to kode_standard_ao.c.                    */
    KODE_SCREEN_REQ_SIG,        /**< The standard layer's AO (direct post):
                                 *   change the screen (KodeScreenReqEvt).    */
    KODE_SCREEN_TICK_SIG,       /**< The standard layer's AO private one-shot
                                 *   QTimeEvt: the screen's render pass.     */
    KODE_TOUCH_LISTEN_SIG,      /**< The standard layer's AO (direct post, from
                                 *   kode_standard_ao_touch_listen()):
                                 *   subscribe to the finger and turn it into
                                 *   events. Private to kode_standard_ao.c.  */
    KODE_PIN_EDGE_SIG,          /**< The standard layer's AO private: a
                                 *   listened J3 pin moved. Posted from the
                                 *   pin's interrupt through
                                 *   kqp_post_from_isr(), one static event
                                 *   per pin, never by an AO.                */
    KODE_PIN_SETTLE_SIG,        /**< The standard layer's AO private one-shot
                                 *   QTimeEvt, one per pin: a pin's edge has
                                 *   had KODE_PIN_SETTLE_MS; read its level. */
    KODE_PIN_LISTEN_SIG,        /**< The standard layer's AO (direct post, from
                                 *   kode_standard_ao_pin_arm()): a pin is
                                 *   listened to from now on, at this level.
                                 *   Private to kode_standard_ao.c.          */

    DISPLAY_HEALTH_SIG,         /**< AO_Display private QTimeEvt: the glass is looked at. */

#if CONFIG_KODE_LOWPOWER
    LOWPOWER_REQ_SIG,           /**< AO_LowPower (direct post, from
                                 *   lowpower_request()): go to a state
                                 *   (LowPowerReqEvt).                      */
    LOWPOWER_READY_SIG,         /**< AO_LowPower (direct post, from
                                 *   lowpower_ready()): one participant's
                                 *   answer to a round (LowPowerReadyEvt).  */
    LOWPOWER_INPUT_SIG,         /**< AO_LowPower (direct post, from
                                 *   lowpower_input_note()): a console line
                                 *   came in; the idle time starts over, and
                                 *   asleep the board wakes. One static event
                                 *   per port (LowPowerInputEvt).           */
    LOWPOWER_REST_SIG,          /**< AO_LowPower (direct post, from
                                 *   lowpower_request_rest()): rest once the
                                 *   work is done, unless the board is in
                                 *   use (LowPowerReqEvt, target ASLEEP).   */
    LOWPOWER_TOP_SIG,           /**< AO_Buttons to AO_LowPower: GPIO35 moved. Asleep
                                 *   the tick is stopped and no settle runs, so
                                 *   this is what wakes the board on the top
                                 *   button.                                  */
    LOWPOWER_MINUTE_SIG,        /**< AO_LowPower (posted by the minute timer
                                 *   while ASLEEP): the minute is over, show
                                 *   the next one. Static event.            */
    LOWPOWER_ROUND_TMO_SIG,     /**< AO_LowPower private QTimeEvt, one-shot:
                                 *   a participant did not answer the round
                                 *   in KODE_LOWPOWER_ROUND_MS.             */
    LOWPOWER_IDLE_TMO_SIG,      /**< AO_LowPower private QTimeEvt, one-shot:
                                 *   the idle time ran out.                 */
    DISPLAY_MINUTE_REQ_SIG,     /**< AO_Display (direct post, from
                                 *   AO_LowPower): light the glass for the
                                 *   minute's glyphs and let it go dark
                                 *   again. Static event, no payload.       */
    DISPLAY_GLYPHS_SIG,         /**< AO_Display (from display_glyphs_hand()):
                                 *   the glyph strip the minute is drawn
                                 *   from (DisplayGlyphsEvt).               */
    RADIO_SLEEP_NOTE_SIG,       /**< AO_Radio private: the C5 woke the P4, or
                                 *   the P4 woke it (RadioSleepNoteEvt).    */
    RADIO_NET_LEFT_SIG,         /**< AO_Radio (direct post, from
                                 *   radio_net_left()): AO_Net has closed its
                                 *   sessions for an OFF round, so the link
                                 *   may go down (RadioNetLeftEvt).         */
    DISPLAY_GLYPHS_TMO_SIG,     /**< AO_Display private QTimeEvt, one-shot:
                                 *   the renderer handed no glyphs in
                                 *   KODE_LOWPOWER_GLYPHS_WAIT_MS.          */
    TOUCH_RESTORE_SIG,          /**< AO_Touch private QTimeEvt, one-shot:
                                 *   try again to write the controller back
                                 *   to its awake settings.                 */
    DISPLAY_RESTORE_SIG,        /**< AO_Display private QTimeEvt, one-shot:
                                 *   try again to put the glass back after
                                 *   an AWAKE it could not carry out.       */
    LOWPOWER_STATE_RETRY_SIG,   /**< AO_LowPower private QTimeEvt, one-shot:
                                 *   publish the state change again, the
                                 *   event pool having had none to carry it. */
    LOWPOWER_WAKE_AT_SIG,       /**< AO_LowPower (direct post, from
                                 *   lowpower_wake_at()): an owner has work
                                 *   due at a real time; asleep, the board
                                 *   wakes for it (LowPowerWakeAtEvt).      */
    LOWPOWER_RADIO_DONE_SIG,    /**< AO_LowPower (direct post, from
                                 *   lowpower_radio_done()): a radio owner's
                                 *   work in a radio wake is finished
                                 *   (LowPowerRadioDoneEvt).                */
    LOWPOWER_RADIO_TMO_SIG,     /**< AO_LowPower private QTimeEvt, one-shot:
                                 *   a radio wake ran KODE_LOWPOWER_RADIO_MAX_MS. */
    LOWPOWER_RETRY_TMO_SIG,     /**< AO_LowPower private QTimeEvt, one-shot:
                                 *   begin a requested round again, an owner
                                 *   having been busy or silent in the last
                                 *   one (KODE_LOWPOWER_REQUEST_RETRY_MS). */
#endif

    SCHED_SET_REQ_SIG,          /**< AO_Scheduler (direct post, from
                                 *   sched_request_set()): a console entry
                                 *   (SchedSetReqEvt).                      */
    SCHED_SET_AO_REQ_SIG,       /**< AO_Scheduler (direct post, from
                                 *   sched_request_set_ao()): an entry that
                                 *   posts to an AO (SchedSetAoReqEvt).     */
    SCHED_REMOVE_REQ_SIG,       /**< AO_Scheduler (direct post, from
                                 *   sched_request_remove()): drop an entry
                                 *   of code (SchedNameReqEvt).             */
    SCHED_LIST_REQ_SIG,         /**< AO_Scheduler (direct post, from
                                 *   sched_request_list()): one entry or all
                                 *   (SchedNameReqEvt).                     */
    SCHED_ENTRY_SIG,            /**< To the asker: one entry (SchedEntryEvt). */
    SCHED_END_SIG,              /**< To the asker: the request is answered
                                 *   (SchedEndEvt).                         */
    SCHED_LINE_DONE_SIG,        /**< AO_Scheduler (direct post, from
                                 *   sched_line_done()): the console has
                                 *   answered the line it was given. Static
                                 *   event.                                 */
    SCHED_LINE_BUSY_SIG,        /**< AO_Scheduler (direct post, from
                                 *   sched_line_busy()): the console was busy
                                 *   with another command when the line it
                                 *   was given arrived, and answered nothing.
                                 *   Static event.                          */
    SCHED_DUE_SIG,              /**< AO_Scheduler private QTimeEvt, one-shot:
                                 *   the earliest minute is near, or the
                                 *   clock is asked again.                  */
    SCHED_RETRY_SIG,            /**< AO_Scheduler private QTimeEvt, one-shot:
                                 *   a line the console was busy for is given
                                 *   again, KODE_SCHED_RETRY_MS after the
                                 *   refusal.                               */
    SCHED_WORK_TMO_SIG,         /**< AO_Scheduler private QTimeEvt, one-shot:
                                 *   the work of a minute ran
                                 *   KODE_SCHED_WORK_BOUND_MS.              */
    SCHED_CHECK_DONE_SIG,       /**< AO_Scheduler (direct post, from
                                 *   sched_check_done(), AO_Net): the `mqtt`
                                 *   check was made (SchedCheckDoneEvt).    */
    SCHED_UP_SIG,               /**< AO_Scheduler (direct post, from
                                 *   sched_system_up(), kode_init()): every
                                 *   AO has its queue, so the scheduler may
                                 *   post. Static event.                    */
    SCHED_WALL_SET_SIG,         /**< AO_Scheduler (direct post, from the hook
                                 *   it registered with kdrv_clocks): the wall
                                 *   clock was written by a person or a time
                                 *   server, so the minute timer is armed
                                 *   again. Static event.                   */
    BUTTONS_TEST_REQ_FROM_SIG,  /**< AO_Buttons: the guided button test, asked
                                 *   by a client other than AO_Autotest
                                 *   (TestReqEvt). The same script and
                                 *   criteria as BUTTONS_TEST_REQ_SIG; every
                                 *   step it publishes carries `origin`.      */
    TOUCH_TEST_REQ_FROM_SIG,    /**< AO_Touch: the same for the guided touch
                                 *   test (TestReqEvt).                       */
    BUTTONS_TEST_CANCEL_SIG,    /**< AO_Buttons: the guided button test that
                                 *   client `origin` asked for ends here,
                                 *   with no verdict (TestReqEvt).            */
    TOUCH_TEST_CANCEL_SIG,      /**< AO_Touch: the same for the guided touch
                                 *   test (TestReqEvt).                       */
    POWER_BOOST_TEST_REQ_SIG,   /**< AO_Power: the pads delivery test's boost
                                 *   on or off (PowerBoostTestReqEvt),
                                 *   power_boost_test_request(). Answered
                                 *   once with POWER_BOOST_TEST_SIG.          */
    POWER_BOOST_TEST_SIG,       /**< To the asker: the boost as the delivery
                                 *   test left it (PowerBoostTestEvt).        */
    LOG_DOCK_REQ_SIG,           /**< AO_Log: lend the dock console's pins, or
                                 *   take them back (LogDockReqEvt),
                                 *   log_dock_lend_request(). Answered once
                                 *   with LOG_DOCK_SIG.                       */
    LOG_DOCK_SIG,               /**< To the asker: the dock's pins are lent or
                                 *   back (LogDockEvt).                       */
    TEST_DOT_REQ_SIG,           /**< AO_TestDot: a station verb from the
                                 *   console (TestDotReqEvt). Answered with
                                 *   TEST_DOT_REC_SIG records and one
                                 *   TEST_DOT_ANS_SIG, posted to the asker.   */
    TEST_DOT_REC_SIG,           /**< To the asker: one record of a station
                                 *   verb (TestDotRecEvt).                    */
    TEST_DOT_ANS_SIG,           /**< To the asker: a station verb's end
                                 *   (TestDotAnsEvt).                         */
    TEST_DOT_LEND_SIG,          /**< To the asker: a blob lent for the wire
                                 *   (TestDotLendEvt). The asker sends it and
                                 *   gives it back with test_dot_blob_back(). */
    TEST_DOT_BACK_SIG,          /**< AO_TestDot: the lent blob is sent and no
                                 *   longer read. Static event.               */
    TEST_DOT_SCREEN_DONE_SIG,   /**< AO_TestDot: the screen host has the
                                 *   screen or the pattern on the panel
                                 *   (TestDotScreenDoneEvt),
                                 *   test_dot_screen_done().                  */
    TEST_DOT_JOB_SIG,           /**< AO_TestDot: its worker's job returned
                                 *   (KqpWorkerDoneEvt).                      */
    TEST_DOT_TMO_SIG,           /**< AO_TestDot private QTimeEvt, one-shot:
                                 *   the wait in hand ran out.                */
    TEST_DOT_REST_SIG,          /**< AO_TestDot private QTimeEvt, one-shot:
                                 *   the rest before the photos, or the
                                 *   operator's screen settling.              */
    TEST_DOT_WATCH_SIG,         /**< AO_TestDot private QTimeEvt, one-shot:
                                 *   the run's watchdog.                      */
    TEST_DOT_DRAIN_SIG,         /**< AO_TestDot private QTimeEvt, one-shot:
                                 *   post the records the console's queue
                                 *   refused.                                 */

    MAX_SIG                   /**< First value free for a consumer's own
                                 *   private signals: kgui numbers its from
                                 *   here, and an app numbers its the same
                                 *   way (see examples/buttons_led). Self-posted
                                 *   signals never cross AOs, so consumers
                                 *   may repeat each other's values.         */
};

/** @brief LED on/off command (LED_SET_SIG, LED_HEARTBEAT_ENABLE_SIG). */
typedef struct {
    QEvt super;
    bool on;
} LedEvt;

/** @brief Heartbeat period change (LED_BLINK_PERIOD_SIG). */
typedef struct {
    QEvt super;
    uint16_t period_ms;
} LedBlinkEvt;

/** @brief Colour for the RGB LED (LED_RGB_SIG). */
typedef struct {
    QEvt super;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} LedRgbEvt;

/**
 * @brief Request to write a .bin from the SD into the user slot.
 *
 * Carries the file name the operator typed, not an open file: the mount, the
 * lookup and the whole copy happen on AO_AppMgr's own thread.
 */
typedef struct {
    QEvt super;
    char file[KODE_APP_FILE_MAX];
    bool launch;   /**< Boot the slot as soon as the write succeeds. */
} AppFlashEvt;

/**
 * @brief Assign or drop an app's categories (APP_CAT_REQ_SIG).
 *
 * Posted to AO_AppMgr, the table's only writer. Reads do not come this way -
 * they are a guarded kdrv call any task may make - so this event exists only
 * to keep the single writer single.
 */
typedef struct {
    QEvt    super;
    char    project[32];                 /**< esp_app_desc_t.project_name. */
    char    cats[KODE_APP_CATEGORY_MAX]; /**< Ignored when clear is true.  */
    bool    clear;                       /**< Drop the row instead.        */
} AppCatReqEvt;

/**
 * @brief An image is about to arrive on the console (APP_RECV_REQ_SIG, and
 *        RADIO_RECV_REQ_SIG for the coprocessor's).
 *
 * Everything the receiver needs before a single byte moves: where the image
 * goes, how much room to plan for, what the bytes must add up to, and
 * whether to boot the result. The app's *name* is not the request's to give
 * - it belongs to the image, and kdrv_apps_desc() reads it from the manifest
 * once the write verifies. `name` is only where to make room: the host's
 * reading of the image's project name, so a new version of an installed app
 * frees the old copy before any other app.
 */
typedef struct {
    QEvt     super;
    uint8_t  dst;     /**< `install src="terminal"`'s `dst=`: kfw_target_t
                       *   (KFW_TARGET_APP, the region; KFW_TARGET_OS, the
                       *   inactive kodeOS slot; KFW_TARGET_C5, AO_Radio's)
                       *   or KODE_FLASH_DST_PENDING, the mailbox.        */
    uint32_t size;    /**< Payload length in bytes.                          */
    uint32_t crc32;   /**< What khal_crc32() over the payload must equal.    */
    bool     launch;  /**< Boot it as soon as the install verifies.          */
    uint8_t  src;     /**< KODE_CONSOLE_SRC_*: which port the payload
                       *   follows on. The console has two, and only one of
                       *   them can be handed over at a time.                */
    char     name[KODE_APP_PROJECT_MAX]; /**< The image's project name as the
                                          *   host read it, or "".          */
} AppRecvReqEvt;

/**
 * @brief One chunk of the arriving image (RECV_DATA_SIG).
 *
 * The bytes travel in the event, not behind a pointer into the reader task's
 * memory: that task has no queue to be told on, so it could never learn that
 * a borrowed buffer had come back. Paying for the copy buys the loan protocol
 * away entirely. (Flow control is NOT this pool's job - that is the `>|ack`
 * protocol's; an empty pool just delays the reader a beat.)
 *
 * The biggest event in the system by an order of magnitude, and the whole
 * reason the jumbo pool exists.
 */
typedef struct {
    QEvt     super;
    uint16_t len;
    uint8_t  data[KODE_RECV_CHUNK];
} RecvDataEvt;

/** @brief Hand the input stream over to @c who, raw (CONSOLE_RAW_REQ_SIG). */
typedef struct {
    QEvt     super;
    uint32_t size;    /**< Exactly this many bytes, then raw mode ends.      */
    QActive *who;     /**< Where the chunks go.                              */
    uint8_t  src;     /**< KODE_CONSOLE_SRC_*: the port to hand over.        */
} ConsoleRawReqEvt;

/** @brief Whether raw mode is armed (CONSOLE_RAW_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t status;   /**< kode_status_t */
} ConsoleRawRspEvt;

/** @brief A log level for AO_Log to apply (LOG_LEVEL_SET_SIG). */
typedef struct {
    QEvt    super;
    char    tag[32];  /**< The tag, a copy; "" = the global default, which is
                       *   the one level that is stored. Twice klog's own
                       *   limit, so a tag too long for it arrives still too
                       *   long, and klog refuses it by name rather than
                       *   applying a truncated tag.                       */
    uint8_t level;    /**< kode_log_lvl_t                                  */
} LogLevelEvt;

/**
 * @brief What a flash write is, and how it ends (AppProgressEvt.what).
 *
 * One value for both, because the Opening sheet says both in one sentence:
 * what is being written and whether the Dot restarts when it is done.
 */
typedef enum {
    KWRITE_APP = 0,   /**< An app into the region; the Dot stays where it is. */
    KWRITE_APP_RUN,   /**< An app, and the Dot then restarts into it.        */
    KWRITE_OS,        /**< kodeOS's other slot; the Dot restarts into it.    */
    KWRITE_C5,        /**< The radio coprocessor; the Dot stays where it is. */
} kwrite_t;

/**
 * @brief How far along a flash write is (APP_FLASH_PROGRESS_SIG).
 *
 * Carries what is being written, not just how far: the Opening sheet has to
 * be true for writes the GUI did not start - every console `install`
 * lands here too - so every report says what it is and names the
 * file when there is one. A copy, as always, never a pointer into the
 * writer's own tables.
 */
typedef struct {
    QEvt    super;
    uint8_t percent;
    uint8_t what;                     /**< kwrite_t                           */
    char    file[KODE_APP_FILE_MAX];  /**< What is being written; "" for a
                                       *   stream, which has no file name.    */
} AppProgressEvt;

/**
 * @brief APP_FLASH_DONE_SIG: a flash write ended well.
 *
 * Names the app the write installed, so whoever asked for a write without a
 * launch can run it afterwards: the name is what appmgr_run_request() looks
 * up. A copy, carried in a pool event, never a pointer into the writer's
 * memory.
 *
 * Every APP_FLASH_DONE_SIG is one of these, never a bare QEvt, so `name` is
 * always there to read. The end of a write has to arrive even when the pool
 * is dry - the Opening sheet and the LED both end on it - so AO_AppMgr then
 * publishes a static instance with an empty name instead. AO_Radio publishes
 * a static one of its own, unnamed too: a C5 image installs no app.
 */
typedef struct {
    QEvt super;
    char name[32];   /**< The installed app's project name
                      *   (esp_app_desc_t.project_name is 32 bytes), the
                      *   identity appmgr_find_idx() compares. "" for
                      *   AO_Radio's C5 image, for a park in the mailbox
                      *   (no app is installed), for an image whose
                      *   descriptor could not be read, and from a dry pool. */
} AppFlashDoneEvt;

/** @brief Why a write failed, as far as the glass needs to know. */
typedef enum {
    KFAIL_WRITE = 0,  /**< The write itself: the image, the medium, the flash. */
    KFAIL_KEEP,       /**< Nothing was written: the card could not keep an app
                       *   this install would have removed.                  */
} kfail_t;

/**
 * @brief APP_FLASH_FAIL_SIG: a flash write failed, and why.
 *
 * Every APP_FLASH_FAIL_SIG is one of these. Each publisher keeps one static,
 * immutable instance per reason it can give: a failure has to arrive even
 * when the pool is dry, because the Opening card and the LED both end on it.
 */
typedef struct {
    QEvt    super;
    uint8_t why;     /**< kfail_t */
} AppFlashFailEvt;

/**
 * @brief One transfer: where the bytes come from, where they go.
 *
 * Copies, not pointers (rule 7): the console's parse buffer is gone by the
 * time AO_AppMgr reads this.
 */
typedef struct {
    QEvt    super;
    uint8_t src_kind;                  /**< kfw_src_t, as a plain integer.  */
    uint8_t dst_kind;                  /**< kfw_target_t, likewise.         */
    bool    launch;                    /**< Boot it once written.           */
    char    src[KODE_FLASH_SRC_MAX];   /**< File name, or a URL.            */
} FlashReqEvt;

/**
 * @brief One installed app, in answer to APP_LIST_REQ_SIG (APP_ENTRY_SIG).
 *
 * One event per app rather than a shared table, for the same reason
 * StorageEntryEvt is: each subscriber ends up with its own copy, read
 * nowhere near AO_AppMgr's thread.
 */
typedef struct {
    QEvt     super;
    char     name[32];    /* esp_app_desc_t.project_name is 32 bytes */
    char     version[16];
    uint32_t size;
    uint32_t seq;
    uint32_t offset;      /**< Where its image sits in the apps region: the
                            *   directory entry's own number, and the one
                            *   boot_app_offset() answers with.            */
    /* From the app's own manifest; both empty when it carries none, which
     * every app built before manifests existed does. */
    char     display[KODE_APP_DISPLAY_MAX];
    char     category[KODE_APP_CATEGORY_MAX];
    /* From IDF's descriptor in the image: when it was built, as the compiler
     * wrote it ("Oct 24 2026", "21:14:03"). Both empty when the image's
     * descriptor cannot be read. */
    char     build_date[KODE_APP_DATE_MAX];
    char     build_time[KODE_APP_TIME_MAX];
    uint16_t pos;         /**< Its place in the listing, 0 first, counted
                            *   across pages: a subscriber stores entry
                            *   `pos` at index `pos`, and 0 begins a new
                            *   listing.                                  */
} AppEntryEvt;

/**
 * @brief An app an install evicted to make room (APP_EVICTED_SIG).
 */
typedef struct {
    QEvt super;
    char name[KODE_APP_PROJECT_MAX]; /**< Its project name.                 */
    bool kept;                       /**< A copy of it is on the card.      */
} AppEvictedEvt;

/**
 * @brief Ask AO_AppMgr for a page of the installed apps (APP_LIST_REQ_SIG).
 *
 * The page is the directory's entries from `skip` on, at most
 * KODE_STORAGE_LIST_MAX of them, then APP_LIST_END_SIG, all posted to `who`
 * alone: a page published to every subscriber would reach the slowest of them
 * as fast as the fastest asker chains the next one.
 */
typedef struct {
    QEvt     super;
    uint16_t skip;        /**< Entries to leave out: what earlier pages sent. */
    QActive *who;         /**< The asker; the page is posted to it alone.   */
} AppListReqEvt;

/**
 * @brief The end of one page of a listing (STORAGE_LIST_END_SIG,
 *        APP_LIST_END_SIG).
 *
 * Sent from static events, so the end of a page can never be lost to an
 * empty pool. Whoever asked for the listing reads `more` and asks the next
 * page with a skip of the entries it holds.
 */
typedef struct {
    QEvt super;
    bool more;            /**< Another page follows this one.               */
} ListEndEvt;

/**
 * @brief Ask which installed apps an install would evict (APP_EVICT_REQ_SIG).
 *
 * The request carries the asker because the answer is for it alone: it is
 * posted there, never published.
 *
 * The project name is what lets the answer be exact. An install replaces the
 * installed app of its own name in one step, so that app is no loss and its
 * directory slot is not taken from another: with the name, the answer leaves
 * it out and names no app for the slot. Without it (`project` empty) the
 * answer cannot tell, and an app the install replaces is listed as if lost -
 * in particular the last app listed may be the prediction of a directory slot
 * that an install of the same name spares.
 */
typedef struct {
    QEvt     super;
    uint32_t size;     /**< Bytes the image takes in the region once written:
                        *   for a compressed container its decoded length
                        *   (`image_len` of its header), not the file's.      */
    char     project[KODE_APP_PROJECT_MAX]; /**< The install's project name, as
                        *   its image's descriptor will say; empty when it is
                        *   not known.                                        */
    QActive *who;      /**< Where the entries and the end are posted.         */
} AppEvictReqEvt;

/**
 * @brief One app the install would evict (APP_EVICT_ENTRY_SIG).
 *
 * A copy, not a pointer into the directory, and one event per app, in the
 * order the apps would go.
 */
typedef struct {
    QEvt super;
    char name[KODE_APP_PROJECT_MAX];   /**< The app's project name.            */
    bool on_card;                      /**< `/sdcard/<name>.bin` is already
                                        *   there. false also when there is no
                                        *   card or the card did not say.      */
} AppEvictEntryEvt;

/**
 * @brief The end of an eviction answer (APP_EVICT_END_SIG).
 *
 * An answer with no entries is one of three things: nothing would go (`fits`
 * set, no victims), the image cannot fit the region at all (`fits` clear), or
 * AO_AppMgr could not look (`busy` set, and `fits` and `victims` then mean
 * nothing).
 *
 * When the request named no project, `victims` may count an app that an
 * install of that name would replace and so spare (AppEvictReqEvt).
 */
typedef struct {
    QEvt    super;
    uint8_t victims;   /**< How many apps the install would evict. At most
                        *   KODE_APP_EVICT_MAX of them came as entries; more
                        *   than that and the list was cut.                  */
    bool    fits;      /**< The image fits the region once those are gone;
                        *   false for one bigger than the region, or no
                        *   region.                                          */
    bool    busy;      /**< AO_AppMgr was writing, receiving, keeping or
                        *   answering another asker: ask again.              */
} AppEvictEndEvt;

/**
 * @brief Ask where an app is (APP_FIND_REQ_SIG).
 *
 * The answer is for the asker alone: it is posted there, never published.
 */
typedef struct {
    QEvt     super;
    char     project[KODE_APP_PROJECT_MAX]; /**< The app's project name.        */
    QActive *who;                           /**< Where the answer is posted.    */
} AppFindReqEvt;

/**
 * @brief Where an app is (APP_FIND_RSP_SIG).
 *
 * `installed` and `on_card` are the two places an app can be found: the apps
 * region, with its extent, or a file on the card, which is asked of the card
 * only when the app is not installed. When `busy` is set AO_AppMgr could not
 * look, and nothing else means anything: the asker asks again.
 */
typedef struct {
    QEvt     super;
    char     project[KODE_APP_PROJECT_MAX]; /**< The name that was asked.       */
    bool     installed;                     /**< The app is in the apps region. */
    uint32_t offset;                        /**< Its extent's offset in the
                                             *   region; 0 unless installed.    */
    uint32_t size;                          /**< Its extent's length in bytes;
                                             *   0 unless installed.            */
    bool     on_card;                       /**< `/sdcard/<project>.bin` is
                                             *   there. Asked only when the app
                                             *   is not installed; false also
                                             *   when there is no card or it
                                             *   did not say.                   */
    bool     busy;                          /**< AO_AppMgr was not idle: ask
                                             *   again.                         */
} AppFindRspEvt;

/**
 * @brief Remove every installed app but one (APP_KEEP_ONLY_REQ_SIG).
 *
 * The answer is for the asker alone: it is posted there, never published.
 */
typedef struct {
    QEvt     super;
    char     project[KODE_APP_PROJECT_MAX]; /**< The one app that stays.        */
    QActive *who;                           /**< Where the answer is posted.    */
} AppKeepOnlyReqEvt;

/**
 * @brief How a keep-only request went (APP_KEEP_ONLY_RSP_SIG).
 *
 * The status is a kode_status_t: KODE_OK, KODE_DENIED when AO_AppMgr was not
 * idle or the app named is not installed (nothing is erased), KODE_ERROR when
 * the directory could not be committed (nothing is erased). A data folder that
 * could not be removed does not change it: the app is gone, its folder is an
 * orphan.
 */
typedef struct {
    QEvt    super;
    uint8_t status;    /**< kode_status_t.                                      */
    uint8_t erased;    /**< How many apps were removed.                         */
} AppKeepOnlyRspEvt;

/**
 * @brief One entry found on a storage medium or on a path (STORAGE_ENTRY_SIG).
 *
 * The list travels as one event per entry rather than as a shared table: each
 * subscriber ends up owning its own copy, so nothing is read from
 * AO_Storage's thread while AO_Storage writes it.
 */
typedef struct {
    QEvt super;
    char     name[KODE_APP_FILE_MAX];
    uint32_t size;
    bool     from_sd;   /**< false = the internal FAT partition. */
    bool     is_dir;    /**< Only the path-based walk (`ls`) knows this; the
                          *   medium-based one (`sd_ls`) always sets it false,
                          *   because kdrv_storage_entry_cb carries no such
                          *   argument. A bool, not a size, so the cap this
                          *   event's own KODE_STORAGE_LIST_MAX bounds does
                          *   not move. */
    uint16_t pos;       /**< Its place in the listing, 0 first, counted
                          *   across pages and across both media: a
                          *   subscriber stores entry `pos` at index `pos`,
                          *   and 0 begins a new listing.                 */
} StorageEntryEvt;

/** @brief What to do to a medium (STORAGE_CMD_SIG). */
typedef struct {
    QEvt    super;
    uint8_t medium;      /**< kdrv_medium_t                                   */
    uint8_t op;          /**< storage_op_t - see kaos.h                        */
    bool    images_only; /**< LIST: only files that could be booted.          */
    bool    last;        /**< LIST: publish STORAGE_LIST_END_SIG afterwards.  */
    QActive *who;        /**< Who is told the command has run
                          *   (STORAGE_CMD_RSP_SIG); NULL for nobody.         */
} StorageCmdEvt;

/**
 * @brief Ask AO_Storage for a page of the bootable images on every medium
 *        (STORAGE_LIST_PAGE_SIG).
 *
 * Its own event rather than a field of StorageCmdEvt: that one fills a small
 * block on the target to the byte, and a page needs nothing else it carries.
 */
typedef struct {
    QEvt     super;
    uint16_t skip;        /**< Images to leave out: what earlier pages sent. */
    QActive *who;         /**< The asker; the page is posted to it alone.   */
} StorageListPageEvt;

/** @brief Open this file for reading (STORAGE_OPEN_REQ_SIG). */
typedef struct {
    QEvt     super;
    char     name[KODE_APP_FILE_MAX];
    QActive *who;       /**< Where the response and the data go.              */
} StorageOpenEvt;

/**
 * @brief Open this URL for reading (NET_STREAM_OPEN_REQ_SIG).
 *
 * Its own event rather than StorageOpenEvt's because a URL does not fit a
 * file name: everything after the open is the same conversation, and shares
 * StorageOpenRspEvt, StorageDataEvt and StorageBufEvt unchanged.
 */
typedef struct {
    QEvt     super;
    char     url[KODE_FLASH_SRC_MAX];
    QActive *who;       /**< Where the response and the data go.              */
} NetStreamOpenEvt;

/** @brief Whether the open worked, and how big the file is (STORAGE_OPEN_RSP_SIG). */
typedef struct {
    QEvt     super;
    uint32_t size;
    uint8_t  status;    /**< kode_status_t                                    */
    uint8_t  medium;    /**< Where it was found.                              */
} StorageOpenRspEvt;

/** @brief A borrowed buffer being handed back (STORAGE_NEXT_SIG). */
typedef struct {
    QEvt    super;
    uint8_t idx;
} StorageBufEvt;

/**
 * @brief A chunk of the open file (STORAGE_DATA_SIG).
 *
 * @c idx names a buffer belonging to AO_Storage; read it with storage_buf()
 * and hand it back with STORAGE_NEXT_SIG. Until you do, that buffer is yours
 * and AO_Storage will not touch it.
 */
typedef struct {
    QEvt     super;
    uint16_t len;
    uint8_t  idx;
    uint8_t  status;    /**< kode_status_t; anything but KODE_OK ends the read. */
    bool     eof;
} StorageDataEvt;

/**
 * @brief Write a whole buffer to a file (STORAGE_WRITE_REQ_SIG).
 *
 * The one place in kodeOS where an event carries a pointer to somebody else's
 * memory, and the exception is narrow enough to state completely: @c data must
 * stay valid and unchanging until STORAGE_WRITE_RSP_SIG comes back. Its only
 * caller today is a frozen display capture, which is exactly a buffer that
 * cannot move.
 *
 * The read side is a conversation because a reader cannot hold a file that
 * arrives in pieces from a medium nobody can pause. This side has the
 * opposite shape - the payload is already whole - so a conversation would be
 * ceremony around a pointer that is not going anywhere.
 */
typedef struct {
    QEvt        super;
    char        name[KODE_APP_FILE_MAX];
    const void *data;
    uint32_t    len;
    QActive    *who;    /**< Where the response goes.                         */
} StorageWriteEvt;

/** @brief How a write ended (STORAGE_WRITE_RSP_SIG). */
typedef struct {
    QEvt     super;
    uint32_t written;
    uint8_t  status;    /**< kode_status_t                                    */
    uint8_t  medium;    /**< Where it landed.                                 */
} StorageWriteRspEvt;

/**
 * @brief Ask what one file says it is (STORAGE_DESC_REQ_SIG).
 *
 * Two types rather than one shared struct: the request carries a QActive*
 * that has no business travelling back out in the answer, and the answer
 * carries three strings the request would otherwise allocate for nothing.
 */
typedef struct {
    QEvt     super;
    char     name[KODE_APP_FILE_MAX];
    QActive *who;
} StorageDescReqEvt;

/** @brief What that file says it is (STORAGE_DESC_RSP_SIG). */
typedef struct {
    QEvt    super;
    char    name[KODE_APP_FILE_MAX];             /**< Echoed: answers can cross. */
    char    project[32];                         /**< esp_app_desc_t.project_name. */
    char    display[KODE_APP_DISPLAY_MAX];       /**< Empty when unmanifested.   */
    char    category[KODE_APP_CATEGORY_MAX];     /**< Empty when unmanifested.   */
    uint8_t status;                              /**< kode_status_t of the read. */
} StorageDescRspEvt;

/** @brief Resolve one app's data folder (APPDATA_REQ_SIG). */
typedef struct {
    QEvt     super;
    char     project[32];
    QActive *who;
} AppDataReqEvt;

/** @brief Where an app's folder lives, or that it does not. */
typedef enum { APPDATA_MEDIUM_NONE = 0, APPDATA_MEDIUM_FS, APPDATA_MEDIUM_SD } AppDataMedium;

/** @brief How the icon read of app.json's `icon` field ended. */
typedef enum { APPDATA_ICON_OK = 0, APPDATA_ICON_NONE, APPDATA_ICON_SIZE,
               APPDATA_ICON_PATH, APPDATA_ICON_BUSY } AppDataIconStatus;

/** @brief The folder's fields, back to `who` (APPDATA_FIELDS_SIG). */
typedef struct {
    QEvt    super;
    char    project[32];
    uint8_t medium;        /**< AppDataMedium.                              */
    uint8_t json_status;   /**< kapp_json_status_t.                         */
    uint8_t icon_status;   /**< AppDataIconStatus.                          */
    bool    color_valid;
    uint8_t color[3];
    char    name[32];
    char    category[64];
    char    version[16];
    char    description[56];
} AppDataFieldsEvt;         /* 208 bytes plus QEvt: the medium tier */

/**
 * @brief One third of the icon, back to `who` (APPDATA_ICON_SIG).
 *
 * KAPP_ICON_BYTES (12288) does not fit one KODE_RECV_CHUNK-wide jumbo block,
 * so the icon travels as three: seq 0..2, each a full 4096-byte third.
 */
typedef struct {
    QEvt     super;
    char     project[32];
    uint8_t  seq;          /**< 0..2: which 4096-byte third.               */
    uint8_t  data[KODE_RECV_CHUNK];
} AppDataIconEvt;           /* the jumbo tier's second tenant */

/**
 * @brief How a delete of one app's data folder ended (APPDATA_RM_RSP_SIG).
 *
 * Four outcomes, spelled in kode_status_t rather than in an enum of this
 * event's own, because each of them is already one of its members and the
 * console turns every one into the single word an operator reads:
 * KODE_OK the folder is gone, KODE_ERROR there was none, KODE_DENIED a
 * transfer was in flight, KODE_IO_ERROR the folder is there and something
 * in it would not go.
 */
typedef struct {
    QEvt    super;
    char    project[KODE_APP_PROJECT_MAX]; /**< Echoed: an asker with a delete
                                            *   of its own in flight takes only
                                            *   the answer that names it.     */
    uint8_t status;       /**< kode_status_t, as the four lines above read it. */
} AppDataRmRspEvt;

/** @brief What became of an app's data folder on one medium. */
typedef enum {
    APP_WIPE_FOLDER_NONE = 0,   /**< There was none.                          */
    APP_WIPE_FOLDER_REMOVED,    /**< It is gone.                              */
    APP_WIPE_FOLDER_LEFT        /**< It is there and something in it would not go. */
} AppWipeFolder;

/** @brief What a wipe of one app did on one medium. */
typedef struct {
    bool    present;   /**< The medium was mounted and its root was walked.   */
    uint8_t files;     /**< Images of the project removed.                    */
    uint8_t left;      /**< Images of the project that would not go.          */
    uint8_t folder;    /**< AppWipeFolder.                                    */
} AppWipeMedium;

/**
 * @brief How a wipe of one app's files ended (STORAGE_APP_WIPE_RSP_SIG).
 *
 * KODE_OK says the wipe ran to its end, and the two media say what it found
 * and whether it went: a file left behind is in `left`, not in the status.
 * KODE_DENIED a transfer was in flight and KODE_NO_RESOURCES there was no
 * memory for the list of files: nothing was touched.
 */
typedef struct {
    QEvt          super;
    char          project[KODE_APP_PROJECT_MAX]; /**< Echoed: answers can cross. */
    uint8_t       status;  /**< kode_status_t, as the lines above read it.     */
    AppWipeMedium fs;      /**< The internal partition.                        */
    AppWipeMedium sd;      /**< The card.                                      */
} StorageAppWipeRspEvt;

/**
 * @brief How an erase ended (APP_ERASE_DONE_SIG).
 *
 * KODE_OK nothing of the app is left; KODE_ERROR there was nothing of that
 * name anywhere; KODE_IO_ERROR something would not go, and the two media say
 * what; KODE_DENIED AO_AppMgr or AO_Storage was busy and KODE_TIMEOUT
 * AO_Storage did not answer: the files were not looked at.
 */
typedef struct {
    QEvt          super;
    char          name[KODE_APP_PROJECT_MAX]; /**< The project asked about.    */
    uint8_t       status;  /**< kode_status_t, as the lines above read it.     */
    bool          image;   /**< An installed image was removed.                */
    AppWipeMedium fs;      /**< The internal partition.                        */
    AppWipeMedium sd;      /**< The card.                                      */
} AppEraseDoneEvt;

/**
 * @brief The result of a self-test that could not be answered on the spot
 *        (TEST_RESULT_SIG).
 *
 * Some tests belong to an AO that is not the one running `test`, so the
 * answer arrives later. The console counts these to keep the "N/M passed"
 * tally honest rather than reporting a number it guessed.
 *
 * @c origin is who asked for the run it ends: every owner but AO_Buttons and
 * AO_Touch sets TEST_ORIGIN_AUTOTEST, and those two echo the origin of the
 * request (TestReqEvt). AO_Autotest and the console act only on a verdict of
 * TEST_ORIGIN_AUTOTEST, so another client's run never closes a row or a
 * command that is not its own. TEST_SKIP_SIG has the same shape and the same
 * rule.
 */
typedef struct {
    QEvt    super;
    char    name[KODE_TEST_NAME_LEN];
    bool    ok;
    uint8_t origin;   /**< TEST_ORIGIN_AUTOTEST, or the client that asked (TestReqEvt). */
} TestResultEvt;

/** @brief The groups a self-test row may belong to, one bit each. A group is
 *         a word that runs several rows; a row in none answers by name.
 *         A row whose owner the app does not link is never registered, so it
 *         is in no group and answers to no name. */
#define AUTOTEST_G_NONE 0x00U  /**< By name only. `all` is not a group: it is
                                *   the E5 final test, AO_TestDot's, which
                                *   asks for its rows one at a time by name. */
#define AUTOTEST_G_MAIN 0x02U  /**< `main`: the main PCB's I2C chips and its
                                *   two crystals - what a main board answers
                                *   with no panel, no antenna PCB, no card
                                *   and nobody at the glass.                  */
#define AUTOTEST_G_USER 0x04U  /**< `user`: what the owner of a unit can run
                                *   with nothing but the charger - first the
                                *   rows that need nobody, then a hold, then
                                *   the rows that need hands. Nothing in it
                                *   writes to a card's files or to a tag.     */

/** @brief Who asked for a run. Every answer is published, so each carries
 *         the client it is for and a client reads only its own. */
#define AUTOTEST_CLIENT_CONSOLE 1U
#define AUTOTEST_CLIENT_GUI     2U
#define AUTOTEST_CLIENT_TEST_DOT 3U  /**< AO_TestDot: the rows of the E5 final test. */

/** @brief How AO_Autotest took a run request. */
#define AUTOTEST_TAKEN   0U    /**< The run starts; its rows follow.          */
#define AUTOTEST_BUSY    1U    /**< A run is in flight; this one is refused.  */
#define AUTOTEST_UNKNOWN 2U    /**< No row answers to that name.              */

/** @brief Where a self-test row is. */
#define AUTOTEST_ROW_RUNNING 0U /**< Asked; no verdict yet.                   */
#define AUTOTEST_ROW_PASS    1U /**< Its owner said pass.                     */
#define AUTOTEST_ROW_FAIL    2U /**< Its owner said fail.                     */
#define AUTOTEST_ROW_TIMEOUT 3U /**< Nobody judged it: its owner said nothing
                                 *   in its budget, or a step of it went
                                 *   unanswered and none was answered no.     */
#define AUTOTEST_ROW_SKIPPED 4U /**< What it needs is not there.              */
#define AUTOTEST_ROW_STATES  5U /**< How many states there are.               */

/**
 * @brief The rows, by a number that never changes.
 *
 * The kept report stores these, so a build reads the report an older one
 * wrote: a number is never reused and a new row takes the next one.
 */
#define AUTOTEST_ID_NONE        0U
#define AUTOTEST_ID_SCREENSHOT  1U
#define AUTOTEST_ID_JPEG        2U
#define AUTOTEST_ID_FS          3U
#define AUTOTEST_ID_PMIC        4U
#define AUTOTEST_ID_GAUGE       5U
#define AUTOTEST_ID_IOEXP       6U
#define AUTOTEST_ID_CLOCKS      7U
#define AUTOTEST_ID_IMU         8U
#define AUTOTEST_ID_MAG         9U
#define AUTOTEST_ID_HAPTIC     10U
#define AUTOTEST_ID_SD         11U
#define AUTOTEST_ID_C5         12U
#define AUTOTEST_ID_IR         13U
#define AUTOTEST_ID_NFC        14U
#define AUTOTEST_ID_RFID       15U
#define AUTOTEST_ID_AUDIO      16U
#define AUTOTEST_ID_BUTTONS    17U
#define AUTOTEST_ID_LED        18U
#define AUTOTEST_ID_DISPLAY    19U
#define AUTOTEST_ID_TP         20U
#define AUTOTEST_ID_MEM        21U
#define AUTOTEST_ID_BOOT       22U
#define AUTOTEST_ID_APPS       23U
#define AUTOTEST_ID_KFW        24U
#define AUTOTEST_ID_ID         25U
#define AUTOTEST_ID_BLE_CONSOLE 26U
#define AUTOTEST_ID_NET        27U
#define AUTOTEST_ID_SD_USER    28U
#define AUTOTEST_ID_C5_USER    29U
#define AUTOTEST_ID_NFC_FIELD  30U
#define AUTOTEST_ID_RFID_FIELD 31U
#define AUTOTEST_ID_POWER_PLUG 32U
#define AUTOTEST_ID_NFC_CARD   33U
#define AUTOTEST_ID_RFID_CARD  34U
#define AUTOTEST_ID_PSRAM      35U

/** @brief The most rows one run may hold. */
#define AUTOTEST_RUN_MAX       24U

/** @brief Run a group of rows, or one by name (AUTOTEST_RUN_REQ_SIG). */
typedef struct {
    QEvt    super;
    uint8_t client;                   /**< AUTOTEST_CLIENT_*.                 */
    uint8_t group;                    /**< AUTOTEST_G_*; NONE means @c name. */
    char    name[KODE_TEST_NAME_LEN]; /**< The row, when @c group is NONE.   */
} AutotestRunReqEvt;

/** @brief A request that carries only who sends it (AUTOTEST_CANCEL_REQ_SIG,
 *         AUTOTEST_CONTINUE_SIG, AUTOTEST_REPORT_REQ_SIG). */
typedef struct {
    QEvt    super;
    uint8_t client;                   /**< AUTOTEST_CLIENT_*.                 */
} AutotestClientEvt;

/** @brief The answer to a run request (AUTOTEST_BEGIN_SIG). */
typedef struct {
    QEvt    super;
    uint8_t client;                   /**< Whose request this answers.        */
    uint8_t status;                   /**< AUTOTEST_TAKEN, _BUSY or _UNKNOWN. */
    uint8_t rows;                     /**< How many rows the run holds.       */
} AutotestBeginEvt;

/** @brief One row starting or ending (AUTOTEST_ROW_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  client;                  /**< Whose run this row belongs to.     */
    uint8_t  id;                      /**< AUTOTEST_ID_*.                     */
    uint8_t  pos;                     /**< Its place in the run, from 1.      */
    uint8_t  state;                   /**< AUTOTEST_ROW_*.                    */
    char     name[KODE_TEST_NAME_LEN];
    uint32_t budget_ms;               /**< RUNNING: the most the owner may
                                       *   take, 0 for a row judged on the
                                       *   spot. 0 otherwise.                 */
} AutotestRowEvt;

/** @brief The run waits for its client (AUTOTEST_HOLD_SIG). */
typedef struct {
    QEvt    super;
    uint8_t client;                   /**< Who has to answer.                 */
} AutotestHoldEvt;

/** @brief The run is over (AUTOTEST_END_SIG). */
typedef struct {
    QEvt    super;
    uint8_t client;                   /**< Whose run it was.                  */
    uint8_t tally[AUTOTEST_ROW_STATES]; /**< Rows that ended in each state,
                                       *   by AUTOTEST_ROW_*.                 */
    bool    cancelled;                /**< It ended before its last row.      */
} AutotestEndEvt;

/** @brief One row of the kept report, or its end (AUTOTEST_REPORT_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  client;                  /**< Who asked for the report.          */
    bool     last;                    /**< The end marker: no row in it.      */
    uint8_t  id;                      /**< A row: AUTOTEST_ID_*.              */
    uint8_t  state;                   /**< A row: how it ended.               */
    uint8_t  rows;                    /**< The marker: rows in the report; 0
                                       *   when none was ever kept.           */
    uint8_t  problems;                /**< The marker: rows that failed or
                                       *   were never judged.                 */
    uint32_t when;                    /**< The marker: UTC seconds of the run,
                                       *   0 when the clock was not set.      */
} AutotestReportEvt;

/** @brief Run the coprocessor's row (RADIO_TEST_REQ_SIG). */
typedef struct {
    QEvt super;
    bool user;      /**< The owner's row: the link, the snapshot, a sweep that
                     *   completes and the controller, with no floor on what
                     *   the sweep hears. false: the production line's.      */
} RadioTestReqEvt;

/** @brief What a reader's row is asked to judge (NFC_TEST_REQ_SIG,
 *         RFID_TEST_REQ_SIG). One set for both readers. */
#define READER_TEST_ROW    0U  /**< The production line's: the fixture's tag. */
#define READER_TEST_FIELD  1U  /**< The part and its field, nothing in front
                                *   of it. Nothing is read or written.        */
#define READER_TEST_CARD   2U  /**< Whatever tag is held to it: detected is a
                                *   pass, none is TEST_SKIP_SIG. Detection
                                *   only.                                     */

/** @brief Run a reader's row (NFC_TEST_REQ_SIG, RFID_TEST_REQ_SIG). */
typedef struct {
    QEvt    super;
    uint8_t mode;   /**< READER_TEST_*.                                       */
} ReaderTestReqEvt;

/** @brief The steps of the `power_plug` row. */
#define POWER_TEST_ASK_PLUG    0U  /**< Waiting for the charger.              */
#define POWER_TEST_PLUGGED     1U  /**< It is there; the chips are judged.    */
#define POWER_TEST_ASK_UNPLUG  2U  /**< Waiting for it to go.                 */
#define POWER_TEST_PASS        3U  /**< This step is answered.                */
#define POWER_TEST_FAIL        4U  /**< A chip failed with the charger in.    */
#define POWER_TEST_TIMEOUT     5U  /**< Nobody did what was asked.            */

/** @brief One step of the `power_plug` row (POWER_TEST_SIG). */
typedef struct {
    QEvt    super;
    uint8_t step;                     /**< 1-based.                           */
    uint8_t total;                    /**< Steps in the row.                  */
    uint8_t phase;                    /**< POWER_TEST_*.                      */
} PowerTestEvt;

/** @brief How long a boot request stands (BootRunEvt.mode). */
typedef enum {
    KBOOT_RUN_ONCE,     /**< NEW: one boot, then any reset comes home.        */
    KBOOT_RUN_STICKY,   /**< ALWAYS: re-armed across USB-Serial/JTAG resets;
                         *   what the mailbox asks for, so a sketch survives
                         *   the Serial Monitor opening the port.             */
} kboot_run_mode_t;

/**
 * @brief Boot a specific app extent (BOOT_APP_RUN_SIG).
 *
 * @c offset and @c size are region-relative, exactly as kdrv_apps_req_set()
 * wants them - this event exists so the caller (AO_AppMgr, eventually) never
 * has to know that "which app" and "who boots" are two different owners.
 * @c mode picks the request state AO_Boot writes: KBOOT_RUN_ONCE becomes NEW,
 * KBOOT_RUN_STICKY becomes ALWAYS.
 */
typedef struct {
    QEvt             super;
    uint32_t         offset;
    uint32_t         size;
    kboot_run_mode_t mode;
} BootRunEvt;

/** @brief Ask AO_Boot for a factory reset (FACTORY_RESET_REQ_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  end;       /**< KBOOT_RESET_RESTART or KBOOT_RESET_SHIP.           */
    QActive *who;       /**< Where FACTORY_RESET_RSP_SIG is posted.             */
} FactoryResetReqEvt;

/** @brief How far a factory reset got (FACTORY_RESET_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t step;       /**< kboot_reset_step_t: the step that stopped the
                         *   reset, or KBOOT_RESET_STEP_DONE.                  */
    uint8_t status;     /**< kode_status_t of that step.                        */
} FactoryResetRspEvt;

/** @brief One button changed state (BUTTON_EVT_SIG).
 *
 * Published on change only, already debounced, so a subscriber never has to
 * know that a tactile switch bounces. The event says when as well as what: a
 * subscriber that decides on elapsed time (a double press) cannot stamp the
 * event itself, because it may be dispatched late. */
typedef struct {
    QEvt     super;
    uint8_t  id;        /**< kdrv_button_t */
    bool     pressed;
    uint32_t t_ms;      /**< khal_time_ms() when the edge was confirmed. It
                         *   wraps at about 49 days, so an interval is an
                         *   unsigned subtraction of two stamps. */
} ButtonEvt;

/** @brief The top or the bottom button is being held (BUTTON_HOLD_SIG).
 *
 * Published once per press, KODE_BUTTON_HOLD_MS after the debounce
 * confirmed it and only if the button is still down at that moment. The
 * release that follows arrives as the usual ButtonEvt. The console's
 * `btn id=top|bot state=hold` publishes the same event on request, with no
 * press before it and no release after it: a host reproducing a thumb sends
 * the press, the hold and the release. */
typedef struct {
    QEvt    super;
    uint8_t id;        /**< kdrv_button_t: KDRV_BTN_TOP or KDRV_BTN_BOT. */
} ButtonHoldEvt;

/** @brief The top button was pressed twice in quick succession (BUTTON_DOUBLE_SIG).
 *
 * Published once, right after the ButtonEvt of the second press, when that
 * press was confirmed within KODE_BUTTON_DOUBLE_MS of the first press's
 * release and the first press never reached hold. A press that completes a
 * double is spent: it pairs with nothing after it, so three quick presses
 * make one double and not two. The press and release ButtonEvts are
 * unchanged, so a subscriber that wants the double ignores the second press
 * itself. Only the top button doubles, so @c id is always KDRV_BTN_TOP; it is
 * carried so the event reads like its siblings.
 *
 * Presses and releases injected by the console never make one: only the
 * confirmed edges of AO_Buttons feed the detector, and `btn id=top
 * state=double` publishes this event on request. */
typedef struct {
    QEvt     super;
    uint8_t  id;        /**< kdrv_button_t: always KDRV_BTN_TOP. */
    uint32_t t_ms;      /**< The second press's t_ms: khal_time_ms() when it was
                         *   confirmed, or when the console injected the double.
                         *   It wraps at about 49 days, so an interval is an
                         *   unsigned subtraction of two stamps. */
} ButtonDoubleEvt;

/** @brief Turn the pad with the glass (BUTTONS_ROTATE_SIG).
 *
 * Always one of four static events, so a request is never lost to a dry
 * pool. */
typedef struct {
    QEvt    super;
    uint8_t quarter_turns;   /**< 0..3: 0, 90, 180 or 270 degrees. */
} ButtonsRotateEvt;

/**
 * @brief One reading of the battery (POWER_EVT_SIG).
 *
 * Published on every poll rather than on change: a value that has not moved is
 * still the answer to "how is the battery", and the GUI wants it after a
 * repaint as much as after a change.
 */
typedef struct {
    QEvt     super;
    uint16_t vbat_mv;
    uint16_t vbus_mv;
    int16_t  current_ma;   /**< Into the cell positive, out negative. */
    uint8_t  soc_pct;
    uint8_t  charging;     /**< kdrv_bq25896_chrg_stat_t              */
    uint8_t  fault;        /**< BQ25896 REG0C, 0 when all is well.    */
    bool     vbus;
    bool     boost;        /**< The chip is making its own 5 V (VBUS_STAT OTG). */
    uint8_t  ntc;          /**< NTC_FAULT, live: 0 normal, 2 warm, 3 cool, 5 cold, 6 hot.
                            *   Not 0, charging is held (AO_Power's charge guard). */
} PowerEvt;

/* What a field request asks of a chip. Three verbs, shared by every chip
 * driver that carries a field table - the PMIC, the gauge, the expander - so
 * that the console speaks one language to all of them. */
#define FIELD_OP_GET      0U   /**< Read the field.                       */
#define FIELD_OP_SET      1U   /**< Write it, then read it back.          */
#define FIELD_OP_DUMP     2U   /**< Log the whole chip; `field` is unused. */

/* Which chip a power field request names. The POWER_OP_* names are the same
 * three verbs; they predate the second chip family and stay as aliases. */
#define POWER_CHIP_PMIC   0U   /**< The BQ25896, kdrv_bq25896_field_t.    */
#define POWER_CHIP_GAUGE  1U   /**< The CW2217, kdrv_cw2217_field_t.      */
#define POWER_CHIP_PLUG   2U   /**< Not a chip: the `power_plug` row, both
                                *   chips judged around a charger going in
                                *   and coming out. A test request only.   */
#define POWER_OP_GET      FIELD_OP_GET
#define POWER_OP_SET      FIELD_OP_SET
#define POWER_OP_DUMP     FIELD_OP_DUMP

/**
 * @brief Read, write or dump one field of one power chip (POWER_FIELD_REQ_SIG).
 *
 * The field is the driver's own enum value; whoever posts this has already
 * resolved the name, so AO_Power never handles a string. Every request is
 * answered with a PowerFieldEvt, a refusal included.
 */
typedef struct {
    QEvt     super;
    uint8_t  chip;      /**< POWER_CHIP_*                              */
    uint8_t  op;        /**< POWER_OP_*                                */
    uint16_t field;     /**< kdrv_bq25896_field_t or kdrv_cw2217_field_t */
    int32_t  value;     /**< For POWER_OP_SET, in the field's unit.    */
} PowerFieldReqEvt;

/**
 * @brief The answer to a PowerFieldReqEvt (POWER_FIELD_SIG).
 *
 * For a read, `value` is the field; for a write, the value read back after
 * it; for a dump, nothing. `rc` is the driver's verdict: KODE_DENIED for a
 * field that cannot be written or a value out of its range, KODE_ERROR when
 * the chip did not answer or did not keep the write.
 */
typedef struct {
    QEvt     super;
    uint8_t  chip;
    uint8_t  op;
    uint16_t field;
    uint8_t  rc;        /**< kode_status_t                             */
    int32_t  value;
} PowerFieldEvt;

/* How much a power chip's self-test judges. The main station proves the
 * charge path; the assembled Kode's final test only asks that the chips
 * answer and measure, because its rows run with the Kode under load and its
 * cell at whatever charge it arrived with. */
#define POWER_DEPTH_FULL    0U   /**< The chip doing its job: charging, SYS
                                  *   regulated, the gauge's ammeter proven. */
#define POWER_DEPTH_ANSWER  1U   /**< The chip answering and measuring: no
                                  *   verdict on the charge, the load or the
                                  *   current.                              */

/**
 * @brief Run one power chip's self-test (POWER_TEST_REQ_SIG).
 *
 * Communication and identity decide the verdict; what the chip measures and
 * what state it is in are reported alongside, as log lines, because they are
 * what a person running the test is there to read.
 */
typedef struct {
    QEvt    super;
    uint8_t chip;       /**< POWER_CHIP_*                              */
    uint8_t depth;      /**< POWER_DEPTH_*                             */
} PowerTestReqEvt;

/**
 * @brief What the expander's eight pins read (IOEXP_INPUT_SIG).
 *
 * One transaction, all eight bits, so a diagonal on the pad is two buttons
 * at the same instant. Bit n is Pn (KBSP_IOEXP_BIT_*), the pin's level, not
 * its meaning - decoding "low means pressed" is the subscriber's, because the
 * same event carries a button, the touch controller's IRQ and the gauge's.
 */
typedef struct {
    QEvt    super;
    uint8_t levels;     /**< The input register.                        */
    uint8_t changed;    /**< Bits that differ from the previous read.  */
    bool    ok;         /**< false: the chip did not answer; levels and
                         *   changed are 0 and mean nothing.            */
} IoexpInputEvt;

/**
 * @brief Read, write or dump one field of the expander (IOEXP_FIELD_REQ_SIG).
 *
 * The field is kdrv_tca6408a_field_t, already resolved from its name by
 * whoever asks. Answered to @c who with an IoexpFieldEvt, a refusal included.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the answer goes.                    */
    uint8_t  op;        /**< FIELD_OP_*                                */
    uint16_t field;     /**< kdrv_tca6408a_field_t                     */
    int32_t  value;     /**< For FIELD_OP_SET.                         */
} IoexpFieldReqEvt;

/**
 * @brief The answer to an IoexpFieldReqEvt (IOEXP_FIELD_RSP_SIG).
 *
 * For a read, `value` is the field; for a write, the value read back after
 * it; for a dump, nothing. `rc` is the driver's verdict: KODE_DENIED for a
 * field that cannot be written or a value out of its range, KODE_ERROR when
 * the chip did not answer or did not keep the write.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint16_t field;
    uint8_t  rc;        /**< kode_status_t                             */
    int32_t  value;
} IoexpFieldEvt;

/* ---- AO_Ir -----------------------------------------------------------
 * addr, cmd, symbols and nec below mirror kdrv_ir_frame_t (kdrv_ir.h) field
 * for field, and that is deliberate rather than lazy: kqp's own manifest
 * declares REQUIRES qpc khal and nothing else, so kqp_signals.h - a header
 * every layer above kdrv includes - must not spell kdrv_ir_frame_t, or kqp
 * would gain a dependency on kdrv and invert the layer order the rest of
 * the tree relies on. The house already carries a driver's own type across
 * this boundary as plain integers rather than including it, three times
 * over: PowerEvt's `charging` (kdrv_bq25896_chrg_stat_t), ButtonEvt's `id`
 * (kdrv_button_t), and the `medium` field several storage events share
 * (kdrv_medium_t). IrRxEvt is the same rule applied to a whole struct
 * instead of one field of it - do not "tidy" this into a wrapped
 * kdrv_ir_frame_t; that is precisely the change this paragraph exists to
 * head off.
 */

/** @brief One frame heard by the receiver (IR_RX_SIG). */
typedef struct {
    QEvt     super;
    uint16_t addr;      /**< NEC address, when @c nec. Mirrors
                         *   kdrv_ir_frame_t.addr.                        */
    uint16_t cmd;       /**< NEC command, when @c nec. Mirrors
                         *   kdrv_ir_frame_t.cmd.                         */
    uint16_t symbols;   /**< How many symbols arrived, decoded or not.
                         *   Mirrors kdrv_ir_frame_t.symbols.             */
    bool     nec;       /**< Whether the frame decoded as NEC. Mirrors
                         *   kdrv_ir_frame_t.nec.                         */
} IrRxEvt;

/**
 * @brief An answer that is only a status: kode_sys_ir_send_raw() (IR_SEND_RAW_RSP_SIG),
 *        kode_cert_nfc_carrier() (NFC_CERT_RSP_SIG) and kode_cert_rfid_carrier()
 *        (RFID_CERT_RSP_SIG).
 */
typedef struct {
    QEvt    super;
    uint8_t rc;     /**< kode_status_t: what the owner came to; KODE_DENIED
                     *   when the device was held, KODE_TIMEOUT when a send
                     *   waited too long to start.                        */
} StatusRspEvt;

/** @brief One infrared symbol: kdrv_ir_symbol_t field for field, for AudioInfo's reason. */
typedef struct {
    uint16_t mark_us;   /**< Carrier on, in microseconds.                  */
    uint16_t space_us;  /**< The space after it, in microseconds.          */
} IrSymbol;

/** @brief What one received frame amounted to: kdrv_ir_frame_t field for field. */
typedef struct {
    uint16_t addr;      /**< NEC address, when @c nec.                     */
    uint16_t cmd;       /**< NEC command, when @c nec.                     */
    uint16_t symbols;   /**< How many symbols arrived, decoded or not.     */
    bool     nec;       /**< Whether the frame decoded as NEC.             */
} IrFrame;

/** @brief Answers kode_sys_ir_capture() (IR_CAPTURE_RSP_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  rc;    /**< kode_status_t: KODE_OK with a frame heard,
                     *   KODE_TIMEOUT when the window closed empty,
                     *   KODE_IO_ERROR with no infrared block, KODE_DENIED
                     *   when AO_Ir was busy or the capture was displaced. */
    uint32_t n;     /**< Symbols written to the lent buffer; 0 unless
                     *   @c rc is KODE_OK.                                 */
} IrCaptureRspEvt;

/** @brief Answers kode_sys_ir_last() (IR_LAST_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t rc;     /**< kode_status_t: KODE_OK; the frame is in @c frame
                     *   only when @c have.                                */
    bool    have;   /**< Whether anything was ever heard.                  */
    IrFrame frame;  /**< The last frame; zero unless @c have.              */
} IrLastRspEvt;

/**
 * @brief Transmit one NEC frame (IR_SEND_REQ_SIG).
 *
 * NEC only: a frame of raw symbols travels as IrSendRawEvt instead, lent
 * by pointer. At two
 * uint16_t fields this fits KqpSmallBlock (sizeof
 * TouchEvt) with room to spare, so it lands in the small pool
 * (KQP_SMALL_POOL_COUNT), not the jumbo tier a symbol array would have
 * forced it into.
 */
typedef struct {
    QEvt     super;
    uint16_t addr;
    uint16_t cmd;
} IrSendEvt;

/**
 * @brief Fill this buffer from the next frame (IR_CAPTURE_REQ_SIG).
 *
 * @c out is untyped rather than a kdrv_ir_symbol_t*, for the same reason
 * IrRxEvt's fields are plain integers and not a wrapped kdrv_ir_frame_t:
 * naming that type here would mean including kdrv_ir.h, and kqp's own
 * CMakeLists.txt is explicit that khal is the only outside component this
 * public header is allowed to name (REQUIRES qpc khal - see the comment
 * there on why khal is public at all). The precedent is StorageWriteEvt.data,
 * the one other event in kodeOS that carries a pointer to somebody else's
 * memory - also declared `void *` rather than its real type, for the same
 * reason. Both sides of this conversation - the requester that filled in
 * @c out and AO_Ir, which never dereferences it as anything but a byte
 * range it copies into - already have kdrv_ir.h in scope to know what it
 * really points to; this header does not need to.
 *
 * The pointers must outlive the request, and @c ms does not make them do so.
 * A requester waits with a deadline, and a deadline can run out while this
 * event is still in AO_Ir's queue - so the memory named here may not be a
 * stack frame the requester is free to leave. It has to be storage that
 * survives a requester which has given up and returned, because AO_Ir writes
 * through these pointers whenever it gets to them.
 *
 * @c gen and @c ack are how the requester tells its own answer from an
 * earlier one it abandoned. AO_Ir writes @c gen into @c ack immediately
 * before it signals @c done, so a requester woken by a token it did not
 * earn - a binary semaphore carries one token whoever left it - reads a
 * generation that is not its own and knows to keep waiting. That write is a
 * release and the requester's read is an acquire, which is also what orders
 * everything AO_Ir wrote through @c out and @c n before the requester reads
 * any of it, on a board whose two cores need telling.
 */
typedef struct {
    QEvt        super;
    IrSymbol   *out;    /**< At least @c cap entries.                     */
    size_t      cap;    /**< How many symbols @c out can hold.            */
    size_t     *n;      /**< How many were actually written.              */
    khal_sem_t  done;   /**< Posted by AO_Ir when @c out and @c n are filled. */
    uint32_t    ms;     /**< How long the requester waits. 0 is answered at
                         *   once, empty, since nobody is waiting at all.  */
    uint32_t    posted_ms; /**< khal_time_ms() when it was posted: a capture
                         *   held for the link is answered at @c ms after. */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                         */
    QActive    *ask;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    IrCaptureRspEvt *rsp; /**< Its answer, taken with the request; @c out is
                         *   then the asker's, lent until the answer.      */
} IrCaptureEvt;

/**
 * @brief Transmit the frame the requester lends (IR_SEND_RAW_REQ_SIG).
 *
 * The symbols travel by loan, not in the event: a full frame is a kilobyte,
 * which only the jumbo pool holds. @c sym stays the requester's until it is
 * answered; AO_Ir copies it into its own frame when it starts the call.
 * The answer is the IrCaptureEvt shape: the driver's status through @c rc,
 * @c gen written back through @c ack, then @c done.
 */
typedef struct {
    QEvt        super;
    const IrSymbol *sym;    /**< @c n symbols, mark first.                 */
    uint16_t    n;          /**< How many symbols.                         */
    uint32_t    carrier_hz; /**< The carrier to send on.                   */
    uint32_t    posted_ms;  /**< khal_time_ms() when it was posted.        */
    uint32_t    start_ms;   /**< How long after @c posted_ms AO_Ir may still
                             *   start the call: past it the requester has
                             *   stopped waiting, and the frame must not go
                             *   out. Answered KODE_TIMEOUT instead.        */
    uint32_t   *rc;         /**< Where the driver's status goes.           */
    khal_sem_t  done;       /**< Posted by AO_Ir once @c rc is written.    */
    uint32_t    gen;        /**< The requester's tag for this request.     */
    uint32_t   *ack;        /**< Where @c gen is written back, just before
                             *   @c done. May be NULL.                     */
    QActive    *ask;        /**< The AO @c rsp is posted to; NULL for a waiter.
                             *   @c sym is then the asker's, lent until the
                             *   answer.                                    */
    StatusRspEvt *rsp;      /**< Its answer, taken with the request.       */
} IrSendRawEvt;

/**
 * @brief Copy out the last frame AO_Ir heard (IR_LAST_REQ_SIG).
 *
 * @c out is untyped rather than a kdrv_ir_frame_t*, for the reason given at
 * the top of this section and already spelt out on IrCaptureEvt.out: naming
 * that type here would mean including kdrv_ir.h, which kqp's own
 * CMakeLists.txt forbids this header.
 *
 * The pointers must outlive the request, exactly as IrCaptureEvt's must.
 * There is no @c ms field because there is nothing here to wait for: this
 * asks for state AO_Ir already holds, so it is answered - written, and
 * @c done signalled - inside the very step that receives the event, and AO_Ir
 * never holds these pointers beyond one run-to-completion step. What that
 * bounds is how long AO_Ir keeps them once it has the event, not how long the
 * event waits in the queue for a step of its own, so a requester that waits
 * with a deadline of its own can still return first.
 *
 * @c gen and @c ack carry the same meaning as IrCaptureEvt's, for the same
 * reason: one abandoned request's answer must not be read as the next one's,
 * and the release/acquire pair on @c ack is what orders @c out and @c have
 * against a reader on the other core.
 */
typedef struct {
    QEvt        super;
    void       *out;    /**< kdrv_ir_frame_t *, the requester's.           */
    bool       *have;   /**< Set false when nothing has ever been heard, in
                         *   which case @c out is left alone.              */
    khal_sem_t  done;   /**< Posted by AO_Ir when @c out and @c have are
                         *   filled.                                       */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                         */
    QActive    *ask;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    IrLastRspEvt *rsp;  /**< Its answer, taken with the request.           */
} IrLastEvt;

/**
 * @brief Ask AO_Ir for its own status (IR_INFO_REQ_SIG).
 *
 * No key beyond @c who: there is nothing here for a caller to choose, the
 * console's `ir` asks for the whole thing every time. Answered to @c who
 * with an IrInfoEvt, the IoexpFieldReqEvt shape.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the answer goes.                      */
} IrInfoReqEvt;

/**
 * @brief The block's own status, in answer to an IrInfoReqEvt (IR_INFO_RSP_SIG).
 *
 * @c up is whether both channels came up at kdrv_ir_init(). One flag rather
 * than two: the block owns no state that could tell the transmitter and the
 * receiver apart here, since they come up together and a driver that failed
 * to open one failed to open the block. @c deaf is the echo suppression
 * window - true for the length of this board's own last transmission plus
 * its tail, during which nothing the receiver hears is published.
 *
 * @c rx_high is one resting-level read of the receiver, taken while this
 * request was served - kdrv_ir_rx_idle(), which costs nothing and emits
 * nothing, so serving this never touches the RX channel's arming or a
 * capture in flight. Meaningless when @c up is false.
 *
 * addr/cmd/symbols/nec mirror kdrv_ir_frame_t, and for the reason given at
 * the top of this section they are not that type: the last capture the
 * receiver produced, decoded or not, whether it was published, suppressed as
 * this board's own echo, or consumed by a production row. Meaningless unless
 * @c have_last. @c age_ms is how long ago it arrived, in khal_time_ms()
 * terms; meaningless unless @c have_last.
 */
typedef struct {
    QEvt     super;
    bool     up;
    bool     deaf;
    bool     rx_high;
    bool     have_last;
    uint16_t addr;
    uint16_t cmd;
    uint16_t symbols;
    bool     nec;
    uint32_t age_ms;
} IrInfoEvt;

/* ---- AO_Nfc ----------------------------------------------------------
 * The same rule the AO_Ir section opens with, and for the same reason: kqp
 * declares REQUIRES qpc khal and nothing else, so this header may not spell
 * knfc_tag_t. NfcTagEvt therefore mirrors that struct field for field as
 * plain integers, and every buffer that travels here is `void *`. Do not
 * "tidy" either into the real type - that inverts the layer order the whole
 * tree rests on.
 */

/** @brief The longest NDEF text a card carries. Mirrors KNFC_EMU_NDEF_MAX (knfc_emulate.h). */
#define KQP_NFC_EMU_NDEF_MAX  96U

/** @brief The longest ready-made NDEF message a card carries. Mirrors KNFC_EMU_RAW_MAX. */
#define KQP_NFC_EMU_RAW_MAX  140U

/** @brief The longest NFCID1 there is. Mirrors KNFC_UID_MAX (knfc_tag.h). */
#define KQP_NFC_UID_MAX  10U

/** @brief The longest NDEF message any NFC request or answer carries, NUL
 *         included: the console's, an ask's, and kode_sys.h's KODE_SYS_NFC_TEXT_MAX. */
#define KQP_NFC_TEXT_MAX  96U

/**
 * @brief One tag the poller activated (NFC_TAG_SIG).
 *
 * uid/uid_len/atqa/sak/type mirror knfc_tag_t. @c type is knfc_tag_type_t as
 * a plain integer, on the rule at the top of this section - the same way
 * PowerEvt carries kdrv_bq25896_chrg_stat_t and ButtonEvt carries
 * kdrv_button_t.
 *
 * Identity only. An NDEF message does not travel on the bus: it is hundreds
 * of bytes for exactly one caller, so it goes through NfcNdefEvt's buffer,
 * which belongs to that caller and is written by one writer at a time.
 */
typedef struct {
    QEvt     super;
    uint8_t  uid[KQP_NFC_UID_MAX];  /**< NFCID1, @c uid_len bytes of it.   */
    uint8_t  uid_len;               /**< 4, 7 or 10.                       */
    uint16_t atqa;                  /**< SENS_RES.                         */
    uint8_t  sak;                   /**< SEL_RES.                          */
    uint8_t  type;                  /**< A KODE_SYS_NFC_TYPE_x.            */
} NfcTagEvt;

/* Answers an ask carries in its request, named before they are defined. */
typedef struct NfcNdefRspEvt  NfcNdefRspEvt;
typedef struct NfcFieldRspEvt NfcFieldRspEvt;
typedef struct NfcEmuRspEvt   NfcEmuRspEvt;

/** @brief One activated tag: knfc_tag_t field for field, @c type as a plain integer. */
typedef struct {
    uint8_t  uid[KQP_NFC_UID_MAX];  /**< NFCID1, @c uid_len bytes of it.   */
    uint8_t  uid_len;               /**< 4, 7 or 10.                       */
    uint16_t atqa;                  /**< SENS_RES.                         */
    uint8_t  sak;                   /**< SEL_RES.                          */
    uint8_t  type;                  /**< A KODE_SYS_NFC_TYPE_x.            */
} NfcTag;

/** @brief Answers kode_sys_nfc_read_tag() (NFC_READ_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t rc;     /**< kode_status_t: KODE_OK with @c tag, KODE_TIMEOUT
                     *   when the window closed empty, KODE_IO_ERROR with
                     *   no reader fitted, KODE_DENIED while the reader is
                     *   held.                                            */
    NfcTag  tag;    /**< Zero unless @c rc is KODE_OK.                     */
} NfcReadRspEvt;

/**
 * @brief Wait for a tag and copy its identity out (NFC_READ_REQ_SIG).
 *
 * @c out is untyped rather than a knfc_tag_t*, on the rule at the top of
 * this section. Both sides already have knfc_tag.h in scope; this header
 * does not need it.
 *
 * The pointers must outlive the request, exactly as IrCaptureEvt's must: a
 * requester waits with a deadline, and that deadline can run out while this
 * event is still in AO_Nfc's queue, so the memory named here cannot be a
 * stack frame the requester is free to leave.
 *
 * @c gen and @c ack carry IrCaptureEvt's meaning exactly - AO_Nfc writes
 * @c gen into @c ack immediately before signalling @c done, so a requester
 * woken by a token it did not earn reads a generation that is not its own
 * and keeps waiting. That write is a release and the requester's read is an
 * acquire, which is also what orders everything written through @c out.
 */
typedef struct {
    QEvt        super;
    void       *out;    /**< knfc_tag_t *, the requester's.                */
    uint32_t   *rc;     /**< kode_status_t: KODE_OK with @c out written,
                         *   KODE_TIMEOUT when the window closed with no
                         *   tag, KODE_DENIED when the request was refused
                         *   (no reader, busy, @c ms 0). May be NULL.     */
    khal_sem_t  done;   /**< Posted when @c out and @c rc are filled.      */
    uint32_t    ms;     /**< How long to keep the field up. 0 is refused
                         *   at once, since nobody is waiting.            */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                         */
    QActive    *ask;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    NfcReadRspEvt *rsp; /**< Its answer, taken with the request.           */
} NfcReadEvt;

/**
 * @brief Read or write one NDEF message (NFC_NDEF_REQ_SIG).
 *
 * One event for both directions. @c write says which, and @c uri says which
 * record to make when writing; reading, both are ignored and the message
 * comes back as text whatever record carried it.
 *
 * @c text is the requester's buffer and the same ownership rules apply as to
 * IrCaptureEvt.out: it must outlive the request, exactly one writer touches
 * it at a time, and nothing reads it until that writer is done. Writing, it
 * is AO_Nfc that reads and the requester that filled it in; reading, the
 * other way about.
 */
typedef struct {
    QEvt        super;
    QActive    *who;    /**< When set, the answer is POSTED here as an
                         *   NfcNdefRspEvt and the buffer below is used only
                         *   on the way in. An Active Object cannot sleep on
                         *   @c done, so this is the shape it asks in; a
                         *   blocked task leaves it NULL.                   */
    char       *text;   /**< The message. Read into it, or written from it. */
    size_t      cap;    /**< How many bytes @c text holds, terminator
                         *   included.                                     */
    size_t     *n;      /**< Reading: how many bytes were written, the
                         *   terminator excluded. May be NULL.             */
    bool       *is_uri; /**< Reading: set true for a URI record, false for a
                         *   Text one, only when the read succeeds. May be
                         *   NULL.                                         */
    bool        write;  /**< true to write @c text to the tag.             */
    bool        uri;    /**< Writing: a URI record rather than a Text one. */
    khal_sem_t  done;   /**< Posted when the exchange is over, either way. */
    uint32_t    ms;     /**< How long to keep the field up.                */
    uint32_t   *rc;     /**< Out: kode_status_t, what the exchange came to.
                         *   A pointer and not a value, because the event is
                         *   recycled the moment AO_Nfc's step returns and
                         *   the requester is still asleep at that point.  */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                         */
    QActive    *ask;    /**< The AO @c rsp is posted to; NULL for a console
                         *   or a waiter. Its text travels in @c wtext and
                         *   the answer, never by pointer.                 */
    NfcNdefRspEvt *rsp; /**< Its answer, taken with the request.           */
    char        wtext[KQP_NFC_TEXT_MAX]; /**< An ask's text to write, copied. */
} NfcNdefEvt;

/**
 * @brief The answer to an NfcNdefEvt that named a @c who (NFC_NDEF_RSP_SIG).
 *
 * Carries the tag as well as the message, so one event serves both `nfc_read`
 * and `nfc_write`: the identity says something was found, and @c rc says what
 * the exchange with it came to. @c have_tag false means the window closed
 * with nothing on the antenna, which is a different answer from a tag that
 * would not be read.
 *
 * uid/atqa/sak/type mirror knfc_tag_t, on the rule at the top of this
 * section.
 */
struct NfcNdefRspEvt {
    QEvt     super;
    uint32_t rc;        /**< kode_status_t of the exchange.               */
    bool     have_tag;  /**< Whether anything activated at all.           */
    uint8_t  uid[KQP_NFC_UID_MAX];
    uint8_t  uid_len;
    uint16_t atqa;
    uint8_t  sak;
    uint8_t  type;
    bool     uri;                     /**< A read's record was a URI, not Text. */
    uint16_t len;                     /**< Bytes of @c text, terminator
                                       *   excluded.                      */
    char     text[KQP_NFC_TEXT_MAX];  /**< What was read; empty on a write. */
};

/**
 * @brief Ask AO_Nfc to sweep the Mifare dictionary on a block
 *        (NFC_KEYS_REQ_SIG).
 *
 * Answered to @c who with an NfcKeysRspEvt when the sweep ends - a key found
 * or the dictionary exhausted. Progress goes to the log as it runs.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the final answer goes.                */
    uint8_t  block;     /**< The block whose sector to open.             */
} NfcKeysReqEvt;

/**
 * @brief The outcome of a dictionary sweep (NFC_KEYS_RSP_SIG).
 *
 * @c found says whether any key opened the block. When it did, @c key holds
 * the six bytes and @c type says A or B; when it did not, the key is a card
 * with keys on no public list, and kodeOS stops there rather than recovering
 * them.
 */
typedef struct {
    QEvt     super;
    bool     found;
    bool     have_tag;  /**< Whether a card was there to sweep at all.   */
    uint8_t  block;
    uint8_t  key[6];
    uint8_t  type;      /**< 0 = A, 1 = B; meaningless unless found.     */
    uint32_t tried;     /**< How many dictionary entries were tried.     */
} NfcKeysRspEvt;

/**
 * @brief Ask AO_Nfc to dump a Mifare Classic (NFC_DUMP_REQ_SIG).
 *
 * Every sector the fast dictionary opens is read and streamed to the log as
 * it goes; the final NfcDumpRspEvt to @c who says how much was read. Sectors
 * no known key opens are reported and skipped, not treated as failures.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the final answer goes. */
} NfcDumpReqEvt;

/**
 * @brief The end of a dump (NFC_DUMP_RSP_SIG).
 *
 * @c sectors_read against @c sectors_total says how complete it was; a card
 * with sectors on non-default keys reads partially and says so.
 */
typedef struct {
    QEvt     super;
    bool     have_tag;
    uint8_t  sectors_total;
    uint8_t  sectors_read;
    uint16_t blocks_read;
} NfcDumpRspEvt;

/**
 * @brief Ask AO_Nfc for the reader's status (NFC_INFO_REQ_SIG).
 *
 * No key beyond @c who: the console's `nfc` asks for the whole thing every
 * time. Answered to @c who with an NfcInfoEvt, the IrInfoReqEvt shape.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the answer goes.                        */
} NfcInfoReqEvt;

/**
 * @brief The reader's status, in answer to an NfcInfoReqEvt (NFC_INFO_RSP_SIG).
 *
 * @c present is whether the chip answered its identity at startup - false on
 * a board whose Antenna PCB is not fitted, which is a normal state and not a
 * fault. @c identity is the IC_IDENTITY register as read, meaningless when
 * @c present is false.
 *
 * @c field is whether the 13.56 MHz field is up as this is answered, which
 * is only ever true while a request is in flight.
 *
 * The last tag mirrors NfcTagEvt's fields, on the rule at the top of this
 * section, and is meaningless unless @c have_last. @c age_ms is how long ago
 * it was activated, in khal_time_ms() terms.
 */
typedef struct {
    QEvt     super;
    bool     present;
    bool     busy;
    bool     field;
    uint8_t  identity;
    bool     have_last;
    uint8_t  uid[KQP_NFC_UID_MAX];
    uint8_t  uid_len;
    uint16_t atqa;
    uint8_t  sak;
    uint8_t  type;
    uint32_t age_ms;
    uint32_t tags;      /**< Tags activated since boot.                    */
    uint32_t windows;   /**< Windows that closed with nothing.             */
    uint32_t errors;    /**< Exchanges that failed after activation.       */
    bool     emulating; /**< An emulation window is open now.              */
    uint32_t emu_reads; /**< READs served in the open emulation window.    */
} NfcInfoEvt;

/**
 * @brief Ask AO_Nfc to hold or drop the certification field (NFC_CERT_REQ_SIG).
 *
 * The FCC 15.225 bench path, present only in a CONFIG_KODEOS_NFC_CERT build.
 * @c mode is 0 to drop the field, 1 for a bare maximum-drive carrier, 2 for a
 * continuously ASK-modulated one.
 *
 * The outcome travels back so a caller can tell a raised field from one that
 * never came up: AO_Nfc writes it into @c rc, stamps @c gen into @c ack, and
 * signals @c done, all inside the step that receives the request - the field
 * is raised or dropped synchronously, not over a window. Success is a reader
 * that is present and a carrier the driver raised; a reader that is absent,
 * busy or refuses the drive is a failure. OFF is success except while a
 * listen window (NFC_FIELD_REQ_SIG) holds the reader: that stop is refused
 * with KODE_DENIED, nothing radiates meanwhile, and the window ends on its
 * own deadline. The three pointers may be NULL, which is the fire-and-forget
 * request that wants no answer.
 */
typedef struct {
    QEvt        super;
    uint8_t     mode;   /**< 0 off, 1 unmodulated carrier, 2 modulated.    */
    uint32_t   *rc;     /**< Out: the outcome as kode_status_t. May be NULL. */
    khal_sem_t  done;   /**< Signalled once @c rc is written. May be NULL. */
    uint32_t    gen;    /**< The caller's generation for this request.     */
    uint32_t   *ack;    /**< Where @c gen is written before @c done. NULL ok.*/
    QActive    *ask;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    StatusRspEvt *rsp;  /**< Its answer, taken with the request.           */
} NfcCertEvt;

/**
 * @brief Listen with the reader's own field off, and describe the field on
 *        the antenna (NFC_FIELD_REQ_SIG).
 *
 * @c out is untyped rather than a knfc_field_t *, on the rule at the top of
 * this section. Both sides already have knfc_field.h in scope; this header
 * does not need it.
 *
 * Two ways to be answered, and the requester picks by what it is. A blocked
 * task names @c done and waits on it; an Active Object cannot sleep, so it
 * names @c to and is posted an NfcFieldRspEvt. AO_Nfc answers on whichever is
 * not NULL, and on both when both are.
 *
 * The pointers are the requester's memory, lent for this one request and
 * written only before its answer, exactly as NfcReadEvt's are: a requester
 * waits with a deadline, and that deadline can run out while this event is
 * still in AO_Nfc's queue, so the memory named here cannot be a stack frame
 * the requester is free to leave.
 *
 * @c gen and @c ack carry NfcReadEvt's meaning exactly - AO_Nfc writes
 * @c gen into @c ack immediately before signalling @c done, so a requester
 * woken by a token it did not earn reads a generation that is not its own
 * and keeps waiting. That write is a release and the requester's read is an
 * acquire, which is also what orders everything written through @c out and
 * @c rc.
 */
typedef struct {
    QEvt        super;
    void       *out;    /**< knfc_field_t *, the requester's. Written only
                         *   when the window closed cleanly. May be NULL. */
    uint32_t   *rc;     /**< Out: the outcome as kode_status_t, written
                         *   before @c done: KODE_OK when @c out was
                         *   written, otherwise the status
                         *   NfcFieldRspEvt.st names, or KODE_NO_RESOURCES
                         *   when nfc_field_request() found the event pool
                         *   empty. @c out is left alone on anything but
                         *   KODE_OK. May be NULL.                         */
    khal_sem_t  done;   /**< Posted when the request is over, either way.
                         *   May be NULL.                                  */
    uint32_t    ms;     /**< How long to listen. 0 is refused at once; a
                         *   longer window than KODE_NFC_FIELD_MS_MAX
                         *   (kaos.h) is capped at it.                     */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                         */
    QActive    *to;     /**< When set, the answer is also POSTED here as an
                         *   NfcFieldRspEvt. NULL for a blocked task.      */
    QActive    *ask;    /**< The AO @c rsp is posted to, taken with the
                         *   request; NULL for a console or a waiter.      */
    NfcFieldRspEvt *rsp; /**< Its answer.                                  */
} NfcFieldEvt;

/**
 * @brief The answer to an NfcFieldEvt that named a @c to (NFC_FIELD_RSP_SIG).
 *
 * The window's statistics by value, mirroring knfc_field_t field for field on
 * the rule at the top of this section. They are all zero unless @c st is
 * KODE_OK.
 *
 * Each status in @c st means one thing, because a requester acts on each
 * differently: an absent reader stays absent, a busy one is free again once
 * the request holding it ends, and a failed window may simply be asked for
 * again.
 */
struct NfcFieldRspEvt {
    QEvt     super;
    uint32_t st;    /**< kode_status_t: KODE_OK with the stats below;
                     *   KODE_IO_ERROR when no reader is fitted - the
                     *   Antenna PCB is absent or its flex unseated, and
                     *   the reader is looked for only at boot, so asking
                     *   again answers the same; KODE_DENIED while another
                     *   request holds the reader (a read, an NDEF
                     *   exchange, a sweep, a dump, the production row or
                     *   a certification carrier), which passes;
                     *   KODE_ERROR for an @c ms of 0 and for a window that
                     *   failed: it would not open, a burst did not
                     *   complete, or the close could not put the reader
                     *   back.                                             */
    uint8_t  min;   /**< Lowest amplitude byte in the window.              */
    uint8_t  mean;  /**< Integer mean of every amplitude byte.             */
    uint8_t  peak;  /**< Highest amplitude byte in the window.             */
    uint8_t  mod;   /**< peak - min: how much the field moved.             */
    bool     ext;   /**< The chip's external field detector was set.       */
    uint16_t n;     /**< Samples taken.                                    */
};

/** @brief The card to present: knfc_emu_card_t field for field, for AudioInfo's reason. */
typedef struct {
    uint8_t  uid[KQP_NFC_UID_MAX];  /**< NFCID1 to clone; ignored when @c uid_len is 0. */
    uint8_t  uid_len;               /**< 4, 7 or 10 to clone; 0 to synthesise.         */
    uint16_t atqa;                  /**< SENS_RES to clone; 0 to derive.               */
    uint8_t  sak;                   /**< SEL_RES to clone.                             */
    bool     uri;                   /**< @c ndef is a URI rather than text.            */
    char     ndef[KQP_NFC_EMU_NDEF_MAX + 1U]; /**< The message; "" for default.        */
    uint8_t  raw_len;               /**< Bytes of @c raw; 0 = not used.                */
    uint8_t  raw[KQP_NFC_EMU_RAW_MAX]; /**< A ready-made NDEF message.                 */
} NfcCard;

/**
 * @brief Present the Dot as a passive NFC-A card, or stop (NFC_EMU_REQ_SIG).
 *
 * @c on is 1 to start and 0 to stop. When starting, @c card points at a
 * knfc_emu_card_t (untyped here, on the rule at the top of this section: kqp
 * declares no kdrv type); it is copied by AO_Nfc's driver at once, so it need
 * only outlive this round trip, exactly as NfcNdefEvt.text does. Emulation is
 * held until a stop, like the certification carrier - there is no window and no
 * deadline.
 *
 * Two answer channels, NfcCertEvt's arrangement: a blocked task names @c done
 * (and @c rc, @c reads, @c gen, @c ack) and waits; the console names @c who and
 * is posted an NfcEmuRspEvt. @c gen is written into @c ack immediately before
 * @c done is signalled. @c reads is written the total READs served, on a stop.
 */
typedef struct {
    QEvt        super;
    uint8_t     on;     /**< 1 start, 0 stop.                               */
    const void *card;   /**< knfc_emu_card_t *, used only when @c on is 1.  */
    uint32_t   *rc;     /**< Out: the outcome as kode_status_t. May be NULL. */
    uint32_t   *reads;  /**< Out: READs served, on a stop. May be NULL.     */
    khal_sem_t  done;   /**< Signalled once @c rc is written. May be NULL.  */
    uint32_t    gen;    /**< The caller's generation for this request.      */
    uint32_t   *ack;    /**< Where @c gen is written before @c done. NULL ok. */
    QActive    *who;    /**< Console: NFC_EMU_RSP_SIG is posted here. NULL ok. */
    QActive    *ask;    /**< The AO @c rsp is posted to, taken with the
                         *   request; NULL for a console or a waiter. A start
                         *   then presents @c copy, which @c card names.   */
    NfcEmuRspEvt *rsp;  /**< Its answer.                                   */
    NfcCard     copy;   /**< An ask's card, copied.                        */
} NfcEmuReqEvt;

/**
 * @brief A start or stop the console asked for finished (NFC_EMU_RSP_SIG).
 *
 * The async answer a console command waits on, since the console AO cannot
 * block on a semaphore. @c reads is meaningful on a stop.
 */
struct NfcEmuRspEvt {
    QEvt     super;
    uint32_t rc;        /**< kode_status_t of the start or stop.           */
    uint32_t reads;     /**< READs served this session, on a stop.         */
};

/** @brief What an NfcEmuEvt reports. */
typedef enum {
    NFC_EMU_KIND_FIELD = 0,  /**< A reader's field arrived on the antenna.  */
    NFC_EMU_KIND_SELECTED,   /**< A reader selected (activated) the card.   */
    NFC_EMU_KIND_READ,       /**< A Type-2 READ was served.                 */
    NFC_EMU_KIND_STOPPED,    /**< The emulation window ended.               */
} nfc_emu_kind_t;

/**
 * @brief One turn of an emulation window, published (NFC_EMU_SIG).
 *
 * A served READ is what says the card was read, not the mere selection: a
 * reader that only enumerates the tag raises SELECTED and never READ. @c reads
 * carries the running total, and on STOPPED the final one.
 */
typedef struct {
    QEvt     super;
    uint8_t  kind;      /**< nfc_emu_kind_t as a plain integer.            */
    uint32_t reads;     /**< READs served so far, the total on STOPPED.    */
} NfcEmuEvt;

/* ---- AO_Rfid ----------------------------------------------------------
 * The 125 kHz LF front end's events. The decoded identity travels by value -
 * the five ID bytes and the bit-rate divisor - so this header spells no kdrv
 * type, exactly as the AO_Nfc block above does and for the same layer reason:
 * kqp declares REQUIRES qpc khal and nothing else.
 *
 * The request events carry the requester's own memory and NfcReadEvt's
 * generation contract: @c gen is written into @c ack immediately before
 * @c done is signalled, so a caller woken by a token it did not earn reads a
 * generation that is not its own and keeps waiting. The pointers must outlive
 * the request, since a requester's deadline can run out while the event is
 * still in AO_Rfid's queue.
 */

/* Answers an ask carries in its request, named before they are defined. */
typedef struct RfidOpRspEvt    RfidOpRspEvt;
typedef struct RfidFieldRspEvt RfidFieldRspEvt;

/** @brief The bytes of an EM4100 ID the RFID events carry. */
#define KQP_RFID_ID_BYTES  5U

/**
 * @brief Energise and copy out the first EM4100 ID decoded (RFID_READ_REQ_SIG).
 *
 * The read never boosts VO4. @c id and @c rf_div are the requester's, filled
 * when @c have is set true; a window that closed empty leaves them alone.
 */
typedef struct {
    QEvt        super;
    uint8_t    *id;      /**< The requester's 5-byte ID buffer.            */
    uint8_t    *rf_div;  /**< Out: 16, 32 or 64, the bit rate decoded at.  */
    bool       *have;    /**< Set false when the window closed with no tag,
                          *   in which case @c id is left alone.           */
    khal_sem_t  done;    /**< Posted when @c id and @c have are filled.    */
    uint32_t    gen;     /**< The requester's tag for this request.        */
    uint32_t   *ack;     /**< Where @c gen is written back, just before
                          *   @c done. May be NULL.                        */
    uint32_t    ms;      /**< How long to keep the field up. 0 is answered
                          *   at once, empty, since nobody is waiting.     */
    QActive    *who;     /**< The requesting AO, or NULL for a facade call
                          *   that waits on @c done.                       */
    QActive    *ask;     /**< The AO @c rsp is posted to, taken with the
                          *   request; NULL for a console or a waiter.     */
    RfidOpRspEvt *rsp;   /**< Its answer.                                   */
} RfidReqEvt;

/**
 * @brief Clone a 5-byte ID onto a T5577 (RFID_WRITE_REQ_SIG).
 *
 * The clone boosts VO4 to 3.3 V for the write and drops it after. @c ok is
 * set to whether the read-back matched.
 */
typedef struct {
    QEvt           super;
    const uint8_t *id;    /**< The 5-byte ID to write. The requester's.    */
    bool          *ok;    /**< Out: whether the read-back matched.         */
    khal_sem_t     done;  /**< Posted when @c ok is filled.                */
    uint32_t       gen;   /**< The requester's tag for this request.       */
    uint32_t      *ack;   /**< Where @c gen is written back, just before
                           *   @c done. May be NULL.                       */
    uint32_t       ms;    /**< How long to keep the field up.              */
    QActive       *who;   /**< The requesting AO, or NULL for a facade call
                           *   that waits on @c done.                      */
    QActive       *ask;   /**< The AO @c rsp is posted to, taken with the
                           *   request; NULL for a console or a waiter. The
                           *   ID is then copied into @c wid.              */
    RfidOpRspEvt  *rsp;   /**< Its answer.                                 */
    uint8_t        wid[KQP_RFID_ID_BYTES]; /**< An ask's ID to write, a copy. */
} RfidWriteReqEvt;

/**
 * @brief One EM4100 tag decoded (RFID_TAG_SIG).
 *
 * Published only while a request is in flight: the field is off at rest, so
 * silence here means nobody asked, not that no tag was presented.
 */
typedef struct {
    QEvt    super;
    uint8_t id[KQP_RFID_ID_BYTES]; /**< 40-bit ID, most-significant nibble first. */
    uint8_t rf_div;   /**< 16, 32 or 64: the bit rate it was read at. */
} RfidTagEvt;

/**
 * @brief A console `rfid_read` or `rfid_write` finished (RFID_OP_RSP_SIG).
 *
 * The async answer AO_Rfid posts back to the console AO, so a console command
 * never blocks on a field window - the NfcNdefRspEvt arrangement. @c write
 * tells the two apart. For a read, @c rc is KODE_OK with @c id / @c rf_div set
 * on a decode, or KODE_TIMEOUT when the window closed empty. For a clone,
 * @c rc is KODE_OK when the read-back matched the ID written (echoed in
 * @c id), else KODE_TIMEOUT. Either is KODE_DENIED when AO_Rfid could not
 * take it: the front end busy, or an argument it refuses.
 */
struct RfidOpRspEvt {
    QEvt     super;
    bool     write;    /**< true = a clone's result, false = a read's.     */
    uint32_t rc;       /**< KODE_OK on success; KODE_TIMEOUT, or KODE_DENIED when refused. */
    uint8_t  id[KQP_RFID_ID_BYTES]; /**< The ID read, or the ID a clone wrote back. */
    uint8_t  rf_div;   /**< Read only: the bit rate it was read at.        */
};

/**
 * @brief Ask AO_Rfid for the front end's status (RFID_INFO_REQ_SIG).
 *
 * No key beyond @c who: the console's `rfid` asks for the whole thing every
 * time. Answered to @c who with a RfidInfoEvt, the NfcInfoReqEvt shape.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the answer goes. */
} RfidInfoReqEvt;

/**
 * @brief The front end's status, in answer to a RfidInfoReqEvt
 *        (RFID_INFO_RSP_SIG).
 *
 * @c field is whether the carrier is up as this is answered: true while a
 * read's, a clone's or the production row's job is with the worker - a
 * deadline may already have answered its request - or a certification carrier
 * is held, and never for a capture. The last decode mirrors RfidTagEvt's
 * fields and is meaningless unless @c have; @c age_ms is how long ago it was
 * decoded, in khal_time_ms() terms.
 */
typedef struct {
    QEvt     super;
    bool     field;     /**< Whether the carrier is up right now.          */
    bool     have;      /**< Whether a tag has ever been decoded.          */
    uint8_t  id[KQP_RFID_ID_BYTES]; /**< The last ID, meaningless unless @c have. */
    uint8_t  rf_div;    /**< The bit rate it was read at.                  */
    uint32_t age_ms;    /**< How long ago it was decoded.                  */
    uint32_t reads;     /**< Read windows that decoded a tag since boot.   */
    uint32_t writes;    /**< Clones written since boot.                    */
    uint32_t windows;   /**< Windows that closed with nothing.             */
} RfidInfoEvt;

/**
 * @brief Ask AO_Rfid to hold or drop the 125 kHz certification field
 *        (RFID_CERT_REQ_SIG).
 *
 * The FCC 15.209 bench path, present only in a CONFIG_KODEOS_NFC_CERT build.
 * @c mode is 0 to drop the field, 1 for a bare maximum-drive carrier, 2 for a
 * continuously OOK-modulated one.
 *
 * The outcome travels back the same way NfcCertEvt's does, and inside the same
 * step: AO_Rfid writes it into @c rc, stamps @c gen into @c ack, and signals
 * @c done. The front end cannot detect whether the LF antenna is populated, so
 * "success" here means the carrier was commanded and the driver configured it -
 * not that RF radiated; a busy front end or a driver that refuses is a
 * failure. OFF is success except while a read, a clone, a capture
 * (RFID_FIELD_REQ_SIG) or the production row holds the front end, or while the
 * worker still holds a job whose request a deadline already answered: that
 * stop is refused with KODE_DENIED, no certification carrier is held
 * meanwhile, and the request ends on its own outcome or deadline. The three
 * pointers may be NULL for a fire-and-forget request.
 */
typedef struct {
    QEvt        super;
    uint8_t     mode;   /**< 0 off, 1 unmodulated carrier, 2 OOK modulated. */
    uint32_t   *rc;     /**< Out: the outcome as kode_status_t. May be NULL. */
    khal_sem_t  done;   /**< Signalled once @c rc is written. May be NULL. */
    uint32_t    gen;    /**< The caller's generation for this request.     */
    uint32_t   *ack;    /**< Where @c gen is written before @c done. NULL ok.*/
    QActive    *ask;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    StatusRspEvt *rsp;  /**< Its answer, taken with the request.           */
} RfidCertEvt;

/**
 * @brief Read the demod line with the carrier OFF, describe it, and optionally
 *        copy the waveform out (RFID_FIELD_REQ_SIG).
 *
 * @c out is untyped rather than a krfid_field_t *, on the rule at the top of
 * this section.
 *
 * Two ways to be answered, as NfcFieldEvt has: a blocked task names @c done
 * and waits on it, an Active Object names @c to and is posted a
 * RfidFieldRspEvt, and AO_Rfid answers on whichever is not NULL, and on both
 * when both are.
 *
 * The pointers are the requester's memory, lent for this one request. The
 * worker writes @c out, @c wave and @c n and only then reports back, so on
 * a KODE_OK answer they are written before it. AO_Rfid's deadline can answer
 * first, KODE_ERROR in @c rc, and the worker may then still write them once;
 * AO_Rfid refuses every request until the worker has reported, so that late
 * write never lands in memory lent to a newer request. So lend memory kept
 * for these requests alone; an ask's wave is the asker's, and its answer waits
 * for the worker to let go of it. The pointers must also
 * outlive the request for NfcReadEvt's reason: a requester's deadline can run
 * out while this event is still in AO_Rfid's queue. @c gen and @c ack carry
 * NfcReadEvt's meaning exactly: AO_Rfid writes @c gen into @c ack immediately
 * before signalling @c done, and that write orders everything written through
 * @c out, @c wave, @c n and @c rc.
 *
 * The front end's presence cannot be sensed: a board without the Antenna PCB
 * is answered KODE_OK over whatever the unconnected demod pin reads, and
 * KODE_DENIED never means a missing front end.
 */
typedef struct {
    QEvt        super;
    void       *out;    /**< krfid_field_t *, the requester's. Written only
                         *   when the window was read. May be NULL.        */
    uint16_t   *wave;   /**< The requester's buffer of @c cap points, or
                         *   NULL for statistics only.                     */
    size_t      cap;    /**< How many points @c wave holds.                */
    size_t     *n;      /**< Out: points written to @c wave, 0 with no
                         *   wave. May be NULL.                            */
    uint32_t   *rc;     /**< Out: the outcome as kode_status_t, written
                         *   before @c done: KODE_OK when @c out was
                         *   written, otherwise the status
                         *   RfidFieldRspEvt.st names, or
                         *   KODE_NO_RESOURCES when rfid_capture_request()
                         *   found the event pool empty. May be NULL.      */
    khal_sem_t  done;   /**< Posted when the request is over, either way.
                         *   May be NULL.                                  */
    uint32_t    ms;     /**< How long to read. 0 is refused at once; a
                         *   longer window than KDRV_RFID_LF_CAPTURE_MS_MAX
                         *   (kdrv_rfid_lf.h) is capped at it.             */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                         */
    QActive    *to;     /**< When set, the answer is also POSTED here as a
                         *   RfidFieldRspEvt. NULL for a blocked task.     */
    QActive    *ask;    /**< The AO @c rsp is posted to, taken with the
                         *   request; NULL for a console or a waiter. Its
                         *   @c wave is then the asker's, lent until the
                         *   answer.                                       */
    RfidFieldRspEvt *rsp; /**< Its answer.                                 */
} RfidFieldEvt;

/**
 * @brief The answer to a RfidFieldEvt that named a @c to (RFID_FIELD_RSP_SIG).
 *
 * The window's statistics by value, mirroring krfid_field_t field for field
 * on the rule at the top of this section, and how many waveform points went
 * into the requester's buffer. They are all zero unless @c st is KODE_OK.
 */
struct RfidFieldRspEvt {
    QEvt     super;
    uint32_t st;      /**< kode_status_t: KODE_OK with the stats below;
                       *   KODE_DENIED while the front end is busy - a
                       *   read, a clone, another capture, the production
                       *   row, a certification carrier, or a job the
                       *   worker still holds after a deadline answered
                       *   its request - which passes, and when the
                       *   worker would not take the job; never for a
                       *   missing front end, which cannot be sensed;
                       *   KODE_ERROR for an @c ms of 0, a window the
                       *   worker could not read, and a window whose
                       *   worker did not report before AO_Rfid's
                       *   deadline.                                       */
    uint16_t min;     /**< Lowest ADC count in the window.                 */
    uint16_t mean;    /**< Integer mean of the counts.                     */
    uint16_t peak;    /**< Highest ADC count in the window.                */
    uint16_t ac;      /**< Mean absolute deviation from the mean:
                       *   modulation.                                     */
    uint16_t n;       /**< Samples the window held before decimation.      */
    size_t   points;  /**< Points written to the requester's wave.         */
};

/** @brief Answers kode_sys_rfid_emulate_start() and _stop() (RFID_EMU_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t rc;     /**< kode_status_t: KODE_DENIED while the front end is
                     *   held, or for a stop with nothing playing.         */
    bool    stop;   /**< true = a stop's answer, false = a start's.        */
} RfidEmuRspEvt;

/** @brief Start or stop an EM4100 tag (RFID_EMU_REQ_SIG, RFID_EMU_STOP_REQ_SIG). */
typedef struct {
    QEvt        super;
    uint8_t     id[KQP_RFID_ID_BYTES]; /**< The ID, a copy; unused by a stop. */
    uint32_t   *rc;     /**< Out: kode_status_t. May be NULL.              */
    khal_sem_t  done;   /**< Signalled once @c rc is written. May be NULL. */
    uint32_t    gen;    /**< The caller's generation.                      */
    uint32_t   *ack;    /**< Where @c gen is written before @c done.       */
    QActive    *ask;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    RfidEmuRspEvt *rsp; /**< Its answer, taken with the request.           */
} RfidEmuEvt;

/* What a control request asks of the PMIC's owner. */
#define POWER_CTRL_BOOST   0U   /**< value 1 on, 0 off. Answer: 1 boosting.   */
#define POWER_CTRL_CHARGE  1U   /**< value 1 on, 0 off. Answer: the bit.      */
#define POWER_CTRL_ICHG    2U   /**< value mA, 64..the cell's. Answer: kept.  */
#define POWER_CTRL_RESET   3U   /**< value ignored. Answer: 0.                */

/**
 * @brief One owner-level operation on the PMIC (POWER_CTRL_REQ_SIG).
 *
 * The owner adds what the register write does not have: a precondition only
 * it knows, a check afterwards, a log that explains, a limit that is the
 * cell's. Every request is answered with a PowerCtrlEvt, a refusal included.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;        /**< POWER_CTRL_*                              */
    int32_t  value;     /**< The op's argument.                        */
} PowerCtrlReqEvt;

/**
 * @brief Arm the countdown after the cable leaves (POWER_SHIP_ARM_SIG).
 *
 * The asker is an Active Object's identity, not a table: AO_Power posts
 * POWER_SHIP_DUE_SIG to it and keeps nothing else of it.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Who is told when the cable has been out long enough. */
} PowerShipArmEvt;

/**
 * @brief The answer to a PowerCtrlReqEvt (POWER_CTRL_SIG).
 *
 * `rc` is kode_status_t: KODE_DENIED refused before any write,
 * KODE_NO_RESOURCES another request is still waiting on the timer,
 * KODE_ERROR the chip did not do what was asked, KODE_TIMEOUT the reset
 * bit never cleared.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint8_t  rc;        /**< kode_status_t                             */
    int32_t  value;
} PowerCtrlEvt;

/** @brief A touch sample from AO_Touch (TOUCH_EVT_SIG).
 *
 * It carries the time it was read, for the same reason ButtonEvt does: a
 * subscriber that measures a drag cannot stamp a sample it was handed late. */
typedef struct {
    QEvt     super;
    int16_t  x;
    int16_t  y;
    bool     pressed;
    uint32_t t_ms;      /**< khal_time_ms() when the sample was read. It wraps
                         *   at about 49 days, so an interval is an unsigned
                         *   subtraction of two stamps. */
    bool     stale;     /**< On a release only: the contact did not lift, it
                         *   stayed still past KODE_TOUCH_STILL_MS and stopped
                         *   counting as a finger (a drop of water, a cloth).
                         *   Nothing was tapped and nobody is there. */
} TouchEvt;

/**
 * @brief Read, write or dump one field of the touch controller
 *        (TOUCH_FIELD_REQ_SIG).
 *
 * The field is kdrv_cst820_field_t, already resolved from its name by
 * whoever asks. Answered to @c who with a TouchFieldEvt, a refusal included.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the answer goes.                    */
    uint8_t  op;        /**< FIELD_OP_*                                */
    uint16_t field;     /**< kdrv_cst820_field_t                       */
    int32_t  value;     /**< For FIELD_OP_SET.                         */
} TouchFieldReqEvt;

/**
 * @brief The answer to a TouchFieldReqEvt (TOUCH_FIELD_RSP_SIG).
 *
 * For a read, `value` is the field; for a write, the value read back after
 * it; for a dump, nothing. `rc` is the driver's verdict: KODE_DENIED for a
 * field that cannot be written, a value out of its range, or no controller
 * at all; KODE_ERROR when the chip did not answer - which on this part
 * includes a chip that is asleep - or did not keep the write.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint16_t field;
    uint8_t  rc;        /**< kode_status_t                             */
    int32_t  value;
} TouchFieldEvt;

/* What one step of the guided touch test is saying. */
#define TP_TEST_PROMPT   0U   /**< Tap the target at (tx, ty), now.            */
#define TP_TEST_PASS     1U   /**< The tap at (gx, gy) was close enough.       */
#define TP_TEST_FAIL     2U   /**< It was not: (gx, gy) says where it landed.  */
#define TP_TEST_TIMEOUT  3U   /**< Nobody tapped, within KODE_TOUCH_TEST_STEP_MS. */
#define TP_TEST_DONE     4U   /**< The last step is over; the verdict follows. */
#define TP_TEST_MISS     5U   /**< A tap off the target, forgiven: (gx, gy) says where
                               *   it landed and the same target is asked again. The
                               *   third one on a step is TP_TEST_FAIL instead.      */

#define TEST_ORIGIN_AUTOTEST 0U   /**< The origin of a guided test the autotest asked for. */
#define TEST_ORIGIN_TEST_DOT 3U   /**< The origin of a guided test AO_TestDot asked for: the E5 operator part. */

/**
 * @brief A request for a guided test, with who asked for it
 *        (BUTTONS_TEST_REQ_FROM_SIG, TOUCH_TEST_REQ_FROM_SIG).
 *
 * Every step of the run carries @c origin back (ButtonsTestEvt, TouchTestEvt),
 * and AO_Autotest and the console count only the steps of
 * TEST_ORIGIN_AUTOTEST, so a run another client asked for never marks the
 * row the autotest happens to be running.
 */
typedef struct {
    QEvt    super;
    uint8_t origin;    /**< Who asked: TEST_ORIGIN_AUTOTEST or a client's own number. */
} TestReqEvt;

/**
 * @brief One step of the guided touch test (TOUCH_TEST_SIG).
 *
 * The console prints a prompt and every outcome as a record; the GUI draws
 * the target on the glass. (gx, gy) is where the tap read, in the
 * controller's own pixels, which are the panel's, so a failure says where the
 * finger landed.
 */
typedef struct {
    QEvt    super;
    uint8_t phase;     /**< TP_TEST_*                                        */
    uint8_t step;      /**< 1-based.                                          */
    uint8_t total;     /**< How many targets the script has.                  */
    int16_t tx;        /**< The target, in panel pixels.                      */
    int16_t ty;
    int16_t gx;        /**< Where the tap read; 0 for a prompt or a timeout. */
    int16_t gy;
    uint8_t origin;    /**< TEST_ORIGIN_AUTOTEST, or the client that asked (TestReqEvt). */
} TouchTestEvt;

/**
 * @brief The C5 link's state (RADIO_STATE_SIG).
 *
 * The version fields are the slave's own answer, valid only when @c fw_known:
 * a link can be up with the version RPC unanswered, and reporting zeros as a
 * version would be a lie. @c sd_hosted says which mount path the microSD is
 * on right now - AO_Storage acts on the mount events AO_Radio sends around
 * the link's lifetime; this flag is how everyone else can tell.
 */
typedef struct {
    QEvt     super;
    uint8_t  link;       /**< kradio_link_t: off/starting/up/stopping/failed. */
    bool     fw_known;   /**< The version fields below mean something.        */
    uint32_t fw_major;
    uint32_t fw_minor;
    uint32_t fw_patch;
    bool     sd_hosted;  /**< The card mounts through esp-hosted's controller. */
    bool     gave_up;    /**< The link is down, someone wants it, and no more
                          *   tries are coming until somebody asks.           */
} RadioStateEvt;

/** @brief The link to the C5 is gone (RADIO_LINK_LOST_SIG). */
typedef struct {
    QEvt    super;
    uint8_t status;      /**< kode_status_t: KODE_ERROR when the transport
                          *   dropped by itself, KODE_TIMEOUT when a job
                          *   failed over a C5 that stopped answering.       */
} RadioLostEvt;

/** @brief RADIO_PARK_REQ_SIG: a jump is about to restart the board. */
typedef struct {
    QEvt    super;
    uint8_t target;      /**< kdrv_radio_note_t: the role of the firmware the
                          *   restart goes to.                               */
} RadioParkEvt;

/** @brief KODE_SVC_RSP_SIG: which service answered, and how. The payload
 *         stays in AO_Radio's own buffer for that service: one request is
 *         in flight per service, so nothing overwrites it before it is read. */
typedef struct {
    QEvt     super;
    uint8_t  service;   /**< KODE_SVC_*.                                     */
    uint8_t  op;        /**< The request's op.                               */
    uint8_t  status;    /**< KODE_ST_*.                                      */
    uint16_t len;       /**< Payload bytes in the buffer.                    */
} KodeSvcEvt;

/** @brief Longest nonce the unit's key signs (KODE_NONCE_MAX on the wire). */
#define KODE_ID_NONCE_MAX    200U
/** @brief DER bytes per ID_PART_SIG and per `ident_cert` line: 192 base64
 *         characters, which fit both a console line and a reply record. */
#define KODE_ID_PART_BYTES   144U
/** @brief Most parts one certificate pair takes (1536 B of DER). */
#define KODE_ID_PARTS_MAX    11U
_Static_assert(KQP_NET_MQTT_MARGIN >= (KODE_ID_PARTS_MAX + KQP_ID_PART_MARGIN),
               "AO_Net keeps room under its inbound MQTT for every part of a pair and the end");

/** @brief What an identity operation is (IdReqEvt.op). */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps the event its size */
typedef enum {
    ID_OP_CERT = 0,   /**< Read the certificate pair: parts, then the end.  */
    ID_OP_CSR,        /**< Make the key if there is none, and a CSR for it:
                       *   parts, then the end. Factory only.               */
    ID_OP_CERT_PUT,   /**< One part of the pair the factory's CA signed;
                       *   the last one stores it. Factory only.            */
    ID_OP_LOCK        /**< Close CSR and CERT_PUT for good. Factory only.   */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps the event its size */
} IdOp;

/** @brief Sign the cloud's challenge (ID_SIGN_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;                            /**< Where the answer goes.    */
    char     nonce[KODE_ID_NONCE_MAX + 1U];  /**< NUL-terminated.           */
} IdSignReqEvt;

/** @brief The signature (ID_SIGN_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t status;    /**< KODE_ST_* (kode_wire.h).                        */
    uint8_t der_len;   /**< Bytes of der; 0 unless status is OK.            */
    uint8_t der[72];   /**< ECDSA P-256 over SHA-256, DER.                  */
} IdSignRspEvt;

/** @brief One identity operation (ID_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;                       /**< Where the answer goes.         */
    uint8_t  op;                        /**< IdOp.                          */
    uint8_t  n;                         /**< ID_OP_CERT_PUT: this part, 1.. */
    uint8_t  of;                        /**< ID_OP_CERT_PUT: of how many.   */
    uint8_t  len;                       /**< ID_OP_CERT_PUT: bytes of der.  */
    uint8_t  der[KODE_ID_PART_BYTES];   /**< ID_OP_CERT_PUT: the part.      */
} IdReqEvt;

/** @brief One piece of DER read back (ID_PART_SIG). */
typedef struct {
    QEvt    super;
    uint8_t n;                          /**< 1..of.                         */
    uint8_t of;                         /**< Parts in all.                  */
    uint8_t len;                        /**< Bytes of der.                  */
    uint8_t der[KODE_ID_PART_BYTES];    /**< The piece.                     */
} IdPartEvt;

/** @brief The end of an identity operation (ID_END_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  status;     /**< KODE_ST_* (kode_wire.h).                      */
    uint16_t len;        /**< Bytes the parts carried, in all.              */
    uint16_t unit_len;   /**< ID_OP_CERT: of those, the unit certificate;
                          *   the intermediate follows it.                  */
} IdEndEvt;

/** @brief The steps of a wipe, in order (C5WipeRspEvt.step). */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps the event its size */
typedef enum {
    C5_WIPE_VAULT = 0,   /**< The C5's saved networks.                      */
    C5_WIPE_BTKEYS,      /**< The C5's BLE keys.                            */
    C5_WIPE_BLE,         /**< This chip's bond store.                       */
    C5_WIPE_DONE         /**< All three.                                    */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps the event its size */
} C5WipeStep;

/** @brief Forget the user's secrets (C5_WIPE_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;      /**< Where the answer goes.                          */
} C5WipeReqEvt;

/** @brief How far a wipe got (C5_WIPE_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t step;      /**< C5WipeStep: the one that failed, or DONE.       */
    uint8_t status;    /**< KODE_ST_* (kode_wire.h) of that step.           */
} C5WipeRspEvt;

/**
 * @brief Read, write or dump one field of the coprocessor (RADIO_FIELD_REQ_SIG).
 *
 * The field is kdrv_c5_field_t, already resolved from its name by whoever
 * asks. Answered to @c who with a RadioFieldEvt, a refusal included. The
 * answer comes from kdrv_c5's snapshot and never crosses the link, which is
 * why it is served in every state, link or no link.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the answer goes. NULL: served, unanswered. */
    uint8_t  op;        /**< FIELD_OP_*                                */
    uint16_t field;     /**< kdrv_c5_field_t                           */
    int32_t  value;     /**< For FIELD_OP_SET.                         */
} RadioFieldReqEvt;

/**
 * @brief The answer to a RadioFieldReqEvt (RADIO_FIELD_RSP_SIG).
 *
 * For a read, `value` is the field; for a dump, nothing. `rc` is the
 * driver's verdict: KODE_DENIED for a row whose group the last snapshot did
 * not fill (no link yet, an older slave without the descriptor RPC), or for
 * any write - the table has no knobs.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint16_t field;
    uint8_t  rc;        /**< kode_status_t                             */
    int32_t  value;
} RadioFieldEvt;

/**
 * @brief A network to join (WIFI_JOIN_REQ_SIG).
 *
 * The credentials travel *in* the event, copied, like every other payload
 * here - a pointer into whoever parsed the command line would be a promise
 * about a buffer that is already gone by the time the worker reads it.
 */
typedef struct {
    QEvt super;
    char ssid[33];   /**< 32 bytes plus the terminator (802.11).            */
    char pass[65];   /**< 64 plus the terminator (WPA2 PSK as a passphrase). */
    bool remember;   /**< Save it in the C5's vault once it has worked.     */
} WifiJoinEvt;

/** @brief What WIFI_FAST_REQ_SIG carries: the wish, a static event each. */
typedef struct {
    QEvt super;
    bool on;     /**< true: no power save; false: the default back.         */
} WifiFastEvt;

/** @brief What EXP_POWER_REQ_SIG carries: the wish, a static event each. */
typedef struct {
    QEvt super;
    bool on;     /**< true: J3's +3.3V_EXT on; false: off.                  */
} ExpPowerEvt;

/** @brief What EXP_POWER_SIG carries: the switch that is over, a static event each. */
typedef struct {
    QEvt super;
    bool    on;  /**< The switch: true for on, false for off.               */
    uint8_t rc;  /**< kode_status_t: KODE_OK, the rail is as @c on says;
                  *   KODE_IO_ERROR, the C5 refused; KODE_DENIED, the link
                  *   would not come up or went, so the next link-up
                  *   carries it out (EXP_POWER_SIG).                       */
} ExpPowerDoneEvt;

/** @brief One network the C5 found (WIFI_SCAN_ENTRY_SIG). */
typedef struct {
    QEvt    super;
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t authmode;   /**< 0 = open.                                      */
    uint8_t bssid[6];   /**< The access point's MAC, as on the air.         */
} WifiScanEntryEvt;

/** @brief The end of a scan (WIFI_SCAN_END_SIG, WIFI_SAVED_END_SIG). */
typedef struct {
    QEvt    super;
    uint8_t count;      /**< How many entries were published.               */
    bool    ok;         /**< false: the scan itself failed; count is 0.     */
    bool    busy;       /**< true: not started, the C5 was busy with another
                         *   job (ok is false, count 0); ask again.          */
    bool    cut;        /**< true: a Wi-Fi sweep ended before its last step,
                         *   for a stop or for a job that needed the radio
                         *   (ok is false); count is what it had heard.      */
} WifiScanEndEvt;

/** @brief ScanAskEvt.kind: the Wi-Fi sweep; the rows are kdrv_wifi_ap_t. */
#define KQP_SCAN_WIFI  0U
/** @brief ScanAskEvt.kind: the BLE listen; the rows are kdrv_ble_dev_t. */
#define KQP_SCAN_BLE   1U

/**
 * @brief The end of an asked scan, to the asking AO (WIFI_SCAN_RSP_SIG,
 *        BLE_SCAN_RSP_SIG).
 *
 * The entries went before it, posted to the same AO while its queue kept
 * KODE_STORAGE_LIST_FREE_SLOTS free; @c more says some did not fit.
 */
typedef struct {
    QEvt    super;
    uint8_t rc;         /**< kode_status_t: KODE_OK, the scan ended;
                         *   KODE_ERROR, it failed or the link went;
                         *   KODE_NO_RESOURCES, the radio was busy with a
                         *   scan, a test row or an install: ask again;
                         *   KODE_DENIED, a BLE scan with Bluetooth off in
                         *   Settings.                                     */
    uint8_t seen;       /**< Networks or devices handed over, 255 at most.  */
    bool    more;       /**< true: entries were left out, the queue full.   */
} ScanRspEvt;

/**
 * @brief Where AO_Radio writes a blocked task's scan (ScanAskEvt.wait).
 *
 * The task owns it and it outlives the request: a caller that gave up may
 * still be answered, and the generation stamped into @c ack is what tells
 * that answer from the next one's.
 */
typedef struct {
    khal_sem_t done;    /**< Signalled once the answer is written.          */
    uint32_t   ack;     /**< The request's generation, stamped (release)
                         *   just before @c done.                           */
    uint8_t    rc;      /**< kode_status_t: as ScanRspEvt.rc; also
                         *   KODE_NO_RESOURCES when AO_Radio had no room.   */
    void      *rows;    /**< The task's rows, of the request's kind
                         *   (KQP_SCAN_WIFI, KQP_SCAN_BLE), @c cap of them:
                         *   what the scan kept, in the driver's order.     */
    uint8_t    cap;     /**< Rows @c rows holds.                            */
    uint8_t    got;     /**< Rows written; 0 unless @c rc is KODE_OK.       */
} ScanWaiter;

/**
 * @brief Scan, for an AO or a blocked task (RADIO_SCAN_ASK_SIG).
 *
 * Exactly one of @c ask and @c wait is set. ScanWaiter.rows is untyped for
 * the reason IrLastEvt.out is: naming the driver's rows would mean including
 * kdrv_wifi.h and kdrv_ble.h.
 */
typedef struct {
    QEvt        super;
    QActive    *ask;    /**< The AO the entries and @c rsp go to; NULL
                         *   for a waiter.                              */
    ScanRspEvt *rsp;    /**< Its end, taken with the request.           */
    ScanWaiter *wait;   /**< The task's waiter; NULL for an AO.         */
    uint32_t    gen;    /**< The task's tag for this request.           */
    uint32_t    ms;     /**< KQP_SCAN_BLE: how long to listen; 0 for
                         *   KQP_SCAN_WIFI.                             */
    uint8_t     kind;   /**< KQP_SCAN_WIFI or KQP_SCAN_BLE.             */
} ScanAskEvt;

/** @brief Bytes of a network name in the radio check: 32 characters and the terminator. */
#define RADIO_PROBE_SSID_BYTES  33U

/** @brief Bytes of the BLE name in the radio check: what one advert holds
 *         (29 characters) and the terminator. */
#define RADIO_PROBE_BLE_BYTES   30U

/** @brief Bytes of the C5's version in the radio check: major, minor, patch. */
#define RADIO_PROBE_FW_BYTES    3U

/** @brief Bytes of a MAC address in the radio check. */
#define RADIO_PROBE_MAC_BYTES   6U

/** @brief The chip model the radio check reads from an ESP32-C5 (esp_chip_model_t). */
#define RADIO_PROBE_CHIP_C5     23U

/**
 * @brief What the radio check looks for (radio_probe_request()).
 *
 * An empty name or a zero channel leaves that part out; with @c discover the
 * channels are found instead of given.
 */
typedef struct {
    char    ssid24[RADIO_PROBE_SSID_BYTES]; /**< The 2.4 GHz network; "" for none.   */
    uint8_t ch24;                           /**< Its channel; 0 for none.            */
    char    ssid5[RADIO_PROBE_SSID_BYTES];  /**< The 5 GHz network; "" for none.     */
    uint8_t ch5;                            /**< Its channel; 0 for none.            */
    char    ble[RADIO_PROBE_BLE_BYTES];     /**< The BLE name to find; "" for none.  */
    bool    card;                           /**< Read the C5's card.                 */
    bool    discover;                       /**< Sweep every channel once and report
                                             *   where the two networks were heard,
                                             *   instead of probing ch24 and ch5.    */
} radio_probe_t;

/** @brief The radio check's answer (RADIO_PROBE_SIG), allocated by the asker. */
typedef struct {
    QEvt     super;
    uint8_t  status;   /**< kode_status_t: KODE_OK, or why the check could not
                        *   run to its end (the readings so far stand):
                        *   KODE_DENIED the link is not up or went,
                        *   KODE_NO_RESOURCES another check runs or the radio
                        *   is being updated, KODE_TIMEOUT the budget ran out,
                        *   or the status of the job that failed.            */
    bool     card;     /**< chip, fw, kode, sdio and mac are the C5's.        */
    uint16_t chip;     /**< esp_chip_model_t; 23 is an ESP32-C5.              */
    uint8_t  fw[RADIO_PROBE_FW_BYTES];  /**< The esp-hosted version: major, minor, patch. */
    uint8_t  host[RADIO_PROBE_FW_BYTES]; /**< The esp-hosted version this image
                        *   was built against; the card is good when the two agree. */
    bool     kode;     /**< The slave's project is Kode's (kode_c5), not stock. */
    uint8_t  sdio_width; /**< SDIO data lines in use; 0 when unknown.          */
    uint32_t sdio_khz; /**< The SDIO clock the host ended up at.               */
    uint8_t  mac[RADIO_PROBE_MAC_BYTES]; /**< The C5's station MAC.           */
    bool     seen24;   /**< The 2.4 GHz network was heard.                    */
    int8_t   rssi24;   /**< Its strongest reading, dBm.                       */
    uint8_t  ch24;     /**< The channel it was heard on; the one asked for if unheard. */
    bool     seen5;    /**< The 5 GHz network was heard.                      */
    int8_t   rssi5;    /**< Its strongest reading, dBm.                       */
    uint8_t  ch5;      /**< The channel it was heard on; the one asked for if unheard. */
    bool     passive5; /**< The 5 GHz channel was listened to passively.      */
    bool     ble_adv;  /**< The board is advertising now.                     */
    bool     ble_rx;   /**< The BLE name was heard.                           */
    int8_t   rssi_rx;  /**< Its reading, dBm.                                 */
    uint32_t card_ms;  /**< What reading the card took, ms.                   */
    uint32_t w24_ms;   /**< What the 2.4 GHz passes took, ms.                 */
    uint32_t w5_ms;    /**< What the 5 GHz passes took, ms.                   */
    uint32_t ble_ms;   /**< What the BLE listen and the advert's return took, ms. */
    uint32_t ms;       /**< The whole check, from the request, ms.            */
} RadioProbeEvt;

/** @brief Why AO_Radio refused the request a BLE state answers (BleStateEvt.refused). */
#define KQP_BLE_REFUSED_NONE      0U  /**< Not refused: the state is the answer.   */
#define KQP_BLE_REFUSED_BUSY      1U  /**< The radio is busy; ask again.           */
#define KQP_BLE_REFUSED_OFF       2U  /**< Bluetooth is off.                       */
#define KQP_BLE_REFUSED_ATTACHED  3U  /**< A console is attached to the link.      */

/** @brief The radio check, asked (RADIO_PROBE_REQ_SIG). */
typedef struct {
    QEvt           super;
    QActive       *who;   /**< The AO the answer goes to.                     */
    RadioProbeEvt *rsp;   /**< Its answer, taken with the request.            */
    radio_probe_t  req;   /**< What to check.                                 */
} RadioProbeReqEvt;

/** @brief The longest name the scan response carries, NUL included: the
 *         packet's 31 bytes less the name's two of length and type. */
#define KQP_BLE_NAME_MAX  30U
/** @brief The most maker data it carries: the packet less two bytes. */
#define KQP_BLE_DATA_MAX  29U

/** @brief What the BLE half is doing (BLE_STATE_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  state;      /**< kdrv_ble_state_t                              */
    bool     addr_known; /**< false: the controller has not synchronised yet.*/
    uint8_t  addr[6];    /**< This board's own BLE address.                 */
    uint8_t  refused;    /**< KQP_BLE_REFUSED_*: why the request this state
                          *   answers was refused; NONE when it was not.    */
    bool     console;    /**< An authenticated central is subscribed to TX. */
    uint16_t tx_drop;    /**< Lines the output ring refused since attach.   */
    uint8_t  bonds;      /**< Bonded peers in the store.                    */
    bool     hidden;     /**< The advert hides the board from all but its
                          *   bonded devices.                               */
    bool     pair_open;  /**< Pair new device's time is running.            */
    bool     pair_wait;  /**< Pair new device was asked with a device
                          *   connected: its time starts when that one
                          *   leaves.                                       */
    uint16_t backlog;    /**< Bytes still queued for the central.           */
    uint16_t retry;      /**< Times the drain waited on NimBLE's buffers.   */
    char     name[KQP_BLE_NAME_MAX]; /**< The name the board goes by: the
                          *   advert's and its Device Name.                 */
} BleStateEvt;

/**
 * @brief A pairing wants a code shown, or has ended (BLE_PAIR_SIG).
 *
 * The two enums are kdrv_ble's, carried as plain integers exactly as
 * BleStateEvt carries kdrv_ble_state_t: this header cannot include a driver
 * one, and the subscriber names the values from kdrv_ble.h, which it already
 * includes for the state.
 */
typedef struct {
    QEvt     super;
    uint8_t  phase;    /**< kdrv_ble_pair_phase_t: SHOW or DONE.           */
    uint32_t passkey;  /**< Six digits, 0..999999. Valid in SHOW.          */
    uint8_t  outcome;  /**< kdrv_ble_pair_outcome_t. Valid in DONE.        */
} BlePairEvt;

/** @brief How long to listen (BLE_SCAN_REQ_SIG). */
typedef struct {
    QEvt     super;
    uint32_t ms;         /**< 0 = KODE_BLE_SCAN_MS; otherwise
                          *   KODE_BLE_SCAN_MIN_MS..KODE_BLE_SCAN_MAX_MS,
                          *   and anything else is refused.              */
} BleScanReqEvt;

/**
 * @brief What the scan response carries from now on (BLE_RSP_REQ_SIG): the
 *        name and the maker data together, the whole of it each time.
 */
typedef struct {
    QEvt    super;
    char    name[KQP_BLE_NAME_MAX];  /**< "" for the board's serial.      */
    uint8_t len;                     /**< Bytes of @c data; 0 for none.   */
    uint8_t data[KQP_BLE_DATA_MAX];  /**< Company id (LE) first.          */
} BleRspReqEvt;

/** @brief Most maker bytes a BleScanEvt carries. Mirrors KDRV_BLE_MFG_MAX
 *         (kdrv_ble.h); radio_ao.c, which sees both, asserts they agree. */
#define KQP_BLE_MFG_MAX  30U

/** @brief Most 16-bit services a BleScanEvt carries. Mirrors
 *         KDRV_BLE_SVC_MAX (kdrv_ble.h), asserted the same way. */
#define KQP_BLE_SVC_MAX  4U

/**
 * @brief One device seen while scanning (BLE_SCAN_ENTRY_SIG).
 *
 * Every field past the name mirrors kdrv_ble_dev_t: @c addr_type is
 * KDRV_BLE_ADDR_PUBLIC or KDRV_BLE_ADDR_RANDOM, as a plain integer.
 */
typedef struct {
    QEvt     super;
    uint8_t  addr[6];
    int8_t   rssi;
    char     name[32];   /**< Empty when the advert carried no name.        */
    uint8_t  addr_type;  /**< 0 public, 1 random.                           */
    uint8_t  mfg_len;    /**< Bytes in @c mfg; 0 when the advert had none.  */
    uint8_t  mfg[KQP_BLE_MFG_MAX];   /**< Maker data, company id (LE) first. */
    uint8_t  svc_n;      /**< Entries in @c svc16.                          */
    uint16_t svc16[KQP_BLE_SVC_MAX]; /**< 16-bit service UUIDs.             */
} BleScanEvt;

/** @brief Bytes of a characteristic's name in the app's service, NUL included. */
#define KQP_BLE_APP_NAME_MAX   16U

/** @brief Bytes of a value of the app's service. */
#define KQP_BLE_APP_VALUE_MAX  244U

/** @brief A central wrote to the app's own service (BLE_APP_WRITE_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  chr;                              /**< The characteristic's place in the table. */
    char     name[KQP_BLE_APP_NAME_MAX];       /**< Its name.                                */
    uint16_t len;                              /**< Bytes in @c data.                        */
    uint8_t  data[KQP_BLE_APP_VALUE_MAX];      /**< What was written, NULs included.         */
} BleAppWriteEvt;

/**
 * @brief Where AO_Radio writes a blocked task's answer to a value
 *        (BleAppValueReqEvt.wait).
 *
 * One static waiter, kode_sys_ble.c's, lent to one call at a time. A caller
 * that gave up may still be answered into it: the next caller's generation
 * tells that late answer from its own, and drops it.
 */
typedef struct {
    khal_sem_t done;   /**< Signalled once the answer is written.               */
    uint32_t   ack;    /**< The request's generation, stamped (release) just
                        *   before @c done.                                     */
    uint8_t    rc;     /**< kode_status_t: kdrv_ble_app_set_value()'s or
                        *   kdrv_ble_app_notify()'s answer.                     */
} BleValueWaiter;

/**
 * @brief Set or notify one value of the app's own service (BLE_APP_VALUE_REQ_SIG).
 *
 * Posted to AO_Radio, the service's owner, with a copy of the bytes. A
 * command carries no waiter; a blocked task's request carries its own.
 */
typedef struct {
    QEvt            super;
    BleValueWaiter *wait;                          /**< The task's waiter; NULL for a command. */
    uint32_t        gen;                           /**< The task's tag for this request.        */
    bool            notify;                        /**< true: notify; false: set the value.     */
    char            name[KQP_BLE_APP_NAME_MAX];    /**< The characteristic's name.              */
    uint16_t        len;                           /**< Bytes in @c data.                       */
    uint8_t         data[KQP_BLE_APP_VALUE_MAX];   /**< The value, NULs included.               */
} BleAppValueReqEvt;

/** @brief A central connected or left (BLE_APP_LINK_SIG). */
typedef struct {
    QEvt super;
    bool connected;   /**< true: a link began; false: it ended. */
} BleAppLinkEvt;

/** @brief One remembered network (WIFI_SAVED_ENTRY_SIG). Name only. */
typedef struct {
    QEvt super;
    char ssid[33];
} WifiSavedEvt;

/**
 * @brief Where the connection stands (WIFI_STATE_SIG).
 *
 * @c ip4 is meaningful only in the got-ip state; @c ssid is what was last
 * asked for, so a failure still says which network refused.
 */
typedef struct {
    QEvt     super;
    uint8_t  state;     /**< kdrv_wifi_state_t                              */
    uint32_t ip4;       /**< Network byte order, as the stack reports it.   */
    char     ssid[33];
    int8_t   rssi;      /**< dBm of the current association; 0 otherwise.   */
    bool     enabled;   /**< The service flag, not the association: what the
                         *   settings toggle shows. A board that is on WiFi
                         *   but not on a network is enabled and idle.       */
    uint8_t  fail;      /**< kdrv_wifi_fail_t: why the last attempt failed or
                         *   the network was lost, KDRV_WIFI_FAIL_NONE when
                         *   nothing has gone wrong since.                   */
    bool     joining;   /**< A join somebody asked for is still out, waiting or
                         *   with the C5: this state is not its answer.      */
    bool     reconnecting; /**< Off the network, and AO_Radio is working on
                         *   getting back on one, waiting between tries or
                         *   until the board wakes included.                 */
} WifiStateEvt;

/* ---- AO_Net -------------------------------------------------------------
 * Every payload here is bounded by a cap from kqp_config.h, and every one of
 * those caps has a twin in kdrv_net.h. The events that carry data from a
 * stranger - MqttMsgEvt above all - say when they had to cut it rather than
 * pretending they did not.
 */

/** @brief A host to ping, and how many times (NET_PING_REQ_SIG). */
typedef struct {
    QEvt    super;
    char    host[KODE_NET_HOST_MAX];
    uint8_t count;
} NetPingReqEvt;

/** @brief One echo's outcome (NET_PING_ENTRY_SIG). */
typedef struct {
    QEvt     super;
    uint16_t seq;
    uint16_t time_ms;   /**< Meaningless when !ok.                          */
    uint8_t  ttl;
    bool     ok;        /**< false: this one timed out.                     */
} NetPingEntryEvt;

/**
 * @brief The end of a ping session (NET_PING_END_SIG).
 *
 * @c ok false means there was no session at all - no network, or the name did
 * not resolve - and every other field is zero. A session where nothing
 * answered is still ok=true with rx=0: "the host is not there" and "we could
 * not ask" are different answers.
 */
typedef struct {
    QEvt     super;
    uint32_t ip4;       /**< What the name resolved to.                     */
    uint16_t tx;
    uint16_t rx;
    uint16_t min_ms;
    uint16_t avg_ms;
    uint16_t max_ms;
    bool     ok;
} NetPingEndEvt;

/** @brief A URL to fetch (NET_HTTP_REQ_SIG). */
typedef struct {
    QEvt super;
    char url[KODE_NET_URL_MAX];
} NetHttpReqEvt;

/**
 * @brief One slice of a response body, as printable text (NET_HTTP_DATA_SIG).
 *
 * A preview, not the bytes. AO_Net rewrites anything unprintable into '.' -
 * a space is printable and stays, and the console quotes the rest - and
 * terminates it, so a body of JPEG or a body with a NUL in it cannot derail
 * the response stream. The bytes themselves never travel on the bus: an app
 * that wants them calls kode_sys_http_get() and receives them in its own buffer.
 */
typedef struct {
    QEvt     super;
    uint16_t len;       /**< Characters in @c text, terminator excluded.    */
    uint8_t  seq;       /**< Which slice: 0, 1, 2...                        */
    char     text[KODE_NET_HTTP_CHUNK];
} NetHttpDataEvt;

/**
 * @brief How a fetch ended (NET_HTTP_DONE_SIG).
 *
 * @c rc is the transport's verdict and @c status is the server's: a 404 with
 * rc=KODE_OK is a page that does not exist, and rc!=KODE_OK with status 0 is
 * a network that did not answer. Collapsing the two would leave a caller
 * unable to tell them apart.
 */
typedef struct {
    QEvt     super;
    int32_t  content_length;  /**< -1 when chunked or undeclared.           */
    uint32_t body_len;        /**< What the body actually held.             */
    uint32_t kept;            /**< What was published above.                */
    uint16_t status;
    uint8_t  rc;              /**< kode_status_t                            */
    bool     truncated;
} NetHttpDoneEvt;

/** @brief A datagram to send (NET_UDP_REQ_SIG). */
typedef struct {
    QEvt     super;
    char     host[KODE_NET_HOST_MAX];
    uint16_t port;
    uint16_t len;
    uint8_t  data[KODE_NET_UDP_MAX];
} NetUdpReqEvt;

/** @brief Whether it left (NET_UDP_DONE_SIG; NET_UDP_RSP_SIG to an asking AO). */
typedef struct {
    QEvt     super;
    uint16_t sent;
    uint8_t  rc;        /**< kode_status_t                                  */
} NetUdpDoneEvt;

/** @brief What a ping session added up to: kode_sys_ping_stats_t, field for field. */
typedef struct {
    uint32_t ip4;      /**< What the name resolved to.                   */
    uint16_t tx;       /**< Echoes sent.                                 */
    uint16_t rx;       /**< Replies received; 0 means nothing answered.  */
    uint32_t min_ms;   /**< Shortest round trip.                         */
    uint32_t avg_ms;   /**< Mean round trip.                             */
    uint32_t max_ms;   /**< Longest round trip.                          */
} NetPingStats;

/** @brief What an HTTP request came back with: kode_sys_http_result_t, field for field. */
typedef struct {
    uint16_t status;         /**< HTTP status code; 0 if nothing answered.  */
    int32_t  content_length; /**< As declared; -1 when chunked or absent.   */
    uint32_t body_len;       /**< What the body actually held.              */
    uint32_t kept;           /**< What fitted in the caller's buffer.       */
    bool     truncated;      /**< body_len > kept.                          */
} NetHttpResult;

/** @brief Answers kode_sys_net_lookup() (NET_LOOKUP_RSP_SIG, @c stats.ip4 alone) and
 *         kode_sys_net_ping() (NET_PING_RSP_SIG). */
typedef struct {
    QEvt         super;
    uint8_t      rc;     /**< kode_status_t: KODE_IO_ERROR with no network,
                          *   KODE_DENIED while AO_Net's worker is busy.   */
    NetPingStats stats;  /**< The session; a lookup's address in @c ip4.   */
} NetHostRspEvt;

/** @brief Answers kode_sys_http_get() and kode_sys_http_post() (NET_HTTP_RSP_SIG). */
typedef struct {
    QEvt          super;
    uint8_t       rc;      /**< kode_status_t: the transport's verdict, as
                            *   NetHostRspEvt's; @c result.status is the
                            *   server's.                                   */
    bool          post;    /**< A POST; false for a GET.                    */
    NetHttpResult result;  /**< Status and lengths.                         */
} NetHttpRspEvt;

/** @brief An ask's lookup or ping (NET_HOST_ASK_SIG). */
typedef struct {
    QEvt           super;
    bool           ping;    /**< Ping @c host; false looks it up.            */
    uint8_t        count;   /**< A ping's echoes, 1..KODE_NET_PING_MAX.      */
    char           host[KODE_NET_HOST_MAX];
    QActive       *who;     /**< The asking AO.                              */
    NetHostRspEvt *rsp;     /**< Its answer, taken with the request.         */
} NetHostAskEvt;

/** @brief An ask's GET or POST (NET_HTTP_ASK_SIG); @c body and @c buf are lent until the answer. */
typedef struct {
    QEvt           super;
    bool           post;    /**< POST @c body; false GETs.                   */
    char           url[KODE_NET_URL_MAX];
    char           ctype[KODE_NET_CTYPE_MAX]; /**< A POST's Content-Type; empty sends none. */
    const void    *body;    /**< A POST's body, read in place.               */
    uint32_t       len;     /**< Its bytes.                                  */
    void          *buf;     /**< Where the answer's body goes, in place.     */
    uint32_t       cap;     /**< Its room.                                   */
    QActive       *who;     /**< The asking AO.                              */
    NetHttpRspEvt *rsp;     /**< Its answer, taken with the request.         */
} NetHttpAskEvt;

/** @brief An ask's datagram (NET_UDP_ASK_SIG); @c data is lent until the answer. */
typedef struct {
    QEvt           super;
    char           host[KODE_NET_HOST_MAX];
    uint16_t       port;
    uint16_t       len;     /**< Bytes at @c data.                           */
    const void    *data;    /**< Read in place.                              */
    QActive       *who;     /**< The asking AO.                              */
    NetUdpDoneEvt *rsp;     /**< Its answer, taken with the request.         */
} NetUdpAskEvt;

/** @brief This board's addresses and the MQTT client's state (NET_INFO_SIG). */
typedef struct {
    QEvt     super;
    uint32_t ip4;
    uint32_t gw4;
    uint32_t mask4;
    uint32_t dns4;
    uint8_t  mqtt;      /**< kdrv_mqtt_state_t                              */
    bool     online;    /**< AO_Net has an address to work with.            */
    uint32_t rx_dropped; /**< MQTT messages dropped since boot because the
                          *   driver's receive ring (KDRV_MQTT_RX_RING) was
                          *   full when they arrived.                        */
} NetInfoEvt;

/** @brief The MQTT client's own news, off esp-mqtt's task (NET_MQTT_LINK_SIG). */
typedef struct {
    QEvt    super;
    uint8_t evt;        /**< kdrv_net_mqtt_evt_t                            */
    bool    session_present; /**< CONNECTED: the broker resumed the session
                              *   it held for this client id.               */
} NetMqttLinkEvt;

/** @brief A broker to connect to (MQTT_UP_REQ_SIG). */
typedef struct {
    QEvt super;
    char uri[KODE_MQTT_URI_MAX];
    char user[KODE_MQTT_USER_MAX];
    char pass[KODE_MQTT_PASS_MAX];
} MqttUpEvt;

/** @brief A topic to subscribe to or drop (MQTT_SUB_REQ_SIG, _UNSUB_). */
typedef struct {
    QEvt super;
    char topic[KODE_MQTT_TOPIC_MAX];
} MqttTopicEvt;

/** @brief Something to publish (MQTT_PUB_REQ_SIG). */
typedef struct {
    QEvt     super;
    char     topic[KODE_MQTT_TOPIC_MAX];
    uint16_t len;
    uint8_t  data[KODE_MQTT_PAYLOAD_MAX];
} MqttPubEvt;

/** @brief Where the MQTT client is, and why it is not up (MQTT_STATE_SIG). */
typedef struct {
    QEvt    super;
    char    uri[KODE_MQTT_URI_MAX];   /**< Empty before the first connect.  */
    uint8_t state;      /**< kdrv_mqtt_state_t                              */
    uint8_t err;        /**< kdrv_mqtt_err_t: meaningful when not up.       */
} MqttStateEvt;

/** @brief Where the Kode channel stands (CHANNEL_STATE_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  state;     /**< chan_state_t (kaos.h).                         */
    char     why[24];   /**< The reason, one word or two; "" when none.     */
    uint32_t since_s;   /**< Seconds in this state.                         */
    uint32_t next_s;    /**< WAITING: seconds until the next attempt; 0
                         *   otherwise.                                     */
    uint32_t lost;      /**< Reply lines dropped on their way out.          */
    char     broker[KODE_MQTT_URI_MAX]; /**< `mqtts://host:port` once the api
                                         *   has named it; "" before.      */
    bool     fast;      /**< The fast mode is on: the station was asked for
                         *   no power save.                                 */
    uint8_t  shot;      /**< kshot_mode_t (kaos.h): the upload running now. */
    uint32_t shot_sent; /**< Frames the server took: the running stream's,
                         *   or the last one's once it ended.               */
    uint16_t shot_fps_x10; /**< The frames a second that stream achieved,
                            *   times ten.                                  */
    uint32_t shot_left_s;  /**< Seconds the running stream has left; 0 when
                            *   none runs.                                  */
    uint32_t keep_s;    /**< UP: the session age the connect asked the broker
                         *   for, in seconds (0xFFFFFFFF is no expiry); 0
                         *   otherwise.                                     */
    uint8_t  session;   /**< chan_session_t (kaos.h): what the broker said of
                         *   the session at the last connect.               */
} ChannelStateEvt;

/**
 * @brief A message from the broker (MQTT_MSG_SIG).
 *
 * The two cut flags are the contract: a topic or payload longer than this
 * event carries arrives shortened and *says so*, because a message silently
 * truncated is a message that lies, and one silently dropped is worse. A
 * payload that needs more than this belongs behind AO_Storage's loan
 * protocol, not on the event bus.
 */
typedef struct {
    QEvt     super;
    char     topic[KODE_MQTT_TOPIC_MAX];
    uint16_t len;
    bool     topic_cut;
    bool     data_cut;
    bool     retained;  /**< The broker replayed a stored message: it was
                         *   sent before this session began.               */
    uint8_t  data[KODE_MQTT_PAYLOAD_MAX];
} MqttMsgEvt;

/**
 * @brief One rectangle of pixels for the panel (DISPLAY_FLUSH_SIG).
 *
 * @c pixels points into a draw buffer belonging to the renderer (kscreen,
 * inside AO_Gui in kodeOS or an app's own AO). It stays valid because the
 * renderer is blocked on the flush semaphore for the whole transfer and
 * cannot start rendering into that buffer again until AO_Display releases it -
 * so the pointer cannot dangle.
 */
typedef struct {
    QEvt super;
    int32_t  x1;
    int32_t  y1;
    int32_t  x2;
    int32_t  y2;
    uint8_t *pixels;
    bool     last;    /**< Final chunk of this frame: the driver assembles
                       *   chunks into the off-screen framebuffer and flips
                       *   on this one - the flip is what makes scrolling
                       *   tear-free, so a producer that never sets it never
                       *   gets a frame on the glass. */
} FlushEvt;

/** @brief Panel brightness in percent (DISPLAY_BRIGHTNESS_SIG). */
typedef struct {
    QEvt super;
    uint8_t percent;
    bool    persist;   /**< true: this is the user's setting - store it.
                        *   false: a transient (the slider mid-drag). */
} BrightnessEvt;

/**
 * @brief Whether the panel is up, and how bright (DISPLAY_STATE_SIG).
 *
 * The flush semaphore is kdrv_display's alone; a renderer waits through
 * kdrv_display_flush_wait(). One completion per flush is only checkable
 * while one file decides them all, and a handle in an event is an invitation
 * for a second one to decide.
 */
typedef struct {
    QEvt super;
    bool       up;
    uint8_t    bright;   /**< What the panel is driven at, in percent. A
                          *   slider preview or AppMgr's blank is reported
                          *   as it is, 0 included, and the `display` row
                          *   leaves it on the user's value. It seeds the
                          *   settings slider: the GUI never reads the
                          *   settings store itself, so the value the boot
                          *   applied has to ride the announcement it is
                          *   already listening to.
                          *   Meaningless when @c up is false.              */
} DisplayStateEvt;

/** @brief Copy the frame on the glass for @c who (DISPLAY_CAPTURE_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;      /**< Where the response goes.                         */
} DisplayCaptureReqEvt;

/**
 * @brief A frozen capture, ready to be read (DISPLAY_CAPTURE_RSP_SIG).
 *
 * The buffer is not in here, and that is deliberate: 402 KB is not a copy
 * anybody can put in an event, so the requester collects it with
 * display_capture_buf() and holds it until display_capture_release(). Same
 * loan, and same reasoning, as storage_buf().
 *
 * @c painted false means nothing has been flushed since boot, so the image is
 * black. Not an error - the glass is black too - but worth knowing before
 * spending a minute wondering why.
 */
typedef struct {
    QEvt     super;
    uint32_t len;      /**< Bytes in the image, header included.             */
    uint16_t w;
    uint16_t h;
    uint8_t  status;   /**< kode_status_t                                    */
    bool     painted;
} DisplayCaptureRspEvt;

/** @brief Begin streaming the panel (DISPLAY_MIRROR_START_SIG). */
typedef struct {
    QEvt    super;
    uint8_t scale;     /**< 1 or 2; anything else is corrected to 1.        */
    uint8_t quality;   /**< 1..100; 0 or out of range is corrected to 70.   */
} MirrorStartEvt;

/** @brief The host has received up to @c seq (DISPLAY_MIRROR_ACK_SIG). */
typedef struct {
    QEvt     super;
    uint16_t seq;
} MirrorAckEvt;

/**
 * @brief The mirror's live state, in answer to DISPLAY_MIRROR_INFO_REQ_SIG
 *        (DISPLAY_MIRROR_INFO_SIG).
 *
 * Everything console_ao.c's `mirror_info` needs to say what an idle-looking
 * viewer is actually doing: whether the stream merely has nothing new to
 * send, or has paused on a stall, or has never been started at all - three
 * situations that otherwise look identical from a host watching a black or
 * unmoving screen.
 */
typedef struct {
    QEvt     super;
    bool     on;         /**< A mirror_start is in effect (paused counts as
                          *   on - see @c paused).                          */
    bool     paused;     /**< A stall suspended the stream; it holds no
                          *   memory and costs no ticks until the next ack.
                          *   Meaningless when @c on is false.              */
    uint8_t  scale;
    uint8_t  quality;
    uint8_t  window;     /**< Frames that may be outstanding at once.       */
    uint16_t in_flight;  /**< Frames sent but not yet acknowledged - never
                          *   more than @c window.                          */
    uint32_t sent;       /**< Frames sent this session.                    */
    uint32_t stalls;     /**< Times this session has paused.               */
} MirrorInfoEvt;

/** @brief Encode the glass as one JPEG for @c who (DISPLAY_SHOT_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;      /**< Where the response goes.                         */
    uint8_t  quality;  /**< 1..100 for the JPEG encoder.                     */
} DisplayShotReqEvt;

/** @brief What display_jpeg_request() encodes, and where the JPEG goes. */
typedef struct {
    const uint8_t *src;      /**< RGB565, w*h*2 bytes, row-major: the requester's,
                              *   read until the answer.                    */
    uint16_t       w;        /**< Width in pixels.                           */
    uint16_t       h;        /**< Height in pixels.                          */
    uint8_t        quality;  /**< 1..100.                                    */
    uint8_t       *dst;      /**< The requester's buffer for the JPEG.       */
    uint32_t       cap;      /**< Its room, bytes.                           */
} display_jpeg_t;

/**
 * @brief Encode the requester's RGB565 into its own buffer (DISPLAY_JPEG_REQ_SIG).
 *
 * The blocking request of a worker that has pixels of its own (AO_TestDot's
 * photos): AO_Display owns the one engine, encodes in its step and copies the
 * JPEG into @c job.dst before it answers, so nothing the requester keeps
 * points into the engine's buffer. The answer is the imu_read_request()
 * shape: @c len and @c rc written, @c gen stamped into @c ack, then @c done
 * signalled - and nothing at all for a request whose @c gen no longer
 * matches @c cur, because its caller stopped waiting.
 */
typedef struct {
    QEvt            super;
    display_jpeg_t  job;   /**< The pixels and where the JPEG goes.        */
    uint32_t       *len;   /**< Receives the JPEG's length.                */
    uint32_t       *rc;    /**< Receives the kode_status_t.                */
    khal_sem_t      done;  /**< Signalled once @c len and @c rc are written. */
    uint32_t        gen;   /**< The requester's tag for this request.      */
    uint32_t const *cur;   /**< The requester's current tag.               */
    uint32_t       *ack;   /**< Where @c gen is written back, just before
                            *   @c done.                                   */
} DisplayJpegReqEvt;

/**
 * @brief A shot, ready to be read (DISPLAY_SHOT_RSP_SIG).
 *
 * The JPEG is not in here: the requester collects it with display_shot_buf()
 * and holds it until display_shot_release(), the capture's loan. Nothing is
 * lent unless @c status is KODE_OK.
 */
typedef struct {
    QEvt     super;
    uint32_t len;      /**< Bytes of JPEG in display_shot_buf().             */
    uint8_t  status;   /**< kode_status_t: KODE_OK; KODE_DENIED when the
                        *   codec is busy or a shot is already lent;
                        *   KODE_NO_RESOURCES with no codec, no framebuffer
                        *   or no PSRAM; KODE_ERROR when the frame is larger
                        *   than KSHOT_JPEG_CAP.                             */
} DisplayShotRspEvt;

/** @brief Upload the glass to the Kode channel's `shots` link (NET_SHOT_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;                          /**< Where the answer goes.      */
    char     token[KSHOT_TOKEN_MAX + 1U];  /**< The upload token: the link's
                                            *   last path segment.          */
    uint8_t  fps;                          /**< Frames a second; 0 with
                                            *   @c secs 0 is one capture.   */
    uint16_t secs;                         /**< How long a stream runs.     */
} NetShotReqEvt;

/** @brief End the running stream of uploads (NET_SHOT_STOP_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;      /**< Where the answer (NET_SHOT_RSP_SIG) goes.       */
} NetShotStopEvt;

/**
 * @brief How an upload ended (NET_SHOT_RSP_SIG).
 *
 * @c result says which: the console prints one line per value, and
 * kode_status_t has no word for "the server refused" or "the channel is not
 * there".
 */
typedef struct {
    QEvt     super;
    uint8_t  result;            /**< kshot_result_t.                        */
    uint16_t http;              /**< The server's status; 0 when none came. */
    uint32_t bytes;             /**< JPEG bytes sent; 0 when none were.     */
    char     id[KSHOT_ID_MAX];  /**< The capture's id from a 201; "" else.  */
    uint32_t seq;               /**< The capture's sequence from a 201.     */
    uint8_t  fps;               /**< A stream's frames a second.            */
    uint16_t secs;              /**< A stream's length in seconds.          */
} NetShotRspEvt;

/**
 * @brief All six buttons, right now (BTN_STATE_SIG).
 *
 * One event for the whole bank rather than one per button: the six are read
 * in a single expander transaction, so they are one sample, not six.
 */
typedef struct {
    QEvt    super;
    uint8_t pressed;   /**< Bit (1 << kdrv_button_t) set = that button down. */
    bool    ok;        /**< false: the expander did not answer the read.     */
} ButtonsStateEvt;

/* What one step of the guided button test is saying. */
#define BTN_TEST_PROMPT   0U   /**< Press and release `button`, now.           */
#define BTN_TEST_PASS     1U   /**< Exactly `button` was pressed.              */
/* 2 is unused: the phases below keep their numbers. */
#define BTN_TEST_TIMEOUT  3U   /**< Nothing was, within KODE_BUTTONS_TEST_STEP_MS. */
#define BTN_TEST_DONE     4U   /**< The last step is over; the verdict follows. */
#define BTN_TEST_STUCK    5U   /**< Judged, but still down KODE_BUTTONS_TEST_RELEASE_MS
                                *   later: `pressed` says what; the step is a
                                *   fail and is not asked again.               */
#define BTN_TEST_WRONG    6U   /**< Another button instead of the asked one: not
                                *   a fail yet. The same button is asked for
                                *   again; the third wrong press on a step is
                                *   BTN_TEST_GIVEUP.                           */
#define BTN_TEST_DOUBLE   7U   /**< The asked-for button and another went down
                                *   in the same press: not a fail yet. The same
                                *   button is asked for again; the second time
                                *   on a step it is BTN_TEST_DOUBLEFAIL.       */
#define BTN_TEST_DOUBLEFAIL 8U /**< A double for the second time on a step:
                                *   `pressed` names both. The step is a fail.  */
#define BTN_TEST_HELD     9U   /**< A button was already down when the step's
                                *   prompt went out: `pressed` says which. The
                                *   step is a fail.                            */
#define BTN_TEST_GIVEUP  10U   /**< Three wrong presses on one step: the step
                                *   and the test fail, and the run ends.       */

/**
 * @brief One step of the guided button test (BTN_TEST_SIG).
 *
 * The console prints a prompt and every outcome as a record; the GUI shows
 * the prompt on the glass. `pressed` carries the sample that ended the step
 * (bit (1 << kdrv_button_t)), so a failure names what was pressed instead.
 */
typedef struct {
    QEvt    super;
    uint8_t phase;     /**< BTN_TEST_*                                       */
    uint8_t step;      /**< 1-based.                                          */
    uint8_t total;     /**< How many steps the script has.                    */
    uint8_t button;    /**< kdrv_button_t: the one this step asks for.        */
    uint8_t pressed;   /**< The sample that ended the step; 0 for a prompt.  */
    uint8_t origin;    /**< TEST_ORIGIN_AUTOTEST, or the client that asked (TestReqEvt). */
    uint16_t ms;       /**< How long the press lasted, in milliseconds; 0 when
                        *   the phase does not measure one.                  */
} ButtonsTestEvt;

/* What one AUDIO_TEST_SIG record is about. The row has two steps, and the
 * phases below are what each of them produces: step 1 reports its broadband
 * summary and then one record per detector bin, and step 2 reports a tone it
 * is about to emit and then the window that tone came back in. */
#define AUDIO_TEST_MIC     0U  /**< Step 1's summary: RMS, peak, clipping. */
#define AUDIO_TEST_BIN     1U  /**< Step 1: one bin's floor.               */
#define AUDIO_TEST_TONE    2U  /**< Step 2: about to emit @c hz.           */
#define AUDIO_TEST_PROBE   3U  /**< Step 2: the window @c hz came back in. */

/* Bits of AudioTestEvt.flags. */
#define AUDIO_TF_PASS   0x01U  /**< This record's own verdict.             */
#define AUDIO_TF_STUCK  0x02U  /**< MIC: every sample was identical.       */
#define AUDIO_TF_CLIP   0x04U  /**< MIC: the input is railed.              */
#define AUDIO_TF_LOUD   0x08U  /**< MIC: the bench is too loud to judge.   */
#define AUDIO_TF_LOOP   0x10U  /**< The codec's internal DAC-to-ADC loopback
                                *   is on, so nothing measured means
                                *   anything. Its own bit because it
                                *   condemns the run rather than the board. */

/**
 * @brief One step of the audio production row (AUDIO_TEST_SIG).
 *
 * The console prints every record as a line; nothing else subscribes,
 * because unlike the button and touch rows this one needs nobody at the
 * glass. Levels are tenth-dBFS integers, never floats: the wire, the log and
 * this event all carry the same integer the detector produced.
 */
typedef struct {
    QEvt     super;
    uint8_t  phase;      /**< AUDIO_TEST_*                                  */
    uint8_t  step;       /**< 1 or 2.                                       */
    uint8_t  total;      /**< Always 2.                                     */
    uint8_t  attempt;    /**< Which attempt of step 2, 1-based; 0 in step 1. */
    uint8_t  tries;      /**< How many attempts the script allows.          */
    uint8_t  flags;      /**< AUDIO_TF_*                                    */
    uint16_t hz;         /**< The frequency this record is about.           */
    int16_t  level_x10;  /**< That bin's level. MIC: the broadband RMS.     */
    int16_t  other_x10;  /**< PROBE: the loudest of the other seven.        */
    int16_t  floor_x10;  /**< PROBE: step 1's floor at @c hz. MIC: the peak. */
} AudioTestEvt;

/**
 * @brief What the audio path is and what it is set to: kdrv_es8311_info_t
 *        field for field, for IrInfoEvt's reason (this header cannot name it).
 */
typedef struct {
    uint32_t rate;     /**< Always the board's one rate.                   */
    bool     present;  /**< A codec answered and identified itself.        */
    bool     open;     /**< Analog up and the DAC live.                    */
    bool     busy;     /**< A sound is with the codec's worker.            */
    uint8_t  bits;     /**< Bits per sample.                               */
    uint8_t  volume;   /**< 0 to 100.                                      */
    uint8_t  mic_db;   /**< 0 to 42, in 6 dB steps.                        */
    bool     mute;     /**< The output is muted.                           */
} AudioInfo;

/** @brief Answers kode_sys_audio_info_read() (AUDIO_INFO_RSP_SIG). */
typedef struct {
    QEvt      super;
    uint8_t   rc;      /**< kode_status_t: KODE_OK; @c info.present says a codec is fitted. */
    AudioInfo info;    /**< A copy.                                        */
} AudioInfoRspEvt;

/**
 * @brief Ask what the audio path is (AUDIO_INFO_REQ_SIG).
 *
 * @c out is untyped rather than a kdrv_es8311_info_t*, for IrLastEvt's
 * reason: naming that type here would mean including kdrv_es8311.h, which
 * kqp's own CMakeLists.txt forbids this header.
 *
 * Answered inside the step that receives it, so the wait normally ends on
 * that answer - but it is a bounded wait, and one that ended first would
 * otherwise leave AO_Audio writing into a struct that has gone.
 */
typedef struct {
    QEvt        super;
    void       *out;    /**< kdrv_es8311_info_t*.                          */
    khal_sem_t  done;   /**< Signalled once @c out is filled.              */
    uint32_t    gen;    /**< The requester's tag for this request.          */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                          */
    QActive    *who;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    AudioInfoRspEvt *rsp; /**< Its answer, taken with the request.        */
} AudioInfoReqEvt;

/**
 * @brief Emit one tone (AUDIO_TONE_REQ_SIG).
 *
 * @c hz is snapped to the nearest detector bin before anything is emitted,
 * so a bench and the production row are always measuring the same eight
 * frequencies. Whoever reports the outcome reports the snapped value.
 */
typedef struct {
    QEvt     super;
    uint16_t hz;       /**< What was asked for.                            */
    uint16_t ms;       /**< How long to hold it.                           */
    uint8_t  amp_pct;  /**< Peak amplitude as a percentage of full scale.  */
} AudioToneEvt;

/**
 * @brief Read, write or dump one field of the codec (AUDIO_FIELD_REQ_SIG).
 *
 * The field is the driver's own enum value; whoever posts this has already
 * resolved the name, so AO_Audio never handles a string. The same three
 * FIELD_OP_* verbs the PMIC, the gauge and the expander take. Every request
 * is answered with an AudioFieldEvt, a refusal included.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;      /**< FIELD_OP_*                                      */
    uint16_t field;   /**< kdrv_es8311_field_t                             */
    int32_t  value;   /**< For FIELD_OP_SET, in the field's unit.          */
} AudioFieldReqEvt;

/** @brief Write a setting's latest value (AUDIO_SETTING_REQ_SIG). */
typedef struct {
    QEvt     super;
    uint16_t field;   /**< KDRV_ES_VOL, KDRV_ES_MIC_DB or KDRV_ES_OUT_MUTE. */
} AudioSettingReqEvt;

/**
 * @brief The answer to an AudioFieldReqEvt (AUDIO_FIELD_SIG).
 *
 * For a read, @c value is the field; for a write, the value read back after
 * it; for a dump, nothing. @c rc is the driver's verdict: KODE_DENIED for a
 * field that cannot be written or a value out of its range, KODE_IO_ERROR
 * when the chip did not answer or did not keep the write.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint16_t field;
    uint8_t  rc;      /**< kode_status_t                                   */
    int32_t  value;
} AudioFieldEvt;

/**
 * @brief How a recording to a file ended (AUDIO_FILE_RSP_SIG).
 *
 * @c path is the one the write actually used rather than the one that was
 * asked for, which is the same thing today and is the honest one to report
 * either way: whoever prints this did not necessarily post the request.
 * Also the answer kode_sys_audio_record_file() gets, posted rather than
 * published.
 */
typedef struct {
    QEvt     super;
    char     path[KODE_PATH_MAX];
    uint32_t bytes;   /**< How long the file is, header included.          */
    uint32_t ms;      /**< How much sound is in it.                        */
    uint8_t  rc;      /**< kode_status_t                                   */
} AudioFileRspEvt;

/**
 * @brief Record for a while, into a file (AUDIO_REC_FILE_REQ_SIG).
 *
 * @c path is already resolved: the console does that, because it is the one
 * that knows the current directory, and AO_Audio owns neither it nor the
 * filesystem. The bytes go out through AO_Storage, whose whole-buffer write
 * has no size ceiling - unlike the facade's file calls, which are bounded to
 * a few kilobytes on purpose and say so.
 *
 * The waiter fields are optional. The console posts with none of them and
 * closes on the published AudioFileRspEvt; a task posts with them and sleeps.
 */
typedef struct {
    QEvt        super;
    char        path[KODE_PATH_MAX];
    uint32_t    ms;     /**< How long to record.                           */
    uint32_t   *rc;     /**< Where the outcome goes. May be NULL.          */
    uint32_t   *bytes;  /**< How long the file is. May be NULL.            */
    khal_sem_t  done;   /**< Signalled when it is over. May be NULL.       */
    uint32_t    gen;
    uint32_t   *ack;
    QActive    *who;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    AudioFileRspEvt *rsp; /**< Its answer, taken with the request.        */
} AudioFileReqEvt;

/**
 * @brief How a play, a recording or the stream's open or close ended.
 *
 * The answer kode_sys's play, recording and stream requests get, posted to
 * the asking AO under AUDIO_DONE_SIG, AUDIO_REC_RSP_SIG,
 * AUDIO_STREAM_OPEN_RSP_SIG and AUDIO_STREAM_CLOSE_RSP_SIG. Exactly one goes
 * out per request, whichever way it ended: the samples ran out, the worker
 * failed, its guard fired, or the codec was busy and the request was refused
 * before it ever started.
 */
typedef struct {
    QEvt     super;
    uint8_t  rc;     /**< kode_status_t                                    */
    uint32_t frames; /**< AUDIO_REC_RSP_SIG: how many were recorded.       */
} AudioDoneEvt;

/**
 * @brief Play or record a whole sound (AUDIO_PLAY_REQ_SIG, AUDIO_REC_REQ_SIG),
 *        or open or close the stream (AUDIO_STREAM_OPEN_REQ_SIG,
 *        AUDIO_STREAM_CLOSE_REQ_SIG, with no buffer and no frames).
 *
 * The buffer belongs to the caller and outlives the request, which is safe
 * here for the one reason a pointer in an event is ever safe in kodeOS:
 * there is exactly one writer and nothing reads it until that writer is
 * done. The facade owns the only buffer these are ever posted with.
 *
 * @c gen and @c ack are IrCaptureEvt's arrangement, and for its reason: a
 * binary semaphore carries one token whoever left it, so a requester woken
 * by a token it did not earn reads a generation that is not its own and
 * knows to keep waiting.
 */
typedef struct {
    QEvt        super;
    int16_t    *buf;    /**< Mono 16-bit frames at the board's rate.       */
    uint32_t    frames; /**< To play, or the most to record.               */
    uint32_t   *got;    /**< Recording: how many arrived. May be NULL.     */
    uint32_t   *rc;     /**< Where the outcome goes, as a kode_status_t.   */
    khal_sem_t  done;   /**< Signalled once @c rc and @c got are written.  */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen is written back, just before
                         *   @c done. May be NULL.                         */
    QActive    *who;    /**< The AO @c rsp is posted to; NULL for a waiter. */
    AudioDoneEvt *rsp;  /**< Its answer, taken with the request.           */
} AudioBufEvt;

#define IMU_AXES  3U  /**< Axes of each IMU sensor.                        */
#define MAG_AXES  3U  /**< Axes of the magnetometer.                       */

/** @brief One IMU sample: kdrv_qmi8658_sample_t field for field, for AudioInfo's reason. */
typedef struct {
    int32_t a_mg[IMU_AXES];    /**< Acceleration per axis, mg.             */
    int32_t w_mdps[IMU_AXES];  /**< Rotation per axis, mdps.               */
    int32_t temp_dc;           /**< Die temperature, tenths of a degree.   */
} ImuSample;

/** @brief Answers kode_sys_imu_read() (IMU_SAMPLE_RSP_SIG). */
typedef struct {
    QEvt      super;
    uint8_t   rc;      /**< kode_status_t: KODE_IO_ERROR with no IMU,
                        *   KODE_DENIED while its self-test holds it.     */
    ImuSample sample;  /**< Zero unless @c rc is KODE_OK.                  */
} ImuSampleRspEvt;

/**
 * @brief Ask AO_Imu for one sample (IMU_READ_REQ_SIG).
 *
 * @c out is untyped rather than a kdrv_qmi8658_sample_t*, for
 * AudioInfoReqEvt's reason: naming that type here would mean including
 * kdrv_qmi8658.h, which kqp's own CMakeLists.txt forbids this header.
 *
 * Answered inside the step that receives it while the part is awake, and
 * once the gyroscope has turned on when the request found it resting, so the
 * wait normally ends on that answer - but it is a bounded wait, and one that
 * ended first would otherwise leave AO_Imu writing into a struct that has
 * gone. When the IMU
 * is not fitted, @c out is left exactly as the caller gave it and @c rc says
 * so.
 *
 * A request can be queued behind work that outlives its caller's wait, so
 * @c cur is where the requester keeps the generation it is waiting on NOW.
 * A request whose @c gen no longer matches it has nobody waiting, and AO_Imu
 * writes nothing for it and signals nothing: the answer it would give lands
 * in the very storage the newer request is answered into.
 */
typedef struct {
    QEvt            super;
    void           *out;  /**< kdrv_qmi8658_sample_t*.                     */
    uint32_t       *rc;   /**< The read's kode_status_t. May be NULL.      */
    khal_sem_t      done; /**< Signalled once @c out and @c rc are written. */
    uint32_t        gen;  /**< The requester's tag for this request.       */
    uint32_t const *cur;  /**< The requester's current tag. May be NULL:
                           *   the request is then always answered.        */
    uint32_t       *ack;  /**< Where @c gen is written back, just before
                           *   @c done. May be NULL.                       */
    QActive        *who;  /**< The AO @c rsp is posted to; NULL for a waiter. */
    ImuSampleRspEvt *rsp; /**< Its answer, taken with the request.         */
} ImuReadEvt;

/**
 * @brief Arm the FIFO for an external witness capture (IMU_CAPTURE_REQ_SIG).
 *
 * The answer goes back to @c who rather than onto the bus, because a capture
 * belongs to one requester: two of them sharing a FIFO would each drain half
 * of the other's window.
 */
typedef struct {
    QEvt     super;
    QActive *who;   /**< Where the IMU_ARMED_SIG goes.                     */
} ImuCaptureReqEvt;

/** @brief Whether the FIFO is armed and the window is running (IMU_ARMED_SIG). */
typedef struct {
    QEvt    super;
    uint8_t rc;    /**< kode_status_t: KODE_DENIED when somebody else holds
                    *   the capture, KODE_IO_ERROR when the chip is absent. */
} ImuArmedEvt;

/**
 * @brief Drain the armed window and measure it (IMU_DRAIN_REQ_SIG).
 *
 * @c hz is the frequency the answer is about: the drain reports the energy at
 * that bin as well as the window's broadband level, so the caller that drove
 * a motor at a frequency gets its own bin back rather than a total that any
 * movement would raise.
 */
typedef struct {
    QEvt     super;
    QActive *who;   /**< Where the IMU_ENERGY_SIG goes.                    */
    uint16_t hz;    /**< The bin to measure.                               */
} ImuDrainReqEvt;

/**
 * @brief What one drained window held (IMU_ENERGY_SIG).
 *
 * Levels are tenth-milli-g integers, never floats: the event, the log line
 * and the console record all carry the same number the detector produced.
 */
typedef struct {
    QEvt     super;
    uint8_t  rc;       /**< kode_status_t                                  */
    uint16_t n;        /**< Samples the window actually held.              */
    int32_t  bin_x10;  /**< Energy at the requested bin, in 0.1 mg.        */
    int32_t  rms_x10;  /**< The window's broadband RMS, in 0.1 mg.         */
    uint8_t  axis;     /**< Which axis the bin was strongest on.           */
} ImuEnergyEvt;

/* What one IMU_TEST_SIG record is about. Three steps, and each of them puts
 * its own readings in the four scalars the event carries - which is why they
 * are named a..d rather than after any one step's meaning. */
#define IMU_TEST_WHOAMI   0U   /**< step 1: a = whoami read.                */
#define IMU_TEST_ST       1U   /**< step 2: phase per sensor: a, b, c = the
                                *   per-axis deltas, d = 0 XL / 1 GY.      */
#define IMU_TEST_GRAVITY  2U   /**< step 3: a = |a| mg, b = |w| mdps,
                                *   c = temperature in tenths of a degree. */

/** @brief Bit of ImuTestEvt.flags: this record's own verdict. */
#define IMU_TF_PASS       0x01U

/**
 * @brief One step of the IMU production row (IMU_TEST_SIG).
 *
 * The console prints every record as a line; nothing else subscribes,
 * because unlike the button and touch rows this one needs nobody at the
 * glass.
 */
typedef struct {
    QEvt    super;
    uint8_t step;   /**< 1-based.                                          */
    uint8_t phase;  /**< IMU_TEST_*                                        */
    uint8_t flags;  /**< IMU_TF_*                                          */
    int32_t a;      /**< WHOAMI: what the part answered. ST: the X delta.
                     *   GRAVITY: |a| in mg.                               */
    int32_t b;      /**< ST: the Y delta. GRAVITY: |w| in mdps.            */
    int32_t c;      /**< ST: the Z delta. GRAVITY: the die temperature, in
                     *   tenths of a degree.                               */
    int32_t d;      /**< ST: which sensor this phase was, 0 XL / 1 GY.     */
} ImuTestEvt;

/** @brief Answers kode_sys_mag_read() (MAG_SAMPLE_RSP_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  rc;            /**< kode_status_t: KODE_IO_ERROR with no
                             *   magnetometer, KODE_ERROR with no conversion
                             *   ready (ask again).                        */
    int32_t  mg[MAG_AXES];  /**< Raw milligauss on the chip's own axes;
                             *   zero unless @c rc is KODE_OK.             */
} MagSampleRspEvt;

/**
 * @brief Ask AO_Imu for one magnetometer sample (MAG_READ_REQ_SIG).
 *
 * ImuReadEvt's own shape, answered the same way, inside the step that
 * receives it, from the part converting on its own. @c out is an int32_t[3]
 * rather than a struct: the field on the chip's own three axes in
 * milligauss, and nothing else - no heading and no calibration, exactly as
 * the chip measures it. When the magnetometer is not fitted, @c out is left
 * exactly as the caller gave it and @c rc says KODE_IO_ERROR.
 *
 * @c out must be storage owned for the whole boot, not a caller's own stack
 * frame, and @c cur is carried for ImuReadEvt's reason: the wait on @c done
 * is bounded, and a request answered after its caller gave up must land
 * somewhere that still exists and be told apart from the next one.
 */
typedef struct {
    QEvt            super;
    void           *out;  /**< int32_t[3], milligauss.                      */
    uint32_t       *rc;   /**< The read's kode_status_t. May be NULL.      */
    khal_sem_t      done; /**< Signalled once @c out and @c rc are written. */
    uint32_t        gen;  /**< The requester's tag for this request.       */
    uint32_t const *cur;  /**< The requester's current tag. May be NULL:
                           *   the request is then always answered.        */
    uint32_t       *ack;  /**< Where @c gen is written back, just before
                           *   @c done. May be NULL.                       */
    QActive        *who;  /**< The AO @c rsp is posted to; NULL for a waiter. */
    MagSampleRspEvt *rsp; /**< Its answer, taken with the request.         */
} MagReadEvt;

/* What a MagCalEvt asks. */
#define MAG_CAL_OP_GET    0U   /**< Read it into @c out.                    */
#define MAG_CAL_OP_SET    1U   /**< Store the numbers the event carries.    */
#define MAG_CAL_OP_CLEAR  2U   /**< Forget it.                              */
#define MAG_CAL_AXES      MAG_AXES   /**< Numbers of each kind a set carries: one per axis. */

/** @brief A magnetometer calibration: kdrv_qmc5883p_cal_t field for field, for AudioInfo's reason. */
typedef struct {
    int32_t offset_mg[MAG_AXES];    /**< Subtracted from the raw reading, mG, within +-9999. */
    int32_t scale_milli[MAG_AXES];  /**< Multiplies what is left, thousandths, 250 to 4000.  */
} MagCorr;

/**
 * @brief Answers kode_sys_mag_cal_get(), _set() and _clear() (MAG_CAL_RSP_SIG).
 *
 * The calibration as stored once the request was served: @c present and the
 * numbers say what a get read, a set left or a clear forgot.
 */
typedef struct {
    QEvt     super;
    uint8_t  rc;       /**< kode_status_t: KODE_DENIED for a set past the
                        *   limits, KODE_ERROR when the store refused; the
                        *   old one then stays.                          */
    uint8_t  op;       /**< MAG_CAL_OP_*: which request.                 */
    bool     present;  /**< One is stored now.                           */
    MagCorr  corr;     /**< What is stored now, when @c present.         */
} MagCalRspEvt;

/**
 * @brief Ask AO_Imu for the magnetometer's calibration (MAG_CAL_REQ_SIG).
 *
 * MagReadEvt's arrangement: answered inside the step that receives it, @c out
 * is storage owned for the whole boot, and the tag tells a request nobody
 * waits on. A set carries its numbers by value; AO_Imu is the only writer of
 * the stored text.
 */
typedef struct {
    QEvt            super;
    uint8_t         op;              /**< MAG_CAL_OP_*.                         */
    int32_t         offset_mg[MAG_CAL_AXES];    /**< SET: the offsets, mG.      */
    int32_t         scale_milli[MAG_CAL_AXES];  /**< SET: the scales, thousandths. */
    void           *out;             /**< GET: a MagCalSlot. Written only on KODE_OK. */
    uint32_t       *rc;              /**< The call's kode_status_t. May be NULL. */
    khal_sem_t      done;            /**< Signalled once @c out and @c rc are written. */
    uint32_t        gen;             /**< The requester's tag for this request. */
    uint32_t const *cur;             /**< The requester's current tag. May be NULL. */
    uint32_t       *ack;             /**< Where @c gen is written back, just before @c done. */
    QActive        *who;             /**< The AO @c rsp is posted to; NULL for a waiter. */
    MagCalRspEvt   *rsp;             /**< Its answer, taken with the request. */
} MagCalEvt;

/* What one MAG_TEST_SIG record is about. The IMU row's four scalars, named
 * a..d for the same reason: each phase puts its own readings in them. */
#define MAG_TEST_WHOAMI   0U   /**< step 1: a = the WHO_AM_I byte; flags say
                                *   pass, MAG_TF_ABSENT for a part that did
                                *   not answer at all, or MAG_TF_MOVED for
                                *   one at another address (a = it).       */
#define MAG_TEST_ST       1U   /**< step 2: a, b, c = `on - off` per axis,
                                *   milli-gauss, judged on its magnitude. */
#define MAG_TEST_FIELD    2U   /**< step 3: a, b, c = the field on the chip's
                                *   own axes, d = its magnitude, all in
                                *   milli-gauss. A reading, not a verdict:
                                *   flags carry nothing.                   */
#define MAG_TEST_DONE     3U   /**< The last step is over; the verdict
                                *   follows as TEST_RESULT_SIG.            */

/** @brief Bit of MagTestEvt.flags: this record's own verdict. */
#define MAG_TF_PASS       0x01U
/** @brief Bit of MagTestEvt.flags: step 1 found no part at all - a slave
 *         NACK, or no answer - rather than a part that named itself wrong. */
#define MAG_TF_ABSENT     0x02U
/** @brief Bit of MagTestEvt.flags: step 1 found the part at another address,
 *         a = that address (kdrv_qmc5883p_stray()). */
#define MAG_TF_MOVED      0x04U

/**
 * @brief One step of the magnetometer production row (MAG_TEST_SIG).
 *
 * ImuTestEvt's shape. The console prints every record as a line; nothing
 * else subscribes, because this row needs nobody at the glass.
 */
typedef struct {
    QEvt    super;
    uint8_t step;   /**< 1-based.                                          */
    uint8_t phase;  /**< MAG_TEST_*                                        */
    uint8_t flags;  /**< MAG_TF_*                                          */
    int32_t a;      /**< WHOAMI: the byte. ST: the X delta. FIELD: X, mG.  */
    int32_t b;      /**< ST: the Y delta. FIELD: Y, mG.                    */
    int32_t c;      /**< ST: the Z delta. FIELD: Z, mG.                    */
    int32_t d;      /**< FIELD: the magnitude, mG.                         */
} MagTestEvt;

/** @brief A pattern of on/off segments, or a hold (HAPTIC_PLAY_REQ_SIG). */
typedef struct {
    QEvt     super;
    uint16_t seg_ms[KODE_HAPTIC_SEGMENTS_MAX]; /**< On, off, on... in ms;
                                                *   ignored by a hold.     */
    uint8_t  n;         /**< Segments in use.                              */
    uint8_t  gain_pct;  /**< Drive strength as a percentage of full scale.  */
    bool     hold;      /**< "On" until HAPTIC_STOP_REQ_SIG.               */
} HapticPlayEvt;

/**
 * @brief Read, write or dump one field of the haptic driver
 *        (HAPTIC_FIELD_REQ_SIG).
 *
 * ImuFieldReqEvt's contract, for the AW86233's own field table.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;      /**< FIELD_OP_*                                      */
    uint16_t field;   /**< kdrv_aw86233_field_t                            */
    int32_t  value;   /**< For FIELD_OP_SET, in the field's unit.          */
} HapticFieldReqEvt;

/** @brief The answer to a HapticFieldReqEvt (HAPTIC_FIELD_SIG). ImuFieldEvt's
 *         contract, for the AW86233. */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint16_t field;
    uint8_t  rc;      /**< kode_status_t                                   */
    int32_t  value;
} HapticFieldEvt;

/**
 * @brief How a buzz ended (HAPTIC_DONE_SIG).
 *
 * Published rather than posted back, so whoever asked can close on it
 * without AO_Haptic knowing who did - the AUDIO_FILE_RSP_SIG arrangement.
 * @c starved says the RTP FIFO refused a burst at least once during the
 * feed - the chip was ahead of it, not dry - which is the one thing a caller
 * cannot read out of the status code. It is how close a buzz came to the
 * FIFO's own back-pressure, and a buzz that ends KODE_OK with it set played
 * out whole.
 */
typedef struct {
    QEvt     super;
    uint8_t  rc;       /**< kode_status_t                                  */
    uint32_t samples;  /**< Playback samples the chip was fed.             */
    bool     starved;  /**< The FIFO refused a burst at least once.        */
} HapticDoneEvt;

/* What one HAPTIC_TEST_SIG record is about. The row measures the bench first
 * and the motor afterwards, at each gain, and a run that could not measure at
 * all says which of the two stopped it rather than condemning the board. */
#define HAPTIC_TEST_ABSENT    0U  /**< step 0: no chip; flags carry nothing. */
#define HAPTIC_TEST_FLOOR     1U  /**< step 1: floor_x10, n; PASS or LOUD.   */
#define HAPTIC_TEST_GAIN      2U  /**< step 2: gain, on_x10, ratio_x10, n.   */
#define HAPTIC_TEST_NOVERDICT 3U  /**< the row could not measure, and the
                                   *   flags say which half stopped it:
                                   *   HAPTIC_TF_MUTE, HAPTIC_TF_SHORT or
                                   *   HAPTIC_TF_LOUD.                      */

/* Bits of HapticTestEvt.flags. */
#define HAPTIC_TF_PASS    0x01U  /**< This record's own verdict.            */
#define HAPTIC_TF_LOUD    0x02U  /**< The floor already carries 235 Hz: the
                                  *   bench is judged, not the board.       */
#define HAPTIC_TF_SHORT   0x04U  /**< Fewer than KODE_HAPTIC_MIN_SAMPLES
                                  *   came back.                            */
#define HAPTIC_TF_MUTE    0x08U  /**< The IMU did not answer in time, or is
                                  *   not fitted: there is no witness.      */

/**
 * @brief One step of the haptic production row (HAPTIC_TEST_SIG).
 *
 * The console prints every record as a line. Energies are tenth-milli-g
 * integers and the ratio is ten times what it is, never floats: the event,
 * the log line and the console record all carry the same number.
 */
typedef struct {
    QEvt     super;
    uint8_t  step;       /**< 1-based.                                     */
    uint8_t  phase;      /**< HAPTIC_TEST_*                                */
    uint8_t  flags;      /**< HAPTIC_TF_*                                  */
    uint8_t  gain;       /**< GAIN: the drive this record is about.        */
    int32_t  floor_x10;  /**< The still window's energy at the bin, 0.1 mg. */
    int32_t  on_x10;     /**< GAIN: the driven window's, same unit.        */
    uint32_t ratio_x10;  /**< GAIN: on over floor, times ten.              */
    uint16_t n;          /**< Witness samples this record rests on.        */
} HapticTestEvt;

/* What one DISPLAY_TEST_SIG record is about. The row paints a field over the
 * whole glass and asks the operator whether the glass took it - every pixel,
 * the colour that was named, nothing dark and nothing stuck. The pad is the
 * answer: BUTTON_TOP says yes, BUTTON_BOT says no. */
#define DISPLAY_TEST_ASK     0U  /**< `field` is on the glass: answer now.   */
#define DISPLAY_TEST_PASS    1U  /**< The operator said yes.                 */
#define DISPLAY_TEST_FAIL    2U  /**< The operator said no.                  */
#define DISPLAY_TEST_TIMEOUT 3U  /**< Nobody answered within
                                  *   KODE_DISPLAY_TEST_STEP_MS.             */
#define DISPLAY_TEST_DONE    4U  /**< The row is over; the verdict follows.  */
#define DISPLAY_TEST_ABSENT  5U  /**< No panel on this board: the row answers
                                  *   at once and asks nobody anything.      */

/* Which field DisplayTestEvt.field is about. */
#define DISPLAY_FIELD_WHITE 0U  /**< All three channels.                    */
#define DISPLAY_FIELD_RED   1U  /**< Red alone.                             */
#define DISPLAY_FIELD_GREEN 2U  /**< Green alone.                           */
#define DISPLAY_FIELD_BLUE  3U  /**< Blue alone.                            */
#define DISPLAY_FIELD_BLACK 4U  /**< Nothing: no pixel may be lit at all.   */
#define DISPLAY_FIELD_COUNT 5U  /**< How many fields the row asks about.    */

/**
 * @brief One step of the display production row (DISPLAY_TEST_SIG).
 *
 * ButtonsTestEvt's shape: a question and its outcome share a step number, so
 * the console prints them as a pair and whoever draws the glass knows from
 * the first ASK that the pad is not navigation until DONE.
 */
typedef struct {
    QEvt     super;
    uint8_t  step;        /**< 1-based.                                    */
    uint8_t  total;       /**< Steps in the row.                           */
    uint8_t  phase;       /**< DISPLAY_TEST_*                              */
    uint8_t  field;       /**< DISPLAY_FIELD_*                             */
} DisplayTestEvt;

/* What one LED_TEST_SIG record is about. The row lights one colour and asks
 * the operator whether that is the colour the LED shows; the pad answers as
 * it does for the display row. */
#define LED_TEST_ASK     0U  /**< `colour` is lit: answer now.              */
#define LED_TEST_PASS    1U  /**< The operator said yes.                    */
#define LED_TEST_FAIL    2U  /**< The operator said no.                     */
#define LED_TEST_TIMEOUT 3U  /**< Nobody answered within
                              *   KODE_LED_TEST_STEP_MS.                    */
#define LED_TEST_DONE    4U  /**< The row is over; the verdict follows.     */

/* Which colour LedTestEvt.colour is about. */
#define LED_COLOUR_RED   0U  /**< The die on OUT3.                          */
#define LED_COLOUR_GREEN 1U  /**< The die on OUT1.                          */
#define LED_COLOUR_BLUE  2U  /**< The die on OUT2.                          */
#define LED_COLOUR_COUNT 3U  /**< How many colours the row asks about.      */

/**
 * @brief One step of the LED production row (LED_TEST_SIG).
 *
 * DisplayTestEvt's contract, for the three sinks of the KTD2026: a colour the
 * operator confirms is a sink that reaches the cathode it is meant to.
 */
typedef struct {
    QEvt     super;
    uint8_t  step;        /**< 1-based.                                    */
    uint8_t  total;       /**< Steps in the row.                           */
    uint8_t  phase;       /**< LED_TEST_*                                  */
    uint8_t  colour;      /**< LED_COLOUR_*                                */
} LedTestEvt;

/** @brief The boot state, as AO_Boot sees it (BOOT_STATE_SIG). */
typedef struct {
    QEvt     super;
    char     running[20];   /**< Partition label kodeOS is running from.     */
    uint32_t address;       /**< Where that partition starts.                */
    uint32_t size;          /**< How big it is, bytes.                       */
    uint8_t  reason;        /**< kboot_reason_t: how this boot came to be.   */
    bool     otadata_blank; /**< true: next plain boot is factory.           */
    uint8_t  app_end;       /**< kboot_app_end_t: how the app before this
                             *   boot ended.                                 */
} BootStateEvt;

/* Where the interface's drag is, kode_gui_gesture_t.drag. */
#define KODE_GUI_DRAG_IDLE    0U  /**< No drag is on.                       */
#define KODE_GUI_DRAG_PENDING 1U  /**< A finger is down and the recognizer
                                   *   has not decided yet.                 */
#define KODE_GUI_DRAG_FOLLOW  2U  /**< A claimed drag follows the finger.   */
#define KODE_GUI_DRAG_SETTLE  3U  /**< The finger is up and the page lands. */

/**
 * @brief What the interface's finger gestures have done since boot.
 *
 * AO_Gui counts; the console prints it as the `>|gesture` record of
 * `gui_info`. Each counter saturates at UINT16_MAX instead of wrapping. The
 * four rejections are the recognizer's own names for why a press claimed no
 * drag. A claim of another kind (an unlock, a scene, a scroll...) is counted
 * in `claims` and in neither `commits` nor `shorts`, so
 * commits + shorts <= claims.
 */
typedef struct {
    uint8_t  drag;     /**< The drag now, KODE_GUI_DRAG_*.                   */
    uint16_t taps;     /**< Presses that ended as a tap.                     */
    uint16_t claims;   /**< Drags the recognizer claimed.                    */
    uint16_t commits;  /**< Followed drags that landed on the far side.      */
    uint16_t shorts;   /**< Followed drags released short that went back.    */
    uint16_t diagonal; /**< Presses rejected: the axes were under 1.5:1.     */
    uint16_t axis;     /**< Presses rejected: a direction their zone does
                        *   not take.                                       */
    uint16_t zone;     /**< Presses rejected: they landed where no drag is
                        *   born.                                           */
    uint16_t blocked;  /**< Presses rejected: they landed while input was
                        *   shut.                                           */
} kode_gui_gesture_t;

/** @brief What state the interface is in (GUI_STATE_SIG). */
typedef struct {
    QEvt    super;
    char    state[12];      /**< The GUI state machine's own name for it.    */
    bool    screen_up;      /**< false: the screen was never built.          */
    uint8_t pills;          /**< Rows the launcher is showing.               */
    uint8_t installed;      /**< Installed apps the GUI knows about.         */
    uint8_t files;          /**< .bin files the last scan found.             */
    bool    writing;        /**< A flash write is on, its Opening card up.   */
    uint8_t level;          /**< The launcher's level, KODE_GUI_LEVEL_*.     */
    char    group[KODE_GUI_GROUP_MAX]; /**< The open group's name; "" at the
                                        *   root.                            */
    char    sel[KODE_APP_FILE_MAX];     /**< The launcher's selection, by its
                                         *   project name; "" for none.      */
    char    sheet[KODE_GUI_SHEET_MAX]; /**< The sheet that is up, one word;
                                        *   "" for none.                    */
    char    kody[KODE_GUI_KODY_MAX]; /**< Kody on the face: playing, paused,
                                      *   asleep or none.                  */
    uint32_t lock_s;         /**< The auto-lock time in seconds; 0 = never.   */
    uint32_t stack_free;     /**< Bytes of AO_Gui's stack never used.         */
    kode_gui_gesture_t gesture; /**< What the finger gestures have done.      */
} GuiStateEvt;

/** @brief One record of a function's answer, or its end (FUNCTION_ANS_SIG). */
typedef struct {
    QEvt    super;
    uint32_t ticket;    /**< The run it answers (function_ticket()); the
                         *   console drops an answer to any other.            */
    bool    done;       /**< true: the function is over and `status` is how. */
    uint8_t status;     /**< kode_status_t, read only with `done`.           */
    char    msg[KODE_FUNCTION_MSG_MAX]; /**< With `done`: the word after `msg=` of a code 5;
                         *   "" for the function's own name.                 */
    char    rec[KODE_FUNCTION_REC_MAX]; /**< A record without its `>|`; "" for
                                         *   none.                           */
} FunctionAnsEvt;

/** @brief Read or set one setting (CONFIG_REQ_SIG). */
typedef struct {
    QEvt     super;
    uint32_t seq;       /**< Echoed by the answer.                           */
    bool     set;       /**< true: apply and store the value below.          */
    char     key[KODE_CONFIG_KEY_MAX + 1U]; /**< The setting's key.          */
    int32_t  i;         /**< With `set`: an INT value.                       */
    bool     b;         /**< With `set`: a BOOL value.                       */
    uint8_t  e;         /**< With `set`: an ENUM value, as its index.        */
    char     s[KODE_CONFIG_VALUE_MAX + 1U]; /**< With `set`: a STRING value. */
} KodeConfigReqEvt;

/** @brief A setting's value, from its owner (CONFIG_ANS_SIG). */
typedef struct {
    QEvt     super;
    uint32_t seq;       /**< The request it answers.                         */
    uint8_t  status;    /**< kode_status_t: KODE_OK, or the owner's refusal. */
    bool     stored;    /**< The value is in NVS, not the owner's default.   */
    char     value[KODE_CONFIG_VALUE_MAX + 1U]; /**< The value as its word,
                                                 *   unquoted.                */
    char     key[KODE_CONFIG_KEY_MAX + 1U]; /**< The setting it is the value
                                             *   of, for a subscriber that
                                             *   did not ask.                 */
} KodeConfigAnsEvt;

/** @brief How an APP_CAT_REQ_SIG went (APP_CAT_RSP_SIG). */
typedef struct {
    QEvt    super;
    uint8_t status;                          /**< kode_status_t.               */
} AppCatRspEvt;

/** @brief One medium's mount state and usage (STORAGE_INFO_SIG).
 *
 * @c total_kb is the VOLUME's - how much the filesystem on the medium spans -
 * and on a card those are two different numbers: a 32GB card carrying a
 * 450MB FAT answers honestly with both, and a reader asking how big the card
 * is wants @c card_mb, which is what the card itself said during
 * identification. Zero when there is no card, or when the medium is the
 * internal partition, where the volume IS the whole of it.
 */
typedef struct {
    QEvt     super;
    uint8_t  medium;        /**< kdrv_medium_t                               */
    bool     mounted;
    bool     usage_known;   /**< false: total/free could not be read.        */
    uint32_t total_kb;      /**< The volume's size.                          */
    uint32_t free_kb;
    uint32_t card_mb;       /**< The card's own capacity; 0 when unknown.    */
} StorageInfoEvt;

/**
 * @brief Read, write or dump one field of the card (STORAGE_FIELD_REQ_SIG).
 *
 * The field is kdrv_sd_field_t, already resolved from its name by whoever
 * asks. Answered to @c who with a StorageFieldEvt, a refusal included.
 */
typedef struct {
    QEvt     super;
    QActive *who;       /**< Where the answer goes.                    */
    uint8_t  op;        /**< FIELD_OP_*                                */
    uint16_t field;     /**< kdrv_sd_field_t                           */
    int32_t  value;     /**< For FIELD_OP_SET.                         */
} StorageFieldReqEvt;

/**
 * @brief The answer to a StorageFieldReqEvt (STORAGE_FIELD_RSP_SIG).
 *
 * For a read, `value` is the field; for a write, the value read back after
 * it; for a dump, nothing. `rc` is the driver's verdict: KODE_DENIED for a
 * field that cannot be written, a value out of its range, or a field of a
 * card when none is mounted; KODE_ERROR when the volume would not answer.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint16_t field;
    uint8_t  rc;        /**< kode_status_t                             */
    int32_t  value;
} StorageFieldEvt;

/** @brief What one path is: kode_sys_file_status_t, field for field. */
typedef struct {
    bool     exists;  /**< Whether anything is at the path; false is an answer, not a failure. */
    bool     is_dir;  /**< A directory rather than a file; false whenever !exists.             */
    uint32_t size;    /**< Bytes; zero for a directory and whenever !exists.                   */
} FileStat;

/**
 * @brief Answers kode_sys's file requests: FILE_STATUS_RSP_SIG, FILE_LIST_END_SIG,
 *        FILE_FOLDER_RSP_SIG, FILE_DELETE_RSP_SIG, FILE_RENAME_RSP_SIG,
 *        FILE_READ_RSP_SIG and FILE_WRITE_RSP_SIG.
 */
typedef struct {
    QEvt     super;
    uint8_t  rc;      /**< kode_status_t: KODE_DENIED for a bad path or a
                       *   transfer in flight, KODE_ERROR for a volume that
                       *   will not mount or a call that failed.            */
    bool     more;    /**< The end of a listing: entries remain past it.    */
    uint16_t count;   /**< The end of a listing: entries this page posted.  */
    uint32_t got;     /**< A read: bytes written to the lent buffer.        */
    FileStat stat;    /**< A status: what is at the path.                   */
} FileRspEvt;

/**
 * @brief One entry of a listing, written straight into a blocked task's own
 *        storage rather than published as a StorageEntryEvt.
 *
 * The three fields StorageEntryEvt carries that a caller of the facade
 * actually asked about, minus the QEvt only an Active Object can receive and
 * minus `from_sd`, which a caller that named the directory itself already
 * knows. See StorageWaiter for why a second shape exists at all.
 */
typedef struct {
    char     name[KODE_APP_FILE_MAX];
    bool     is_dir;
    uint32_t size;
} StorageRow;

/**
 * @brief Where AO_Storage puts an answer owed to a BLOCKED TASK, and how it
 *        wakes it (StoragePathEvt.wait and the three request events below).
 *
 * Every answer AO_Storage gives travels to a @c QActive: StoragePathRspEvt
 * and StorageLineEvt are posted to one, StorageEntryEvt is published to
 * whichever ones subscribed. That is right for the console and wrong for the
 * only other thing that asks - the `kode_sys_file_*()` facade, which runs on an
 * application's own task. A task is not an Active Object, it has no queue and
 * nothing dispatches into it, so an answer shaped as an event can only reach
 * it through some Active Object standing in for it - and a receiver invented
 * for that job is an extra task, an extra queue and an extra priority level
 * that nothing in this system needs and `ps` would never show.
 *
 * So a request may instead name one of these, and AO_Storage writes the
 * answer into it and signals @c done. That is exactly what IrCaptureEvt
 * already does for the facade's blocking capture, down to the generation: a requester
 * that waits with a deadline can stop waiting before its answer is written,
 * and the answer still arrives - into this same storage, on this same
 * semaphore, while a LATER requester is asleep on it. @c ack is what tells
 * the two apart. AO_Storage stores the answering request's own @c gen there
 * with release ordering immediately before it signals, so a requester woken
 * by a token it did not earn reads a generation that is not its own and keeps
 * waiting. That pairing, not the wake-up, is also what publishes everything
 * written below it to the other core.
 *
 * @c who and @c wait are alternatives, never both: an Active Object gives
 * @c who and no @c wait, a blocked task gives @c wait and no @c who. A
 * listing answered into @c rows is not published at all, so a facade listing
 * costs the event pool nothing and cannot interleave with the operator's own.
 */
typedef struct {
    khal_sem_t  done;    /**< Signalled once, after everything below is
                           *   written. Required, not optional: a waiter with
                           *   nowhere to be woken is a requester stalled for
                           *   its whole deadline over an answer already
                           *   written, which is worse to find than a fault. */
    uint32_t    ack;     /**< The answering request's own @c gen, stored
                           *   with release ordering just before @c done.  */
    uint8_t     rc;      /**< kode_status_t for the whole round.           */
    bool        exists;  /**< STAT only, as StoragePathRspEvt.exists.      */
    bool        is_dir;  /**< STAT only.                                    */
    uint32_t    size;    /**< STAT: the file's bytes.                       */
    StorageRow *rows;    /**< LIST only: at least @c cap entries, filled in
                           *   the order the walk found them.               */
    char       *text;    /**< CAT only: at least @c cap bytes, filled with
                           *   the file's bytes verbatim - no line splitting,
                           *   no NUL, nothing this shape cannot carry.    */
    uint32_t    cap;     /**< Entries in @c rows, or bytes in @c text.     */
    uint32_t    got;     /**< Entries or bytes actually written.            */
    bool        cut;     /**< There was more than @c got: @c cap ran out, or
                           *   AO_Storage's own KODE_STORAGE_LIST_MAX did.
                           *   A caller that must not truncate silently reads
                           *   this rather than guessing from @c got.       */
} StorageWaiter;

/** @brief One path operation (STORAGE_PATH_REQ_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  op;                   /**< storage_path_op_t                    */
    char     path[KODE_PATH_MAX];
    QActive *who;                  /**< Where the response goes. NULL when
                                     *   `wait` names a blocked task instead. */
    uint32_t max;                  /**< CAT only: ceiling on bytes read,
                                     *   already bounded by the console's own
                                     *   key spec. Ignored by every other op. */
    uint32_t offset;               /**< CAT only: first byte to read. */
    StorageWaiter *wait;           /**< A blocked task's own storage, or NULL
                                     *   when `who` is an Active Object.     */
    uint32_t gen;                  /**< This request's tag, echoed into
                                     *   `wait->ack`. Meaningless without
                                     *   `wait`.                             */
    FileRspEvt *rsp;               /**< An ask's answer, taken with the
                                     *   request and posted to `who`; NULL
                                     *   otherwise. An ask's LIST counts
                                     *   `offset` as the entries to skip.    */
    void    *buf;                  /**< An ask's CAT: the lent buffer, `max`
                                     *   bytes, written before the answer.   */
} StoragePathEvt;

/**
 * @brief One operation over two paths (STORAGE_PAIR_REQ_SIG).
 *
 * `mv` and `cp` both reach this, told apart by `op` alone - one shape for
 * both is what let cp arrive beside mv without this type changing shape.
 * Answered with the same StoragePathRspEvt a STAT gets; see
 * STORAGE_PAIR_REQ_SIG's own comment for why one shape does for both.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;                   /**< storage_pair_op_t                    */
    char     src[KODE_PATH_MAX];
    char     dst[KODE_PATH_MAX];
    QActive *who;                  /**< Where the response goes. NULL when
                                     *   `wait` names a blocked task instead. */
    StorageWaiter *wait;           /**< See StoragePathEvt.wait.             */
    uint32_t gen;                  /**< See StoragePathEvt.gen.              */
    FileRspEvt *rsp;               /**< See StoragePathEvt.rsp.              */
} StoragePairEvt;

/**
 * @brief Put text in a file (STORAGE_TEXT_REQ_SIG).
 *
 * `console_fs_write()` is the one caller. `line` is what tells AO_Storage
 * which of two very different operations this is: zero is the whole-file
 * write `answer_text_write()` has always done, a truncate-or-append and one
 * `kdrv_storage_write()` call; nonzero is `answer_text_line_write()`'s
 * read-modify-write, which opens the file for READING first, replaces line
 * `line` (1-based) and rewrites the rest through a temporary before renaming
 * it over the original. `append` means nothing to the second shape - keeping
 * what is there is the whole point of reading it first - and is ignored
 * whenever `line` is nonzero; see storage_text_request()'s own doc for why
 * the console refuses the two together (`appendline`) before either ever
 * reaches this event, rather than letting the combination arrive here to be
 * silently dropped.
 *
 * `len` is the number of bytes actually sitting in `text`, filled in once by
 * whoever builds the event rather than re-derived with strlen() wherever it
 * is read - and it is not always strlen(text): KODE_FS_TEXT_MAX is a rounded-
 * down bound, not the console line's own limit (spec section A), so a tail
 * value longer than the buffer is cut to fit rather than refused, and `len`
 * is the length AFTER that cut, the only length AO_Storage may trust when it
 * calls kdrv_storage_write().
 *
 * `data` is the second way to say what to write, and it exists because `text`
 * is the wrong shape for a payload of any size. Growing that array to hold
 * one would grow every StorageTextEvt in the pool, and this event is already
 * the widest thing the large tier carries; a pointer costs eight bytes and
 * bounds nothing. So when `data` is non-NULL, `text` is unused and `len` is
 * the number of bytes at `data` instead - bytes, not a C string, so an
 * embedded NUL is written like any other byte.
 *
 * That makes this the second event in kodeOS carrying a pointer into somebody
 * else's memory rather than a copy of it, after StorageWriteEvt.data, and it
 * carries the same obligation: the bytes must outlive the request. A
 * requester that waits with a deadline can stop waiting while this event is
 * still in AO_Storage's queue, so `data` may never be a stack frame the
 * requester is free to leave - it has to be storage that survives a requester
 * which has already given up and returned.
 */
typedef struct {
    QEvt     super;
    bool     append;                /**< Keep what is there and add after it.
                                      *   Otherwise truncate - kdrv_storage_
                                      *   create_path()'s own two behaviours,
                                      *   chosen by the same flag here.
                                      *   Meaningless when `line` is nonzero;
                                      *   see this struct's own doc.         */
    uint16_t line;                  /**< 0 = whole file; n = replace line n
                                      *   (1-based), rewritten through a
                                      *   temporary and a rename. Above 32 KB
                                      *   the rewrite refuses outright - see
                                      *   answer_text_line_write() in
                                      *   storage_ao.c.                      */
    uint16_t len;                   /**< Bytes in `text`, or in `data` when
                                      *   that is non-NULL. See above.       */
    char     path[KODE_PATH_MAX];
    char     text[KODE_FS_TEXT_MAX];
    QActive *who;                   /**< Where the response goes. NULL when
                                      *   `wait` names a blocked task.       */
    const void    *data;            /**< The bytes to write, when `text` is
                                      *   too small to hold them. See above. */
    StorageWaiter *wait;            /**< See StoragePathEvt.wait.            */
    uint32_t gen;                   /**< See StoragePathEvt.gen.             */
    FileRspEvt *rsp;                /**< See StoragePathEvt.rsp; an ask's
                                      *   bytes are lent at `data`.          */
} StorageTextEvt;

/**
 * @brief A file is about to arrive on the console (STORAGE_RECV_REQ_SIG).
 *
 * AppRecvReqEvt with a path where that one has a region, and it is worth
 * saying why it is a second event rather than one more field on that one:
 * the two travel to different owners. AO_AppMgr owns the apps region and
 * AO_Storage owns the two volumes, so a shared event would have to be routed
 * by a field, which is the discriminator this codebase keeps taking back out.
 *
 * `who` is where the whole RECV_* conversation goes - `>|ready`, every
 * `>|ack`, and the one verdict - posted rather than published. See
 * RECV_READY_SIG's own comment for why AO_Storage differs from AO_AppMgr
 * there; the short version is that a file copy is not an install and must not
 * take the screen.
 *
 * The destination is NOT opened when this is sent. It is opened once AO_Log
 * has confirmed the wire is raw, in that order and never the other way round:
 * an arming that lands after slow work cannot tell the command's own trailing
 * newline from the payload's first byte, which once cost a whole image
 * shifted by one.
 */
typedef struct {
    QEvt     super;
    uint32_t size;    /**< Payload length in bytes. Never zero.              */
    uint32_t crc32;   /**< What khal_crc32() over the payload must equal.    */
    char     path[KODE_PATH_MAX];  /**< Absolute, normalised, virtual.       */
    QActive *who;     /**< Where the RECV_* conversation is posted.          */
    uint8_t  src;     /**< KODE_CONSOLE_SRC_*: which port the payload
                       *   follows on. The console has two, and only one of
                       *   them can be handed over at a time.                */
} StorageRecvEvt;

/**
 * @brief How a path request ended (STORAGE_PATH_RSP_SIG).
 *
 * `exists` and `rc` answer different questions and must not be collapsed
 * into one another: `rc` is whether the question was answered at all
 * (KODE_OK) or refused (KODE_DENIED for a malformed path, KODE_ERROR for a
 * volume that will not mount); `exists` is the answer itself, valid only
 * when `rc` is KODE_OK. A STAT of a path that is simply not there is
 * KODE_OK with `exists` false - the question was answered, the same way
 * an HTTP GET calls a 404 KODE_OK.
 *
 * MKDIR, UNLINK, RMDIR and the pair op MOVE answer here too, and for them
 * `exists`/`is_dir`/`size` carry nothing - `rc` alone is the whole story,
 * because those ops were not asked what is at a path, they were asked to
 * change one. Reusing this shape rather than inventing a second, emptier one
 * is what lets every path-shaped op close through the console's one shared
 * refusal branch (path_answer() in console_ao.c) instead of a copy of it.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;
    uint8_t  rc;                   /**< kode_status_t                        */
    bool     exists;               /**< STAT only. See above.                */
    bool     is_dir;               /**< STAT only.                           */
    uint32_t size;                 /**< STAT only.                           */
} StoragePathRspEvt;

/**
 * @brief One line of a `cat`, or the terminator that closes it
 *        (STORAGE_LINE_SIG).
 *
 * Posted straight to the asker, the same private shape STORAGE_PATH_RSP_SIG
 * already has: a file's text is nobody's business but the one console that
 * asked for it, so this never goes on the publish/subscribe bus the way a
 * directory listing does.
 *
 * `eof` marks the terminator. On every record before it, `text` holds up to
 * KODE_FS_CAT_LINE_MAX-1 bytes of one line - cut here, once, when the file's
 * own line was longer, rather than left for reply()'s snprintf to cut a
 * second time without saying so - and `rc` is unused. On the terminator,
 * `text` is empty and `rc` is the only thing that matters: KODE_OK when the
 * read that fed every line before it actually happened, anything else when
 * AO_Storage could not open or read the file after all - a race the STAT
 * phase that gated `cat` cannot close, only narrow.
 */
typedef struct {
    QEvt     super;
    char     text[KODE_FS_CAT_LINE_MAX];
    uint8_t  rc;        /**< kode_status_t. Terminator only.                 */
    bool     eof;        /**< True on the terminator; `text` is empty then.  */
} StorageLineEvt;

/* Which phase of the card test a record is about. The order is the order they
 * run in, and every one after the first prints whether or not the one before
 * it passed: a run that stopped at the first failure would tell a production
 * line less than a run that carries on and fails twice. The identify is the
 * exception, and ends the run when it fails - with no card there is nothing
 * below it that means anything.
 *
 * The first two are the BOARD's and carry the verdict. The last two are the
 * ones a card can veto - the counterfeits read perfectly and refuse to be
 * written to - so they are reported and measured without condemning a slot
 * that has already proved itself. */
#define SD_TEST_IDENTIFY  1U   /**< Powered, mounted, and on four data lines. */
#define SD_TEST_READ4     2U   /**< A megabyte read on all four data lines.   */
#define SD_TEST_READ1     3U   /**< The same, on one, so the two can be
                                *   compared - four lines that are not really
                                *   carrying still negotiate four.           */
#define SD_TEST_WRITE     4U   /**< A megabyte written through the volume.    */
#define SD_TEST_VERIFY    5U   /**< The same megabyte, read back and checked. */
#define SD_TEST_STEPS     5U   /**< How many phases the script has.           */

/* `sd_verify`'s phases, which travel in the same event. They ask about the
 * CARD rather than the slot, which is why they are a bench verb and not part
 * of the production row: both are destructive - one writes to the far end of
 * the card, the other rewrites the filesystem - and neither says anything
 * about the board. */
#define SD_VERIFY_PROBE   1U   /**< The far end of the card is really there.  */
#define SD_VERIFY_FORMAT  2U   /**< It takes a filesystem, and a full-sized one. */
#define SD_VERIFY_STEPS   2U

/**
 * @brief One phase of the card test (STORAGE_TEST_SIG).
 *
 * Every phase carries what it measured as well as its verdict, because the
 * numbers are what somebody running the production test is there to read.
 * Which of them mean anything depends on the phase: `name`, `width`,
 * `freq_khz` and `capacity_mb` on the identify, `capacity_mb` on the probe,
 * `total_mb` on the format, and `kb_s` on the two transfers. The rest are
 * zero, and the console prints only the ones its phase names.
 */
typedef struct {
    QEvt     super;
    uint8_t  phase;        /**< SD_TEST_*                                  */
    uint8_t  total;        /**< SD_TEST_STEPS, so a host need not know it.  */
    bool     ok;
    uint8_t  width;        /**< Data lines negotiated.                      */
    uint32_t freq_khz;
    uint32_t capacity_mb;  /**< What the card says it holds.                */
    uint32_t total_mb;     /**< What the filesystem spans after the format. */
    uint32_t kb_s;         /**< Transfer rate; 0 when it was not timed.     */
    char     name[9];      /**< The card's product name.                    */
} StorageTestEvt;

/**
 * @brief What the PSRAM row tested and found (PSRAM_TEST_SIG).
 *
 * The numbers a diagnosis starts from: how much was tested, how much was
 * free, and the first word that read wrong with what it should have held
 * and what it held. A `read` one bit off `wrote` is a data bit; a `read`
 * equal to another word's address is an address line.
 */
typedef struct {
    QEvt     super;
    uint32_t tested_kb; /**< PSRAM tested, in KB.                         */
    uint32_t free_kb;   /**< PSRAM free when the row started, in KB.      */
    uint32_t errors;    /**< Words that read wrong, capped.               */
    uint32_t first;     /**< The first of them; meaningful when errors > 0. */
    uint32_t wrote;     /**< What it should have held.                    */
    uint32_t read;      /**< What it held.                                */
} PsramTestEvt;

/** @brief What a KodeScreenReqEvt asks of the screen. */
#define KODE_SCREEN_OP_NEW    1U  /**< A new screen; @c text is the title, "" for none, and
                                   *   @c num[0] the screen's generation.               */
#define KODE_SCREEN_OP_TEXT   2U  /**< A text widget at @c widget, under the others.    */
#define KODE_SCREEN_OP_SET    3U  /**< Widget @c widget now says @c text: a text, or the
                                   *   number of a value.                               */
#define KODE_SCREEN_OP_VALUE  4U  /**< A value at @c widget: @c text is the number, then
                                   *   a NUL and the unit.                              */
#define KODE_SCREEN_OP_BUTTON 5U  /**< A button at @c widget; @c text is its label.     */
#define KODE_SCREEN_OP_CANVAS 6U  /**< A canvas at @c widget; @c num[0] is its screen's
                                   *   generation, and its pixels are the ones kode_screen
                                   *   keeps for that place.                            */
#define KODE_SCREEN_OP_IMAGE  7U  /**< An image at @c widget; @c num[0] is its screen's
                                   *   generation, and its pixels are the ones kode_screen
                                   *   keeps for that place.                            */
#define KODE_SCREEN_OP_DRAW_TEXT 8U /**< Write @c text on a canvas. @c widget is the canvas's
                                   *   whole number (screen and place), @c num[0] and
                                   *   @c num[1] the text's x and y, @c num[2] its colour
                                   *   (0xRRGGBB) with its kode_align_t in the top byte, and
                                   *   @c num[3] the caller's wait: a ticket over a slot,
                                   *   handed back to the caller when the text is drawn. */
#define KODE_SCREEN_OP_COLOR  9U  /**< Paint widget @c widget in @c num[0] (0xRRGGBB): a
                                   *   button's fill, a text's letters or a value's number. */

/**
 * @brief One change to the standard layer's screen (KODE_SCREEN_REQ_SIG).
 *
 * Posted by an app task, which has already checked it, and applied on the
 * AO's thread. @c widget is the widget's place on the screen, from 0.
 */
typedef struct {
    QEvt     super;
    uint8_t  op;                                /**< KODE_SCREEN_OP_*.       */
    uint32_t widget;                            /**< Which widget.           */
    char     text[KODE_SCREEN_TEXT_MAX];        /**< Its text, NUL-ended.    */
    int32_t  num[KODE_SCREEN_REQ_NUMS];         /**< The op's numbers.       */
} KodeScreenReqEvt;

/** @brief One line of console input (CONSOLE_LINE_SIG): published by AO_Log
 *        for the cable and BLE, and by AO_Net for a line that arrived on
 *        the Kode channel's cmd/in topic (src NET). */
typedef struct {
    QEvt    super;
    uint8_t src;                        /**< KODE_CONSOLE_SRC_*             */
    uint8_t seq;                        /**< Src NET only: the sequence AO_Net
                                         *   gave this line; the console
                                         *   returns it in net_line_done().
                                         *   Not set for any other source.   */
    char    line[KODE_CONSOLE_LINE_MAX];
} ConsoleLineEvt;

/** @brief The console ended the line from the Kode channel that carried
 *         @c seq (NET_LINE_DONE_SIG). */
typedef struct {
    QEvt    super;
    uint8_t seq;                        /**< The ConsoleLineEvt::seq it ends. */
} NetLineDoneEvt;

/** @brief One byte from the console reader, posted to AO_Log (CONSOLE_BYTE_SIG).
 *
 * Per keystroke, which sounds extravagant until you price it: a human types
 * at maybe ten bytes a second and the alternative is a second thread writing
 * to the transport that AO_Log owns. */
typedef struct {
    QEvt    super;
    uint8_t byte;
} ConsoleByteEvt;

/**
 * @brief One formatted log line on its way to the console (LOG_LINE_SIG).
 *
 * Already formatted - timestamp, level, tag, colour and newline - because
 * klog does that on the caller's thread. What AO_Log adds is the console
 * device: erasing the prompt before the line and drawing it again after.
 *
 * The biggest event in the system, and the reason the large pool is sized the
 * way it is. Bounded loss is deliberate: if the pool is empty the line is
 * dropped rather than asserted over.
 */
typedef struct {
    QEvt     super;
    uint8_t  src;       /**< KODE_CONSOLE_SRC_* of the command a `>` line
                         *   answers; KODE_LOG_SRC_NONE (0xFF) for the rest. */
    uint16_t len;
    char     line[KODE_LOG_LINE_MAX];
} LogEvt;

/* ---- AO_LowPower: the board's three states --------------------------------
 * AWAKE: everything runs. ASLEEP: the CPU lock is let go, the P4 light-sleeps
 * whenever every task is idle. OFF: deep sleep. The enums are here for the
 * facade, which uses them whether the option is on or not; the events exist
 * only with CONFIG_KODE_LOWPOWER.
 */

/** @brief The board's state. */
/* cppcheck-suppress misra-c2012-2.4 ; the tag of a public enum, used where CONFIG_KODE_LOWPOWER is set, which cppcheck does not read */
typedef enum {
    KODE_LP_AWAKE = 0,   /**< Everything runs; the CPU holds its lock.       */
    KODE_LP_ASLEEP,      /**< Light sleep whenever every task is idle.       */
    KODE_LP_OFF,         /**< Deep sleep; the pins held at rest.             */
    KODE_LP_RADIO        /**< A radio wake inside ASLEEP: the tick runs and the
                          *   lock is held for the radio owners alone, and the
                          *   glass stays asleep. Published as a state, never
                          *   a target, and never what the board is said to be
                          *   in (lowpower_info says asleep).                 */
/* cppcheck-suppress misra-c2012-2.3 ; a public enum, used where CONFIG_KODE_LOWPOWER is set, which cppcheck does not read */
} kode_lp_state_t;

/** @brief Why the board is in the state it is in. */
/* cppcheck-suppress misra-c2012-2.4 ; the tag of a public enum, used where CONFIG_KODE_LOWPOWER is set, which cppcheck does not read */
typedef enum {
    KODE_LP_CAUSE_REQUEST,    /**< Somebody asked (console, interface, app). */
    KODE_LP_CAUSE_IDLE,       /**< No input for the sleep-after time.        */
    KODE_LP_CAUSE_OFF_AFTER,  /**< Asleep for the off-after time.            */
    KODE_LP_CAUSE_BOT,        /**< The bottom button.                        */
    KODE_LP_CAUSE_BLE,        /**< A BLE central connected.                  */
    KODE_LP_CAUSE_MQTT,       /**< The Kode channel had a message.           */
    KODE_LP_CAUSE_VBUS,       /**< A USB cable appeared.                     */
    KODE_LP_CAUSE_TIMER,      /**< An owner's work fell due while asleep.    */
    KODE_LP_CAUSE_TOP,        /**< The top button.                           */
    KODE_LP_CAUSE_BOOT        /**< The board has just started.               */
/* cppcheck-suppress misra-c2012-2.3 ; a public enum, used where CONFIG_KODE_LOWPOWER is set, which cppcheck does not read */
} kode_lp_cause_t;

/** @brief What woke the P4 from a sleep with the C5 involved. */
/* cppcheck-suppress misra-c2012-2.4 ; the tag of a public enum, used where CONFIG_KODE_LOWPOWER is set, which cppcheck does not read */
typedef enum {
    KODE_RADIO_WOKE_C5 = 0,   /**< The C5 raised its line: traffic for the P4. */
    KODE_RADIO_WOKE_TX,       /**< The P4 has something to send to the C5.     */
    KODE_RADIO_WOKE_DEAF,     /**< The C5 did not answer the wake: given up.   */
    KODE_RADIO_WOKE_STOOD_DOWN /**< The sentinel could not go back down and stood
                                *   down: the bus is up and the P4 is held awake
                                *   until the owner starts or stops it again.   */
/* cppcheck-suppress misra-c2012-2.3 ; a public enum, used where CONFIG_KODE_LOWPOWER is set, which cppcheck does not read */
} kode_radio_wake_t;

/* ---- AO_Scheduler's events ------------------------------------------------ */

/** @brief Why a request's text did not fit its buffer (the @c flaw of the requests). */
enum {
    SCHED_FLAW_NONE = 0,   /**< Every text fit.                      */
    SCHED_FLAW_NAME,       /**< The name is missing or too long.     */
    SCHED_FLAW_WHEN,       /**< The expression is missing or too long. */
    SCHED_FLAW_CMD         /**< The line is too long.                */
};

/** @brief Set an entry of the console (SCHED_SET_REQ_SIG). */
typedef struct {
    QEvt      super;
    QActive  *who;                           /**< The asker; NULL for no answer. */
    uint8_t   flaw;                          /**< A SCHED_FLAW_*: set when a text was cut. */
    char      name[KODE_SCHED_NAME_MAX + 1U];
    char      when[KODE_SCHED_WHEN_MAX + 1U];
    char      cmd[KODE_SCHED_CMD_MAX];       /**< The line; empty for none. */
} SchedSetReqEvt;

/** @brief Set an entry that posts to an AO (SCHED_SET_AO_REQ_SIG). */
typedef struct {
    QEvt      super;
    QActive  *who;                           /**< The asker; NULL for no answer. */
    QActive  *target;                        /**< Where each fire goes. */
    enum_t    fire_sig;                      /**< The signal of that event. */
    uint8_t   flaw;                          /**< A SCHED_FLAW_*: set when a text was cut. */
    char      name[KODE_SCHED_NAME_MAX + 1U];
    char      when[KODE_SCHED_WHEN_MAX + 1U];
} SchedSetAoReqEvt;

/** @brief Remove or list an entry (SCHED_REMOVE_REQ_SIG, SCHED_LIST_REQ_SIG). */
typedef struct {
    QEvt      super;
    QActive  *who;                           /**< The asker. */
    bool      has_name;                      /**< false: every entry (a list). */
    uint8_t   flaw;                          /**< A SCHED_FLAW_*: set when the name was cut. */
    char      name[KODE_SCHED_NAME_MAX + 1U];
} SchedNameReqEvt;

/**
 * @brief One entry, as a record (SCHED_ENTRY_SIG).
 *
 * The small fields come first on purpose: they sit in the padding the
 * 64-bit time needs after the header, which keeps the event inside the large
 * pool's block.
 */
typedef struct {
    QEvt      super;
    uint8_t   owner;                         /**< A sched_owner_t. */
    bool      stored;                        /**< It is kept across a reset. */
    int64_t   next_s;                        /**< Unix seconds of its next minute; -1 while the clock is not set or none falls within the horizon. */
    char      name[KODE_SCHED_NAME_MAX + 1U];
    char      when[KODE_SCHED_WHEN_MAX + 1U];
    char      cmd[KODE_SCHED_CMD_MAX];       /**< The line; empty for an entry of code and for `mqtt`. */
} SchedEntryEvt;

/** @brief The end of an answer (SCHED_END_SIG). */
typedef struct {
    QEvt          super;
    uint8_t       st;                        /**< A kode_status_t: KODE_OK; KODE_DENIED with @c why; or KODE_ERROR with "partial". */
    uint8_t       count;                     /**< SCHED_ENTRY_SIG events that reached the asker before this one. */
    char          why[KODE_SCHED_WHY_MAX];   /**< Short static reason; empty when KODE_OK. */
} SchedEndEvt;

/** @brief The next time the `mqtt` entry falls due, for AO_Net (NET_CHECK_AT_SIG). */
typedef struct {
    QEvt      super;
    int64_t   due_s;                         /**< Unix seconds of that minute; -1 for none. */
} NetCheckAtEvt;

/** @brief AO_Net made the check it was given the time of (SCHED_CHECK_DONE_SIG). */
typedef struct {
    QEvt      super;
    int64_t   passed_s;                      /**< The time it was given, Unix seconds. */
} SchedCheckDoneEvt;

/** @brief An entry of code is due (posted to its target with the signal it asked for). */
typedef struct {
    QEvt      super;
    char      name[KODE_SCHED_NAME_MAX + 1U]; /**< The entry that fired. */
} SchedFireEvt;

/* ---- The pads delivery test's boost (AO_Power) ---------------------------- */

/** @brief The boost on or off for the pads delivery test (POWER_BOOST_TEST_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;      /**< Where POWER_BOOST_TEST_SIG is posted.               */
    bool     on;       /**< true: BOOST_LIM raised, then the boost on; false:
                        *   the boost off, then the board's limit back.        */
} PowerBoostTestReqEvt;

/** @brief The boost as the delivery test left it (POWER_BOOST_TEST_SIG). */
typedef struct {
    QEvt    super;
    uint8_t status;     /**< kode_status_t: KODE_OK, or the refusal of the
                         *   boost (a cable, a low cell) or of the chip.      */
    bool    boost;      /**< The boost is on now.                             */
    uint8_t fault;      /**< The charger's fault register at the next poll.   */
    int16_t current_ma; /**< The gauge's current at the next poll, mA;
                         *   negative is a discharge.                         */
} PowerBoostTestEvt;

/* ---- The dock console's pins (AO_Log) -------------------------------------- */

/** @brief Lend the dock console's pins, or take them back (LOG_DOCK_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;      /**< Where LOG_DOCK_SIG is posted.                       */
    bool     lend;     /**< true: let the pins go; false: the console back.     */
} LogDockReqEvt;

/** @brief The dock's pins as AO_Log left them (LOG_DOCK_SIG). */
typedef struct {
    QEvt    super;
    bool    lent;      /**< The pins are free for the asker now.               */
    uint8_t status;    /**< kode_status_t: KODE_OK, or why the dock refused.   */
} LogDockEvt;

/* ---- The E5 final test (AO_TestDot) ------------------------------------------
 * The station's verbs reach AO_TestDot as one TestDotReqEvt each, stamped by
 * the console with a sequence number; AO_TestDot answers with records and one
 * end, each carrying the same number, so the console tells a late answer from
 * the one it waits for. The screens and patterns are asked of the firmware's
 * LVGL owner with a published TestDotScreenEvt.
 */

/** @brief The station's verbs, as a TestDotReqEvt names them. */
typedef enum {
    TEST_DOT_VERB_RUN = 0,  /**< `test component=all`.                 */
    TEST_DOT_VERB_PLACE,    /**< `test_dot_place`.                     */
    TEST_DOT_VERB_POLL,     /**< `test_dot_poll`.                      */
    TEST_DOT_VERB_SCREEN,   /**< `test_dot_screen`.                    */
    TEST_DOT_VERB_PHOTOS,   /**< `test_dot_photos`.                    */
    TEST_DOT_VERB_BLOB,     /**< `test_dot_blob`.                      */
    TEST_DOT_VERB_RELAYS,   /**< `test_dot_relays`.                    */
    TEST_DOT_VERB_ADC,      /**< `test_dot_adc`.                       */
    TEST_DOT_VERB_LOOP,     /**< `test_dot_loop`.                      */
    TEST_DOT_VERB_RADIO,    /**< `test_dot_radio`.                     */
    TEST_DOT_VERB_BOOST,    /**< `test_dot_boost`.                     */
    TEST_DOT_VERB_RESULT,   /**< `test_dot_result`.                    */
    TEST_DOT_VERB_LOCK,     /**< `test_dot_lock`.                      */
    TEST_DOT_VERB_END,      /**< `test_dot_end`.                       */
    TEST_DOT_VERB_INFO,     /**< `test_dot_info`.                      */
    TEST_DOT_VERB_CAMERA,   /**< `test_dot_camera`.                    */
    TEST_DOT_VERBS          /**< How many.                             */
} test_dot_verb_t;

/** @brief The phases `test_dot_screen` puts on the glass. */
typedef enum {
    TEST_DOT_PHASE_CONNECTING = 0, /**< Before the run: the connectors going in. */
    TEST_DOT_PHASE_RESULT          /**< After it: PASS or FAIL.                   */
} test_dot_phase_t;

/** @brief The parts `test_dot_camera only=` runs; ALL is every one. */
typedef enum {
    TEST_DOT_CAM_FRAMING = 0, /**< The marks, the place and the turn.        */
    TEST_DOT_CAM_PIXELS,      /**< Dead and stuck-on pixels.                 */
    TEST_DOT_CAM_LINES,       /**< Dead columns and rows.                    */
    TEST_DOT_CAM_ZONES,       /**< Dark zones and bright patches.            */
    TEST_DOT_CAM_COLOUR,      /**< The colours against the white.            */
    TEST_DOT_CAM_LED,         /**< The RGB LED, judged by the camera.        */
    TEST_DOT_CAM_ALL          /**< Every part, as a run takes them.          */
} test_dot_cam_t;

/** @brief A station verb, from the console (TEST_DOT_REQ_SIG). */
typedef struct {
    QEvt     super;
    QActive *who;      /**< Where the records and the end go.                  */
    uint8_t  verb;     /**< test_dot_verb_t.                                    */
    uint8_t  seq;      /**< The console's number for this command.              */
    union {
        struct {
            int16_t  win_x;    /**< The camera's window, sensor px; -1 the last good one. */
            int16_t  win_y;    /**< Its top.                                    */
            uint32_t mark_ms;  /**< How long the marks may take to be seen.     */
            uint16_t dx_mm10;  /**< The most the screen may sit off, 0.1 mm.     */
            uint16_t deg10;    /**< The most it may turn, 0.1 degree.           */
        } place;                                /**< TEST_DOT_VERB_PLACE.       */
        struct {
            uint8_t phase;     /**< test_dot_phase_t.                           */
            bool    pass;      /**< RESULT: the verdict.                         */
        } screen;                               /**< TEST_DOT_VERB_SCREEN.      */
        struct {
            bool    one;       /**< Only one photo.                              */
            uint8_t only;      /**< Which, when @c one: from TEST_DOT_PHOTO_FIRST. */
        } photos;                               /**< TEST_DOT_VERB_PHOTOS.      */
        uint8_t blob;          /**< TEST_DOT_VERB_BLOB: 0 location, 1 colours.   */
        uint8_t relays;        /**< TEST_DOT_VERB_RELAYS: the PCF8574's byte.     */
        uint8_t ch;            /**< TEST_DOT_VERB_ADC: the input, 0..3.          */
        uint8_t count;         /**< TEST_DOT_VERB_LOOP: frames at each speed.    */
        radio_probe_t radio;   /**< TEST_DOT_VERB_RADIO: what to check.          */
        bool    on;            /**< TEST_DOT_VERB_BOOST: the boost on.            */
        bool    pass;          /**< TEST_DOT_VERB_RESULT: the verdict.           */
        uint8_t cam;           /**< TEST_DOT_VERB_CAMERA: test_dot_cam_t.        */
    } a;               /**< The verb's keys, already checked by the console.    */
} TestDotReqEvt;

/** @brief One record of a station verb (TEST_DOT_REC_SIG): what follows `>|`. */
typedef struct {
    QEvt    super;
    uint8_t seq;                           /**< The command's number.          */
    char    text[KODE_FUNCTION_REC_MAX];   /**< The record, without `>|`.      */
} TestDotRecEvt;

/** @brief A station verb's end (TEST_DOT_ANS_SIG). */
typedef struct {
    QEvt    super;
    uint8_t seq;                           /**< The command's number.          */
    uint8_t code;                          /**< 0 for `>ok`, else kcli_err_t.   */
    char    msg[KODE_TEST_DOT_WHY_MAX];    /**< The refusal's word.             */
} TestDotAnsEvt;

/**
 * @brief A blob lent for the wire (TEST_DOT_LEND_SIG).
 *
 * The bytes are AO_TestDot's, in PSRAM, and stay as they are until the
 * console says it has sent them with test_dot_blob_back(): the loan
 * display_capture_buf() makes of the framebuffer, for the same reason - a
 * copy of tens of kilobytes into events would be the pool's whole size.
 */
typedef struct {
    QEvt           super;
    uint8_t        seq;                          /**< The command's number. */
    const uint8_t *data;                         /**< The bytes.            */
    uint32_t       len;                          /**< How many.             */
    char           name[KODE_TEST_DOT_NAME_MAX]; /**< The blob's name.      */
} TestDotLendEvt;

/** @brief The screens a run asks for, in kui_test_dot's order. */
#define TEST_DOT_SCR_CONNECTING  0U  /**< The connectors going in.               */
#define TEST_DOT_SCR_AUTO        1U  /**< Hands off, with the grid of the rows.  */
#define TEST_DOT_SCR_PADS        2U  /**< The pads delivery test.                */
#define TEST_DOT_SCR_CAMERA      3U  /**< Between two patterns.                  */
#define TEST_DOT_SCR_LED         4U  /**< The camera looks at the LED.           */
#define TEST_DOT_SCR_BUTTONS     5U  /**< The operator presses.                  */
#define TEST_DOT_SCR_TOUCH       6U  /**< The operator touches.                  */
#define TEST_DOT_SCR_RESULT      7U  /**< PASS or FAIL.                          */
#define TEST_DOT_SCR_COUNT       8U  /**< How many screens.                      */
/** @brief A pattern's kind is its kui_test_dot pattern with this bit set. */
#define TEST_DOT_PAT_FLAG        0x80U
#define TEST_DOT_PAT_COLOUR      (TEST_DOT_PAT_FLAG | 0U) /**< A colour by index.   */
#define TEST_DOT_PAT_GREY        (TEST_DOT_PAT_FLAG | 1U) /**< A grey by level.     */
#define TEST_DOT_PAT_MARKS       (TEST_DOT_PAT_FLAG | 2U) /**< Black and the marks. */
#define TEST_DOT_PAT_OK          (TEST_DOT_PAT_FLAG | 3U) /**< A pass icon.         */
#define TEST_DOT_PAT_FAIL        (TEST_DOT_PAT_FLAG | 4U) /**< A fail icon.         */
#define TEST_DOT_PAT_LINES       (TEST_DOT_PAT_FLAG | 5U) /**< Lit rows.            */
#define TEST_DOT_PAT_DOTS        (TEST_DOT_PAT_FLAG | 6U) /**< A lattice of dots.   */
#define TEST_DOT_PAT_OFF         (TEST_DOT_PAT_FLAG | 7U) /**< The pattern taken away. */
#define TEST_DOT_PAT_COUNT       8U  /**< How many patterns.                     */
#define TEST_DOT_SCREEN_ARG_MAX  18U /**< Bytes of a screen's arg: the grid and its terminator. */
#define TEST_DOT_ST_PENDING      ((uint8_t)'p') /**< A cell not reached yet.     */
#define TEST_DOT_ST_RUNNING      ((uint8_t)'r') /**< A cell running now.         */
#define TEST_DOT_ST_CURRENT      ((uint8_t)'c') /**< The step asked for now.     */
#define TEST_DOT_ST_OK           ((uint8_t)'o') /**< Done and passed; PASS.      */
#define TEST_DOT_ST_FAILED       ((uint8_t)'x') /**< Failed; FAIL.               */
#define TEST_DOT_ST_AGAIN        ((uint8_t)'r') /**< Buttons: the same key again, after a wrong press. */
#define TEST_DOT_SCR_END         0x7FU /**< No screen: the station is done with the glass and the host leaves it. */
#define TEST_DOT_COLOUR_BLACK    0U    /**< A colour pattern's arg: black, no witnesses. */
#define TEST_DOT_COLOUR_WHITE    1U    /**< White.                        */
#define TEST_DOT_COLOUR_RED      2U    /**< Full red.                     */
#define TEST_DOT_COLOUR_GREEN    3U    /**< Full green.                   */
#define TEST_DOT_COLOUR_BLUE     4U    /**< Full blue.                    */
#define TEST_DOT_GREY_RUN        128U  /**< The grey level a run photographs (tdan's golden tables). */
#define TEST_DOT_PHOTO_FIRST     1U    /**< The first photo's number on the wire (`photo_01_...`, `only=1`): the station counts from one. */

/** @brief A screen or a pattern for the glass (TEST_DOT_SCREEN_REQ_SIG). */
typedef struct {
    QEvt    super;
    uint8_t kind;                          /**< TEST_DOT_SCR_* or TEST_DOT_PAT_*. */
    uint8_t seq;                           /**< The request's number, echoed by
                                            *   test_dot_screen_done().           */
    uint8_t len;                           /**< Bytes of @c arg that count.       */
    uint8_t arg[TEST_DOT_SCREEN_ARG_MAX];  /**< A screen's states (TEST_DOT_ST_*),
                                            *   or a pattern's one byte.          */
} TestDotScreenEvt;

/** @brief The host has a screen or a pattern on the panel (TEST_DOT_SCREEN_DONE_SIG). */
typedef struct {
    QEvt    super;
    uint8_t kind;                          /**< The kind it shows.                */
    uint8_t seq;                           /**< The request's number.             */
} TestDotScreenDoneEvt;

#if CONFIG_KODE_LOWPOWER

/** @brief A round of readiness for a target state (LOWPOWER_ROUND_SIG). */
typedef struct {
    QEvt     super;
    uint8_t  target;   /**< The kode_lp_state_t being prepared.               */
    uint32_t round;    /**< The round's number; answers carry it back.        */
} LowPowerRoundEvt;

/**
 * @brief The board changed state (LOWPOWER_STATE_SIG).
 *
 * Participants rest on LowPowerRoundEvt, not on this: it tells them the round
 * was decided. state AWAKE is the signal to restore, and it repeats: after a
 * round abandoned by a busy or silent owner, by BOT or by a request it comes
 * with prev == AWAKE (or prev == ASLEEP when the abandoned round was for OFF
 * and began from ASLEEP), for owners that had already rested. state ASLEEP is
 * published before the QP tick stops, so nobody may arm a QTimeEvt on it.
 *
 * state RADIO says an owner's work fell due while ASLEEP and that the radio
 * owners alone are to do it: the tick runs and the lock is held, but the
 * glass and the other devices stay at rest. Only the owners that joined with
 * lowpower_join_radio() act on it (they restore as for AWAKE); everyone else
 * ignores any state that is not AWAKE. The wake ends with state ASLEEP and
 * prev RADIO, or with AWAKE when something real used the board.
 */
typedef struct {
    QEvt    super;
    uint8_t state;     /**< The new kode_lp_state_t.                          */
    uint8_t cause;     /**< The kode_lp_cause_t of the change.                */
    uint8_t prev;      /**< The kode_lp_state_t it left.                      */
} LowPowerStateEvt;

/**
 * @brief A request or a round did not go through (LOWPOWER_REFUSED_SIG).
 *
 * A copy of the refusal lowpower_last_refusal() holds, as rule 7 asks: the
 * reader is another task, and the next round rewrites the original. @c src
 * says whose request it answers, so a console closes only the verb it asked
 * for and not one another port's request brought about.
 */
typedef struct {
    QEvt    super;
    uint8_t src;       /**< The source the refusal answers: the refused
                        *   request's, or the abandoned round's; the source
                        *   code of "no one" when a timer began the round.  */
    char    why[KODE_LOWPOWER_REFUSAL_MAX]; /**< "who: why", NUL-terminated.   */
} LowPowerRefusedEvt;

/** @brief A console line came in (LOWPOWER_INPUT_SIG). */
typedef struct {
    QEvt    super;
    uint8_t src;       /**< The KODE_CONSOLE_SRC_* port it came on.           */
} LowPowerInputEvt;

/**
 * @brief An owner's work that falls due at a real time (LOWPOWER_WAKE_AT_SIG).
 *
 * The time is khal_time_us(), which runs while the board sleeps, and not a
 * QTimeEvt, which does not.
 */
typedef struct {
    QEvt     super;
    uint64_t due_us;   /**< khal_time_us() when the work is due.              */
    uint8_t  who;      /**< The id lowpower_join() gave the owner, for the log. */
} LowPowerWakeAtEvt;

/** @brief A radio owner's work in a radio wake is finished (LOWPOWER_RADIO_DONE_SIG). */
typedef struct {
    QEvt    super;
    uint8_t who;       /**< The id lowpower_join_radio() gave the owner.     */
} LowPowerRadioDoneEvt;

/** @brief A request to go to a state (LOWPOWER_REQ_SIG). */
typedef struct {
    QEvt    super;
    uint8_t target;    /**< The kode_lp_state_t wanted.                       */
    uint8_t src;       /**< Who asked, for the log: KODE_CONSOLE_SRC_* when a
                        *   command did.                                     */
} LowPowerReqEvt;

/**
 * @brief One participant's answer to a round (LOWPOWER_READY_SIG).
 *
 * @c why is a copy, as rule 7 asks: the participant's own string may change
 * before AO_LowPower reads the answer.
 */
typedef struct {
    QEvt     super;
    uint8_t  who;      /**< The id lowpower_join() gave the participant.      */
    uint32_t round;    /**< The round answered; an older one is dropped.      */
    bool     busy;     /**< true: not now; the state change is refused.       */
    char     why[KODE_LOWPOWER_WHY_MAX]; /**< Why busy; NUL-terminated.       */
} LowPowerReadyEvt;

/**
 * @brief The glyphs the minute is drawn from (DISPLAY_GLYPHS_SIG).
 *
 * The one exception to rule 7 that this event makes, and the reason: the
 * eleven glyphs (0 to 9 and the colon) of a large font are far more than an
 * event can carry, and copying them into the pool for every hand-over would
 * only move the same bytes twice. @c strip is in PSRAM, owned by the sender,
 * and valid from the hand-over until the board is AWAKE again: AO_Display
 * reads it and never frees or writes it.
 */
typedef struct {
    QEvt            super;
    const uint16_t *strip;    /**< The glyphs, RGB565, in one row: the ten
                               *   digits 0 to 9, glyph_w wide each, then the
                               *   colon, colon_w wide; the row is
                               *   10 * glyph_w + colon_w pixels wide and
                               *   glyph_h tall. NULL: the renderer is parked
                               *   and has no glyphs to give.                */
    uint16_t        glyph_w;  /**< One digit's width, pixels.                 */
    uint16_t        glyph_h;  /**< One digit's height, pixels.                */
    uint16_t        x;        /**< Left edge of the time on the glass.        */
    uint16_t        y;        /**< Top edge of the time on the glass.         */
    uint16_t        colon_w;  /**< The colon's width, pixels.                 */
    uint8_t         rest_pct; /**< The glass's brightness while ASLEEP.       */
    bool            h12;      /**< true: the hour is read on a 12 h dial, 1 to
                               *   12 with no AM or PM, and an hour of one
                               *   figure leaves the tens cell blank, the
                               *   way the renderer's own clock does. false:
                               *   00 to 23.                                 */
    uint8_t         adv[11];  /**< From a figure to the next, the ten digits
                               *   then the colon, so the time keeps the
                               *   renderer's own spacing. All 0: fixed cells. */
} DisplayGlyphsEvt;

/**
 * @brief A wake of the P4 that involved the C5 (RADIO_SLEEP_NOTE_SIG).
 *
 * A copy of what the radio driver's sleep note says, in plain integers: this
 * header cannot include a driver. The driver's own kind enum maps onto
 * kode_radio_wake_t.
 */
typedef struct {
    QEvt     super;
    uint8_t  kind;       /**< A kode_radio_wake_t.                            */
    uint16_t knocks;     /**< Wake attempts the C5 needed.                    */
    uint32_t tx_sta;     /**< Frames the P4 sent for the station interface.   */
    uint32_t tx_serial;  /**< Frames sent on the serial (RPC) interface.      */
    uint32_t tx_hci;     /**< Frames sent on the HCI (BLE) interface.         */
} RadioSleepNoteEvt;

/** @brief AO_Net has left for an OFF round (RADIO_NET_LEFT_SIG). */
typedef struct {
    QEvt     super;
    uint32_t round;      /**< The round it left for.                          */
} RadioNetLeftEvt;

#endif /* CONFIG_KODE_LOWPOWER */

#ifdef __cplusplus
}
#endif

#endif /* KQP_SIGNALS_H_ */
