This guide is for apps written against the pre-release versions of the SDK, before it was public. An app that starts on the first public release, `v0.1.0`, already uses the v4 API and needs none of it.

# Moving an app from v3 to v4

v4 is the SDK from `v0.1.0` on (`KODE_VERSION_MAJOR` 4). v3 is the pre-release SDK (`KODE_VERSION_MAJOR` 3). Every v3 app needs source changes.

v4 has two layers:

- **`kode.h`, the standard layer.** Blocking calls with plain types, for `app_main()` and the app's own tasks. A call returns once its work is done. It is refused in an Active Object and in an interrupt, and `kode_error()` says why.
- **`kode_sys.h`, the extended layer.** Nothing blocks. It replaces v3's `kode.h` together with `kode_ao.h`. It has three kinds of call:
  - **commands** post to the device's owner;
  - **requests** take the asking Active Object, `me`, and answer it later by event;
  - **immediate calls** read a copy, a flag or a pin, or subscribe the calling Active Object.

## First steps

1. Pin `version: v0.1.0` for `kodediy/kode` in `main/idf_component.yml`. Pin the same tag for `kodediy/kscreen` if the app names it.
2. Delete `sdkconfig` and `build/`. A cached value beats a new default, so an old `sdkconfig` keeps v3's `KODEOS_CHANNEL=y` (see [Linking and the build options](#linking-by-use-and-the-build-options)).
3. Choose the header for each file:
   - a file that builds an Active Object includes `kode_sys.h` in place of `kode_ao.h`;
   - a task that only waits for its work includes `kode.h`.
4. Stop including `kaos.h`. It holds the system's own Active Objects and is not part of an app's API: `kode_sys.h` is the door to them. [The kaos requests a v3 Active Object used](#the-kaos-requests-a-v3-active-object-used) maps each one.
5. Stop relying on what the v3 headers brought in. `kode_sys.h` speaks its own types (see [Types and constants](#types-and-constants)). [What the headers include](#what-the-headers-include) says where each item comes from now.

## What the headers include

v3's `kode.h` included the log, the console tables, the categories and the driver headers whose types its calls took. v4's `kode.h` includes only `kode_colors.h`, `kode_defs.h`, `kode_init.h` and `kapp_desc.h`. v3's `kode_ao.h` added `kqp.h` and `kaos.h`.

| What v3 brought in | Through | Where it comes from in v4 |
|---|---|---|
| `kode_defs.h` (`kode_status_t`) | `kode.h` | `kode.h` and `kode_sys.h` |
| `kapp_desc.h` (`KODE_APP_MANIFEST`) | `kode.h` | `kode.h` and `kode_sys.h` |
| `kode_init()` | `kode.h` | `kode_init.h`, which `kode.h` and `kode_sys.h` include |
| `kode_log.h` (`KLOG_*`) | `kode.h` | `kode_sys.h`. A file that includes only `kode.h` and logs includes `kode_log.h` itself |
| `kode_variables.h`, `kode_functions.h`, `kode_config.h` (the console tables) | `kode.h` | `kode_sys.h`, or each header by name |
| `kapp_cat.h` | `kode.h` | `kode_sys.h`, or `kapp_cat.h` by name |
| `kdrv_buttons.h`, `kdrv_ir.h`, `kdrv_es8311.h`, `kes_fields.h`, `kdrv_qmi8658.h`, `knfc_tag.h`, `knfc_field.h`, `knfc_emulate.h`, `krfid_wave.h`, `khal_exp.h` | `kode.h` | nowhere: their types are `kode_sys_*` types in `kode_sys.h`, and `kode.h` takes plain types |
| `kqp.h` (QP/C, the signals, the pools, `KODE_PRIO_*`) | `kode_ao.h` | `kode_sys.h` |
| `kaos.h` | `kode_ao.h` | nowhere: an app does not include it |
| `kode_worker.h` | by name | by name, from `kode_sys` |

## Renamed calls

The call does what it did in v3, under its new name. The **Change** column lists the rest.

| v3 | v4 | Change |
|---|---|---|
| `kode_time_ms()` | `kode_sys_time_ms()` | |
| `kode_device_id()` | `kode_sys_device_id()` | `kode.h` keeps `kode_device_id()` |
| `kode_buttons_read()` | `kode_sys_button_read_mask()` | bits are `KODE_SYS_BUTTON_x` |
| `kode_led_set()` | `kode_sys_led_set()` | posted to `AO_LedMgr`, not published |
| `kode_led_toggle()` | `kode_sys_led_toggle()` | posted, not published |
| `kode_led_rgb()` | `kode_sys_led_set_rgb()` | posted, not published |
| `kode_ir_send_nec()` | `kode_sys_ir_send_nec()` | |
| `kode_audio_tone()` | `kode_sys_audio_tone()` | |
| `kode_audio_volume()` | `kode_sys_audio_volume()` | |
| `kode_audio_mic_gain()` | `kode_sys_audio_mic_gain()` | |
| `kode_haptic_play()` | `kode_sys_haptic_play_buzz()` | |
| `kode_flash()` | `kode_sys_flash()` | `KODE_DENIED`, not `KODE_ERROR`, for arguments that do not parse and for a source over 256 characters |
| `kode_mqtt_connect()` | `kode_sys_mqtt_connect()` | see [Behaviour changes](#behaviour-changes) |
| `kode_mqtt_connected()` | `kode_sys_mqtt_is_connected()` | |
| `kode_mqtt_disconnect()`, `_publish()`, `_subscribe()`, `_unsubscribe()` | `kode_sys_mqtt_disconnect()`, `_publish()`, `_subscribe()`, `_unsubscribe()` | |
| `kode_power_sleep()` | `kode_sys_power_sleep()` | returns `kode_status_t`; it returned nothing. It asks `AO_LowPower` for light sleep, and every owner may refuse; `KODE_DENIED` without `CONFIG_KODE_LOWPOWER`. It no longer opens the battery path: ship mode is the console's `ship_mode` alone |
| `kode_power_off()` | `kode_sys_power_off()`, or `kode.h`'s `kode_power_off()`, which blocks | returns `kode_status_t`; it returned nothing. It asks `AO_LowPower` for deep sleep, as `kode_sys_power_sleep()` asks for light sleep |
| `kode_power_boost()`, `_charge()`, `_charge_limit()` | `kode_sys_power_boost()`, `_charge()`, `_charge_limit()` | return `kode_status_t`; they returned nothing |
| `kode_exit_to_os()` | `kode_sys_exit_to_os()` | returns `kode_status_t`, and returns after posting: an AO runs on until the reset. `kode.h`'s `kode_exit()` never returns unless refused |
| `kode_home_defer()` | `kode_sys_home_defer()` | |
| `kode_home_now()` | `kode_sys_home_now()` | returns `kode_status_t`; it returned nothing |
| `kode_nvs_get()` | `kode_sys_nvs_get()` | |
| `kode_exp_input()`, `_output()`, `_write()`, `_level()`, `_adc()`, `_pwm()`, `_irq()`, `_irq_enable()`, `_release()`, `_lend()`, `_return()`, `_power()` | `kode_sys_exp_*()`, the same suffixes | `kode_sys_` types for the pull, the interrupt kind and the callback |
| `kode_exp_ctrl()` | `kode_sys_exp_ctrl()` | takes `kode_sys_exp_bus_t` |
| `kode_worker_start()`, `kode_worker_submit()` | unchanged | in `kode_sys`'s `kode_worker.h`; the job and its end are `kode_worker_fn_t` and `kode_worker_done_t` |
| `kode_init()` | unchanged | declared in `kode_init.h`, which both headers include. A second call, from any task, waits for the first and returns |

## Calls that now take `me` and answer by event

In v3 these calls blocked: until the owner answered, or, for the network rows, while `kdrv_net` ran in the caller's task with no owner. In v4, an AO calls the request with `me`, returns at once, and handles the answer event; the network requests go to `AO_Net`. A task calls the `kode.h` call in the last column.

| v3 | v4 request | Answer signal (type) | From a task (`kode.h`) |
|---|---|---|---|
| `kode_audio_info(out)` | `kode_sys_audio_info_read(me)` | `AUDIO_INFO_RSP_SIG` (`kode_sys_audio_info_answer_t`) | `kode_audio_get()` |
| `kode_audio_play(frames, n)` | `kode_sys_audio_play(me, frames, n)` | `AUDIO_DONE_SIG` (`kode_sys_audio_answer_t`) | `kode_audio_play_samples()` |
| `kode_audio_record(out, cap, &n, ms)` | `kode_sys_audio_record(me, buf, cap, ms)` | `AUDIO_REC_RSP_SIG` (`kode_sys_audio_answer_t`, `frames`) | `kode_audio_record_start()` |
| `kode_audio_record_file(path, ms, &bytes)` | `kode_sys_audio_record_file(me, path, ms)` | `AUDIO_FILE_RSP_SIG` (`kode_sys_audio_file_answer_t`) | `kode_audio_record_file()` |
| `kode_audio_stream_open(rate, ch)` | `kode_sys_audio_stream_open(me)` | `AUDIO_STREAM_OPEN_RSP_SIG` (`kode_sys_audio_answer_t`) | `kode_audio_play_start()` (a file) |
| `kode_audio_stream_write(frames, n, ms, &taken)` | `kode_sys_audio_stream_write(me, frames, n, channels, &taken)`, an immediate call | none: it returns at once, `KODE_TIMEOUT` when the ring took fewer | none |
| `kode_audio_stream_close()` | `kode_sys_audio_stream_close(me)` | `AUDIO_STREAM_CLOSE_RSP_SIG` (`kode_sys_audio_answer_t`) | `kode_audio_play_stop()` |
| `kode_imu_read(out)` | `kode_sys_imu_read(me)` | `IMU_SAMPLE_RSP_SIG` (`kode_sys_imu_answer_t`) | `kode_imu_read_axis()`, `kode_imu_read()` |
| `kode_mag_read(mg)` | `kode_sys_mag_read(me)` | `MAG_SAMPLE_RSP_SIG` (`kode_sys_mag_answer_t`) | `kode_mag_read_axis()`, `kode_mag_read()` |
| `kode_ir_send_raw(sym, n, hz)` | `kode_sys_ir_send_raw(me, sym, n, hz)` | `IR_SEND_RAW_RSP_SIG` (`kode_sys_ir_send_answer_t`) | `kode_ir_send_raw()` (durations in µs) |
| `kode_ir_capture(out, cap, &n, ms)` | `kode_sys_ir_capture(me, out, cap, ms)` | `IR_CAPTURE_RSP_SIG` (`kode_sys_ir_capture_answer_t`) | `kode_ir_receive_raw()`, `kode_ir_receive()` |
| `kode_ir_last(out)` | `kode_sys_ir_last(me)` | `IR_LAST_RSP_SIG` (`kode_sys_ir_last_answer_t`) | `kode_ir_last()` |
| `kode_nfc_read(out, ms)` | `kode_sys_nfc_read_tag(me, ms)` | `NFC_READ_RSP_SIG` (`kode_sys_nfc_read_answer_t`) | `kode_nfc_read_id()` |
| `kode_nfc_ndef_read(out, cap, &n, ms)` | `kode_sys_nfc_ndef_read(me, ms)` | `NFC_NDEF_RSP_SIG` (`kode_sys_nfc_ndef_answer_t`) | `kode_nfc_read()` |
| `kode_nfc_ndef_write_uri(uri, ms)`, `_text(text, ms)` | `kode_sys_nfc_ndef_write_uri(me, uri, ms)`, `_text(me, text, ms)` | `NFC_NDEF_RSP_SIG` (`kode_sys_nfc_ndef_answer_t`) | `kode_nfc_write()` |
| `kode_nfc_emulate_start(card)` | `kode_sys_nfc_emulate_start(me, card)` | `NFC_EMU_RSP_SIG` (`kode_sys_nfc_emu_answer_t`) | `kode_nfc_emulate_start()` |
| `kode_nfc_emulate_stop(&reads)` | `kode_sys_nfc_emulate_stop(me)` | `NFC_EMU_RSP_SIG` (`kode_sys_nfc_emu_answer_t`, `reads`) | `kode_nfc_emulate_stop()` |
| `kode_rfid_read(id, &rf_div, ms)` | `kode_sys_rfid_read(me, ms)` | `RFID_OP_RSP_SIG` (`kode_sys_rfid_answer_t`) | `kode_rfid_read_id()` |
| `kode_fs_stat(path, out)` | `kode_sys_file_status(me, path)` | `FILE_STATUS_RSP_SIG` (`kode_sys_file_answer_t`) | `kode_file_status()`, `kode_file_exists()` |
| `kode_fs_list(path, cb, ctx)` | `kode_sys_file_list(me, path, skip)` | `STORAGE_ENTRY_SIG` per entry (`kode_sys_file_entry_t`), then `FILE_LIST_END_SIG` (`kode_sys_file_answer_t`) | `kode_file_list()` |
| `kode_fs_mkdir(path)` | `kode_sys_file_create_folder(me, path)` | `FILE_FOLDER_RSP_SIG` (`kode_sys_file_answer_t`) | `kode_file_create_folder()` |
| `kode_fs_remove(path)` | `kode_sys_file_delete(me, path)` | `FILE_DELETE_RSP_SIG` (`kode_sys_file_answer_t`) | `kode_file_delete()` |
| `kode_fs_rename(from, to)` | `kode_sys_file_rename(me, from, to)` | `FILE_RENAME_RSP_SIG` (`kode_sys_file_answer_t`) | `kode_file_move()`, `kode_file_rename()` |
| `kode_fs_read(path, buf, cap, &got)` | `kode_sys_file_read(me, path, offset, buf, cap)` | `FILE_READ_RSP_SIG` (`kode_sys_file_answer_t`, `got`) | `kode_file_read()`, `kode_file_read_bytes()` |
| `kode_fs_write(path, data, len, append)` | `kode_sys_file_write(me, path, data, len, append)` | `FILE_WRITE_RSP_SIG` (`kode_sys_file_answer_t`) | `kode_file_write()`, `kode_file_append()` |
| `kode_net_lookup(host, &ip4)` | `kode_sys_net_lookup(me, host)` | `NET_LOOKUP_RSP_SIG` (`kode_sys_net_answer_t`, `stats.ip4`) | `kode_net_resolve()` |
| `kode_net_ping(host, count, &stats)` | `kode_sys_net_ping(me, host, count)` | `NET_PING_RSP_SIG` (`kode_sys_net_answer_t`) | `kode_net_ping()` |
| `kode_http_get(url, buf, cap, &out)` | `kode_sys_http_get(me, url, buf, cap)` | `NET_HTTP_RSP_SIG` (`kode_sys_http_answer_t`) | `kode_http_get()`, `kode_http_status()` |
| `kode_udp_send(host, port, data, len)` | `kode_sys_udp_send(me, host, port, data, len)` | `NET_UDP_RSP_SIG` (`kode_sys_udp_answer_t`) | `kode_udp_send()` |
| `kode_nfc_carrier(mode)` | `kode_cert_nfc_carrier(me, mode)` | `NFC_CERT_RSP_SIG` (`kode_cert_carrier_answer_t`) | none |
| `kode_nfc_field_read(out, ms)` | `kode_cert_nfc_field_read(me, ms)` | `NFC_FIELD_RSP_SIG` (`kode_cert_nfc_field_answer_t`) | none |
| `kode_rfid_carrier(mode)` | `kode_cert_rfid_carrier(me, mode)` | `RFID_CERT_RSP_SIG` (`kode_cert_carrier_answer_t`) | none |
| `kode_rfid_field_read(out, wave, cap, &n, ms)` | `kode_cert_rfid_field_read(me, wave, cap, ms)` | `RFID_FIELD_RSP_SIG` (`kode_cert_rfid_field_answer_t`) | none |

The four `kode_cert_*` calls are in `kode_cert.h`, which `kode_sys.h` does not include. The two carriers still need `CONFIG_KODEOS_NFC_CERT`; the two field reads are in every build, as in v3. Their carrier modes are `KODE_CERT_NFC_CARRIER_*` and `KODE_CERT_RFID_CARRIER_*`, in the types `kode_cert_nfc_carrier_mode_t` and `kode_cert_rfid_carrier_mode_t`.

Some arguments changed:

- `kode_sys_audio_stream_open(me)` takes no rate. The stream runs at `KODE_SYS_AUDIO_RATE_HZ`.
- `kode_sys_audio_stream_write(me, frames, n, channels, &taken)` takes the channels and no `ms`, and only the AO that opened the stream may write. It never waits: when the ring takes fewer frames it returns `KODE_TIMEOUT`, with `taken` set. v3 waited up to `ms` for room.
- `kode_sys_file_read()` takes an offset. A bigger file is read in windows of up to `KODE_SYS_FILE_READ_MAX` (32768) bytes.
- `kode_sys_rfid_read()` takes no `rf_div` pointer; the divider is the answer's `rf_div`.
- `kode_sys_udp_send()` takes a `uint16_t` length.

### The request rules

- A request returns `KODE_OK` when it was taken: its answer will come.
- It returns `KODE_DENIED`, and posts nothing, in four cases:
  - `me` is NULL;
  - an argument is bad;
  - `me` already has a request of that kind waiting for its answer;
  - the build has no such owner.
- It returns `KODE_NO_RESOURCES`, and posts nothing, when the owner's queue is within its margin or the event pool is dry.
- Each asker has one door per kind of request. A second request of the same kind is refused until its answer. The door opens again just before the answer is posted, so the answer's handler may ask again.
- A request taken is answered exactly once. The answer is posted to `me`, never published, with the final status in `rc`. The owner's refusals (the device busy, absent or failed) arrive in the answer.
- An answer is posted with no margin, and an answer that finds `me`'s queue full is QP's assert. So `me`'s queue must hold, all at the same time:
  - one answer per open door;
  - `me`'s own events;
  - during a listing, its entries.

  `kode_sys.h` gives the rule and a worked example.
- A listing posts entries until only `KODE_SYS_LIST_FREE_SLOTS` (8) slots are free in `me`'s queue, and then its end. An entry that would take one of those slots ends the page early, with `more` set. A page that has no room even for its first entry ends at once with `KODE_NO_RESOURCES`.
- Never subscribe to an answer signal. The owners also publish or post those signals for the console and for kodeOS, so a subscriber would receive answers that are not its own.
- A buffer marked LENT on the call stays the caller's. Keep it valid and unchanged from the call until the answer. The owner never writes it after the answer.

## The kaos requests a v3 Active Object used

v3's `kode_ao.h` included `kaos.h`, and so did v3's `kscreen.h`, through `kode_ao.h`. The v3 examples wrote their Active Objects against its `*_request()` calls. In v4 an app does not include `kaos.h`: it is the system's own, and `kode_sys.h` is the door. v4's `kscreen.h` includes `kode_sys.h`.

| v3 (`kaos.h`) | v3 answer | v4 (`kode_sys.h`) | v4 answer |
|---|---|---|---|
| `audio_tone_request(hz, ms, amp)` | none | `kode_sys_audio_tone(hz, ms, amp)` | none: a command |
| `audio_record_file_request(path, ms, …)` | `AUDIO_FILE_RSP_SIG`, published | `kode_sys_audio_record_file(me, path, ms)` | `AUDIO_FILE_RSP_SIG`, posted to `me` |
| `haptic_play_request(ms, gain)` | `HAPTIC_DONE_SIG`, published | `kode_sys_haptic_play_buzz(ms, gain)` | `HAPTIC_DONE_SIG`, published |
| `imu_read_request(…)` | the caller's storage | `kode_sys_imu_read(me)` | `IMU_SAMPLE_RSP_SIG` |
| `mag_read_request(…)` | the caller's storage | `kode_sys_mag_read(me)` | `MAG_SAMPLE_RSP_SIG` |
| `ir_send_request(addr, cmd)` | none | `kode_sys_ir_send_nec(addr, cmd)` | none: a command |
| `ir_capture_request(…)` | the caller's storage; `IR_RX_SIG`, published | `kode_sys_ir_capture(me, out, cap, ms)`, or `kode_sys_ir_last(me)` for the last frame | `IR_CAPTURE_RSP_SIG`, `IR_LAST_RSP_SIG` |
| `radio_up_request()` | `RADIO_STATE_SIG`, published | none: an infrared request brings the link up itself, and `kode_sys_wifi_connect()` joins a network | |
| `wifi_state_request()` | `WIFI_STATE_SIG`, published | `kode_sys_wifi_status(&status)`, an immediate read | none |
| `mqtt_up_request(uri, user, pass)` | `MQTT_STATE_SIG`, published | `kode_sys_mqtt_connect(uri, user, pass)` | `MQTT_STATE_SIG`, published |
| `mqtt_sub_request(topic)` | `MQTT_MSG_SIG`, published | `kode_sys_mqtt_subscribe(topic)` | `MQTT_MSG_SIG`, published |
| `mqtt_pub_request(topic, data, len)` | none | `kode_sys_mqtt_publish(topic, data, len)` | none: a command |
| `net_ping_request(host, count)` | `NET_PING_ENTRY_SIG`, `NET_PING_END_SIG`, published | `kode_sys_net_ping(me, host, count)` | `NET_PING_RSP_SIG` |
| `net_http_get_request(url)` | `NET_HTTP_DATA_SIG`, `NET_HTTP_DONE_SIG`, published | `kode_sys_http_get(me, url, buf, cap)` | `NET_HTTP_RSP_SIG`, the body in `buf` |
| `net_udp_send_request(host, port, data, len)` | `NET_UDP_DONE_SIG`, published | `kode_sys_udp_send(me, host, port, data, len)` | `NET_UDP_RSP_SIG` |
| `nfc_info_request(who)` | `NFC_INFO_RSP_SIG` | none: v4 gives an app no read of the reader's status. With no reader fitted, a tag read or an NDEF request is answered `KODE_IO_ERROR`, and an emulation start `KODE_ERROR` | |
| `nfc_read_request(…)` | `NFC_TAG_SIG`, published on a hit, nothing on an empty window | `kode_sys_nfc_read_tag(me, ms)` | `NFC_READ_RSP_SIG`, `KODE_TIMEOUT` on an empty window |
| `nfc_ndef_request(who, text, cap, write, uri, ms)` | `NFC_NDEF_RSP_SIG` | `kode_sys_nfc_ndef_read(me, ms)`, `kode_sys_nfc_ndef_write_uri(me, uri, ms)`, `kode_sys_nfc_ndef_write_text(me, text, ms)` | `NFC_NDEF_RSP_SIG` |
| `rfid_read_request(…)`, `rfid_read_console_request(who, ms)` | the caller's storage; `RFID_OP_RSP_SIG` | `kode_sys_rfid_read(me, ms)` | `RFID_OP_RSP_SIG` |
| `storage_path_request(who, op, path)` | `STORAGE_PATH_RSP_SIG` | `kode_sys_file_status(me, path)`, `kode_sys_file_create_folder(me, path)`, `kode_sys_file_delete(me, path)` | `FILE_STATUS_RSP_SIG`, `FILE_FOLDER_RSP_SIG`, `FILE_DELETE_RSP_SIG` |
| `storage_path_request(who, STORAGE_PATH_LIST, path)` | `STORAGE_ENTRY_SIG` per entry, then `STORAGE_LIST_END_SIG` | `kode_sys_file_list(me, path, skip)`, a page at a time | `STORAGE_ENTRY_SIG` per entry, then `FILE_LIST_END_SIG` with `count` and `more`, posted to `me` |
| `storage_cat_request(who, path, max)` | `STORAGE_LINE_SIG` per line | `kode_sys_file_read(me, path, offset, buf, cap)` | `FILE_READ_RSP_SIG`, the bytes in `buf` |
| `storage_text_request(who, path, text, append, 0)` | `STORAGE_PATH_RSP_SIG` | `kode_sys_file_write(me, path, data, len, append)` | `FILE_WRITE_RSP_SIG` |
| `storage_text_request(who, path, text, append, line)`, `line` above 0 | `STORAGE_PATH_RSP_SIG` | none: v4 writes a whole file or appends to it. To replace one line, read the file, change the line and write the file back | |
| `power_sleep_request()` | `POWER_SLEEP_SIG`, published | `kode_sys_power_sleep()`, which goes to `AO_LowPower` and never to ship mode | `LOWPOWER_STATE_SIG`, published |
| `display_state_request()` | `DISPLAY_STATE_SIG`, published | none: `kscreen_start()` asks `AO_Display`, and the owner hands the `DISPLAY_STATE_SIG` it receives to `kscreen_open()` | `DISPLAY_STATE_SIG`, published |
| `display_brightness_request(pct, persist)` | none | `kode_sys_screen_brightness(pct)`, a command, for as long as the app runs: 0, or `KODE_SYS_SCREEN_BRIGHT_MIN` to `_MAX`. `persist` is gone: v4 never stores an app's brightness, and the user's own setting comes back when the app ends | none |
| `touch_subscribe(who)` | `TOUCH_EVT_SIG`, published | `kode_sys_touch_subscribe(me)`, an immediate call that links `AO_Touch`. `kscreen` subscribes its own owner | `TOUCH_EVT_SIG`, published |
| `console_congested()` | none: it returns the answer | `kode_sys_console_congested()`, an immediate call | none |
| `storage_request(medium, STORAGE_OP_MOUNT)` | none: the log says how the mount went | `kode_sys_file_status(me, "/sdcard")`, or `"/storage"` for the internal FAT. Every file request mounts its volume itself | `FILE_STATUS_RSP_SIG`: `KODE_ERROR` when the volume will not mount (no card) |
| `storage_request(medium, STORAGE_OP_INFO)` | `STORAGE_INFO_SIG`, published | none: v4 gives an app no read of a volume's size. `kode_sys_file_status(me, "/sdcard")` says whether the card mounts | |
| `storage_request(medium, STORAGE_OP_LIST)` | `STORAGE_ENTRY_SIG` per entry, then `STORAGE_LIST_END_SIG`, published | `kode_sys_file_list(me, "/sdcard", skip)`, a page at a time | `STORAGE_ENTRY_SIG` per entry, then `FILE_LIST_END_SIG`, posted to `me` |
| `storage_request(medium, op)`, the other ops (`UNMOUNT`, `SELFTEST`, `VERIFY`) | | none: they are the console's and kodeOS's | |
| `appdata_request()`, `appdata_rm_request()` | `APPDATA_FIELDS_SIG`, `APPDATA_ICON_SIG`, `APPDATA_END_SIG`; `APPDATA_RM_RSP_SIG` | none: an app's metadata folder is the installer's and the console's | |

The signals v3 published as answers, such as `NET_PING_END_SIG` and `STORAGE_PATH_RSP_SIG`, still exist for the console and kodeOS. An AO that subscribes to one hears the answers to other askers. Handle the answer posted to `me` instead. The state and news signals (`BUTTON_EVT_SIG`, `MQTT_MSG_SIG`, `WIFI_STATE_SIG`, `POWER_EVT_SIG`, `HAPTIC_DONE_SIG` and the rest) are still published, and subscribing to them is right, once a call links their owner.

## Calls removed

| v3 | Replacement |
|---|---|
| `kode_delay_ms()` | An AO arms a `QTimeEvt`. A task calls `kode_delay()` |
| `kode_nvs_set()` | `kode_setting_set()` in `kode.h`, from a task. No AO door |
| `kode_nvs_del()` | none |
| `kode_sd_lend()`, `kode_sd_return()`, `kode_sd_read_sectors()`, `kode_sd_write_sectors()` | none |
| `kode_rfid_clone(id, ms)` | `kode_sys_rfid_write(me, id, ms)`, answered by `RFID_OP_RSP_SIG`. A task calls `kode_rfid_write()` |
| `kode_fs_entry_cb` | the listing's `STORAGE_ENTRY_SIG` events |
| `kode_ao.h` | `kode_sys.h` |
| a play tagged through `kaos.h`, and its published `AUDIO_DONE_SIG` | `kode_sys_audio_play(me, …)`, whose answer is posted to `me`. `AudioDoneEvt` has no `gen` field, and `AUDIO_DONE_SIG` is a posted signal, above `MAX_PUB_SIG`: subscribing to it is QP's assert |

The function and setting answer calls (`function_ticket()`, `function_record()`, `function_end()`, `function_quote()`, `FUNCTION_QUOTED_MAX` and `kode_config_answer()`) moved from `kaos.h` to `kode_functions.h` and `kode_config.h`. `kode_sys.h` includes both.

### v3 names that `kode.h` uses for something else

`kode.h` reuses these v3 function names, with new arguments: `kode_audio_play`, `kode_audio_record_file`, `kode_audio_tone`, `kode_haptic_play`, `kode_http_get`, `kode_imu_read`, `kode_mag_read`, `kode_ir_last`, `kode_ir_send_raw`, `kode_mqtt_connect`, `kode_mqtt_publish`, `kode_net_ping`, `kode_nfc_read`, `kode_nfc_emulate_start` and `kode_nfc_emulate_stop`. The compiler refuses a v3 call to any of them. `kode.h` also defines `KODE_NFC_TEXT_MAX` again, as 97: the bytes of an NFC text or link, NUL included, as `kode.h`'s tag reads and writes and its emulation profiles take it. v3's `KODE_NFC_TEXT_MAX` (96) is `KODE_SYS_NFC_TEXT_MAX` in v4.

**Three calls compile unchanged and mean the opposite.** `kode_mqtt_disconnect()`, `kode_mqtt_subscribe()` and `kode_udp_send()` take the v3 arguments. They return `bool`, and `true` means done. In v3 they returned `kode_status_t`, where `KODE_OK` is 0. So `if (kode_udp_send(…) != KODE_OK)` now treats a success as a failure. Rename each call to its `kode_sys_` form, or rewrite the test.

## Types and constants

| v3 | v4 |
|---|---|
| `kode_fs_stat_t` | `kode_sys_file_status_t` |
| `kode_http_result_t` | `kode_sys_http_result_t` |
| `kode_ping_stats_t` | `kode_sys_ping_stats_t` |
| `kdrv_es8311_info_t` | `kode_sys_audio_info_t` |
| `kdrv_qmi8658_sample_t` | `kode_sys_imu_sample_t` |
| `kdrv_ir_symbol_t`, `kdrv_ir_frame_t` | `kode_sys_ir_symbol_t`, `kode_sys_ir_frame_t` |
| `knfc_tag_t`, `knfc_emu_card_t` | `kode_sys_nfc_tag_t`, `kode_sys_nfc_card_t` |
| `knfc_field_t`, `krfid_field_t` | the `NFC_FIELD_RSP_SIG` and `RFID_FIELD_RSP_SIG` answers |
| `khal_gpio_pull_t`, `khal_gpio_irq_t`, `khal_gpio_cb_t` | `kode_sys_gpio_pull_t`, `kode_sys_gpio_irq_t`, `kode_sys_gpio_cb_t` |
| `KHAL_GPIO_*` | `KODE_SYS_GPIO_*` |
| `kbsp_exp_bus_t`, `KHAL_EXP_*` | `kode_sys_exp_bus_t`, `KODE_SYS_EXP_*` |
| `kdrv_button_t`, `KDRV_BTN_UP` … `KDRV_BTN_BOT`, `KDRV_BTN_COUNT` | `KODE_SYS_BUTTON_UP` … `KODE_SYS_BUTTON_BOT`, `KODE_SYS_BUTTON_COUNT` |
| `kode_nfc_carrier_mode_t`, `kode_rfid_carrier_mode_t` | `kode_cert_nfc_carrier_mode_t`, `kode_cert_rfid_carrier_mode_t` |
| `KODE_NFC_CARRIER_*`, `KODE_RFID_CARRIER_*` | `KODE_CERT_NFC_CARRIER_*`, `KODE_CERT_RFID_CARRIER_*` |
| `KODE_AUDIO_RATE_HZ`, `KODE_AUDIO_BUF_SECONDS`, `KODE_AUDIO_MAX_FRAMES`, `KODE_AUDIO_STREAM_FRAMES` | `KODE_SYS_AUDIO_RATE_HZ`, `KODE_SYS_AUDIO_BUF_SECONDS`, `KODE_SYS_AUDIO_MAX_FRAMES`, `KODE_SYS_AUDIO_STREAM_FRAMES` |
| `KODE_BUTTONS_SETTLE_MS` | `KODE_SYS_BUTTON_SETTLE_MS` |
| `KODE_FS_LIST_MAX` | `KODE_SYS_FILE_LIST_MAX` (24) |
| `KODE_FS_READ_MAX` (4096) | `KODE_SYS_FILE_READ_MAX` (32768) |
| `KODE_HAPTIC_MAX_MS` | `KODE_SYS_HAPTIC_MAX_MS` |
| `KODE_HOME_DEFER_MAX_MS`, `KODE_HOME_GRACE_MS` | `KODE_SYS_HOME_DEFER_MAX_MS`, `KODE_SYS_HOME_GRACE_MS` |
| `KODE_IR_RAW_MAX_SYMBOLS` | `KODE_SYS_IR_RAW_MAX_SYMBOLS` (256) |
| `KODE_NFC_TEXT_MAX` | `KODE_SYS_NFC_TEXT_MAX` (96, NUL included) |
| `KQP_NFC_UID_MAX`, `KNFC_UID_MAX` | `KODE_SYS_NFC_UID_MAX` (10), new in v4 |
| `knfc_tag_type_t`: `KNFC_TAG_UNKNOWN`, `_T2T`, `_T4T`, `_MIFARE` | `KODE_SYS_NFC_TYPE_UNKNOWN`, `_T2T`, `_T4T`, `_MIFARE`, new in v4: the values of a tag's `type` |
| an NDEF answer's `KODE_ERROR`, `KODE_DENIED`, `KODE_NO_RESOURCES`, `KODE_TIMEOUT` with `have_tag` | `KODE_SYS_NFC_NDEF_NO_MESSAGE`, `_LOCKED`, `_TOO_LONG`, `_LOST`, new in v4: the same values, by name |
| `KODE_NVS_KEY_MAX`, `KODE_NVS_VALUE_MAX` | `KODE_SYS_NVS_KEY_MAX`, `KODE_SYS_NVS_VALUE_MAX` |

Status codes, `KODE_APP_MANIFEST` and the console-table macros keep their names.

## Behaviour changes

**Commands.**
- A command posts with a margin and returns the truth:
  - `KODE_OK` when the owner's queue took it;
  - `KODE_NO_RESOURCES` when the event pool is dry or the owner's queue is within its margin.

  In v3 a full queue was QP's assert. A loop of commands now sees refusals. Handle them.
- The margin is `KODE_SYS_CMD_MARGIN` (4) slots. AO_Net's is 16 of its 24 slots, so that a loop of MQTT commands leaves inbound MQTT room.
- The haptic plays, the audio settings and the commands that return nothing keep one request pending at most. A call made while one is pending adds nothing.
- In v3 these commands dropped their event in silence on a dry pool. They now return `KODE_NO_RESOURCES`:
  - `kode_sys_ir_send_nec()` and `kode_sys_audio_tone()`;
  - `kode_sys_audio_volume()` and `kode_sys_audio_mic_gain()`;
  - `kode_sys_flash()`;
  - the MQTT commands, which returned `KODE_OK` whatever happened;
  - `kode_sys_power_boost()`, `_charge()` and `_charge_limit()`, which returned nothing.

  `kode_sys_exp_power()` posts a static event, which a dry pool cannot refuse; for it only the full queue changed. `kode_sys_power_sleep()` and `kode_sys_power_off()` take an event from the pool and post it to `AO_LowPower` with its own margin, so both refusals apply to them.
- The LED commands are posted to `AO_LedMgr`, so another AO subscribed to `LED_SET_SIG`, `LED_TOGGLE_SIG` or `LED_RGB_SIG` no longer hears an app's LED calls.

**Bad arguments.**
- A bad argument is `KODE_DENIED`, never `KODE_ERROR`. This changed for `kode_sys_flash()` and `kode_sys_nvs_get()`.
- A broker, user, password or topic that does not fit is refused, never cut. The limits are `KODE_SYS_MQTT_URI_MAX`, `_USER_MAX`, `_PASS_MAX` and `_TOPIC_MAX`, NUL included. A payload is still cut to `KODE_MQTT_PAYLOAD_MAX` (240) bytes.
- An NFC message holds at most 95 characters (`KODE_SYS_NFC_TEXT_MAX` − 1). v3 cut a longer one to 95; v4 refuses it. `kode.h` has the same limit, `KODE_NFC_TAG_DATA_MAX` (95).
- A network request's URL is shorter than `KODE_SYS_NET_URL_MAX` (80, NUL included) and a host shorter than `KODE_SYS_NET_HOST_MAX` (64). A longer one is refused.
- `kode_sys_audio_record_file()` refuses an empty path at once.

**Answers.**
- `AO_Net` has one worker. A network request that finds it busy is answered `KODE_DENIED`: ask again.
- `kode_sys_audio_stream_close()` is answered `KODE_DENIED` while no stream is open, while another AO holds the stream, and while the open's own answer has not arrived.
- `kode_cert_nfc_carrier()` is answered `KODE_DENIED`, in every mode, while another NFC request holds the reader. Its mode, and `kode_cert_rfid_carrier()`'s, is an `int32_t`.

**Infrared.**
- The infrared block is on the C5, so infrared needs `AO_Radio`: an app that uses infrared links the radio.
- A request that finds the C5 link down is held while `AO_Ir` has `AO_Radio` bring the link up. It keeps its own budget:
  - a raw send, its start;
  - a capture, its listening time;
  - anything else, about 10 s.

  Past that budget it is answered `KODE_TIMEOUT`, unsent. Once `AO_Radio` says the link will not come, every infrared request is answered `KODE_DENIED` at once, until the link is up again.
- A capture displaces any earlier capture, whoever asked for it: the earlier one is answered `KODE_DENIED`.

**The standard layer.**
- The first `kode.h` call that needs the layer's own AO starts it at `KODE_PRIO_USER_GUI` (3): an event call, `kode_mqtt_subscribe()`, or a screen call. An app's own AO cannot then take level 3.
- In `kode.h`, three events need a constant to link their owner:
  - `ir_received`: `KODE_EVENT_IR_RECEIVED`;
  - `wifi_connected`: `KODE_EVENT_WIFI_CONNECTED`;
  - `wifi_lost`: `KODE_EVENT_WIFI_LOST`.

  Typed as a string in an app that links no such owner, the event is refused.

## Linking by use and the build options

- **Linking by use.** An app links an owner by calling one of its calls, directly or through a module that it uses. `kode_init()` then starts it. The owner is not linked when the app only subscribes to one of its signals or publishes one: v3 code that only subscribed to `IR_RX_SIG`, `WIFI_STATE_SIG` and the like now hears nothing. Call one of the owner's commands or requests. Touch has no command for an app: `kode_sys_touch_subscribe(me)` is the subscription to `TOUCH_EVT_SIG` that links `AO_Touch`. `kode_sys_wifi_status()` reads the radio's state and links nothing; `kode_sys_wifi_connect()` links `AO_Radio`.
- **The core.** `AO_Log`, `AO_Boot`, `AO_Power`, `AO_IoExp` and `AO_Buttons` always start. With `KODEOS_CONSOLE` (on by default), so do `AO_Console` and `AO_Autotest`.
- **Parks.** An owner that the app does not link has its part parked: the part is put to rest at boot and its resources stay claimed. `ps` shows `parked(AO_<name>)`.
- **Dependencies.** Linking one owner can link others:
  - sound and infrared link `AO_Radio`, because the speaker amplifier's enable and the infrared block are on the C5;
  - `AO_Radio` links `AO_Storage`, which it asks to release the card around a C5 reset, and `AO_Net` in a build with `KODEOS_NET`.
  - sound links `AO_Storage` as well, through its record-to-file door, which writes the recording with the storage owner's whole-buffer write;
  - `AO_AppMgr` links `AO_Storage` and `AO_Net`, which carry the images it writes;
  - haptic links `AO_Imu`, which captures and drains the motion that the haptic row is judged by.
- **The C5 without `AO_Radio`.** An app that does not link `AO_Radio` (every app built with `KODEOS_RADIO=n`) holds the C5 in reset from boot. The exception is such an app with sound: the C5 is reset for 10 ms and left running, so that its firmware raises the amplifier's enable. A sound in the first 1.5 s after that reset is silent.
- **The C5 for the app.** `KODEOS_C5_RUN=y` (off by default) keeps the C5 running in an app without `AO_Radio`, for an app that talks to it by its own road, such as its UART. The core resets it with the same 10 ms pulse and leaves it running, and `ps` shows the claim as `running for the app`. The C5 then draws current from boot whether the app talks to it or not, and nothing in the SDK talks to it.
- **The radio without sound.** An app that links the radio, plays no sound and never brings the link up keeps the C5 and the amplifier powered. `KODEOS_C5_HOLD=y` avoids it.
- **The OS role.** In the OS role (`KODEOS_BOOT_ROLE_OS`), `kode_init()` names every start strongly. A module that the build compiles is then always linked.
- **The Kode channel.** `KODEOS_CHANNEL` is now off by default in the app role. It is still on in the OS role. In v3 it was on in every role.
  - Delete `sdkconfig` after the bump, or the cached `y` stays and keeps `AO_Net` and `AO_Radio` in the app.
  - An app that wants the channel sets `CONFIG_KODEOS_CHANNEL=y`.

## Also new in v4

These came late in v3's life and touch an app only where it uses them. The last v3 versions have the first four too.

- **The kit.** `kui.h` adds a level (`kui_level_create()`, `kui_level_set_colour()`), a colour swatch (`kui_swatch_create()`) and a lamp (`kui_lamp_create()`, `kui_lamp_set()`), in the kit's own styles and tokens.
- **App metadata in a container.** A `.kfw` container for an app may carry the app's `app.json` and a 64x64 icon, behind header flag bit 0 (`KFW_FLAG_APPDATA`). The install writes the app's folder on the internal FAT (`/storage/.kode/appsdata/<project>/`) before it ends. A container without the block deletes the folder, and a bare image leaves it as it is. `app_erase` deletes the folder too.
- **When an install ends.** The install's done, its launch and the console's `>ok` come only after that folder is written, or after `APPMGR_TIDY_MS` (5 s) without an answer from `AO_Storage`. A folder that cannot be written is one warning, and the install still succeeds.
- **Terminal installs of containers.** `install src="terminal"` of a container now counts the bytes on the wire against the size the console announced. Before, the transfer closed at the image's length, early for a container.
- **Installs from a URL.** `kode_sys_flash()` takes an `http://` or `https://` URL of up to 256 characters and refuses a longer source. The log names the download by the URL's last path segment and never prints its query.

## Added in this release

Every item below is new, and nothing above changes for it: an app written for v4 builds unchanged, unless it registers settings of its own (**Settings' owners**, below). One promise of v4 is withdrawn: an app no longer starts with J3's module switched off, so an app that needs it off asks with `kode_pin_power_3v3(false)` (see [The board: things that surprise](README.md#the-board-things-that-surprise)). The README's sections [The expansion header J3](README.md#the-expansion-header-j3), [Touch, brightness and local time](README.md#touch-brightness-and-local-time), [Scans](README.md#scans) and [Bluetooth for your app](README.md#bluetooth-for-your-app) say how they work together.

**`kode_event_t` gains two fields at its end, `int16_t x` and `int16_t y`.** They are the point of a touch event, in glass pixels; every other event carries 0 and 0. An initialiser that names its fields, or lists only the first four, compiles as before.

**`kode.h`, the standard layer.**
- The console: `kode_print()`, one printf-style line as `!N <ms> app <text>`, shown whatever `config key="log"` filters unless the log is off, cut at `KODE_PRINT_MAX`.
- Chance: `kode_random(min, max)`, from the hardware random generator, `min` to `max` both included.
- The screen: `kode_widget_set_color(widget, colour)` paints a button's fill, a text's letters or a value's number, until the next `kode_screen_new()`.
- Time: `kode_time_t` and `kode_time_local()`.
- LED: `kode_led_toggle()`.
- Wi-Fi: `kode_wifi_network_t`, `kode_wifi_scan()`, `KODE_WIFI_SCAN_MS`, `KODE_WIFI_SSID_MAX`.
- MQTT: `kode_mqtt_unsubscribe()`, `kode_mqtt_is_connected()`.
- Battery and power: `kode_power_is_usb_connected()`, `kode_power_set_charging()`, `kode_power_set_charge_current()` with `KODE_CHARGE_CURRENT_MIN_MA` and `KODE_CHARGE_CURRENT_MAX_MA`, `kode_pin_power_5v()`. None is stored.
- J3's supply: `kode_pin_power_3v3()`, `KODE_PIN_POWER_MS`.
- Buses on J3: `kode_i2c_begin()`, `kode_i2c_probe()`, `kode_i2c_write()`, `kode_i2c_read()`, `kode_i2c_write_read()`, `kode_i2c_end()`; `kode_spi_begin()`, `kode_spi_transfer()`, `kode_spi_end()`; `kode_uart_begin()`, `kode_uart_write()`, `kode_uart_read()`, `kode_uart_end()`; the tags `KODE_BUS_TAG_I2C`, `_SPI`, `_UART`, and the limits `KODE_I2C_HZ_MIN`, `_HZ_MAX`, `_TIMEOUT_MS`, `_ADDR_MAX`, `KODE_SPI_HZ_MIN`, `_HZ_MAX`, `_MODE_MAX`, `_XFER_MAX`, `KODE_UART_BAUD_MIN`, `_BAUD_MAX`, `_RX_BYTES`.
- Brightness: `kode_display_set_brightness()`, `KODE_DISPLAY_BRIGHTNESS_MIN`, `KODE_DISPLAY_BRIGHTNESS_MAX`. It is in the `kode` component: an app that sets it needs no `kode_screen`.
- Touch: `kode_touch_read()`, and the constants `KODE_EVENT_TOUCH_PRESSED`, `_MOVED` and `_RELEASED`, which link `AO_Touch`.
- Bluetooth: `kode_ble_set_name()`, `kode_ble_set_manufacturer_data()`, `kode_ble_device_t`, `kode_ble_scan()`; the service: `kode_ble_characteristic_t`, `kode_ble_service_t`, `KODE_BLE_SERVICE()`, the properties `KODE_BLE_READ`, `_WRITE`, `_NOTIFY`, `kode_ble_set_value()`, `kode_ble_notify()`; the constants `KODE_EVENT_BLE_CONNECTED`, `_DISCONNECTED` and `_WRITTEN`, which link `AO_Radio`; the limits `KODE_BLE_NAME_MAX`, `_ADDR_MAX`, `_SCAN_RSP_MAX`, `_DATA_MIN`, `_SCAN_MS_MIN`, `_SCAN_MS_MAX`, `_CHARS_MAX`, `_VALUE_MAX`, `_CHAR_NAME_MAX`.
- Events: `touch_pressed`, `touch_moved` and `touch_released`, with the point in `x` and `y`; `pin_<n>_changed` for `n` from 1 to 14, its value the new level, with `KODE_PIN_SETTLE_MS`; `ble_connected`, `ble_disconnected`, and `ble_written`, whose source is the characteristic, whose value is the number of bytes and whose text is the bytes.
- `kode_touch_read()` and `kode_pin_power_3v3()` start the standard layer's Active Object at `KODE_PRIO_USER_GUI` (3), as an event call does.

**`kode_sys.h`, the extended layer.**
- Commands: `kode_sys_screen_brightness()` with `KODE_SYS_SCREEN_BRIGHT_MIN` and `_MAX`; `kode_sys_ble_set_name()`, `kode_sys_ble_set_data()`, `kode_sys_ble_set_value()`, `kode_sys_ble_notify()`.
- Requests: `kode_sys_wifi_scan(me)`, answered by `WIFI_SCAN_ENTRY_SIG` (`kode_sys_wifi_scan_entry_t`) for each network, then `WIFI_SCAN_RSP_SIG` (`kode_sys_wifi_scan_answer_t`); `kode_sys_ble_scan(me, ms)`, answered by `BLE_SCAN_ENTRY_SIG` (`kode_sys_ble_scan_entry_t`), then `BLE_SCAN_RSP_SIG` (`kode_sys_ble_scan_answer_t`). Each answer carries `rc`: `KODE_OK`, `KODE_NO_RESOURCES` for a radio that is busy (ask again), `KODE_ERROR` for a scan that failed, and for a Bluetooth listen `KODE_DENIED` while Bluetooth is off in Settings. A Wi-Fi scan and a Bluetooth listen take turns: one refuses the other.
- Immediate calls: `kode_sys_clock_local()` with `kode_sys_time_t`; `kode_sys_ble_enabled()`; `kode_sys_ble_service_fault()`; `kode_sys_ble_subscribe(me)`, the subscription to `BLE_APP_WRITE_SIG` (`kode_sys_ble_written_t`) and `BLE_APP_LINK_SIG` (`kode_sys_ble_link_t`) that links `AO_Radio`.
- The service: `kode_sys_ble_char_t`, `kode_sys_ble_service_t`, `KODE_SYS_BLE_SERVICE()`, `KODE_SYS_BLE_READ`, `_WRITE`, `_NOTIFY`, and the limits `KODE_SYS_BLE_NAME_MAX`, `_SCAN_RSP_MAX`, `_DATA_MIN`, `_SCAN_MAX`, `_SCAN_MS_MIN`, `_SCAN_MS_MAX`, `_ADDR_MAX`, `_CHARS_MAX`, `_VALUE_MAX`, `_CHAR_NAME_MAX`, `_FAULT_MAX`.
- Types for the scans: `kode_sys_wifi_ap_t` and `KODE_SYS_WIFI_SCAN_MAX`; `kode_sys_ble_device_t`.
- J3's supply: `kode_sys_exp_power()` announces its end. `AO_Radio` publishes `EXP_POWER_SIG` (`kode_sys_exp_power_done_t`) with `on` and `rc`: `KODE_OK` when the supply is as asked, `KODE_IO_ERROR` when the C5 refused, `KODE_DENIED` when the link would not come up or went down, and the next link-up switches it. With no link, an "off" brings the link up first, and the C5 stays up for the rest of the boot.
- Power: `KODE_SYS_CHARGE_LIMIT_MIN_MA`, `KODE_SYS_CHARGE_LIMIT_MAX_MA` and `KODE_SYS_BOOST_MIN_MV`: the limits of the fast-charge current and of the cell under the boost.

**Flashing.** In an app, `idf.py flash` and `idf.py app-flash` write the app to the mailbox and nothing else: kbsp's `project_include.cmake` leaves those targets one image, at `app_pending`'s offset. Before, they wrote a bootloader, the partition table, a blank otadata and the app over kodeOS's factory slot. kodeOS's own build is the OS role and flashes as before.

**The mailbox.** It is 6 MB at `0x1A00000`, not 4 MB at `0x1C00000`, and the `apps` region ends at `0x1800000`, where the FAT now starts. An app up to 6 MB less 4 KB goes through the mailbox. Write it with `esptool write-flash 0x1A00000 app.bin`; `idf.py flash` reads the address from the table. A board with the old table needs one full cable flash of kodeOS first.

**Settings.** The zone of the local time is the system's: `config key="zone"` exists in every image, owned by `AO_Boot`, which applies it at boot, before the code of the app runs. kodeOS and every app read the same zone.

**Settings' owners.** A table of settings names the AO that answers it. `KODE_CONFIG_ADD(t)` is now `KODE_CONFIG_ADD(owner, margin, t)`, and `kode_config_add(table, n)` is `kode_config_add(owner, margin, table, n)`:
- `owner` is the AO that answers the table's keys. Register after its start: an owner with no queue is refused.
- `margin` is the slots a request leaves free in the owner's queue. For an app's own owner it is `KODE_CONFIG_MARGIN` (4). A margin the queue cannot leave is refused.

The console posts each request to the owner (`kode_config_post()`), never on the bus. When the owner's queue is within the margin, the console answers `>err code=4 msg=busy`. An owner subscribes to nothing for its settings. `KodeConfigAnsEvt` carries the setting's `key` in place of `owner`.

## Console answers that changed

- A system verb that needs an owner the app does not link answers `>err code=5 msg=absent`. Those verbs are:
  - `ls`, `cd`, `stat`, `cat`, `mkdir`, `rm`, `rmdir`, `mv`, `cp`, `write`, `file_put` and `file_get` (`AO_Storage`);
  - `appdata_ls`, `appdata_info` and `appdata_rm`;
  - `install`: `AO_AppMgr`, or `AO_Radio` for `dst=c5`;
  - `time` with `sync=on` (`AO_Net`);
  - `screenshot`: `AO_Display`, `AO_Storage` for a file, `AO_Net` for `dst=http`.
- A device's own verbs are registered by its owner's start. In an app without that owner, the verb does not exist and answers `>err code=1` (unknown verb).
- Settings are registered the same way. `config` leaves out the keys of an owner the app does not link, and `config key=` of one answers `>err code=3 msg=key`.
- `help` lists the console's own verbs first, then the owners' verbs.
- `config` lists the keys in the order their owners start: `log` (`AO_Log`) and `zone` (`AO_Boot`) first, `led_activity`, `charge` and `charge_ma` (`AO_Power`) last. `AO_Imu` adds `mag_cal`, the compass calibration.
