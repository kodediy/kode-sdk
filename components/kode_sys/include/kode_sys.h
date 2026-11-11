/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys.h
 * @brief The system Active Objects' contract with an app's own AOs and tasks.
 */
#ifndef KODE_SYS_H_
#define KODE_SYS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "kode_defs.h"
#include "kode_log.h"
#include "kapp_desc.h"      /* KODE_APP_MANIFEST */
#include "kode_variables.h"
#include "kode_functions.h"
#include "kode_config.h"
#include "kapp_cat.h"
#include "kqp.h"
#include "kode_init.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Commands
 *
 * A command posts to the device's owner, leaving the owner's command margin of
 * its queue free, and returns:
 *   KODE_OK            the owner's queue took it.
 *   KODE_NO_RESOURCES  the event pool is dry, or the owner's queue is within
 *                      the margin; nothing was posted, so call again later.
 *   anything else      the call's own refusal, named on it; nothing was posted.
 * The margin is KODE_SYS_CMD_MARGIN, or more for an owner that keeps slots for
 * inbound traffic of its own: AO_Net's commands stop above the slots inbound
 * MQTT keeps, so a loop of them never starves the messages. The audio settings
 * and the haptic plays keep one request pending at most instead, as each call
 * says, and so does a command that returns nothing: a call while one is
 * pending adds nothing. So any one command is safe in a loop: it is refused,
 * never QP's assert. Loops of several kinds to one owner share its slots.
 * What the owner then does shows in its log and in the events it publishes.
 * Safe from an AO and from any task.
 *
 * Stops: a command carries no caller, so a stop that is a command
 * (kode_sys_audio_record_stop(), kode_sys_haptic_stop()) ends what runs,
 * whoever started it. A stop that is a request takes @p me and ends only what
 * @p me opened (the stream's close, the emulations' stops).
 * ========================================================================= */

/* cppcheck-suppress misra-c2012-2.5 ; a limit for the apps that read it, never used inside the SDK */
/** @brief Slots a command leaves free in its owner's queue by default: half of the smallest, 8. */
#define KODE_SYS_CMD_MARGIN  KQP_CMD_MARGIN

/* ---- The status LED: to AO_LedMgr ---- */

/**
 * @brief Turn the LED on (last colour, or white) or off.
 * @param on true to light it, false to darken it.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_led_set(bool on);

/**
 * @brief Invert the LED.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_led_toggle(void);

/**
 * @brief Set the colour; (0,0,0) turns the LED off.
 * @param r Red component.
 * @param g Green component.
 * @param b Blue component.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/* ---- The screen: to AO_Display ---- */

#define KODE_SYS_SCREEN_BRIGHT_MIN 5U    /**< The lowest lit brightness, per cent; 0 is dark. */
#define KODE_SYS_SCREEN_BRIGHT_MAX 100U  /**< The highest brightness, per cent.               */

/**
 * @brief Set the panel's brightness for as long as the app runs.
 * @note Never stored: the user's own setting comes back when the app ends.
 *       0 darkens the glass and leaves the panel and the touch running.
 * @param pct 0, or KODE_SYS_SCREEN_BRIGHT_MIN to KODE_SYS_SCREEN_BRIGHT_MAX.
 * @return As the commands' rule above; KODE_DENIED for a level out of range.
 */
kode_status_t kode_sys_screen_brightness(uint8_t pct);

/* ---- Infrared: to AO_Ir; without KODEOS_RADIO (the block is on the C5) AO_Ir refuses ----
 * The C5 link, for this command and the infrared requests below: one that
 * finds the link down is held while AO_Ir has AO_Radio bring it up, within its
 * own budget (a raw send its start, a capture its @p ms, the rest about 10 s);
 * a request past it is answered KODE_TIMEOUT. Once AO_Radio has said the link
 * will not come, one is answered KODE_DENIED at once until the link is up
 * again. The last-frame read never waits. */

/**
 * @brief Send one NEC frame.
 * @note AO_Ir drops a frame that finds more requests for the C5 held than it
 *       has room for (KODE_IR_DEFER_DEPTH), with a log line.
 * @param addr The address.
 * @param cmd  The command.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_ir_send_nec(uint16_t addr, uint16_t cmd);

/* ---- Audio: to AO_Audio. The speaker amplifier is the C5's: with another C5 image every call still returns KODE_OK and plays silence ---- */

/** @brief The lowest tone frequency, Hz. */
#define KODE_SYS_AUDIO_HZ_MIN  400U

/** @brief The highest tone frequency, Hz. */
#define KODE_SYS_AUDIO_HZ_MAX  3400U

/** @brief The loudest tone amplitude, per cent of full scale. */
#define KODE_SYS_AUDIO_AMP_MAX_PCT  100U

/**
 * @brief Emit one tone.
 * @param hz      KODE_SYS_AUDIO_HZ_MIN to KODE_SYS_AUDIO_HZ_MAX, snapped to the nearest frequency the board's detector uses.
 * @param ms      How long to hold it.
 * @param amp_pct Peak amplitude, 1 to KODE_SYS_AUDIO_AMP_MAX_PCT.
 * @return As the commands' rule above; KODE_DENIED for an argument out of range.
 *         A codec in use refuses inside AO_Audio, with a log line.
 */
kode_status_t kode_sys_audio_tone(uint16_t hz, uint16_t ms, uint8_t amp_pct);

/**
 * @brief Set the output volume.
 * @note One request per setting is pending at most: a call while it waits
 *       changes the value it will apply, and is taken.
 * @param pct 0 to 100; 100 is 11 dB under the codec's full scale. Only the latest value is kept.
 * @return KODE_OK when taken; KODE_NO_RESOURCES when the pool is dry; KODE_DENIED above 100.
 */
kode_status_t kode_sys_audio_volume(uint8_t pct);

/**
 * @brief Set the microphone gain.
 * @note Pending as kode_sys_audio_volume() is.
 * @param db 0 to 42, rounded down to a 6 dB step. Only the latest value is kept.
 * @return KODE_OK when taken; KODE_NO_RESOURCES when the pool is dry; KODE_DENIED above 42.
 */
kode_status_t kode_sys_audio_mic_gain(uint8_t db);

/**
 * @brief Mute or unmute the output for this boot only.
 * @note Pending as kode_sys_audio_volume() is.
 * @param on true to mute.
 * @return KODE_OK when taken; KODE_NO_RESOURCES when the pool is dry.
 */
kode_status_t kode_sys_audio_mute(bool on);

/**
 * @brief End a recording into memory early, whoever asked for it (the stops' rule above).
 * @note That recording answers KODE_OK with the frames so far. One stop waits
 *       at most; another call meanwhile adds nothing. Ignored while nothing
 *       records, and for a recording into a file.
 */
void kode_sys_audio_record_stop(void);

/* ---- The haptic motor: to AO_Haptic, which drives the LRA ---- */

/** @brief The longest buzz one call asks for, in milliseconds; longer is refused, not cut. */
#define KODE_SYS_HAPTIC_MAX_MS  2000U

/** @brief The strongest drive a caller may ask for, per cent of full scale. */
#define KODE_SYS_HAPTIC_GAIN_MAX  100U

/** @brief The most segments one pattern holds. */
#define KODE_SYS_HAPTIC_SEGMENTS_MAX  16U

/**
 * @brief Buzz the LRA once.
 * @param ms       1 to KODE_SYS_HAPTIC_MAX_MS.
 * @param gain_pct 0 to KODE_SYS_HAPTIC_GAIN_MAX; the driver clamps at the LRA's rating (80 %).
 * @note One play is pending or playing at most: another is refused until it ends.
 * @return KODE_OK once posted. KODE_DENIED for an argument out of range or while a
 *         buzz or the haptic row is playing. KODE_NO_RESOURCES when the event
 *         pool is dry. A board with no haptic logs inside AO_Haptic.
 */
kode_status_t kode_sys_haptic_play_buzz(uint16_t ms, uint8_t gain_pct);

/**
 * @brief Vibrate a pattern of on/off segments, on first.
 * @param seg_ms   Segment lengths in ms, each above 0; copied.
 * @param n        1 to KODE_SYS_HAPTIC_SEGMENTS_MAX.
 * @param gain_pct 0 to KODE_SYS_HAPTIC_GAIN_MAX.
 * @return KODE_OK once posted. KODE_DENIED for a bad argument or while the motor is
 *         in use. KODE_NO_RESOURCES when the event pool is dry.
 */
kode_status_t kode_sys_haptic_play_pattern(const uint16_t *seg_ms, uint8_t n,
                                           uint8_t gain_pct);

/**
 * @brief Vibrate until kode_sys_haptic_stop().
 * @param gain_pct 0 to KODE_SYS_HAPTIC_GAIN_MAX.
 * @return As kode_sys_haptic_play_pattern().
 */
kode_status_t kode_sys_haptic_hold(uint8_t gain_pct);

/**
 * @brief End a pattern or a hold early, whoever started it (the stops' rule above).
 * @note One stop waits at most; another call meanwhile adds nothing. Ignored
 *       while the motor is idle.
 */
void kode_sys_haptic_stop(void);

/* ---- Firmware images: to AO_AppMgr ---- */

/**
 * @brief Write an image from @p src into @p dst.
 * @param src "fs:<file>", "sd:<file>", a bare file name for the media in their search order,
 *            or an "http://" or "https://" URL of up to 256 characters. A file name has up to 63.
 * @param dst "os" or "app".
 * @param run Boot the image once it is written and verified; ignored for "os", where selecting otadata is the launch.
 * @return As the commands' rule above. KODE_DENIED if this firmware has no
 *         AO_AppMgr, if the arguments do not parse, or if @p src is too long.
 */
kode_status_t kode_sys_flash(const char *src, const char *dst, bool run);

/* ---- MQTT: to AO_Net, which owns the client; needs Wi-Fi joined; DENIED without CONFIG_KODEOS_NET ----
 * A broker, a name, a password and a topic are copied, and a longer one is
 * refused with KODE_DENIED, never cut.
 */

/* cppcheck-suppress misra-c2012-2.5 ; a limit for the apps that read it, never used inside the SDK */
/** @brief The longest broker a connect names, NUL included. */
#define KODE_SYS_MQTT_URI_MAX    KODE_MQTT_URI_MAX
/* cppcheck-suppress misra-c2012-2.5 ; a limit for the apps that read it, never used inside the SDK */
/** @brief The longest user name a connect carries, NUL included. */
#define KODE_SYS_MQTT_USER_MAX   KODE_MQTT_USER_MAX
/* cppcheck-suppress misra-c2012-2.5 ; a limit for the apps that read it, never used inside the SDK */
/** @brief The longest password a connect carries, NUL included. */
#define KODE_SYS_MQTT_PASS_MAX   KODE_MQTT_PASS_MAX
/* cppcheck-suppress misra-c2012-2.5 ; a limit for the apps that read it, never used inside the SDK */
/** @brief The longest topic a publish or a subscription names, NUL included. */
#define KODE_SYS_MQTT_TOPIC_MAX  KODE_MQTT_TOPIC_MAX

/**
 * @brief Ask AO_Net to connect to an MQTT broker.
 * @note Watch kode_sys_mqtt_is_connected() or MQTT_STATE_SIG.
 * @param uri  mqtt://host[:port] or mqtts://host[:port], shorter than KODE_SYS_MQTT_URI_MAX.
 * @param user NULL or "" for an anonymous broker; shorter than KODE_SYS_MQTT_USER_MAX.
 * @param pass NULL or "" likewise; shorter than KODE_SYS_MQTT_PASS_MAX.
 * @return As the commands' rule above; KODE_DENIED for an argument that does
 *         not fit, and in a build without CONFIG_KODEOS_NET.
 */
kode_status_t kode_sys_mqtt_connect(const char *uri, const char *user,
                                    const char *pass);

/**
 * @brief Ask AO_Net to stop the client.
 * @return As the commands' rule above; KODE_DENIED without CONFIG_KODEOS_NET.
 */
kode_status_t kode_sys_mqtt_disconnect(void);

/**
 * @brief Publish @p data to @p topic.
 * @note Accepted, not delivered.
 * @param topic The topic, shorter than KODE_SYS_MQTT_TOPIC_MAX.
 * @param data  The payload, cut to KODE_MQTT_PAYLOAD_MAX bytes; NULL only when @p len is 0.
 * @param len   Its length.
 * @return As the commands' rule above; KODE_DENIED for an argument that does
 *         not fit, and without CONFIG_KODEOS_NET.
 */
kode_status_t kode_sys_mqtt_publish(const char *topic, const void *data,
                                    uint16_t len);

/**
 * @brief Subscribe to @p topic at QoS 0.
 *
 * Messages arrive as MQTT_MSG_SIG on the event bus, so receiving them takes an
 * Active Object of your own, and a full queue is an assert, not a drop. AO_Net
 * runs at KODE_PRIO_NET (2), so a subscriber above it takes each message before
 * the next is read; one at KODE_PRIO_USER_LOW (1), below AO_Net, does not, and
 * must size its queue for the broker's rate.
 * @param topic The topic filter, shorter than KODE_SYS_MQTT_TOPIC_MAX.
 * @return As the commands' rule above; KODE_DENIED for a topic that does not
 *         fit, and without CONFIG_KODEOS_NET.
 */
kode_status_t kode_sys_mqtt_subscribe(const char *topic);

/**
 * @brief Stop receiving @p topic.
 * @param topic The topic filter given to kode_sys_mqtt_subscribe().
 * @return As the commands' rule above; KODE_DENIED for a topic that does not
 *         fit, and without CONFIG_KODEOS_NET.
 */
kode_status_t kode_sys_mqtt_unsubscribe(const char *topic);

/* ---- Wi-Fi: to AO_Radio; DENIED without CONFIG_KODEOS_WIFI ---- */

#define KODE_SYS_WIFI_SSID_MAX 33U  /**< Bytes of a network name, NUL included. */
#define KODE_SYS_WIFI_PASS_MAX 65U  /**< Bytes of a password, NUL included.     */

/**
 * @brief Ask AO_Radio to join a network; it is remembered once it has worked.
 * @param ssid The network, 1 to 32 bytes; NULL for a remembered one (@p pass ignored).
 * @param pass Its password, up to 64 bytes; NULL or "" for an open or remembered network.
 * @return As the commands' rule above; KODE_DENIED for an empty or too long
 *         @p ssid, a too long @p pass, or a build without CONFIG_KODEOS_WIFI.
 */
kode_status_t kode_sys_wifi_connect(const char *ssid, const char *pass);

/**
 * @brief Ask AO_Radio to leave the network; the credentials stay remembered.
 * @return As the commands' rule above; KODE_DENIED without CONFIG_KODEOS_WIFI.
 */
kode_status_t kode_sys_wifi_disconnect(void);

/* ---- Bluetooth: to AO_Radio; DENIED without CONFIG_KODEOS_BLE ----
 * The board's advert carries the console's service and is never changed; an
 * app's name and maker data ride in the scan response, together at most
 * KODE_SYS_BLE_SCAN_RSP_MAX bytes, and what does not fit is refused, not cut.
 * Neither is stored: every boot starts from the board's serial and no data.
 * AO_Radio keeps them while Bluetooth is off in Settings, the user's switch
 * that an app never changes, and they go on the air once it is on.
 */

#define KODE_SYS_BLE_NAME_MAX     30U  /**< Bytes of a name, NUL included.              */
#define KODE_SYS_BLE_SCAN_RSP_MAX 31U  /**< Bytes the scan response holds, two of them a
                                        *   length and a type per part.                 */
#define KODE_SYS_BLE_DATA_MIN     2U   /**< The least maker data: its company id.       */

/**
 * @brief The name the scan response carries.
 * @param name 1 to KODE_SYS_BLE_NAME_MAX - 1 bytes; NULL or "" for the board's serial.
 * @return As the commands' rule above; KODE_DENIED for a name too long, or one that
 *         does not fit beside the data set last, or without CONFIG_KODEOS_BLE.
 */
kode_status_t kode_sys_ble_set_name(const char *name);

/**
 * @brief The maker data the scan response carries.
 * @param data The bytes, the company id first, little-endian, as Bluetooth defines it;
 *             NULL when @p len is 0.
 * @param len  0 for none, or KODE_SYS_BLE_DATA_MIN up to what fits beside the name.
 * @return As the commands' rule above; KODE_DENIED for data that does not fit beside
 *         the name set last, a @p len of 1, or without CONFIG_KODEOS_BLE.
 */
kode_status_t kode_sys_ble_set_data(const uint8_t *data, size_t len);

/* ---- Bluetooth: the values of the app's own service, to AO_Radio ----
 * The service and its rules are in the immediate calls, beside
 * KODE_SYS_BLE_SERVICE(). Each call checks the declared table at once and is
 * DENIED, posting nothing, for a service left out or not declared, a name no
 * characteristic has, a notify of one without KODE_SYS_BLE_NOTIFY, more than
 * KODE_SYS_BLE_VALUE_MAX bytes or NULL data with a length, and without
 * CONFIG_KODEOS_BLE. Otherwise it posts a copy of the bytes to AO_Radio,
 * which owns the service, as the commands' rule above says. Both work before
 * the host has started and while Bluetooth is off in Settings: the value
 * waits for the first read. What AO_Radio then refuses has nobody to tell: a
 * notification the full ring has no room for is not sent, the value is set
 * all the same, and PSRAM refusing the values leaves it unset; a task that
 * needs to know uses kode.h's kode_ble_notify(), which waits for AO_Radio's
 * answer.
 */

/**
 * @brief Set what a read of characteristic @p chr gets. Nothing is sent.
 * @param chr  The characteristic's name.
 * @param data The value; NULL when @p len is 0.
 * @param len  0 to KODE_SYS_BLE_VALUE_MAX bytes.
 * @return As the commands' rule above; KODE_DENIED as the section says.
 */
kode_status_t kode_sys_ble_set_value(const char *chr, const void *data, size_t len);

/**
 * @brief Set characteristic @p chr's value, and notify a subscribed central.
 *
 * With nobody subscribed the value is set and nothing is sent. A
 * notification longer than the link's MTU less 3 is not sent; the value
 * stays for a read.
 *
 * @param chr  The characteristic's name; it carries KODE_SYS_BLE_NOTIFY.
 * @param data The bytes; NULL when @p len is 0.
 * @param len  0 to KODE_SYS_BLE_VALUE_MAX bytes.
 * @return As the commands' rule above; KODE_DENIED as the section says.
 */
kode_status_t kode_sys_ble_notify(const char *chr, const void *data, size_t len);

/* ---- Resting the board: to AO_LowPower ----
 * AO_LowPower asks every started owner to put its device away before the board
 * changes state, and an owner with work in hand can refuse. Both calls are
 * commands: the answer is the state the board ends up in, published as
 * LOWPOWER_STATE_SIG, or the refusal, published as LOWPOWER_REFUSED_SIG (an
 * app with an AO of its own subscribes to them). The same signal also says
 * RADIO when the radio alone wakes in ASLEEP: ignore any state that is not
 * AWAKE, ASLEEP or OFF. While a cable is in the board does not sleep or switch
 * off: the request is refused and the board stays awake.
 *
 * Neither reaches the battery's ship mode. That is a console verb,
 * `ship_mode`, and the only door to it.
 */

/**
 * @brief Take the board to ASLEEP: light sleep with the radios kept up.
 *
 * Every owner puts its device at rest, the glass goes dark (it shows the
 * minute once a minute), and the P4 light-sleeps whenever every task is idle.
 * BOT, a message on BLE, a message on the Kode channel or a cable wakes it,
 * and every owner restores its device. After the off-after time
 * (`config key="off_after"`) the board goes on to OFF by itself.
 * @return As the commands' rule above; KODE_DENIED in a build without
 *         CONFIG_KODE_LOWPOWER, which logs `low power not built`.
 */
kode_status_t kode_sys_power_sleep(void);

/**
 * @brief Take the board to OFF: deep sleep, the pins held at rest.
 *
 * The sessions close, the C5 is put in reset and the P4 enters deep sleep;
 * only BOT wakes it, and the boot that follows is a normal one (the clock
 * kept counting).
 * @return As kode_sys_power_sleep().
 */
kode_status_t kode_sys_power_off(void);

/* ---- Power: to AO_Power, which owns the PMIC; the outcome shows in its log and in POWER_CTRL_SIG / POWER_EVT_SIG ---- */

#define KODE_SYS_CHARGE_LIMIT_MIN_MA 64U   /**< The smallest fast-charge current, mA: the chip's step.   */
#define KODE_SYS_CHARGE_LIMIT_MAX_MA 896U  /**< The largest, the pack's ceiling, mA.                     */
#define KODE_SYS_BOOST_MIN_MV        3600U /**< The cell voltage under which the boost is refused, mV.   */

/**
 * @brief 5 V on the expansion header from the battery, or not.
 * @note 0.5 A, not persisted.
 *
 * Refused while USB is present and while the cell is under the pack's floor;
 * AO_Power stops it on a low cell or a boost fault.
 * @param on true to switch it on.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_power_boost(bool on);

/**
 * @brief Allow or forbid charging the cell.
 *
 * AO_Power also holds charging outside the pack's 10 to 45 C window; a request
 * to charge while held takes effect when the window returns.
 * @param on true to allow charging.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_power_charge(bool on);

/**
 * @brief The fast-charge current, 64 mA steps up to the pack's ceiling.
 *
 * The chip keeps the nearest step; above the ceiling nothing is written. The
 * board's input limit may hold the real current lower.
 * @param ma The current in milliamps.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_power_charge_limit(uint16_t ma);

/* ---- J3's module supply: to AO_Radio ---- */

/* cppcheck-suppress misra-c2012-2.3 ; an announcement's name for the apps that read it, never used inside the SDK */
/**
 * @brief Says a switch of J3's supply is over (EXP_POWER_SIG, published):
 *        @c on, the switch, and @c rc, a kode_status_t that says how it went.
 */
typedef ExpPowerDoneEvt kode_sys_exp_power_done_t;

/**
 * @brief Switch J3's +3.3V_EXT, the module's supply (the C5's IO5), on or off.
 *
 * Its state at boot is not promised: a C5 that the last firmware parked, held
 * in reset or never linked lets go of IO5, and R72 then holds the rail on;
 * one whose link drove it off leaves it off. The wish starts off, and a
 * link-up drives the pin as the wish is. AO_Radio takes the rail as unknown
 * until the C5 has driven or let go of the pin, and again after a failed
 * bring-up, a lost link or a C5 update, so a request is carried out, never
 * said done because the rail looked as asked; whoever needs a state asks for
 * it. The rail follows once the C5 can
 * carry the request out: a Wi-Fi sweep delays it by about 40 s and up to
 * 90 s, and an autojoin round by that sweep and up to 20 s for each
 * remembered network that will not have the board (kode.h's
 * KODE_PIN_POWER_MS is the bound). With no C5 link, "on" resets the C5 for
 * about 10 ms to release the pin: the speaker amplifier is off meanwhile and
 * during its boot. "Off" brings the link up, as an asked Wi-Fi scan does: no
 * service is switched on and nothing is stored, and the C5 then stays up for
 * the rest of the boot (about 85 mA). Claims nothing; J3's 5 V (pins 1 and
 * 20) is not switched.
 *
 * AO_Radio publishes EXP_POWER_SIG (kode_sys_exp_power_done_t) when the
 * switch is over, with one of three codes in @c rc:
 * - KODE_OK: the rail is as asked. The C5 drove the pin, the reset with no
 *   link put the rail on, or the rail was known to be as asked already.
 * - KODE_IO_ERROR: the C5 refused to drive the pin. The wish is dropped and
 *   the rail stays as it was; a new request asks again.
 * - KODE_DENIED: the link to the C5 would not come up, or went with the
 *   switch, so the C5 was not asked. The wish stays and the next link-up
 *   carries it out.
 *
 * An AO that waits subscribes to it before the call; one that does not hears
 * nothing.
 * @param on true for on, false for off.
 * @return As the commands' rule above; KODE_DENIED in an image built without
 *         KODEOS_RADIO.
 */
kode_status_t kode_sys_exp_power(bool on);

/* ---- Going home: to AO_Boot ---- */

/**
 * @brief Ask AO_Boot to reset the board.
 * @note AO_Boot leaves the network, parks the C5 and resets within
 *       KODE_LEAVE_BUDGET_MS; the caller runs on until then. It writes no
 *       boot request, so the reset lands in kodeOS after a launcher start and
 *       in this app again after a mailbox install (README, The boot model).
 *       kode.h's kode_exit() is this call, asked again until taken, and that
 *       wait.
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_exit_to_os(void);

/*
 * With CONFIG_KODEOS_HOME_HOLD (on by default in an app) holding BOT publishes
 * HOME_SIG and resets the board KODE_SYS_HOME_GRACE_MS later, as
 * kode_sys_exit_to_os() does. An app with something to save defers from its
 * HOME_SIG handler, writes, then resets; the wait is capped at
 * KODE_SYS_HOME_DEFER_MAX_MS from the hold.
 */

/**
 * @brief Ask AO_Boot for a moment before it resets the board on a BOT hold.
 *
 * Granted while AO_Boot's KODE_SYS_HOME_GRACE_MS window after a hold is open.
 * HOME_SIG goes out just before that window opens, so an AO that answers it
 * at once from the other core can be early: on KODE_DENIED, ask again a few
 * milliseconds later.
 *
 * @return As the commands' rule above; KODE_DENIED when no hold is pending.
 */
kode_status_t kode_sys_home_defer(void);

/**
 * @brief Reset the board now (after a deferral, or whenever the app decides).
 * @return As the commands' rule above.
 */
kode_status_t kode_sys_home_now(void);

/* =========================================================================
 * Requests
 *
 * A request takes the asking Active Object, @p me, posts to the device's
 * owner and returns at once. Its answer arrives to @p me later, as an event
 * carrying a copy: each call names the answer's signal, and the answer's
 * type, a kode_sys_*_answer_t, names its fields.
 *
 * Immediate returns:
 *   KODE_OK            taken; the answer will come.
 *   KODE_DENIED        @p me is NULL or not started (a plain task asks through
 *                      kode.h), an argument is bad, @p me's door of that kind is
 *                      busy, or the build has no such owner. Nothing was posted.
 *   KODE_NO_RESOURCES  the owner's queue is within its margin, or the event
 *                      pool is dry. Nothing was posted.
 *
 * Doors: each asker has one door per kind of request, and each section names
 * its kinds. A request holds its door until its answer; a second request of
 * the same kind before then is refused at once. The door is free again just
 * before the answer is posted, so @p me may ask again from the answer's
 * handler.
 *
 * Who calls: any thread may make a request in @p me's name, @p me's own or a
 * task's, as long as @p me is started. The answer arrives at @p me, never at
 * the task: @p me's handler hands it on. The doors are @p me's, so a task and
 * @p me share them: while one holds the door of a kind, the other asking that
 * kind is refused at once. Two threads asking at the same moment get the same:
 * one is taken, the other refused. A task adds no door, so it adds no slot to
 * @p me's queue. The lent-buffer rule binds whichever thread made the call.
 * The audio stream's ring has one writer: one thread at a time calls
 * kode_sys_audio_stream_write().
 *
 * Answers: a request taken is answered exactly once, posted to @p me with no
 * margin and never published, with the final status in @c rc. The owner's
 * refusals (the device busy, absent or failed) arrive in the answer, not as a
 * return: an owner whose device is not fitted still answers.
 *
 * Lent buffers: a payload too big for an event stays in the caller's buffer,
 * marked LENT on the call. Keep it valid, and leave it alone, from the call
 * until the answer arrives. The owner never writes it after the answer, and
 * never writes it at all for a request refused, at once or in the answer.
 *
 * Queue depth, the one rule for every asker: an answer that finds @p me's
 * queue full is QP's assert, so the queue holds everything that can wait in
 * it at once:
 *   - one slot for each request waiting for its answer: one per door open;
 *   - @p me's own events: its timers, the signals it subscribes to (a button
 *     press and its release are two) and whatever else is posted to it;
 *   - during a listing, its entries: AO_Storage posts them until only
 *     KODE_SYS_LIST_FREE_SLOTS are free, then the end in one of those.
 * So while a listing runs, the rest must fit in KODE_SYS_LIST_FREE_SLOTS - 1.
 * Priority changes the count: an asker above AO_Storage (KODE_PRIO_STORAGE)
 * takes each entry as it is posted, so entries never pile up and only the
 * rest counts. KQP_AO_QUEUE_LEN (16) suits an asker above AO_Storage, or one
 * whose rest stays within KODE_SYS_LIST_FREE_SLOTS - 1 while it lists. A
 * busier asker below AO_Storage takes a queue of KODE_SYS_FILE_LIST_MAX +
 * KODE_SYS_LIST_FREE_SLOTS (32) plus every slot its rest needs.
 * For example, an AO at KODE_PRIO_USER_LOW with 16 slots, a 1 s timer and
 * BUTTON_EVT_SIG asks on one tick for a listing, a file write, an HTTP GET,
 * an IMU and a magnetometer sample. AO_Storage runs first: 8 entries leave 8
 * free, the end 7, the four answers 3, the next tick 2, a press and release
 * 0, and the next event asserts. The same AO at KODE_PRIO_USER_HIGH is fine,
 * and so is one at KODE_PRIO_USER_LOW with 32 + 7 = 39 slots or more.
 *
 * Answer signals: the signals the answers arrive on (named on each call) are
 * also published or posted for the console and kodeOS's own screens, so an
 * AO never subscribes to one: it would receive answers that are not its own
 * and could not tell them apart.
 * ========================================================================= */

/* ---- Infrared, AO_Ir: three kinds, a raw send, a capture and a last-frame read ----
 * The C5 link: as the infrared command's section says. */

/** @brief The most symbols one raw infrared frame carries. */
#define KODE_SYS_IR_RAW_MAX_SYMBOLS  256U

/** @brief The most symbols one capture holds. */
#define KODE_SYS_IR_RX_MAX_SYMBOLS   256U

/** @brief One mark (carrier on) and the space after it: @c mark_us, @c space_us. */
typedef IrSymbol kode_sys_ir_symbol_t;

/** @brief What one received frame amounted to: @c addr, @c cmd (when @c nec), @c symbols, @c nec. */
typedef IrFrame kode_sys_ir_frame_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_ir_send_raw() (IR_SEND_RAW_RSP_SIG): @c rc. */
typedef StatusRspEvt kode_sys_ir_send_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_ir_capture() (IR_CAPTURE_RSP_SIG): @c rc, and @c n, the symbols written to the lent buffer. */
typedef IrCaptureRspEvt kode_sys_ir_capture_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_ir_last() (IR_LAST_RSP_SIG): @c rc, @c have and @c frame, zero unless @c have. */
typedef IrLastRspEvt kode_sys_ir_last_answer_t;

/**
 * @brief Send a raw frame.
 * @note LENT: AO_Ir copies @p sym as the call starts; keep it valid and unchanged
 *       until IR_SEND_RAW_RSP_SIG arrives: once the frame went, or once the C5
 *       call has returned after a stall. A frame that waited too long to start
 *       is answered KODE_TIMEOUT, unsent.
 * @param me         The asking AO; IR_SEND_RAW_RSP_SIG answers it.
 * @param sym        Symbols, mark first.
 * @param n          1 to KODE_SYS_IR_RAW_MAX_SYMBOLS.
 * @param carrier_hz The carrier, not 0; 38000 is what the board's own receiver hears.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_ir_send_raw(QActive *me, const kode_sys_ir_symbol_t *sym, size_t n,
                                   uint32_t carrier_hz);

/**
 * @brief Listen for the next frame.
 * @note LENT: AO_Ir fills @p out in place just before IR_CAPTURE_RSP_SIG and never
 *       after; keep it valid until then. A longer capture is truncated to @p cap.
 *       KODE_OK: a frame, @c n symbols. KODE_TIMEOUT: @p ms went by in silence.
 *       KODE_IO_ERROR: no infrared block. KODE_DENIED: AO_Ir was busy, or a
 *       later capture displaced this one: whoever asks (kode.h, another AO, the
 *       console), the newest capture is the one AO_Ir keeps.
 * @param me  The asking AO; IR_CAPTURE_RSP_SIG answers it.
 * @param out Where the symbols go; not NULL.
 * @param cap How many it holds, not 0; at most KODE_SYS_IR_RX_MAX_SYMBOLS are used.
 * @param ms  How long to listen, not 0; longer than ten minutes is ten minutes.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_ir_capture(QActive *me, kode_sys_ir_symbol_t *out, size_t cap,
                                  uint32_t ms);

/**
 * @brief Ask for the last frame AO_Ir heard.
 * @param me The asking AO; IR_LAST_RSP_SIG answers it, @c have false when none was heard.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_ir_last(QActive *me);

/* ---- Audio, AO_Audio: six kinds, the info, a play, a recording, a recording
 *      into a file, the stream's open and its close ---- */

/** @brief The one sample rate: everything is 48 kHz, 16-bit, mono; resample before calling. */
#define KODE_SYS_AUDIO_RATE_HZ     48000U

/** @brief The longest sound one call carries, in seconds. */
#define KODE_SYS_AUDIO_BUF_SECONDS   30U

/** @brief The longest sound one call takes, in frames; longer is KODE_DENIED, never truncated. */
#define KODE_SYS_AUDIO_MAX_FRAMES  (KODE_SYS_AUDIO_BUF_SECONDS * KODE_SYS_AUDIO_RATE_HZ)

/** @brief The channel count of a mono stream. */
#define KODE_SYS_AUDIO_CHANNELS_MONO    1U

/** @brief The channel count of a stereo stream. */
#define KODE_SYS_AUDIO_CHANNELS_STEREO  2U

/** @brief The frames the stream holds ahead of the speaker (200 ms); it holds the codec from open to close. */
#define KODE_SYS_AUDIO_STREAM_FRAMES  9600U

/** @brief The longest path a recording into a file names, NUL included. */
#define KODE_SYS_AUDIO_PATH_MAX  96U

/**
 * @brief What the audio path is and what it is set to: @c rate, @c present,
 *        @c open, @c busy, @c bits, @c volume, @c mic_db, @c mute.
 */
typedef AudioInfo kode_sys_audio_info_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_audio_info_read() (AUDIO_INFO_RSP_SIG): @c rc and @c info. */
typedef AudioInfoRspEvt kode_sys_audio_info_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers a play (AUDIO_DONE_SIG), a recording (AUDIO_REC_RSP_SIG, with
 *        @c frames written) and the stream's open and close
 *        (AUDIO_STREAM_OPEN_RSP_SIG, AUDIO_STREAM_CLOSE_RSP_SIG): @c rc.
 */
typedef AudioDoneEvt kode_sys_audio_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_audio_record_file() (AUDIO_FILE_RSP_SIG): @c rc, @c path, @c bytes, @c ms. */
typedef AudioFileRspEvt kode_sys_audio_file_answer_t;

/**
 * @brief Ask what the audio path is and what it is set to.
 * @param me The asking AO; AUDIO_INFO_RSP_SIG answers it.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_audio_info_read(QActive *me);

/**
 * @brief Play a whole sound.
 * @note LENT: @p frames is played in place, so keep it valid and unchanged
 *       until AUDIO_DONE_SIG arrives: once the sound was heard, or once the
 *       codec's worker has let go of it after a stall (KODE_TIMEOUT).
 * @param me     The asking AO; AUDIO_DONE_SIG answers it, KODE_DENIED while the codec is busy.
 * @param frames Mono 16-bit samples at KODE_SYS_AUDIO_RATE_HZ.
 * @param n      1 to KODE_SYS_AUDIO_MAX_FRAMES.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_audio_play(QActive *me, const int16_t *frames, size_t n);

/**
 * @brief Record a whole sound into @p buf.
 * @note LENT: the codec writes @p buf in place until AUDIO_REC_RSP_SIG arrives.
 *       kode_sys_audio_record_stop() ends it early. A stalled codec is
 *       answered KODE_TIMEOUT once its worker returns, which each I2S
 *       transfer's own one-second wait bounds.
 * @param me  The asking AO; AUDIO_REC_RSP_SIG answers it, with the frames written.
 * @param buf Where the frames go.
 * @param cap Its capacity in frames; above 0.
 * @param ms  How long to record, above 0; capped at @p cap and KODE_SYS_AUDIO_BUF_SECONDS.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_audio_record(QActive *me, int16_t *buf, size_t cap, uint32_t ms);

/**
 * @brief Record for @p ms into a WAV file at @p path.
 * @param me   The asking AO; AUDIO_FILE_RSP_SIG answers it.
 * @param path Absolute and virtual, shorter than KODE_SYS_AUDIO_PATH_MAX; copied.
 * @param ms   How long to record, above 0; capped at KODE_SYS_AUDIO_BUF_SECONDS.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_audio_record_file(QActive *me, const char *path, uint32_t ms);

/**
 * @brief Open the stream at KODE_SYS_AUDIO_RATE_HZ; it holds the codec until closed.
 * @note Only @p me writes (kode_sys_audio_stream_write()) and closes the stream it opened.
 * @param me The asking AO; AUDIO_STREAM_OPEN_RSP_SIG answers it once the stream plays.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_audio_stream_open(QActive *me);

/**
 * @brief Close the stream once what the ring holds has been heard.
 * @param me The asking AO; AUDIO_STREAM_CLOSE_RSP_SIG answers it once drained,
 *           with KODE_DENIED when no stream is open, another opened it, or
 *           its open is not answered yet.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_audio_stream_close(QActive *me);

/* ---- Motion, AO_Imu (the QMI8658A and the QMC5883P): three kinds, an IMU
 *      sample, a magnetometer sample and the calibration (get, set and clear) ---- */

/** @brief The axes of a magnetometer reading. */
#define KODE_SYS_MAG_AXES  3U

/** @brief The axes of each IMU sensor. */
#define KODE_SYS_IMU_AXES  3U

/** @brief One IMU sample: @c a_mg, @c w_mdps (KODE_SYS_IMU_AXES each), @c temp_dc. */
typedef ImuSample kode_sys_imu_sample_t;

/** @brief A magnetometer calibration, AO_Imu's `mag_cal` setting: @c offset_mg and @c scale_milli. */
typedef MagCorr kode_sys_mag_corr_t;

#define KODE_SYS_MAG_CAL_GET    0U  /**< The answer to kode_sys_mag_cal_get().   */
#define KODE_SYS_MAG_CAL_SET    1U  /**< The answer to kode_sys_mag_cal_set().   */
#define KODE_SYS_MAG_CAL_CLEAR  2U  /**< The answer to kode_sys_mag_cal_clear(). */

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_imu_read() (IMU_SAMPLE_RSP_SIG): @c rc and @c sample, zero unless KODE_OK. */
typedef ImuSampleRspEvt kode_sys_imu_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_mag_read() (MAG_SAMPLE_RSP_SIG): @c rc and @c mg, raw mG on the chip's axes. */
typedef MagSampleRspEvt kode_sys_mag_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers the calibration requests (MAG_CAL_RSP_SIG): @c rc, @c op (a
 *        KODE_SYS_MAG_CAL_x), @c present and, when present, @c corr.
 */
typedef MagCalRspEvt kode_sys_mag_cal_answer_t;

/**
 * @brief Ask for one IMU sample; a resting IMU wakes first, about 200 ms.
 * @param me The asking AO; IMU_SAMPLE_RSP_SIG answers it, KODE_IO_ERROR with no IMU.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_imu_read(QActive *me);

/**
 * @brief Ask for one raw magnetometer sample.
 * @param me The asking AO; MAG_SAMPLE_RSP_SIG answers it, KODE_IO_ERROR with no magnetometer.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_mag_read(QActive *me);

/**
 * @brief Ask for the stored calibration.
 * @param me The asking AO; MAG_CAL_RSP_SIG answers it.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_mag_cal_get(QActive *me);

/**
 * @brief Store a calibration; AO_Imu judges its limits.
 * @param me   The asking AO; MAG_CAL_RSP_SIG answers it, KODE_DENIED past the limits.
 * @param corr The calibration; copied.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_mag_cal_set(QActive *me, const kode_sys_mag_corr_t *corr);

/**
 * @brief Forget the stored calibration.
 * @param me The asking AO; MAG_CAL_RSP_SIG answers it.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_mag_cal_clear(QActive *me);

/* ---- NFC, AO_Nfc (the ST25R3916B on the Antenna PCB): three kinds, a tag
 *      read, an NDEF read or write, and the emulation's start and stop ----
 * The reader is the one device and its field is up only while a request
 * waits: a request it cannot take is answered KODE_DENIED, no reader fitted
 * KODE_IO_ERROR. Nothing is lent: a message travels by value, in the request
 * and in the answer, at most KODE_SYS_NFC_TEXT_MAX - 1 characters; a longer
 * one is refused, never cut. An emulated card's @c ndef is the exception: it
 * holds KODE_SYS_NFC_EMU_NDEF_MAX characters, one more.
 */

/** @brief The longest message the NFC requests carry either way, NUL included. */
#define KODE_SYS_NFC_TEXT_MAX  KQP_NFC_TEXT_MAX

/* cppcheck-suppress misra-c2012-2.5 ; a limit for the apps that read it, never used inside the SDK */
/** @brief The longest @c ndef an emulated card carries, NUL excluded. */
#define KODE_SYS_NFC_EMU_NDEF_MAX  KQP_NFC_EMU_NDEF_MAX

/** @brief The longest NFCID1 a tag or a card carries, in bytes. */
#define KODE_SYS_NFC_UID_MAX  KQP_NFC_UID_MAX

/* What kind of tag activated: a tag's and an NDEF answer's @c type. */
#define KODE_SYS_NFC_TYPE_UNKNOWN  0U  /**< NFC-A, and nothing the reader can read.                    */
#define KODE_SYS_NFC_TYPE_T2T      1U  /**< Type-2: NTAG and its relatives.                            */
#define KODE_SYS_NFC_TYPE_T4T      2U  /**< Type-4: ISO-DEP, as a phone or a DESFire presents.         */
#define KODE_SYS_NFC_TYPE_MIFARE   3U  /**< Mifare Classic: its NDEF opens only with a key kodeOS knows. */

/*
 * What an NDEF answer's @c rc says when @c have_tag is true: the tag was there
 * and the exchange failed. With @c have_tag false, KODE_TIMEOUT is no tag in
 * the window, KODE_DENIED a reader held by another request and KODE_IO_ERROR
 * no reader fitted.
 */
#define KODE_SYS_NFC_NDEF_NO_MESSAGE  KODE_ERROR         /**< A read: no readable message on the tag. A write: any other failure. */
#define KODE_SYS_NFC_NDEF_LOCKED      KODE_DENIED        /**< A tag type that cannot be read or written (a Mifare Classic whose key is not known), or a read-only tag. */
#define KODE_SYS_NFC_NDEF_TOO_LONG    KODE_NO_RESOURCES  /**< A read: the message is longer than KODE_SYS_NFC_TEXT_MAX - 1. A write: it does not fit the tag, nothing written. */
#define KODE_SYS_NFC_NDEF_LOST        KODE_TIMEOUT       /**< The tag stopped answering; a write may have written part of it. */

/** @brief One tag as it identified itself: @c uid (KODE_SYS_NFC_UID_MAX), @c uid_len, @c atqa, @c sak, @c type (a KODE_SYS_NFC_TYPE_x). */
typedef NfcTag kode_sys_nfc_tag_t;

/**
 * @brief The card to present: @c uid, @c uid_len, @c atqa and @c sak to clone
 *        (zero to synthesise), and @c ndef with @c uri, or @c raw_len bytes of
 *        a ready-made @c raw message.
 */
typedef NfcCard kode_sys_nfc_card_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_nfc_read_tag() (NFC_READ_RSP_SIG): @c rc and @c tag, zero unless KODE_OK. */
typedef NfcReadRspEvt kode_sys_nfc_read_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers the NDEF requests (NFC_NDEF_RSP_SIG): @c rc (KODE_OK, or a
 *        KODE_SYS_NFC_NDEF_x when @c have_tag), @c have_tag and the tag's
 *        @c uid, @c uid_len, @c atqa, @c sak and @c type; a read's @c uri and
 *        @c text, @c len bytes; @c text is empty on a write.
 */
typedef NfcNdefRspEvt kode_sys_nfc_ndef_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers the emulation requests (NFC_EMU_RSP_SIG): @c rc and, for a stop, the @c reads served. */
typedef NfcEmuRspEvt kode_sys_nfc_emu_answer_t;

/**
 * @brief Wait up to @p ms for a tag.
 * @param me The asking AO; NFC_READ_RSP_SIG answers it. KODE_TIMEOUT: none turned up.
 * @param ms How long to keep the field up, not 0.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_nfc_read_tag(QActive *me, uint32_t ms);

/**
 * @brief Wait for a tag and read its NDEF message as text (a URI record as its full URI).
 * @param me The asking AO; NFC_NDEF_RSP_SIG answers it. A message that does not fit
 *           KODE_SYS_NFC_TEXT_MAX is KODE_SYS_NFC_NDEF_TOO_LONG.
 * @param ms How long to keep the field up, not 0.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_nfc_ndef_read(QActive *me, uint32_t ms);

/**
 * @brief Wait for a tag and write @p uri to it as an NDEF URI record.
 * @param me  The asking AO; NFC_NDEF_RSP_SIG answers it.
 * @param uri The link, copied; shorter than KODE_SYS_NFC_TEXT_MAX.
 * @param ms  How long to keep the field up, not 0.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_nfc_ndef_write_uri(QActive *me, const char *uri, uint32_t ms);

/**
 * @brief Wait for a tag and write @p text to it as an NDEF Text record.
 * @param me   The asking AO; NFC_NDEF_RSP_SIG answers it.
 * @param text The text, copied; shorter than KODE_SYS_NFC_TEXT_MAX.
 * @param ms   How long to keep the field up, not 0.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_nfc_ndef_write_text(QActive *me, const char *text, uint32_t ms);

/**
 * @brief Present the Dot as a passive NFC-A card until kode_sys_nfc_emulate_stop().
 * @param me   The asking AO; NFC_EMU_RSP_SIG answers it once the window is armed
 *             or refused: KODE_ERROR with no reader fitted (not KODE_IO_ERROR)
 *             or a driver that will not emulate, KODE_DENIED while the reader
 *             is held. Every other NFC request is KODE_DENIED meanwhile, and
 *             only @p me may stop the card.
 * @param card What to present; copied.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_nfc_emulate_start(QActive *me, const kode_sys_nfc_card_t *card);

/**
 * @brief Stop presenting the card.
 * @param me The asking AO; NFC_EMU_RSP_SIG answers it, KODE_DENIED when nothing
 *           played or another opened it (the card plays on).
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_nfc_emulate_stop(QActive *me);

/* ---- RFID (125 kHz LF), AO_Rfid (the front end on the Antenna PCB): two
 *      kinds, a read or a write, and the emulation's start and stop ----
 * The field is up only while a request waits: a request the front end cannot
 * take is answered KODE_DENIED. A missing front end is not detected: a read
 * times out. Nothing is lent: an ID travels by value.
 */

/** @brief The bytes of an EM4100 ID. */
#define KODE_SYS_RFID_ID_BYTES  5U

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers kode_sys_rfid_read() and _write() (RFID_OP_RSP_SIG): @c write,
 *        @c rc, @c id (read, or written back) and a read's @c rf_div.
 */
typedef RfidOpRspEvt kode_sys_rfid_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers the emulation requests (RFID_EMU_RSP_SIG): @c rc, and @c stop for a stop's answer. */
typedef RfidEmuRspEvt kode_sys_rfid_emu_answer_t;

/**
 * @brief Energise for up to @p ms and read the first EM4100 ID. A read never boosts VO4.
 * @param me The asking AO; RFID_OP_RSP_SIG answers it, KODE_TIMEOUT when nothing was decoded.
 * @param ms How long to keep the field up, not 0.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_rfid_read(QActive *me, uint32_t ms);

/**
 * @brief Write an ID onto a T5577 and read it back; the write raises VO4 to 3.3 V.
 * @param me The asking AO; RFID_OP_RSP_SIG answers it, KODE_OK when the read-back matched.
 * @param id The 40-bit ID, most-significant nibble first; copied.
 * @param ms How long to keep the field up, not 0.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_rfid_write(QActive *me, const uint8_t id[KODE_SYS_RFID_ID_BYTES],
                                  uint32_t ms);

/**
 * @brief Play an EM4100 ID as a tag until kode_sys_rfid_emulate_stop().
 * @param me The asking AO; RFID_EMU_RSP_SIG answers it once the tag plays or is
 *           refused, however long the front end takes; only @p me may stop it.
 * @param id The 40-bit ID, most-significant nibble first; copied.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_rfid_emulate_start(QActive *me,
                                          const uint8_t id[KODE_SYS_RFID_ID_BYTES]);

/**
 * @brief Stop playing the tag.
 * @param me The asking AO; RFID_EMU_RSP_SIG answers it, KODE_DENIED when none
 *           played or another opened it (the tag plays on).
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_rfid_emulate_stop(QActive *me);

/* ---- Files, AO_Storage: eight kinds, a status, a listing, a folder, a
 *      delete, a rename, a read, a write and a volume's space ----
 * Paths are absolute and virtual ("/sdcard/...", "/storage/..."), copied,
 * shorter than KODE_SYS_FILE_PATH_MAX. Every answer is a
 * kode_sys_file_answer_t; its @c rc is KODE_DENIED for a bad path or a
 * transfer in flight, and KODE_ERROR for a volume that will not mount (no
 * card) or a call that failed.
 */

/**
 * @brief The most bytes one call moves, for a read and for a write alike.
 * @note A bigger file is read in windows and written in parts, each with append.
 */
#define KODE_SYS_FILE_READ_MAX  32768U

/** @brief The most entries one directory listing carries. */
#define KODE_SYS_FILE_LIST_MAX  24U

/* cppcheck-suppress misra-c2012-2.5 ; a limit for the apps that read it, never used inside the SDK */
/**
 * @brief Slots a listing leaves free in the asker's queue, its end's among them;
 *        the requests' queue rule above says what else must fit in them.
 */
#define KODE_SYS_LIST_FREE_SLOTS  KODE_STORAGE_LIST_FREE_SLOTS

/** @brief The longest path a request names, NUL included. */
#define KODE_SYS_FILE_PATH_MAX  96U

/** @brief What one path is: @c exists, @c is_dir, @c size. */
typedef FileStat kode_sys_file_status_t;

/** @brief How big a volume is: @c total_kb, and @c free_kb of it, in whole kilobytes. */
typedef FileSpace kode_sys_file_space_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers the file requests: @c rc; a status's @c stat (FILE_STATUS_RSP_SIG);
 *        a space's @c space (FILE_SPACE_RSP_SIG); a read's @c got
 *        (FILE_READ_RSP_SIG); a listing's end, @c count and @c more
 *        (FILE_LIST_END_SIG); the rest @c rc alone (FILE_FOLDER_RSP_SIG,
 *        FILE_DELETE_RSP_SIG, FILE_RENAME_RSP_SIG, FILE_WRITE_RSP_SIG).
 */
typedef FileRspEvt kode_sys_file_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief One entry of a listing (STORAGE_ENTRY_SIG): @c name, @c size, @c is_dir,
 *        and @c pos, its place in the listing.
 */
typedef StorageEntryEvt kode_sys_file_entry_t;

/**
 * @brief Ask what is at @p path.
 * @param me   The asking AO; FILE_STATUS_RSP_SIG answers it. Nothing there is
 *             KODE_OK with @c stat.exists false.
 * @param path Absolute, virtual.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_status(QActive *me, const char *path);

/**
 * @brief Ask how big a volume is and how much of it is free.
 * @note On a large card the first count can take hundreds of milliseconds; the
 *       answer is an event, so nothing waits for it.
 * @param me     The asking AO; FILE_SPACE_RSP_SIG answers it, @c space filled
 *               in whole kilobytes on KODE_OK and zero otherwise. A volume that will not mount (no
 *               card) is KODE_ERROR, and @p volume that is not a volume's own
 *               root is KODE_DENIED.
 * @param volume A volume's root: "/sdcard" or "/storage".
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_space(QActive *me, const char *volume);

/**
 * @brief List a page of a directory.
 * @note Up to KODE_SYS_FILE_LIST_MAX STORAGE_ENTRY_SIG arrive, then
 *       FILE_LIST_END_SIG with @c count; @c more asks for the next page from
 *       @p skip + @c count. The listing always leaves KODE_SYS_LIST_FREE_SLOTS
 *       slots free in @p me's queue, the end's among them: an entry that would
 *       take one ends the page early, @c more set, and a page with no room for
 *       its first entry ends at once, KODE_NO_RESOURCES. Size @p me's queue by
 *       the requests' queue rule above.
 * @param me   The asking AO. A path that is no directory is KODE_DENIED.
 * @param path Absolute, virtual.
 * @param skip Entries earlier pages brought.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_list(QActive *me, const char *path, uint16_t skip);

/**
 * @brief Make one directory; its parent must exist.
 * @param me   The asking AO; FILE_FOLDER_RSP_SIG answers it.
 * @param path Absolute, virtual.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_create_folder(QActive *me, const char *path);

/**
 * @brief Delete a file or an empty directory.
 * @param me   The asking AO; FILE_DELETE_RSP_SIG answers it.
 * @param path Absolute, virtual.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_delete(QActive *me, const char *path);

/**
 * @brief Rename a path within its volume.
 * @param me   The asking AO; FILE_RENAME_RSP_SIG answers it, KODE_DENIED across volumes.
 * @param from Absolute, virtual.
 * @param to   On the same volume.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_rename(QActive *me, const char *from, const char *to);

/**
 * @brief Read a window of a file.
 * @note LENT: AO_Storage writes @p buf in place just before FILE_READ_RSP_SIG.
 *       @c got below @p cap is the end of the file, and an empty file is
 *       KODE_OK with @c got 0.
 * @param me     The asking AO; FILE_READ_RSP_SIG answers it.
 * @param path   Absolute, virtual.
 * @param offset The first byte; at or past the end reads nothing.
 * @param buf    Where the bytes go.
 * @param cap    1 to KODE_SYS_FILE_READ_MAX.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_read(QActive *me, const char *path, uint32_t offset, void *buf,
                                 uint32_t cap);

/**
 * @brief Write bytes to a file.
 * @note LENT: @p data is read in place, so keep it valid and unchanged until
 *       FILE_WRITE_RSP_SIG arrives.
 * @param me     The asking AO; FILE_WRITE_RSP_SIG answers it.
 * @param path   Absolute, virtual.
 * @param data   The bytes; NULL only when @p len is 0.
 * @param len    0 to KODE_SYS_FILE_READ_MAX.
 * @param append Keep what is there and add after it; otherwise truncate.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_file_write(QActive *me, const char *path, const void *data, uint32_t len,
                                  bool append);

/* ---- The network, AO_Net: four kinds, a lookup, a ping, an HTTP request
 *      (GET or POST) and a UDP send ----
 * Needs Wi-Fi joined. AO_Net's one worker does the I/O, one job at a time.
 * Names and URLs are copied, and a longer one is refused, never cut. In the
 * answer, KODE_IO_ERROR is no network, KODE_DENIED the worker busy with
 * another job (ask again), KODE_ERROR a name that does not resolve or a
 * server that refused, KODE_TIMEOUT a job past its guard. Without
 * CONFIG_KODEOS_NET every request is refused at once with KODE_DENIED.
 */

/** @brief What a ping session added up to: @c ip4, @c tx, @c rx, @c min_ms, @c avg_ms, @c max_ms. */
typedef NetPingStats kode_sys_ping_stats_t;

/** @brief What an HTTP request came back with: @c status, @c content_length, @c body_len, @c kept, @c truncated. */
typedef NetHttpResult kode_sys_http_result_t;

/** @brief The longest host a request names, NUL included. */
#define KODE_SYS_NET_HOST_MAX   64U
/** @brief The longest URL a request names, NUL included. */
#define KODE_SYS_NET_URL_MAX    80U
/** @brief The longest Content-Type a POST names, NUL included. */
#define KODE_SYS_NET_CTYPE_MAX  48U
/** @brief The most echoes one ping sends. */
#define KODE_SYS_NET_PING_MAX   10U

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers kode_sys_net_lookup() (NET_LOOKUP_RSP_SIG, the address in @c stats.ip4)
 *        and kode_sys_net_ping() (NET_PING_RSP_SIG): @c rc, @c stats.
 */
typedef NetHostRspEvt kode_sys_net_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_http_get() and _post() (NET_HTTP_RSP_SIG): @c rc, @c post, @c result. */
typedef NetHttpRspEvt kode_sys_http_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers kode_sys_udp_send() (NET_UDP_RSP_SIG): @c rc, @c sent. */
typedef NetUdpDoneEvt kode_sys_udp_answer_t;

/**
 * @brief Resolve a host name to an IPv4 address.
 * @param me   The asking AO; NET_LOOKUP_RSP_SIG answers it, network byte order.
 * @param host Name or dotted quad, shorter than KODE_SYS_NET_HOST_MAX.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_net_lookup(QActive *me, const char *host);

/**
 * @brief Ping a host, one echo a second.
 * @param me    The asking AO; NET_PING_RSP_SIG answers it, KODE_OK with @c rx 0 when
 *              nothing answered.
 * @param host  As kode_sys_net_lookup().
 * @param count 1 to KODE_SYS_NET_PING_MAX.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_net_ping(QActive *me, const char *host, uint8_t count);

/**
 * @brief GET a URL into @p buf; https:// is verified.
 * @note LENT: AO_Net's worker writes @p buf until NET_HTTP_RSP_SIG arrives. A 404
 *       is KODE_OK with @c result.status 404.
 * @param me  The asking AO; NET_HTTP_RSP_SIG answers it.
 * @param url Shorter than KODE_SYS_NET_URL_MAX.
 * @param buf Where the body goes, its first @p cap bytes; NULL keeps none.
 * @param cap Its room.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_http_get(QActive *me, const char *url, void *buf, uint32_t cap);

/**
 * @brief POST @p body to a URL and keep the answer's body in @p buf.
 * @note LENT: @p body is read and @p buf written in place until NET_HTTP_RSP_SIG.
 * @param me    The asking AO; NET_HTTP_RSP_SIG answers it, @c post true.
 * @param url   Shorter than KODE_SYS_NET_URL_MAX.
 * @param body  The body; NULL only when @p len is 0.
 * @param len   Its bytes.
 * @param ctype Content-Type, not empty, shorter than KODE_SYS_NET_CTYPE_MAX; NULL sends none.
 * @param buf   Where the answer's body goes; NULL keeps none.
 * @param cap   Its room.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_http_post(QActive *me, const char *url, const void *body, uint32_t len,
                                 const char *ctype, void *buf, uint32_t cap);

/**
 * @brief Send one UDP datagram.
 * @note LENT: @p data is read in place until NET_UDP_RSP_SIG.
 * @param me   The asking AO; NET_UDP_RSP_SIG answers it, @c sent the bytes that left.
 * @param host As kode_sys_net_lookup().
 * @param port The port.
 * @param data The datagram; NULL only when @p len is 0.
 * @param len  Its bytes.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_udp_send(QActive *me, const char *host, uint16_t port, const void *data,
                                uint16_t len);

/* ---- Wi-Fi, AO_Radio: one kind, a scan ----
 * The sweep the console's `wifi_scan` runs, over every channel of both bands
 * (about 38 s). With no link AO_Radio brings it up first, as a join does,
 * and switches no service on. The networks are a listing: AO_Radio posts one
 * WIFI_SCAN_ENTRY_SIG per network to @p me while its queue keeps
 * KODE_SYS_LIST_FREE_SLOTS free - the queue rule above, with AO_Radio
 * (KODE_PRIO_RADIO) in AO_Storage's place - then the answer. A scan waits for
 * the link's bring-up and the reconcile chain, seconds, and nothing longer:
 * while a sweep holds the radio (another asked scan, of this or any AO, the
 * console's, the boot's autojoin round), or a Bluetooth listen (asked, or the
 * console's `ble_scan`), the production row or an install does, it is
 * answered at once with @c rc KODE_NO_RESOURCES. Without
 * CONFIG_KODEOS_WIFI the request is refused at once with KODE_DENIED.
 */

/** @brief The most networks one sweep keeps: the strongest, the rest dropped. */
#define KODE_SYS_WIFI_SCAN_MAX  24U

/** @brief One network a sweep found, as the standard layer hands it on. */
typedef struct {
    char    ssid[KODE_SYS_WIFI_SSID_MAX];  /**< The name, NUL-terminated; "" when hidden. */
    int8_t  rssi;                          /**< Strength, dBm.                            */
    uint8_t channel;                       /**< Primary channel.                          */
    bool    open;                          /**< No password.                              */
} kode_sys_wifi_ap_t;

/* cppcheck-suppress misra-c2012-2.3 ; an entry's name for the apps that read it, never used inside the SDK */
/** @brief One network (WIFI_SCAN_ENTRY_SIG): @c ssid, @c rssi, @c channel, @c authmode (0 = open), @c bssid. */
typedef WifiScanEntryEvt kode_sys_wifi_scan_entry_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers kode_sys_wifi_scan() (WIFI_SCAN_RSP_SIG): @c rc, KODE_OK when the sweep
 *        ended, KODE_ERROR when it failed or the link went, KODE_NO_RESOURCES when the
 *        radio was busy (ask again); @c seen, the networks the sweep handed over;
 *        @c more, entries left out.
 */
typedef ScanRspEvt kode_sys_wifi_scan_answer_t;

/**
 * @brief Scan for Wi-Fi networks.
 * @param me The asking AO; WIFI_SCAN_ENTRY_SIG brings each network, then
 *           WIFI_SCAN_RSP_SIG the answer, its @c rc as kode_sys_wifi_scan_answer_t says.
 * @return As the requests' rules above.
 */
kode_status_t kode_sys_wifi_scan(QActive *me);

/* ---- Bluetooth, AO_Radio: one kind, a scan ----
 * The listen the console's `ble_scan` runs, for @p ms. A device that
 * advertises repeatedly is listed once, at its strongest. The devices are a
 * listing: AO_Radio posts one BLE_SCAN_ENTRY_SIG per device to @p me while
 * its queue keeps KODE_SYS_LIST_FREE_SLOTS free, then the answer. It shares
 * the Wi-Fi scan's one slot and rules: it waits for the link's bring-up and
 * the reconcile chain, and nothing longer; with another scan of either kind
 * held, or the console's, the autojoin round, the production row or an
 * install holding the radio, it is answered at once with @c rc
 * KODE_NO_RESOURCES. The autojoin round runs each time the link comes up with
 * Wi-Fi on, and a listen owed behind that bring-up is refused when the round
 * starts, so the first listen asked on a cold link with Wi-Fi on is nearly
 * always busy: ask again once the round ends. With Bluetooth off in Settings
 * it is answered at once with @c rc KODE_DENIED, in every state. Without
 * CONFIG_KODEOS_BLE the request is refused at once with KODE_DENIED.
 */

#define KODE_SYS_BLE_SCAN_MAX     24U     /**< The most devices one listen keeps.       */
#define KODE_SYS_BLE_SCAN_MS_MIN  500U    /**< The shortest listen, ms.                 */
#define KODE_SYS_BLE_SCAN_MS_MAX  30000U  /**< The longest listen, ms.                  */
#define KODE_SYS_BLE_ADDR_MAX     18U     /**< Bytes of an address as text, NUL included. */

/** @brief One device a listen heard, as the standard layer hands it on. */
typedef struct {
    char   name[KODE_SYS_BLE_NAME_MAX];     /**< Its name, cut to fit; "" when it gave none. */
    char   address[KODE_SYS_BLE_ADDR_MAX];  /**< "c0:00:00:00:00:01", as `ble_scan` prints it. */
    int8_t rssi;                            /**< Strength, dBm.                              */
} kode_sys_ble_device_t;

/* cppcheck-suppress misra-c2012-2.3 ; an entry's name for the apps that read it, never used inside the SDK */
/** @brief One device (BLE_SCAN_ENTRY_SIG): @c addr, @c rssi, @c name, @c addr_type, @c mfg_len, @c mfg, @c svc_n, @c svc16. */
typedef BleScanEvt kode_sys_ble_scan_entry_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers kode_sys_ble_scan() (BLE_SCAN_RSP_SIG): @c rc, KODE_OK when the listen
 *        ended, KODE_ERROR when it failed or the link went, KODE_NO_RESOURCES when the
 *        radio was busy (ask again), KODE_DENIED with Bluetooth off in Settings; @c seen,
 *        the devices handed over; @c more, entries left out.
 */
typedef ScanRspEvt kode_sys_ble_scan_answer_t;

/**
 * @brief Listen for Bluetooth devices.
 * @param me The asking AO; BLE_SCAN_ENTRY_SIG brings each device, then BLE_SCAN_RSP_SIG
 *           the answer, its @c rc as kode_sys_ble_scan_answer_t says.
 * @param ms How long, KODE_SYS_BLE_SCAN_MS_MIN to KODE_SYS_BLE_SCAN_MS_MAX.
 * @return As the requests' rules above; KODE_DENIED also for @p ms out of range.
 */
kode_status_t kode_sys_ble_scan(QActive *me, uint32_t ms);

/* =========================================================================
 * Immediate calls
 *
 * Each reads a copy, a flag or a pin, or subscribes the calling AO, and
 * returns: it posts nothing, and waits on nothing but a lock held for
 * microseconds, except kode_sys_nvs_get(), whose lock a flash write elsewhere
 * holds for milliseconds. Safe from an AO and from any task, except where the
 * call says otherwise.
 * ========================================================================= */

/* ---- Time ---- */

/**
 * @brief Milliseconds since boot.
 * @return The count, wrapping after roughly 49 days.
 */
uint32_t kode_sys_time_ms(void);

/**
 * @brief This board's serial, "KODE78YR5EEV0XS": no separators, safe in a topic, a filename or a URL.
 * @return A string owned by kodeOS, valid for the life of the firmware; never
 *         NULL, and empty when the board could not read its factory MAC.
 */
const char *kode_sys_device_id(void);

/**
 * @brief The wall clock as Unix seconds (UTC).
 * @note A time restored after a power cut counts as known and can lag by as long as the board had no power.
 * @param[out] unix_s Filled on KODE_OK only.
 * @return KODE_OK on a known clock; KODE_ERROR while it is unknown (no SNTP,
 *         `time set` or saved value); KODE_DENIED if @p unix_s is NULL.
 */
kode_status_t kode_sys_clock_read(int64_t *unix_s);

/** @brief A moment of local time, broken down; the extended layer's kode_time_t. */
typedef struct {
    uint16_t year;     /**< The year itself: 2026.                       */
    uint8_t  month;    /**< 1 (January) to 12.                           */
    uint8_t  day;      /**< The day of the month, 1 to 31.               */
    uint8_t  hour;     /**< 0 to 23.                                     */
    uint8_t  minute;   /**< 0 to 59.                                     */
    uint8_t  second;   /**< 0 to 59, 60 for a leap second.               */
    uint8_t  weekday;  /**< 0 = Sunday to 6 = Saturday.                  */
} kode_sys_time_t;

/**
 * @brief The wall clock as local time, in the zone the system keeps.
 *
 * The zone is the setting `config key="zone"`, a POSIX TZ string AO_Boot
 * applies at boot and on every change; UTC while none is stored. A reading
 * takes the C library's environment lock for one conversion, the lock a change
 * of zone takes too, so it is wholly one zone's.
 *
 * @note A time restored after a power cut counts as known and can lag by as long as the board had no power.
 * @param[out] out Filled on KODE_OK only.
 * @return KODE_OK on a known clock; KODE_ERROR while it is unknown (no SNTP,
 *         `time set` or saved value); KODE_DENIED if @p out is NULL.
 */
kode_status_t kode_sys_clock_local(kode_sys_time_t *out);

/* ---- The buttons: for a polling app; an AO subscribes to BUTTON_EVT_SIG ---- */

#define KODE_SYS_BUTTON_UP     0U  /**< Pad up.                                */
#define KODE_SYS_BUTTON_DOWN   1U  /**< Pad down.                              */
#define KODE_SYS_BUTTON_LEFT   2U  /**< Pad left.                              */
#define KODE_SYS_BUTTON_RIGHT  3U  /**< Pad right.                             */
#define KODE_SYS_BUTTON_TOP    4U  /**< The upper individual button.           */
#define KODE_SYS_BUTTON_BOT    5U  /**< The lower individual button.           */
#define KODE_SYS_BUTTON_COUNT  6U  /**< How many buttons there are.            */

/**
 * @brief Which buttons are down: bit (1U << KODE_SYS_BUTTON_x) set = pressed.
 *
 * Bits 0 to 5 are UP, DOWN, LEFT, RIGHT, TOP and BOT; the rest are clear. A
 * press shows after KODE_SYS_BUTTON_SETTLE_MS and a shorter tap shows nowhere,
 * so find an edge by comparing with the previous call. A press injected with
 * `btn` reads as down until the next word about that button.
 * @note Touches no bus.
 * @return The mask; 0 whenever nothing is pressed, and before kode_init().
 */
uint32_t kode_sys_button_read_mask(void);

/**
 * @brief How many presses AO_Buttons has confirmed since boot, injected ones included.
 * @note Unlike the mask it misses no tap between two polls.
 * @param id A KODE_SYS_BUTTON_x, the bit position in kode_sys_button_read_mask().
 * @return The presses since boot; 0 for an id out of range, and before kode_init().
 */
uint32_t kode_sys_button_read_presses(uint8_t id);

/* ---- The audio stream, the haptic hold, the emulations ---- */

/**
 * @brief Put frames into the open stream's ring, as many as fit now.
 * @note From the AO that opened the stream (kode_sys_audio_stream_open()), or its own task.
 * @param me       The AO the open answered.
 * @param frames   16-bit samples; left-right pairs for stereo.
 * @param n        Frames (pairs for stereo).
 * @param channels KODE_SYS_AUDIO_CHANNELS_MONO, or _STEREO played as their mean.
 * @param taken    Out, may be NULL: the frames the ring took.
 * @return KODE_OK when all fitted; KODE_TIMEOUT when the ring took fewer;
 *         KODE_DENIED when @p me did not open the stream, or a bad argument.
 */
kode_status_t kode_sys_audio_stream_write(QActive const *me, const int16_t *frames, size_t n,
                                          uint8_t channels, size_t *taken);

/**
 * @brief Whether a hold is on, whoever started it.
 * @return true from a hold's request until its stop is requested, or it ends.
 */
bool kode_sys_haptic_is_holding(void);

/**
 * @brief Whether the Dot is presenting an emulated card.
 * @return true from the window coming up until its reader is released.
 */
bool kode_sys_nfc_is_emulating(void);

/**
 * @brief Whether a tag is being emulated.
 * @return true from the start's KODE_OK until the stop ends.
 */
bool kode_sys_rfid_is_emulating(void);

/* ---- Touch: AO_Touch's reports of the finger ---- */

/**
 * @brief Subscribe @p me to TOUCH_EVT_SIG, AO_Touch's report of the finger, and link AO_Touch.
 *
 * Each press, each move while pressed and each release arrives as a
 * TouchEvt: the point, whether the finger is down, and the time it was read.
 * A bare subscription to TOUCH_EVT_SIG links nothing, so no AO_Touch starts
 * and nothing arrives: this call is that subscription with its owner linked.
 * kscreen subscribes its own owner; an AO that reads touch by hand calls this.
 * @param me The listening AO, started.
 * @return KODE_OK; KODE_DENIED when @p me is NULL or not started.
 */
kode_status_t kode_sys_touch_subscribe(QActive *me);

/* ---- The console's output: AO_Log's count of lines waiting for its ports ---- */

/**
 * @brief Whether the console is behind, so a producer of many records waits before its next slice.
 * @note The slowest attached port decides: BLE carries far fewer lines than the
 *       cable. kodeOS's own listings pace themselves on the same answer.
 * @return true while the output is backed up, so ask again shortly; false
 *         when the console keeps up.
 */
bool kode_sys_console_congested(void);

/* ---- The store: strings kept across resets under keys you choose; plain flash, readable by any console and app, not a vault ---- */

/* The canonical values are kqp_config.h's; guarded so a file seeing both has one. */
#ifndef KODE_SYS_NVS_KEY_MAX
#define KODE_SYS_NVS_KEY_MAX    16U   /**< Key ceiling, terminator included. */
#endif
#ifndef KODE_SYS_NVS_VALUE_MAX
#define KODE_SYS_NVS_VALUE_MAX 192U   /**< Value ceiling, terminator included. */
#endif

/**
 * @brief Read back what is stored under @p key.
 * @note Waits on the store's lock while another task writes or erases flash.
 *       Not from a kode_worker job. kode.h's kode_setting_set() stores a value.
 * @param key At most KODE_SYS_NVS_KEY_MAX - 1 characters, not empty.
 * @param out Destination, NUL-terminated on KODE_OK and untouched otherwise.
 * @param cap Size of @p out; a value that does not fit is an error, not a truncation. KODE_SYS_NVS_VALUE_MAX always fits.
 * @return KODE_OK. KODE_DENIED when nothing is stored under @p key (an
 *         unprovisioned board, not a fault), from a kode_worker job, and for
 *         an unusable @p key, a NULL @p out or a zero @p cap. Otherwise the
 *         store's status.
 */
kode_status_t kode_sys_nvs_get(const char *key, char *out, size_t cap);

/* ---- MQTT and Wi-Fi state ---- */

/**
 * @brief Whether the client is connected right now.
 * @return true when connected; false otherwise, and in a build without CONFIG_KODEOS_NET.
 */
bool kode_sys_mqtt_is_connected(void);

#define KODE_SYS_MQTT_STATE_DOWN        0U  /**< No client, or it has been stopped.  */
#define KODE_SYS_MQTT_STATE_CONNECTING  1U  /**< Started; waiting for the broker.    */
#define KODE_SYS_MQTT_STATE_UP          2U  /**< Connected.                          */

#define KODE_SYS_MQTT_ERR_NONE     0U  /**< No failure.                                  */
#define KODE_SYS_MQTT_ERR_NO_NET   1U  /**< The board has no address.                    */
#define KODE_SYS_MQTT_ERR_REFUSED  2U  /**< The broker or the transport said no.         */
#define KODE_SYS_MQTT_ERR_TIMEOUT  3U  /**< Nothing answered in time.                    */
#define KODE_SYS_MQTT_ERR_BUSY     4U  /**< The Kode channel holds the client.           */

/** @brief Where the MQTT client stands and AO_Net's last answer about it. */
typedef struct {
    uint8_t  state;                      /**< KODE_SYS_MQTT_STATE_x.                          */
    uint8_t  err;                        /**< KODE_SYS_MQTT_ERR_x of the last answer.         */
    uint32_t answers;                    /**< Answers since boot; a change means a new one.   */
    char     uri[KODE_MQTT_URI_MAX];     /**< The broker; "" before the first connect.        */
    bool     channel;                    /**< The Kode channel holds the client.              */
} kode_sys_mqtt_status_t;

/**
 * @brief Read where the client stands.
 * @note The fields are read one by one, so an answer between them can pair a
 *       state with the next answer's error.
 * @param out The status.
 * @return KODE_OK; KODE_DENIED for a NULL @p out, and without CONFIG_KODEOS_NET.
 */
kode_status_t kode_sys_mqtt_status(kode_sys_mqtt_status_t *out);

#define KODE_SYS_WIFI_STATE_IDLE          0U  /**< Nothing tried yet.                    */
#define KODE_SYS_WIFI_STATE_CONNECTING    1U  /**< Joining a network.                    */
#define KODE_SYS_WIFI_STATE_GOT_IP        2U  /**< Joined, with an address.              */
#define KODE_SYS_WIFI_STATE_DISCONNECTED  3U  /**< Left or lost the network.             */

/** @brief Where the Wi-Fi connection stands. */
typedef struct {
    uint8_t  state;                          /**< KODE_SYS_WIFI_STATE_x.                           */
    uint32_t ip4;                            /**< Network byte order; 0 without an address.        */
    char     ssid[KODE_SYS_WIFI_SSID_MAX];   /**< The network joined or being joined; "" for none. */
} kode_sys_wifi_status_t;

/**
 * @brief Read where the connection stands.
 * @note The state and the name are read one by one, so a drop between them can
 *       read GOT_IP with an empty name.
 * @param out The status; address and name are 0 and "" unless connecting (name)
 *            or connected (both).
 * @return KODE_OK; KODE_DENIED for a NULL @p out, and without CONFIG_KODEOS_WIFI.
 */
kode_status_t kode_sys_wifi_status(kode_sys_wifi_status_t *out);

/* ---- Bluetooth: the user's switch ---- */

/**
 * @brief Whether Bluetooth is on in Settings, the `ble` flag AO_Radio keeps.
 * @note Naming it links AO_Radio, as every Bluetooth call does.
 * @return true when it is on; false when it is off, or without CONFIG_KODEOS_BLE.
 */
bool kode_sys_ble_enabled(void);

/* ---- Bluetooth: the app's own GATT service, served by AO_Radio's driver ----
 * One service, declared at file scope with KODE_SYS_BLE_SERVICE(): it defines
 * the one constant the driver finds by name each time it builds the
 * attribute table, at the host's start. An app that declares none pays
 * nothing, and the table is the console's alone. Declare it in the file that
 * holds app_main(), or in one the app otherwise links: the driver's reference
 * is weak, and a weak reference does not pull an object out of a library.
 *
 * Security is the console's, whatever the build makes it: with console
 * pairing and the passkey on, the default, every characteristic needs an
 * encrypted, authenticated, bonded link, so only a phone bonded in kodeOS
 * with the code on the glass reads, writes or subscribes; with Just Works
 * pairing (no passkey) the link is encrypted and bonded, not authenticated;
 * with console pairing off the service is as open as the console. The
 * console's service stays in the table and in the advert. A table the driver
 * cannot serve (a UUID that is not 128 bits as 8-4-4-4-12 hex, no
 * characteristics or more than KODE_SYS_BLE_CHARS_MAX, a name missing, too
 * long or used twice, properties that are none of the three) is left out
 * whole with one log line;
 * kode_sys_ble_service_fault() says why, and every call below is DENIED.
 * Without CONFIG_KODEOS_BLE_CONSOLE, whose pairing it needs, it is always
 * left out.
 *
 * A value is what a read gets. A notification goes into a ring in PSRAM that
 * NimBLE's host task drains, and shares the link with the console. The set
 * and the notify are commands to AO_Radio (above, beside the scan response's
 * name). A write arrives as a copy in BLE_APP_WRITE_SIG, and each edge of the
 * link as BLE_APP_LINK_SIG, to the AOs subscribed with
 * kode_sys_ble_subscribe().
 */

#define KODE_SYS_BLE_CHARS_MAX      4U     /**< Characteristics one service may have.            */
#define KODE_SYS_BLE_VALUE_MAX      244U   /**< Bytes of a value: one notification at an MTU of 247. */
#define KODE_SYS_BLE_CHAR_NAME_MAX  16U    /**< Bytes of a characteristic's name, NUL included.  */
#define KODE_SYS_BLE_FAULT_MAX      80U    /**< Bytes of the reason a service is left out, NUL included. */
#define KODE_SYS_BLE_READ           0x01U  /**< A central may read the value.                    */
#define KODE_SYS_BLE_WRITE          0x02U  /**< A central may write it.                          */
#define KODE_SYS_BLE_NOTIFY         0x04U  /**< A central may subscribe to its notifications.    */

/** @brief One characteristic of the app's service. */
typedef struct {
    const char *name;   /**< 1 to KODE_SYS_BLE_CHAR_NAME_MAX - 1 bytes, unique in the service. */
    const char *uuid;   /**< 128 bits as text, "6e400002-b5a3-f393-e0a9-e50e24dcca9e".          */
    uint8_t     props;  /**< KODE_SYS_BLE_READ, _WRITE and _NOTIFY, at least one.              */
} kode_sys_ble_char_t;

/** @brief The app's service: what KODE_SYS_BLE_SERVICE() defines. */
typedef struct {
    const char                *uuid;   /**< 128 bits as text, as a characteristic's.   */
    const kode_sys_ble_char_t *chars;  /**< Its characteristics, in table order.       */
    size_t                     count;  /**< How many; 1 to KODE_SYS_BLE_CHARS_MAX.      */
} kode_sys_ble_service_t;

/* The constant's linkage: C's, in C++ too, which the driver's reference has. */
#ifdef __cplusplus
#define KODE_SYS_BLE_LINKAGE_ extern "C"   /**< The constant's linkage in C++. */
#else
/* cppcheck-suppress misra-c2012-2.5 ; an app expands it, through KODE_SYS_BLE_SERVICE() */
#define KODE_SYS_BLE_LINKAGE_ extern       /**< The constant's linkage in C.   */
#endif

/* cppcheck-suppress misra-c2012-2.5 ; a name for the apps that declare a service, never used inside the SDK */
/**
 * @brief Declare the app's service: @p uuid_ and the array @p chars_ of
 *        kode_sys_ble_char_t, at file scope, followed by a semicolon.
 *
 * Defines the constant kode_ble_app_service, the one name the driver looks
 * for; a second service in the same app is a link error.
 */
#define KODE_SYS_BLE_SERVICE(uuid_, chars_) \
    KODE_SYS_BLE_LINKAGE_ const kode_sys_ble_service_t kode_ble_app_service; \
    const kode_sys_ble_service_t kode_ble_app_service = { \
        (uuid_), (chars_), sizeof(chars_) / sizeof((chars_)[0]) }

/* cppcheck-suppress misra-c2012-2.3 ; an event's name for the apps that read it, never used inside the SDK */
/**
 * @brief A central wrote to the app's service (BLE_APP_WRITE_SIG): @c chr, its place in
 *        the table; @c name; @c len and @c data, the bytes, NULs included.
 */
typedef BleAppWriteEvt kode_sys_ble_written_t;

/* cppcheck-suppress misra-c2012-2.3 ; an event's name for the apps that read it, never used inside the SDK */
/** @brief A central connected or left (BLE_APP_LINK_SIG): @c connected. */
typedef BleAppLinkEvt kode_sys_ble_link_t;

/**
 * @brief Subscribe @p me to the app's service: BLE_APP_WRITE_SIG and
 *        BLE_APP_LINK_SIG, and link AO_Radio.
 *
 * A bare subscription to either signal links nothing; this call is that
 * subscription with its owner linked.
 *
 * @param me The listening AO, started.
 * @return KODE_OK; KODE_DENIED when @p me is NULL or not started, or without
 *         CONFIG_KODEOS_BLE.
 */
kode_status_t kode_sys_ble_subscribe(QActive *me);

/**
 * @brief Whether the app's service is declared and left out, and why.
 * @param why Receives the reason as a sentence when this answers true; may be NULL.
 * @param cap Bytes @p why holds; KODE_SYS_BLE_FAULT_MAX holds every reason.
 * @return true when it is left out; false when it is served or not declared,
 *         and always without CONFIG_KODEOS_BLE.
 */
bool kode_sys_ble_service_fault(char *why, size_t cap);

/* ---- The battery: AO_Power's last reading ---- */

#define KODE_SYS_CHARGE_NONE  0U  /**< Not charging.                     */
#define KODE_SYS_CHARGE_PRE   1U  /**< Pre-charge, the cell is very low. */
#define KODE_SYS_CHARGE_FAST  2U  /**< Constant current or voltage.      */
#define KODE_SYS_CHARGE_DONE  3U  /**< Termination reached.              */

/** @brief AO_Power's last reading. */
typedef struct {
    uint16_t vbat_mv;   /**< The cell, mV: the gauge's when it answers, else the PMIC's in 20 mV steps. */
    uint8_t  soc_pct;   /**< The level, 0-100; only meaningful when gauge_ok.                          */
    uint8_t  charging;  /**< KODE_SYS_CHARGE_x.                                                        */
    bool     vbus;      /**< The PMIC sees a good input.                                               */
    bool     gauge_ok;  /**< The fuel gauge answered for this reading.                                 */
} kode_sys_battery_t;

/**
 * @brief The battery as AO_Power last read it.
 * @param out Receives the reading.
 * @return KODE_OK with a copy; KODE_DENIED before AO_Power's first reading,
 *         and when @p out is NULL.
 */
kode_status_t kode_sys_battery_read(kode_sys_battery_t *out);

/* ---- The expansion header J3: positions 1..14 in the connector's order ----
 * The first call below claims J3 for the app. None switches the module's
 * supply: that is kode_sys_exp_power(), a command.
 */

#define KODE_SYS_EXP_POS_MIN        1U      /**< First position, as printed on the connector.  */
#define KODE_SYS_EXP_POS_MAX        14U     /**< Last position, as printed on the connector.   */
#define KODE_SYS_EXP_PWM_HZ_MIN     1U      /**< Lowest PWM frequency, Hz.                     */
#define KODE_SYS_EXP_PWM_HZ_MAX     40000U  /**< Highest PWM frequency, Hz.                    */
#define KODE_SYS_EXP_PWM_DUTY_MAX   1000U   /**< Full duty, in tenths of a percent.            */

/** @brief What an input is pulled to, if anything. */
typedef enum {
    KODE_SYS_GPIO_PULL_NONE = 0,  /**< Leave the net to what the board put on it. */
    KODE_SYS_GPIO_PULL_UP,        /**< The internal pull-up.                      */
    KODE_SYS_GPIO_PULL_DOWN,      /**< The internal pull-down.                    */
} kode_sys_gpio_pull_t;

/** @brief What an interrupt reacts to. */
typedef enum {
    KODE_SYS_GPIO_IRQ_LOW_LEVEL = 0,  /**< While low: a request line held until served.  */
    KODE_SYS_GPIO_IRQ_ANY_EDGE,       /**< On every change: a contact.                   */
    KODE_SYS_GPIO_IRQ_HIGH_LEVEL,     /**< While high: a request line held until served. */
} kode_sys_gpio_irq_t;

/** @brief An interrupt callback. Runs in interrupt context. */
typedef void (*kode_sys_gpio_cb_t)(void *arg);

/** @brief What a position is doing. */
typedef enum {
    KODE_SYS_EXP_FREE = 0,  /**< High-Z, nobody's.                       */
    KODE_SYS_EXP_IN,        /**< Digital input, with or without a pull.  */
    KODE_SYS_EXP_OUT,       /**< Digital output.                         */
    KODE_SYS_EXP_ADC,       /**< Analog input.                           */
    KODE_SYS_EXP_PWM,       /**< PWM output.                             */
    KODE_SYS_EXP_LENT,      /**< Handed to a peripheral driver, by tag.  */
} kode_sys_exp_mode_t;

/** @brief What a position is. */
typedef struct {
    uint8_t             gpio;  /**< The GPIO behind the position.        */
    bool                adc;   /**< It has an ADC channel.               */
    bool                lp;    /**< It is an LP pin.                     */
    kode_sys_exp_mode_t mode;  /**< What it is doing.                    */
    const char         *tag;   /**< LENT: who holds it. NULL otherwise.  */
} kode_sys_exp_info_t;

/** @brief The kinds of controller an app may take for J3. */
typedef enum {
    KODE_SYS_EXP_I2C = 0,  /**< An I2C controller.   */
    KODE_SYS_EXP_SPI,      /**< A general SPI host.  */
    KODE_SYS_EXP_UART,     /**< A UART.              */
} kode_sys_exp_bus_t;

/**
 * @brief Make @p pos a digital input, pulled as @p bias says.
 * @param pos  The position.
 * @param bias The pull-up or pull-down, or none.
 * @return KODE_OK. KODE_EXISTS when something else in this image owns J3 (`ps`
 *         says who; the console's `exp` takes it for the boot). KODE_DENIED when
 *         @p pos is out of range or lent. KODE_ERROR when the pin refused.
 */
kode_status_t kode_sys_exp_input(uint8_t pos, kode_sys_gpio_pull_t bias);

/**
 * @brief Make @p pos an output driven to @p level.
 * @param pos   The position.
 * @param level The level to drive.
 * @return As kode_sys_exp_input().
 */
kode_status_t kode_sys_exp_output(uint8_t pos, bool level);

/**
 * @brief Drive @p pos, which must already be an output.
 * @param pos   The position.
 * @param level The level to drive.
 * @return KODE_OK. KODE_EXISTS as kode_sys_exp_input(). KODE_DENIED when @p pos is
 *         out of range or not an output. Otherwise what the pin answered.
 */
kode_status_t kode_sys_exp_write(uint8_t pos, bool level);

/**
 * @brief The level of an input or output position.
 * @param pos The position.
 * @return The level; false for any other position, and when J3 is not the app's.
 */
bool kode_sys_exp_level(uint8_t pos);

/**
 * @brief Read an analog position, configuring it on the first call.
 * @param pos        The position.
 * @param raw        Out: the 12-bit count.
 * @param mv         Out: millivolts.
 * @param calibrated Out: whether @p mv came through the chip's calibration.
 * @return KODE_OK. KODE_EXISTS as kode_sys_exp_input(). KODE_DENIED when @p pos has
 *         no ADC channel or is lent, or a pointer is NULL. Otherwise what the
 *         conversion answered.
 */
kode_status_t kode_sys_exp_adc(uint8_t pos, uint16_t *raw, int32_t *mv, bool *calibrated);

/**
 * @brief Make @p pos a PWM output, or retune one that already is.
 * @param pos      The position.
 * @param hz       The frequency.
 * @param duty_x10 The duty in tenths of a percent.
 * @return KODE_OK. KODE_EXISTS as kode_sys_exp_input(). KODE_DENIED for an
 *         argument out of range or a lent position. KODE_NO_RESOURCES when no
 *         channel or timer is free. KODE_ERROR when the PWM refused.
 */
kode_status_t kode_sys_exp_pwm(uint8_t pos, uint32_t hz, uint16_t duty_x10);

/**
 * @brief Attach an interrupt to a position that is already an input.
 * @param pos  The position.
 * @param kind What the interrupt reacts to.
 * @param cb   Runs in interrupt context: it may post a static event with kqp_post_from_isr() or give a semaphore, nothing else.
 * @param arg  Handed back to @p cb.
 * @return KODE_OK. KODE_EXISTS as kode_sys_exp_input(). KODE_DENIED when @p pos is
 *         not an input or @p cb is NULL.
 */
kode_status_t kode_sys_exp_irq(uint8_t pos, kode_sys_gpio_irq_t kind, kode_sys_gpio_cb_t cb,
                               void *arg);

/**
 * @brief Re-enable @p pos's interrupt once it has been served.
 * @note Does nothing when J3 is not the app's.
 * @param pos The position.
 */
void kode_sys_exp_irq_enable(uint8_t pos);

/**
 * @brief Put @p pos back to free and high-Z.
 * @param pos The position.
 * @return KODE_OK. KODE_EXISTS as kode_sys_exp_input(). KODE_DENIED when @p pos is
 *         out of range or lent.
 */
kode_status_t kode_sys_exp_release(uint8_t pos);

/**
 * @brief What @p pos is: its GPIO, capabilities, mode and, when lent, who holds it.
 * @param pos The position, 1..14.
 * @param out Filled; untouched when this does not return KODE_OK.
 * @return KODE_OK; KODE_DENIED when @p pos is not on the header or @p out is NULL;
 *         KODE_EXISTS while something else in the image owns the header.
 */
kode_status_t kode_sys_exp_info(uint8_t pos, kode_sys_exp_info_t *out);

/**
 * @brief Take positions for a peripheral you drive with ESP-IDF yourself.
 * @note The other kode_sys_exp_* calls on lent positions are refused.
 * @note A UART on lent pins loses bytes during flash writes unless the image sets CONFIG_UART_ISR_IN_IRAM.
 * @param pos  The positions.
 * @param n    How many.
 * @param tag  The module's name; a string that outlives the loan.
 * @param gpio Out: the GPIO numbers, one per position, in the same order.
 * @return KODE_OK. KODE_EXISTS as kode_sys_exp_input(). KODE_DENIED for a bad
 *         argument or a position that is not free; nothing is lent then.
 */
kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag, uint8_t *gpio);

/**
 * @brief Give back every position lent under @p tag, and every controller taken under it (kode_sys_exp_ctrl()).
 * @param tag The loan's name.
 * @return KODE_OK, also when @p tag holds nothing. KODE_EXISTS as
 *         kode_sys_exp_input(). KODE_DENIED for a NULL @p tag.
 */
kode_status_t kode_sys_exp_return(const char *tag);

/**
 * @brief Take an I2C, SPI or UART controller for a module on J3, recorded under @p tag.
 *
 * An app may take I2C1, SPI3 and UART0..4 (UART0 only while the console is not
 * a UART); the lowest free one is answered, in IDF's numbering (SPI3 is 2).
 * Books only: install IDF's driver yourself. Call it from one task.
 * @param bus KODE_SYS_EXP_I2C, KODE_SYS_EXP_SPI or KODE_SYS_EXP_UART.
 * @param tag The tag given to kode_sys_exp_lend(); a string that outlives the loan.
 * @param id  Out: the controller's number.
 * @return KODE_OK. KODE_NO_RESOURCES when every one of that kind is held.
 *         KODE_DENIED for a bad argument. KODE_EXISTS while an Active Object
 *         owns the header.
 */
kode_status_t kode_sys_exp_ctrl(kode_sys_exp_bus_t bus, const char *tag, uint8_t *id);

#ifdef __cplusplus
}
#endif

#endif /* KODE_SYS_H_ */
