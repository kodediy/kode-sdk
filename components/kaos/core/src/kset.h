/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kset.h
 * @brief The settings store's names, shared by the AOs that own its keys.
 */
#ifndef KSET_H_
#define KSET_H_

/* Each key is read and written only by its owner AO: that keeps khal_nvs's one owner per record
 * without a lock. */
#define KSET_NAMESPACE   "kset"

#define KSET_KEY_BRIGHT  "bright"   /**< u8, 5..100. Owner: AO_Display.   */
#define KSET_KEY_WIFI    "wifi"     /**< u8 as bool. Owner: AO_Radio.     */
#define KSET_KEY_BLE     "ble"      /**< u8 as bool. Owner: AO_Radio.     */
#define KSET_KEY_LOG     "log"      /**< u8, a kode_log_lvl_t: the console's
                                  *   global log level. Absent = the
                                  *   build's own (CONFIG_KLOG_DEFAULT_LEVEL).
                                  *   Owner: AO_Log.                   */
#define KSET_KEY_VOL     "vol"      /**< u8, 0..100: the speaker's level.
                                  *   Owner: AO_Audio.                 */
#define KSET_KEY_MIC_DB  "mic_db"   /**< u8, 0..42 in KSET_MIC_DB_STEP: the
                                  *   microphone's gain. Owner: AO_Audio. */
#define KSET_KEY_MUTE    "mute"     /**< u8 as bool: the speaker muted.
                                  *   Owner: AO_Audio.                 */
#define KSET_KEY_HAPTIC  "haptic"   /**< u8, 0..100: the scale on every play
                                  *   an app asks for. Owner: AO_Haptic. */
#define KSET_KEY_LED_ACT "led_activity" /**< u8 as bool: the LED pulses while a
                                  *   flash writes. Owner: AO_Power.   */
#define KSET_KEY_CHARGE  "charge"   /**< u8 as bool: the cell charges.
                                  *   Owner: AO_Power.                 */
#define KSET_KEY_CHG_MA  "charge_ma" /**< blob, a uint16_t in mA: the charge current.
                                  *   Owner: AO_Power.                 */
#define KSET_KEY_WJOIN   "wifi_join" /**< u8 as bool: join a remembered
                                  *   network when the link comes up.
                                  *   Owner: AO_Radio.                 */
#define KSET_KEY_WIFI_PS "wifi_ps"  /**< u8 as bool: the station's power
                                  *   save. Owner: AO_Radio.           */
#define KSET_KEY_CHAN    "chan"     /**< u8 as bool: join the Kode channel
                                  *   whenever there is Wi-Fi. Owner: AO_Net. */
#define KSET_KEY_MAG_CAL "mag_cal"  /**< string, "ox oy oz sx sy sz": the
                                  *   magnetometer's calibration. Absent = none.
                                  *   Owner: AO_Imu.                   */
#define KSET_KEY_ZONE    "zone"     /**< string, a POSIX TZ string of 1 to
                                  *   KSET_ZONE_MAX printable ASCII
                                  *   characters: the zone local time is
                                  *   read in. Absent = KSET_ZONE_DEF.
                                  *   Owner: AO_Boot.                  */
#define KSET_KEY_CHAN_KEEP "chan_keep" /**< blob, a uint32_t in seconds: how long the
                                  *   broker keeps the channel's session while
                                  *   the Kode is away. Owner: AO_Net.  */
#define KSET_KEY_SLEEP_AFTER "sleep_after" /**< u32, seconds: the idle time before
                                  *   ASLEEP. Owner: AO_LowPower.      */
#define KSET_KEY_OFF_AFTER "off_after" /**< u32, seconds: the time asleep before
                                  *   OFF. Owner: AO_LowPower.         */
#define KSET_KEY_LP_LAST "lp_last"  /**< blob, a lowpower_last_t: the summary of the
                                  *   last rest, written at most once a
                                  *   minute asleep and once when it ends.
                                  *   Owner: AO_LowPower.              */

/* The words of the two low-power settings and the seconds each stands for, in
 * the same order. Both AO_LowPower (which stores the seconds) and the SDK's
 * rows (which list the words) read them, so they are spelled once. A stored
 * 0 is "never". The lists stay within KODE_VALUE_ENUMS_MAX words. */
#define KSET_S_MIN       60U        /**< Seconds in a minute.                */
#define KSET_S_HOUR      3600U      /**< Seconds in an hour.                 */
#define KSET_S_DAY       86400U     /**< Seconds in a day.                   */
#define KSET_S_NEVER     0U         /**< The stored value of "never".        */
#define KSET_S_FOREVER   0xFFFFFFFFU /**< The stored value of "never" for `chan_keep`. */
/** The idle times `sleep_after` offers. */
#define KSET_SLEEP_AFTER_WORDS { "30s", "1m", "2m", "5m", "10m", "never" }
/** The seconds of each, 2 min being KODE_LOWPOWER_SLEEP_AFTER_DEF_S. */
#define KSET_SLEEP_AFTER_SECS  { 30U, KSET_S_MIN, 2U * KSET_S_MIN, \
                                 5U * KSET_S_MIN, 10U * KSET_S_MIN, KSET_S_NEVER }
/** The times asleep `off_after` offers. */
#define KSET_OFF_AFTER_WORDS   { "20m", "1h", "6h", "24h", "7d", "never" }
/** The seconds of each, 20 min being KODE_LOWPOWER_OFF_AFTER_DEF_S. */
#define KSET_OFF_AFTER_SECS    { 20U * KSET_S_MIN, KSET_S_HOUR, 6U * KSET_S_HOUR, \
                                 KSET_S_DAY, 7U * KSET_S_DAY, KSET_S_NEVER }

/** The ages `chan_keep` offers. */
#define KSET_CHAN_KEEP_WORDS   { "1h", "6h", "1d", "3d", "7d", "never" }
/** The seconds of each, 1 day being KSET_CHAN_KEEP_DEF_S; "never" is the
 *  MQTT 5 value for no expiry at all (KDRV_MQTT_EXPIRY_NEVER). */
#define KSET_CHAN_KEEP_SECS    { KSET_S_HOUR, 6U * KSET_S_HOUR, KSET_S_DAY, \
                                 3U * KSET_S_DAY, 7U * KSET_S_DAY, \
                                 KSET_S_FOREVER }
#define KSET_CHAN_KEEP_DEF_S  KSET_S_DAY /**< `chan_keep` when nothing is stored. */

/** On this AMOLED 0 is black even when on; a settings screen must not be
 *  able to turn off the only thing that can turn it back on. */
#define KSET_BRIGHT_MIN  5U
#define KSET_BRIGHT_DEF  100U       /**< The absent-store default: today's boot. */

#define KSET_PCT_MAX     100U       /**< The top of every per-cent setting.  */
#define KSET_MIC_DB_MAX  42         /**< The ES8311's top microphone gain.   */
#define KSET_MIC_DB_STEP 6          /**< The microphone gain's step, in dB.  */
#define KSET_CHG_MA_MIN  64         /**< The charger's smallest non-zero
                                     *   current; 0 is `charge=off`.       */
#define KSET_ZONE_MAX           32U    /**< Characters of a zone, no NUL. */
#define KSET_ZONE_DEF           "UTC0" /**< UTC, the board's own time. */
#define KSET_ZONE_NAME_MIN      3U     /**< Fewest letters in a name. */
#define KSET_ZONE_HOUR_MAX      24U    /**< Most hours in a UTC offset. */
#define KSET_ZONE_RULE_HOUR_MAX 167U   /**< Most hours in a rule's time. */
#define KSET_ZONE_MINSEC_MAX    59U    /**< Most minutes or seconds. */
#define KSET_ZONE_DAY_MAX       365U   /**< Last day of the year a rule has. */
#define KSET_ZONE_MONTH_MAX     12U    /**< Last month of an `M` rule. */
#define KSET_ZONE_WEEK_MAX      5U     /**< Last week of `M`: 5 is last. */
#define KSET_ZONE_WEEKDAY_MAX   6U     /**< Last weekday of `M`: 0 is Sunday. */
#define KSET_ZONE_RADIX         10U    /**< Base of a zone's numbers. */

#endif /* KSET_H_ */
