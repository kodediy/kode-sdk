/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file main.c
 * @brief The target build's only source: bring the system up, nothing more.
 */
/* Every public header, so the whole surface compiles; kode_sys_internal.h refuses an app. */
#include "kode.h"
#include "kode_sys.h"
#include "kode_cert.h"
#include "kode_worker.h"
#include "kui.h"

/** @brief Set by nobody: it keeps link_standard_layer() linked and never called. */
static volatile bool s_link_standard = false;

/** @brief The asker every linked request names; never constructed nor started. */
static QActive s_link_ao;

/**
 * @brief A listener for the link check; it does nothing.
 *
 * @param e The event.
 */
static void link_listener(const kode_event_t *e)
{
    (void)e;
}

/**
 * @brief Call one function of each family the standard layer offers.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_standard_layer(void)
{
    kode_event_t ev;
    kode_buttons_t pad;

    (void)kode_error();

    kode_delay(0U);
    (void)kode_uptime_ms();
    (void)kode_time_unix();
    (void)kode_device_id();
    kode_print("kode_print %u", (unsigned)kode_random(0U, 9U));

    pad = kode_button_read_all();
    (void)pad.top;
    (void)kode_button_is_pressed("top");
    (void)kode_button_was_pressed("top");
    (void)kode_button_get_presses("top");

    (void)kode_led_set_color(kode_rgb(0U, 0U, 0U));
    (void)kode_led_set_rgb(0U, 0U, 0U);
    (void)kode_led_off();

    (void)kode_imu_read_axis("temp");
    (void)kode_imu_read();
    (void)kode_mag_read_axis("x");
    (void)kode_mag_read();
    (void)kode_mag_heading();
    (void)kode_mag_is_calibrated();
    (void)kode_mag_clear_calibration();

    (void)kode_haptic_vibrate(false);
    (void)kode_haptic_play("click");
    (void)kode_haptic_add_profile("pulse", NULL, 0U, 0U);

    (void)kode_audio_play("flash/x.wav");
    (void)kode_audio_play_start("flash/x.wav");
    (void)kode_audio_is_playing();
    (void)kode_audio_play_stop();
    (void)kode_audio_tone(KODE_AUDIO_TONE_HZ_MIN, 1U);
    (void)kode_audio_play_samples(NULL, 0U);
    (void)kode_audio_set("volume", 0U);
    (void)kode_audio_get("volume");
    (void)kode_audio_record_file("flash/x.wav", 1U);
    (void)kode_audio_record_samples(NULL, 0U, 1U, NULL);
    (void)kode_audio_record_start(1U);
    (void)kode_audio_record_status();
    (void)kode_audio_record_stop();
    (void)kode_audio_record_save("flash/x.wav");

    (void)kode_file_read("flash/x");
    (void)kode_file_read_bytes("flash/x", NULL);
    (void)kode_file_write("flash/x", "");
    (void)kode_file_append("flash/x", "");
    (void)kode_file_read_at("flash/x", 0U, NULL, 0U, NULL);
    (void)kode_file_write_bytes("flash/x", NULL, 0U);
    (void)kode_file_append_bytes("flash/x", NULL, 0U);
    (void)kode_file_status("flash/x");
    (void)kode_file_exists("flash/x");
    (void)kode_file_list("flash");
    (void)kode_file_delete("flash/x");
    (void)kode_file_create_folder("flash/x");
    (void)kode_file_rename("flash/x", "y");
    (void)kode_file_move("flash/x", "sd/y");

    (void)kode_screen_new("Title");
    (void)kode_widget_set_text(kode_screen_add_text("text"), "%s", "text");
    (void)kode_widget_set_text(kode_screen_add_value("1", "C"), "%s", "2");
    const kode_widget_t go = kode_screen_add_button("Go");

    (void)kode_widget_set_color(go, KODE_GREEN);
    (void)kode_widget_was_pressed(go);

    const kode_canvas_t canvas = kode_screen_add_canvas(10U, 10U);

    (void)kode_canvas_fill(canvas, KODE_BLACK);
    (void)kode_draw_pixel(canvas, 1, 1, KODE_RED);
    (void)kode_draw_line(canvas, 0, 0, 9, 9, KODE_BLUE);
    (void)kode_fill_rect(canvas, 2, 2, 4, 4, KODE_GREEN);
    (void)kode_draw_circle(canvas, 5, 5, 3, KODE_YELLOW);
    (void)kode_draw_text(canvas, 1, 1, "text", KODE_WHITE, KODE_ALIGN_CENTER);
    (void)kode_draw_png(canvas, 0, 0, "flash/x.png");
    (void)kode_screen_add_image("flash/x.png");
    (void)kode_mag_calibrate();

    (void)kode_event_add_listener("button_top_pressed", link_listener);
    (void)kode_event_add_listener(KODE_EVENT_IR_RECEIVED, link_listener);
    (void)kode_event_add_listener(KODE_EVENT_WIFI_CONNECTED, link_listener);
    (void)kode_event_add_listener(KODE_EVENT_WIFI_LOST, link_listener);
    (void)kode_event_remove_listener("button_top_pressed", link_listener);
    (void)kode_event_wait("button_top_pressed", 0U, &ev);

    (void)kode_wifi_connect("x", "y");
    (void)kode_wifi_is_connected();
    (void)kode_wifi_disconnect();
    {
        kode_wifi_network_t nets[1];
        (void)kode_wifi_scan(nets, 1U);
    }

    (void)kode_ble_set_name("x");
    (void)kode_ble_set_manufacturer_data(NULL, 0U);
    {
        kode_ble_device_t devs[1];
        (void)kode_ble_scan(devs, 1U, KODE_BLE_SCAN_MS_MIN);
    }
    (void)kode_ble_set_value("x", NULL, 0U);
    (void)kode_ble_notify("x", NULL, 0U);

    (void)kode_http_get("http://x");
    (void)kode_http_post("http://x", "y");
    (void)kode_http_status();
    (void)kode_net_ping("x", 1U);
    (void)kode_net_resolve("x", NULL);
    (void)kode_udp_send("x", 1U, NULL, 0U);

    (void)kode_mqtt_connect("mqtt://x");
    (void)kode_mqtt_publish("x", "y");
    (void)kode_mqtt_subscribe("x");
    (void)kode_mqtt_disconnect();

    (void)kode_battery_level();
    (void)kode_battery_is_charging();
    (void)kode_battery_voltage_mv();
    (void)kode_power_off();

    kode_exit();
}

/**
 * @brief Call the extended layer's vibration pattern, hold and stop once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_haptic_window(void)
{
    static const uint16_t seg[] = { 10U, 10U };

    (void)kode_sys_haptic_play_pattern(seg, 2U, 0U);
    (void)kode_sys_haptic_hold(0U);
    (void)kode_sys_haptic_is_holding();
    kode_sys_haptic_stop();
}

/**
 * @brief Call the extended layer's early recording end and mute once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_audio_stop_mute(void)
{
    kode_sys_audio_record_stop();
    (void)kode_sys_audio_mute(false);
}

/**
 * @brief Call each of the extended layer's motion and audio requests once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_motion_audio_requests(void)
{
    static int16_t                   frames[1];
    static const kode_sys_mag_corr_t corr;
    size_t                           taken = 0U;

    (void)kode_sys_imu_read(&s_link_ao);
    (void)kode_sys_mag_read(&s_link_ao);
    (void)kode_sys_mag_cal_get(&s_link_ao);
    (void)kode_sys_mag_cal_set(&s_link_ao, &corr);
    (void)kode_sys_mag_cal_clear(&s_link_ao);
    (void)kode_sys_audio_info_read(&s_link_ao);
    (void)kode_sys_audio_play(&s_link_ao, frames, 1U);
    (void)kode_sys_audio_record(&s_link_ao, frames, 1U, 1U);
    (void)kode_sys_audio_record_file(&s_link_ao, "/storage/x.wav", 1U);
    (void)kode_sys_audio_stream_open(&s_link_ao);
    (void)kode_sys_audio_stream_write(&s_link_ao, frames, 1U, KODE_SYS_AUDIO_CHANNELS_MONO, &taken);
    (void)kode_sys_audio_stream_close(&s_link_ao);
}

/**
 * @brief Call each of the extended layer's infrared, NFC and RFID requests,
 *        and the certification ones, once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_ir_nfc_rfid_requests(void)
{
    static const kode_sys_ir_symbol_t sym[1] = { { 560U, 560U } };
    static kode_sys_ir_symbol_t       rx[1];
    static const kode_sys_nfc_card_t  card;
    static const uint8_t              id[KODE_SYS_RFID_ID_BYTES] = { 0x12U, 0x34U, 0x00U, 0x00U, 0x00U };
    static uint16_t                   wave[1];

    (void)kode_sys_ir_send_raw(&s_link_ao, sym, 1U, 38000U);
    (void)kode_sys_ir_capture(&s_link_ao, rx, 1U, 1U);
    (void)kode_sys_ir_last(&s_link_ao);
    (void)kode_sys_nfc_read_tag(&s_link_ao, 1U);
    (void)kode_sys_nfc_ndef_read(&s_link_ao, 1U);
    (void)kode_sys_nfc_ndef_write_uri(&s_link_ao, "https://x", 1U);
    (void)kode_sys_nfc_ndef_write_text(&s_link_ao, "x", 1U);
    (void)kode_sys_nfc_emulate_start(&s_link_ao, &card);
    (void)kode_sys_nfc_emulate_stop(&s_link_ao);
    (void)kode_sys_rfid_read(&s_link_ao, 1U);
    (void)kode_sys_rfid_write(&s_link_ao, id, 1U);
    (void)kode_sys_rfid_emulate_start(&s_link_ao, id);
    (void)kode_sys_rfid_emulate_stop(&s_link_ao);
    (void)kode_cert_nfc_carrier(&s_link_ao, KODE_CERT_NFC_CARRIER_OFF);
    (void)kode_cert_nfc_field_read(&s_link_ao, 1U);
    (void)kode_cert_rfid_carrier(&s_link_ao, KODE_CERT_RFID_CARRIER_OFF);
    (void)kode_cert_rfid_field_read(&s_link_ao, wave, 1U, 1U);
}

/**
 * @brief Call each of the extended layer's file, card and network requests once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_file_net_requests(void)
{
    static uint8_t buf[1];

    (void)kode_sys_file_status(&s_link_ao, "/storage/x");
    (void)kode_sys_file_list(&s_link_ao, "/storage", 0U);
    (void)kode_sys_file_create_folder(&s_link_ao, "/storage/d");
    (void)kode_sys_file_delete(&s_link_ao, "/storage/x");
    (void)kode_sys_file_rename(&s_link_ao, "/storage/x", "/storage/y");
    (void)kode_sys_file_read(&s_link_ao, "/storage/x", 0U, buf, 1U);
    (void)kode_sys_file_write(&s_link_ao, "/storage/x", buf, 1U, false);
    (void)kode_sys_net_lookup(&s_link_ao, "example.com");
    (void)kode_sys_net_ping(&s_link_ao, "example.com", 1U);
    (void)kode_sys_http_get(&s_link_ao, "http://example.com/", buf, 1U);
    (void)kode_sys_http_post(&s_link_ao, "http://example.com/", buf, 1U, NULL, buf, 1U);
    (void)kode_sys_udp_send(&s_link_ao, "example.com", 9U, buf, 1U);
}

/**
 * @brief Call the extended layer's Wi-Fi connect, status, scan and disconnect once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_wifi(void)
{
    kode_sys_wifi_status_t st;

    (void)kode_sys_wifi_connect("x", "y");
    (void)kode_sys_wifi_status(&st);
    (void)kode_sys_wifi_scan(&s_link_ao);
    (void)kode_sys_wifi_disconnect();
}

/**
 * @brief Call the extended layer's Bluetooth name, data, listen, switch and service doors once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_ble(void)
{
    (void)kode_sys_ble_set_name("x");
    (void)kode_sys_ble_set_data(NULL, 0U);
    (void)kode_sys_ble_scan(&s_link_ao, KODE_SYS_BLE_SCAN_MS_MIN);
    (void)kode_sys_ble_enabled();
    (void)kode_sys_ble_set_value("x", NULL, 0U);
    (void)kode_sys_ble_notify("x", NULL, 0U);
    (void)kode_sys_ble_subscribe(&s_link_ao);
    {
        char why[KODE_SYS_BLE_FAULT_MAX];
        (void)kode_sys_ble_service_fault(why, sizeof(why));
    }
}

/**
 * @brief Reference the MQTT calls and the status, so both variants link them.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_mqtt(void)
{
    kode_sys_mqtt_status_t st;

    (void)kode_sys_mqtt_connect("x", NULL, NULL);
    (void)kode_sys_mqtt_publish("x", "y", 1U);
    (void)kode_sys_mqtt_status(&st);
}

/**
 * @brief Reference the battery reading and the sleep door, so both variants link them.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_battery(void)
{
    kode_sys_battery_t r;

    (void)kode_sys_battery_read(&r);
    (void)kode_sys_power_sleep();
}

/**
 * @brief Reference the settings calls, so both variants link them.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_setting(void)
{
    (void)kode_setting_set("x", "y");
    (void)kode_setting_get("x");
}

/**
 * @brief Reference the expansion pins, J3's supply, the three buses and the header's position report, so both variants link them.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_pin(void)
{
    kode_sys_exp_info_t info;

    (void)kode_pin_write(KODE_PIN_FIRST, true);
    (void)kode_pin_read(KODE_PIN_FIRST);
    (void)kode_pin_read_analog(KODE_PIN_LAST);
    (void)kode_pin_write_pwm(KODE_PIN_FIRST, KODE_PIN_PWM_HZ_MIN, 0U);
    (void)kode_pin_power_3v3(false);
    (void)kode_sys_exp_info(KODE_PIN_FIRST, &info);
    (void)kode_i2c_begin(KODE_PIN_FIRST, KODE_PIN_LAST, KODE_I2C_HZ_MIN);
    (void)kode_i2c_probe(0U);
    (void)kode_i2c_write(0U, NULL, 0U);
    (void)kode_i2c_read(0U, NULL, 0U);
    (void)kode_i2c_write_read(0U, NULL, 0U, NULL, 0U);
    (void)kode_i2c_end();
    (void)kode_spi_begin(KODE_PIN_FIRST, KODE_PIN_FIRST + 1U, KODE_PIN_FIRST + 2U,
                         KODE_PIN_FIRST + 3U, KODE_SPI_HZ_MAX, KODE_SPI_MODE_MAX);
    (void)kode_spi_transfer(NULL, NULL, 0U);
    (void)kode_spi_end();
    (void)kode_uart_begin(KODE_PIN_FIRST, KODE_PIN_LAST, KODE_UART_BAUD_MIN);
    (void)kode_uart_write(NULL, 0U);
    (void)kode_uart_read(NULL, 0U, 0U);
    (void)kode_uart_end();
}

/**
 * @brief Call the extended layer's NFC emulation state once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_nfc_kind_emulating(void)
{
    (void)kode_sys_nfc_is_emulating();
}

/**
 * @brief Call the standard layer's infrared once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_infrared(void)
{
    static const uint16_t us[] = { 560U, 560U };

    (void)kode_ir_send("nec", 0U, 0U);
    (void)kode_ir_receive(0U);
    (void)kode_ir_last();
    (void)kode_ir_receive_raw(0U);
    (void)kode_ir_send_raw(us, 2U, 1U);
    (void)kode_ir_capture_start("flash/ir.txt");
    (void)kode_ir_capture_stop();
    (void)kode_ir_send_file("flash/ir.txt");
}

/**
 * @brief Call the standard layer's NFC once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_nfc(void)
{
    kode_tag_t tag;

    (void)kode_nfc_scan_start();
    (void)kode_nfc_scan_stop();
    tag = kode_nfc_read_id(0U);
    (void)tag.ok;
    (void)kode_nfc_read("text", 0U);
    (void)kode_nfc_write("uri", "https://kode.diy", 0U);
    (void)kode_nfc_add_profile("door", "uid", "04:A2:3B:91");
    (void)kode_nfc_emulate_start("door");
    (void)kode_nfc_emulate_stop();
}

/**
 * @brief Call the standard layer's RFID once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_rfid(void)
{
    kode_tag_t tag;

    tag = kode_rfid_read_id(0U);
    (void)tag.ok;
    (void)kode_rfid_write("01:02:03:04:05", 0U);
    (void)kode_rfid_emulate_start("01:02:03:04:05");
    (void)kode_rfid_emulate_stop();
}

/**
 * @brief Call the extended layer's RFID emulation state once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_rfid_emulation(void)
{
    (void)kode_sys_rfid_is_emulating();
}

/**
 * @brief Call each of the extended layer's commands not linked elsewhere once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_sys_commands(void)
{
    (void)kode_sys_led_set(false);
    (void)kode_sys_led_toggle();
    (void)kode_sys_led_set_rgb(0U, 0U, 0U);
    (void)kode_sys_ir_send_nec(0U, 0U);
    (void)kode_sys_audio_tone(KODE_SYS_AUDIO_HZ_MIN, 1U, 1U);
    (void)kode_sys_audio_volume(0U);
    (void)kode_sys_audio_mic_gain(0U);
    (void)kode_sys_haptic_play_buzz(1U, 0U);
    (void)kode_sys_flash("fs:x.bin", "app", false);
    (void)kode_sys_mqtt_disconnect();
    (void)kode_sys_mqtt_subscribe("x");
    (void)kode_sys_mqtt_unsubscribe("x");
    (void)kode_sys_power_boost(false);
    (void)kode_sys_power_charge(true);
    (void)kode_sys_power_charge_limit(0U);
    (void)kode_sys_exp_power(false);
    (void)kode_sys_home_defer();
    (void)kode_sys_home_now();
    (void)kode_sys_exit_to_os();
}

/**
 * @brief An interrupt callback for the link check; it does nothing.
 *
 * @param arg Unused.
 */
static void link_irq(void *arg)
{
    (void)arg;
}

/**
 * @brief Call each of the extended layer's immediate calls not linked elsewhere once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_sys_immediate(void)
{
    static const uint8_t pos[1] = { KODE_SYS_EXP_POS_MIN };
    int64_t              unix_s = 0;
    char                 out[KODE_SYS_NVS_VALUE_MAX];
    uint16_t             raw = 0U;
    int32_t              mv = 0;
    bool                 cal = false;
    uint8_t              gpio[1];
    uint8_t              id = 0U;

    (void)kode_sys_time_ms();
    (void)kode_sys_device_id();
    (void)kode_sys_clock_read(&unix_s);
    (void)kode_sys_button_read_mask();
    (void)kode_sys_button_read_presses(KODE_SYS_BUTTON_TOP);
    (void)kode_sys_nvs_get("x", out, sizeof(out));
    (void)kode_sys_mqtt_is_connected();
    (void)kode_sys_touch_subscribe(&s_link_ao);
    (void)kode_sys_console_congested();
    (void)kode_sys_exp_input(KODE_SYS_EXP_POS_MIN, KODE_SYS_GPIO_PULL_NONE);
    (void)kode_sys_exp_output(KODE_SYS_EXP_POS_MIN, false);
    (void)kode_sys_exp_write(KODE_SYS_EXP_POS_MIN, false);
    (void)kode_sys_exp_level(KODE_SYS_EXP_POS_MIN);
    (void)kode_sys_exp_adc(KODE_SYS_EXP_POS_MIN, &raw, &mv, &cal);
    (void)kode_sys_exp_pwm(KODE_SYS_EXP_POS_MIN, KODE_SYS_EXP_PWM_HZ_MIN, 0U);
    (void)kode_sys_exp_irq(KODE_SYS_EXP_POS_MIN, KODE_SYS_GPIO_IRQ_ANY_EDGE, link_irq, NULL);
    kode_sys_exp_irq_enable(KODE_SYS_EXP_POS_MIN);
    (void)kode_sys_exp_release(KODE_SYS_EXP_POS_MIN);
    (void)kode_sys_exp_lend(pos, 1U, "x", gpio);
    (void)kode_sys_exp_ctrl(KODE_SYS_EXP_I2C, "x", &id);
    (void)kode_sys_exp_return("x");
}

/**
 * @brief A worker job for the link check; it does nothing.
 *
 * @param arg   Unused.
 * @param value Unused.
 * @return KODE_OK.
 */
static kode_status_t link_job(void *arg, uint32_t *value)
{
    (void)arg;
    (void)value;
    return KODE_OK;
}

/**
 * @brief Call the console tables' registrations and answers, and the worker, once.
 *
 * Never reached at run time; see s_link_standard.
 */
static void link_sys_tables(void)
{
    static kode_worker_t          worker;
    static const KodeConfigReqEvt req;
    char                          quoted[FUNCTION_QUOTED_MAX(1U)];

    (void)kode_variables_add(NULL, 0U);
    (void)kode_functions_add(NULL, 0U);
    (void)kode_config_add(&s_link_ao, KODE_CONFIG_MARGIN, NULL, 0U);
    (void)kode_config_post(NULL, &s_link_ao);
    (void)function_record(function_ticket(), "x");
    (void)function_end(0U, KODE_OK, NULL);
    (void)function_quote(quoted, sizeof(quoted), "x");
    (void)kode_config_answer(&s_link_ao, &req, KODE_OK, false, NULL);
    (void)kode_worker_start(&worker, &s_link_ao, "link", 0U, 0U, 0U);
    (void)kode_worker_submit(&worker, link_job, NULL, 0, NULL);
}

/**
 * @brief ESP-IDF's entry point.
 */
void app_main(void)
{
    kode_init();
    lv_init();
    if ( s_link_standard )
    {
        link_standard_layer();
        link_haptic_window();
        link_audio_stop_mute();
        link_motion_audio_requests();
        link_ir_nfc_rfid_requests();
        link_file_net_requests();
        link_wifi();
        link_ble();
        link_mqtt();
        link_battery();
        link_setting();
        link_pin();
        link_nfc_kind_emulating();
        link_infrared();
        link_nfc();
        link_rfid();
        link_rfid_emulation();
        link_sys_commands();
        link_sys_immediate();
        link_sys_tables();
    }
}
