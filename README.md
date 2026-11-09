<div align="center" markdown="1">

<img src="components/kui/sim/device/kode-dot.png" alt="The Kode Dot" width="200">

# kode-sdk

**The SDK for the Kode Dot: write an app in C, and run it on the board**

[![ESP-IDF v6.1](https://img.shields.io/badge/ESP--IDF-v6.1-E7352C?logo=espressif&logoColor=white)](https://github.com/espressif/esp-idf)
![Chip: ESP32-P4](https://img.shields.io/badge/chip-ESP32--P4-1f6feb)
![Language: C](https://img.shields.io/badge/language-C-555555?logo=c&logoColor=white)
![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-2ea44f)

[Quick start](#quick-start) ·
[The examples](#the-examples) ·
[How an app works](#how-an-app-works) ·
[Use it from your project](#use-the-sdk-from-your-own-project) ·
[The board](#the-board-things-that-surprise)

</div>

---

This is the SDK for the **Kode Dot**. SDK means software development kit: the code and the tools that you use to write a program for the board. A program for the board is a firmware, and it is stored in the flash memory of the board. A firmware for the Kode Dot is written with this SDK: an app, and also kodeOS itself. kodeOS is the operating system of the board. It shows the launcher, the screen that lists the apps, and it installs the apps. Read this file if you write an app for the Kode Dot, or if you add code to the SDK.

The Kode Dot is a small computer that you hold in your hand. Its processor is the ESP32-P4. It has an AMOLED touch screen, six buttons, an RGB LED, a battery with its charger and a microSD slot. It also has sound, motion sensors, NFC, RFID and infrared. An ESP32-C5 gives it Wi-Fi and Bluetooth.

The six buttons are on the front, under the screen. The pad is the black cross on the left: up, down, left and right. The top button is the blue round button on the right. The bottom button is the red round button, lower on the board.

This SDK holds five things:

- a **driver** for each chip: the part of the software that talks to that chip;
- an **Active Object** for each part of the board: the part of the software that owns the part and decides when it is used;
- QP/C, the library that runs the Active Objects;
- `kode.h`, the **standard layer**: calls that wait until their work is done, for `app_main()` and the tasks of your app;
- `kode_sys.h`, the **extended layer**: calls that never wait, for an Active Object of your own.

This is a complete app. It turns the LED green, waits one second and then returns to kodeOS:

```c
#include "kode.h"

void app_main(void)
{
    kode_init();                    /* start the board and every owner the app uses */
    kode_led_set_rgb(0, 255, 0);    /* ask the owner of the LED for green */
    kode_delay(1000);
    kode_exit();                    /* return to kodeOS */
}
```

`kode_led_set_rgb()` does not write to the LED chip. It sends an event to `AO_LedMgr`, the one Active Object that uses the chip, and returns. The code of an app and the code of kodeOS ask for things in the same way.

The SDK's API is version 4 (`KODE_VERSION_MAJOR` in `kode.h`). An app written for version 3 moves with [MIGRATION.md](MIGRATION.md).

Each Active Object runs in its own **task**, a part of a program that runs on its own, next to other tasks. A part of ESP-IDF called FreeRTOS runs the tasks. An Active Object has a queue of **events**. An event is a small message. It has a signal, a number that says what kind of message it is, and sometimes data. The Active Object takes one event from its queue and handles it from start to end. This work is a **step**. Then it takes the next event. Other code never calls the owner of a device. It sends the owner an event. [How an app works](#how-an-app-works) explains this.

## Where to start

| You want to | Go to |
|---|---|
| run an app on your board now | [Quick start](#quick-start) |
| see what an app can do | [The examples](#the-examples) |
| understand how an app works | [How an app works](#how-an-app-works) |
| set the options of the build | [The build options](#the-build-options) |
| use the console of your app | [The console of your app](#the-console-of-your-app) |
| use the SDK from your own repository | [Use the SDK from your own project](#use-the-sdk-from-your-own-project) |
| know what starts at each reset | [The boot model](#the-boot-model) |
| know the addresses of the flash | [The partition table](#the-partition-table) |
| know the facts about the board that surprise people | [The board: things that surprise](#the-board-things-that-surprise) |
| move an app from version 3 of the API | [MIGRATION.md](MIGRATION.md) |
| read about a driver, an Active Object or the console in detail | the SDK and kodeOS pages of the documentation, [docs.kode.diy](https://docs.kode.diy) |

## Quick start

You need a Kode Dot that runs kodeOS, and ESP-IDF v6.1. ESP-IDF is the development kit of Espressif, the maker of the chip.

1. Load ESP-IDF in your terminal (`. <path>/esp-idf/export.sh`). Replace `<path>` with the folder where you installed ESP-IDF.
2. Build the example. Run the command in the folder that holds this README. The first build fetches the SDK from the ESP-IDF component registry before it compiles.

   ```bash
   idf.py -C examples/buttons_led build
   ```

3. Write the app to the mailbox of the board. The mailbox is a place in the flash, at the address `0x1A00000`. kodeOS looks there for a new app at each start. `PORT` is the serial port of the board. It looks like `/dev/cu.usbmodem101` on macOS and `/dev/ttyACM0` on Linux. Run the command in the terminal where you loaded ESP-IDF. Its Python has `esptool`.

   ```bash
   python -m esptool --chip esp32p4 -p PORT write-flash 0x1A00000 examples/buttons_led/build/buttons_led.bin
   ```

   `idf.py -C examples/buttons_led -p PORT flash` does the same, and so does the Flash button of the ESP-IDF extension for VS Code. In an app, kbsp points those commands at the mailbox, so they write the app and nothing else. Only `idf.py erase-flash` still erases the whole chip, kodeOS included.

   The build checks the size of an app against the `apps` region, not against the 4 MB slots of kodeOS. The mailbox holds an app of up to 6 MB less 4 KB. `idf.py flash` refuses a bigger app. Install it over the console instead, with `kcli.py install` from the kodeOS repository.

4. The board restarts. kodeOS finds the app in the mailbox, installs it and starts it.

**Result:** the LED is green. The top button changes it to red and back. The bottom button restarts the app, because an app from the mailbox starts again after a restart.

To go back to kodeOS, hold the bottom button and the left button together for about one second. The launcher then lists `Buttons + LED`. An app from the mailbox also starts again after other resets, for example when a program opens the serial port. [The boot model](#the-boot-model) lists them.

## The examples

**Start from an example, not from an empty file.** The folder `examples/` has one working project for each thing an app does. Each one except `arduino_hello` already holds the build files (`CMakeLists.txt`, `sdkconfig.defaults` and `main/idf_component.yml`) that the section [Use the SDK from your own project](#use-the-sdk-from-your-own-project) explains. Copy the example that is most like your app, and change it.

The column Radio says whether the example needs the ESP32-C5, the chip for Wi-Fi and Bluetooth. `no` means that it does not. `the link only` means that it uses the link to the C5, the connection between the two processors, and no Wi-Fi. `Wi-Fi` means that it uses Wi-Fi, and `Bluetooth` that it uses Bluetooth.

| Example | What it shows | Radio |
|---|---|---|
| `buttons_led` | a button event comes in, an LED colour goes out, the bottom button asks to return to kodeOS | no |
| `console_actions` | console commands of the app: one variable and two functions | no |
| `storage` | a counter in NVS (a store for small settings), and a file on the internal storage | no |
| `audio` | a tone, and a recording to a WAV file | no |
| `imu` | the accelerometer, the gyroscope and the magnetometer | no |
| `haptic` | a short and a long vibration | no |
| `nfc_read` | the type and the UID of a tag, and its first NDEF record | no |
| `rfid_read` | the ID of an EM4100 tag | no |
| `ir_remote` | learn a NEC code from a remote control and send it back | the link only |
| `expansion_io` | an output, a PWM output, an ADC input and an interrupt on the expansion header J3 | no |
| `exp_bus` | an I2C temperature sensor on J3, read from a task with `kode.h` | no |
| `power` | the battery reading, and power off | no |
| `network` | a ping and an HTTP GET | Wi-Fi |
| `mqtt` | an MQTT session: publish and receive | Wi-Fi |
| `ble_peripheral` | a Bluetooth service of the app's own: a value that a phone reads and subscribes to, and a value that it writes | Bluetooth |
| `display_lvgl` | LVGL, a graphics library, on the screen, with the local time, the point of the finger and the brightness | no |
| `arduino_hello` | the same idea as a sketch for the Arduino IDE | no |

Every example except `arduino_hello` starts with `kode_init()` in `app_main()`. An example that starts an Active Object of its own asks an owner through `kode_sys.h` and handles the answer when it arrives. It never waits. An example that calls `kode.h` does so from a task, where waiting is allowed. [How an app works](#how-an-app-works) explains the difference. The expansion header J3 has no owner, so its calls (`kode_sys_exp_*()`, and `kode.h`'s `kode_pin_*()` and buses) act on the pins at once. The one exception is the supply of the module, which is a pin of the C5 (see [The expansion header J3](#the-expansion-header-j3)).

## What is in the SDK

Each component is one layer. A component is a folder of code with its own build file. A layer uses only the layers under it.

| Component | What it holds |
|---|---|
| `kode` | the standard layer, `kode.h`: calls that wait, for the tasks of an app. Its own Active Object serves the events and the screen |
| `kode_sys` | the extended layer, `kode_sys.h`: `kode_init()`, and calls that never wait, for an Active Object of your own. With them `kode_cert.h` (the certification requests) and `kode_worker.h` (a worker task for an Active Object) |
| `kode_screen` | the screen calls of `kode.h`: screens, widgets, canvases, PNG. Optional. It brings `kscreen` |
| `kaos` | every Active Object. The name means "k + AOs" |
| `kdrv` | the chip drivers, one for each part. Each returns `kode_status_t`. Its folder `rfal` holds RFAL, the NFC library of STMicroelectronics, copied without change |
| `kqp` | QP/C in this system: signals, event pools, priorities, the tick |
| `qpc` | QP/C itself, with its FreeRTOS port |
| `khal` | the services that drivers share: the I2C bus, the console bytes, time, GPIO, NVS |
| `klog` | the log |
| `kbsp` | the description of the board: pins, addresses, sizes, the table of owners |
| `kdefs` | one header with one type, `kode_status_t` |
| `kscreen` | LVGL on the screen from an Active Object of your app. Optional |
| `kui` | the UI kit: colours, fonts, components. Optional. `kscreen` brings it |

The SDK is built on ESP-IDF and on libraries from the ESP-IDF component registry, an online store of libraries. When the registry has a library for a chip, the driver of that chip wraps the library. The driver calls the library and hides it behind its own functions.

If you add code to the SDK, this list tells you where a new file goes. If you only write an app, skip it. Go through the lines in order. The first line that fits gives the answer:

- It defines a pin or an I2C address of this board, as a constant: **kbsp**. A file that only uses a pin or an address does not count.
- Several drivers use it, or it can have a second implementation: **khal**.
- It uses the registers of one chip, such as the CO5300 or the BQ25896: **kdrv**.
- It owns a device, or decides when something happens: **kaos**.
- It is what an app calls: **kode_sys**, or **kode** when it waits for its work.
- It uses the system and is not a part of it: it is not in this SDK. The screens of kodeOS are an example, in the kodeOS repository. The optional components `kode_screen`, `kscreen` and `kui` are the exception: the SDK ships them for an app that draws.

## How an app works

### An app is a whole firmware

An app is not a plugin, a piece of code that kodeOS loads into itself. It is a complete firmware image with its own copy of this SDK. An image is the file that holds a firmware. kodeOS stores the image in the `apps` region of the flash. The bootloader starts it from there. The bootloader is the small program that runs first after a reset and decides which firmware starts. While the app runs, kodeOS does not run. The Active Objects in your image are your own copies.

An app that kodeOS started returns to kodeOS after any reset. The app does not have to do anything for this. An app that came through the mailbox is different: after some resets, the bootloader starts the same app again. `kode_exit()` and `kode_sys_exit_to_os()` are only a reset that the app asks for. If kodeOS started the app, this reset returns to kodeOS. If the app came from the mailbox, this reset starts the same app again. The section [The boot model](#the-boot-model) lists the resets of both kinds.

### Two headers: kode.h and kode_sys.h

One question decides which header a file needs: does the code wait for something, or does it react to something? A function **waits** when it returns only after the work is done. For example, a network ping returns after the answers arrive.

| | The work **waits** | The work **reacts** |
|---|---|---|
| Examples | a ping, an HTTP request, a file read, a tag read | a button, an MQTT message, a battery reading, a timer |
| Header | `kode.h` | `kode_sys.h` |
| Shape | `app_main()`, or a FreeRTOS task of your own | an Active Object of your own |

**An Active Object never waits. A task of your own can.** This rule is not about style. While an Active Object waits, it handles no event. The requests of the other parts stay in its queue. The queue has a fixed size. `KQP_AO_QUEUE_LEN` is 16 events. When the queue is full and one more event arrives, QP/C stops the system with an error. A step that computes for a long time, with no wait, can also delay the Active Objects with a lower priority.

**`kode.h`, the standard layer.** Every call returns once its work is done, with plain types: `bool`, numbers and text. When a call fails, `kode_error()` says why, in English. The calls are refused from an Active Object and from an interrupt. The events of `kode.h` (`kode_event_add_listener()`, `kode_event_wait()`), `kode_touch_read()`, `kode_pin_power_3v3()`, `kode_mqtt_subscribe()` and the screen calls run on an Active Object of the standard layer, at `KODE_PRIO_USER_GUI` (3). It starts at the first call that needs it, and an Active Object of your app cannot then take level 3.

**`kode_sys.h`, the extended layer.** Nothing in it waits. It has three kinds of call:

- A **command** posts to the owner of the device and returns `KODE_OK` when the queue of the owner took it. When the event pool is empty, or the queue of the owner is nearly full, it returns `KODE_NO_RESOURCES` and posts nothing. So a command is safe in a loop.
- A **request** takes the asking Active Object, `me`. It posts to the owner and returns at once. The answer arrives to `me` later as an event, with the final status in its `rc`. Each request names the signal of its answer, for example `kode_sys_net_ping(me, "example.com", 4)` is answered by `NET_PING_RSP_SIG`. `me` has one door for each kind of request: a second request of the same kind is refused with `KODE_DENIED` until the first is answered.
- An **immediate** call reads a copy, a flag or a pin, and returns. Two subscribe the calling Active Object instead, and link the owner they subscribe to: `kode_sys_touch_subscribe(me)`, to touch, links `AO_Touch`, and `kode_sys_ble_subscribe(me)`, to the writes and the links of the app's Bluetooth service, links `AO_Radio`.

A task can use the commands and the immediate calls. For anything with an answer, a task uses `kode.h`, because only an Active Object receives events. `kode_sys.h` gives the full rules at the head of each section. The Requests section says how deep the queue of an asking Active Object must be.

`kode_sys.h` also brings QP/C (`kqp.h`), the signals and the event pools, so an Active Object of your own uses the same events as the ones of the SDK. It does not bring `kaos.h` or the headers of the drivers. An app does not include `kaos.h`: it holds the system's own Active Objects, and `kode_sys.h` is the door to them. An app speaks the types of `kode_sys.h`.

### The Active Objects and their priorities

Each device has exactly one Active Object that owns it. All other code asks the owner with an event. An Active Object handles one event from start to end before it takes the next, so the code needs no locks. A lock is a flag that lets only one task use a device at a time.

At startup each Active Object claims what it owns. When two claim the same device, the boot stops with a message that names both.

Each Active Object runs as one FreeRTOS task. The QP priority of the Active Object is the FreeRTOS priority of its task. A higher number is a higher priority. When two tasks are ready on the same core, the task with the higher number runs first. The processor has two cores, and the tasks are not tied to a core.

| Priority | Active Object | What it owns |
|---|---|---|
| 1 | free: `KODE_PRIO_USER_LOW` | the place for an Active Object of your app |
| 2 | `AO_Net` | the network services: ping, HTTP, UDP, MQTT |
| 3 | the renderer: `KODE_PRIO_USER_GUI` | LVGL. `AO_Gui` in kodeOS, or your Active Object with `kscreen` |
| 4 | `AO_Radio` | the ESP32-C5: the link, Wi-Fi, Bluetooth |
| 5 | `AO_Storage` | the microSD card and the internal FAT |
| 6 | `AO_Log` | the console ports |
| 7 | `AO_LedMgr` | the RGB LED |
| 8 | `AO_Power` | the charger and the fuel gauge |
| 9 | `AO_AppMgr` | the `apps` region |
| 10 | `AO_Autotest` | the self-test rows; no hardware |
| 11 | `AO_Console` | the command reader; no hardware |
| 12 | `AO_Boot` | otadata and the boot request; see [The boot model](#the-boot-model) |
| 13 | `AO_Buttons` | the six buttons |
| 14 | `AO_Touch` | the touch controller |
| 15 | `AO_Display` | the panel |
| 16 | `AO_Scheduler` (`KODE_PRIO_SCHEDULER`), or free: `KODE_PRIO_USER_HIGH` | the cron entries, with `CONFIG_KODE_SCHEDULER`. Without it, the place for an Active Object of your app, only if you can state why the drawing may be delayed |
| 17 | `AO_IoExp` | the I2C pin expander |
| 18 | `AO_Ir` | the infrared parts |
| 19 | `AO_Nfc` | the NFC reader |
| 20 | `AO_Audio` | the microphone and the speaker |
| 21 | `AO_Imu` | the motion sensor and the magnetometer |
| 22 | `AO_Haptic` | the vibration motor |
| 23 | `AO_Rfid` | the 125 kHz RFID circuit |
| 24 | `AO_LowPower` | the three states of the board, with `CONFIG_KODE_LOWPOWER` |

The task that reads the console bytes also runs at priority 4. It is a plain task, not an Active Object, because it waits for bytes.

The console command `ps` prints this table and the table of owners from the running board.

Only the Active Objects that the app uses start. [The build options](#the-build-options) explains which.

**Where your Active Object goes.** Start at `KODE_PRIO_USER_LOW`, which is 1. At priority 1, a slow step of your Active Object delays no Active Object of the SDK, because all of them have a higher number. It also runs below `AO_Net`, so an Active Object at 1 that subscribes to `MQTT_MSG_SIG` sizes its queue for the rate of the broker. `KODE_PRIO_USER_HIGH` is 16, one level above `AO_Display`. A slow step at 16 can delay the drawing of the screen. Use 16 only if you can state why the drawing may be delayed. Level 16 is also the level of `AO_Scheduler`. With `CONFIG_KODE_SCHEDULER` on, which is off in the SDK, the scheduler holds it and `KODE_PRIO_USER_HIGH` is not defined: an app that turns the option on gives that level up. The file `kqp_config.h` defines both constants, and `kode_sys.h` includes it.

### What your Active Object can do

Three words have a fixed meaning here. To **subscribe** to a signal is to ask for every event with that signal. To **publish** an event is to send it to every Active Object that subscribed to its signal. To **post** an event is to send it to one Active Object.

- It can **subscribe** to any signal that the SDK publishes, for example `BUTTON_EVT_SIG`, `MQTT_MSG_SIG` or `POWER_EVT_SIG`. It never subscribes to the signal of an answer, such as `NET_PING_RSP_SIG`: it would receive the answers of others too, and could not tell them apart.
- It **asks** an owner for something with a call of `kode_sys.h`: a command, or a request with `me`. A subscription does not link an owner into the app, and neither does a published event: only a call does.
- It can **post** private signals of its own, numbered from `MAX_SIG`, with `QACTIVE_POST`.

It cannot **publish** a signal of its own. The SDK sizes its list of subscribers at startup, up to `MAX_PUB_SIG`, and QP/C stops the program for a signal above that. So two Active Objects of the same app post their own signals to each other.

The example `examples/buttons_led` shows an Active Object of an app: it subscribes to `BUTTON_EVT_SIG`, handles that event, and asks the owner of the LED for a colour.

Two more rules:

- **Ask for what you need.** When `kode_init()` returns, every Active Object that the app links already runs. The events that they published during startup are gone, and your code cannot receive them. Read the state instead: for example `kode_sys_wifi_status()`, `kode_sys_battery_read()` or `kode_sys_button_read_mask()`. For the screen, `kscreen` asks `AO_Display` for its state.
- **Do nothing before `kode_init()` returns.** A post to an Active Object with no queue ends in `assert failed: xQueueGenericSend queue.c:936 (pxQueue)` and a boot loop. In a boot loop, the board restarts again and again.

### Three things that are not events

This section is for a reader who writes a renderer or changes the SDK. To write a normal app, you do not need it. Almost everything between two parts is an event. Three things are not:

- The end of a screen flush is a semaphore, signalled from an interrupt. A renderer waits for it with `kdrv_display_flush_wait()`. `kscreen` does this for your app.
- The log has a direct path for the cases where the normal one cannot run: before `AO_Log` starts, in an interrupt, and in `Q_onError`.
- At a restart, `kdrv_display` drives the reset line of the panel low from a shutdown handler. So the screen goes dark with the restart.

## What an app can ask for

Each call has a comment with its contract in its header. `kode_init()` (in `kode_init.h`, which both headers include) starts the system; a second call waits for the first and returns.

From a task, with `kode.h`:

| Area | Functions |
|---|---|
| Start and time | `kode_delay()`, `kode_uptime_ms()`, `kode_time_unix()`, `kode_time_local()`, `kode_exit()`, `kode_error()` |
| The console and chance | `kode_print()`, `kode_random()` |
| The board | `kode_device_id()` |
| LED | `kode_led_set_rgb()`, `kode_led_set_color()`, `kode_led_off()`, `kode_led_toggle()` |
| Buttons | `kode_button_is_pressed()`, `kode_button_was_pressed()`, `kode_button_get_presses()`, `kode_button_read_all()` |
| Events | `kode_event_add_listener()`, `kode_event_remove_listener()`, `kode_event_wait()` |
| Sound | `kode_audio_play()`, `kode_audio_play_samples()`, `kode_audio_tone()`, `kode_audio_record_file()`, `kode_audio_record_start()`, `kode_audio_set()` and others |
| Vibration | `kode_haptic_vibrate()`, `kode_haptic_play()`, `kode_haptic_add_profile()` |
| Motion | `kode_imu_read()`, `kode_mag_read()`, `kode_mag_heading()`, `kode_mag_calibrate()` and others |
| Infrared | `kode_ir_send()`, `kode_ir_receive()`, `kode_ir_send_raw()`, `kode_ir_last()`, `kode_ir_capture_start()`, `kode_ir_send_file()` and others |
| NFC | `kode_nfc_read_id()`, `kode_nfc_read()`, `kode_nfc_write()`, `kode_nfc_emulate_start()` and others |
| RFID | `kode_rfid_read_id()`, `kode_rfid_write()`, `kode_rfid_emulate_start()` |
| Files | `kode_file_read()`, `kode_file_write()`, `kode_file_list()`, `kode_file_delete()` and others |
| Settings | `kode_setting_set()`, `kode_setting_get()` |
| Network | `kode_wifi_connect()`, `kode_wifi_scan()`, `kode_http_get()`, `kode_http_post()`, `kode_net_ping()`, `kode_udp_send()`, `kode_mqtt_*()` |
| Bluetooth | `kode_ble_set_name()`, `kode_ble_set_manufacturer_data()`, `kode_ble_scan()`, `KODE_BLE_SERVICE()`, `kode_ble_set_value()`, `kode_ble_notify()` |
| Battery and power | `kode_battery_level()`, `kode_battery_is_charging()`, `kode_battery_voltage_mv()`, `kode_power_is_usb_connected()`, `kode_power_off()`, `kode_power_set_charging()`, `kode_power_set_charge_current()` |
| The expansion header J3 | `kode_pin_write()`, `kode_pin_read()`, `kode_pin_read_analog()`, `kode_pin_write_pwm()`, `kode_pin_power_3v3()`, `kode_pin_power_5v()`, and the event `pin_<n>_changed` |
| Buses on J3 | `kode_i2c_*()`, `kode_spi_*()`, `kode_uart_*()` |
| Touch and brightness | `kode_touch_read()`, the events `touch_pressed`, `touch_moved` and `touch_released`, and `kode_display_set_brightness()`. They need no `kode_screen` |
| The screen | `kode_screen_new()` and its widgets, `kode_widget_set_color()`, `kode_screen_add_canvas()` and the `kode_draw_*()` and `kode_fill_*()` calls. They need the component `kode_screen` |

From an Active Object, with `kode_sys.h`. A request takes `me` and names the signal of its answer:

| Area | Commands and immediate calls | Requests |
|---|---|---|
| Time and the board | `kode_sys_time_ms()`, `kode_sys_clock_read()`, `kode_sys_clock_local()`, `kode_sys_device_id()`, `kode_sys_exit_to_os()`, `kode_sys_home_defer()`, `kode_sys_home_now()` | |
| LED | `kode_sys_led_set()`, `kode_sys_led_toggle()`, `kode_sys_led_set_rgb()` | |
| Buttons | `kode_sys_button_read_mask()`, `kode_sys_button_read_presses()`, or `BUTTON_EVT_SIG` | |
| Screen and touch | `kode_sys_screen_brightness()`, `kode_sys_touch_subscribe()` for `TOUCH_EVT_SIG` | |
| Sound | `kode_sys_audio_tone()`, `_volume()`, `_mic_gain()`, `_mute()`, `_record_stop()`, `_stream_write()` | `kode_sys_audio_play()`, `_record()`, `_record_file()`, `_stream_open()`, `_stream_close()`, `_info_read()` |
| Vibration | `kode_sys_haptic_play_buzz()`, `_play_pattern()`, `_hold()`, `_stop()`, `_is_holding()` | |
| Motion | | `kode_sys_imu_read()`, `kode_sys_mag_read()`, `kode_sys_mag_cal_get()`, `_set()`, `_clear()` |
| Infrared | `kode_sys_ir_send_nec()` | `kode_sys_ir_send_raw()`, `kode_sys_ir_capture()`, `kode_sys_ir_last()` |
| NFC | `kode_sys_nfc_is_emulating()` | `kode_sys_nfc_read_tag()`, `kode_sys_nfc_ndef_read()`, `_ndef_write_uri()`, `_ndef_write_text()`, `_emulate_start()`, `_emulate_stop()` |
| RFID | `kode_sys_rfid_is_emulating()` | `kode_sys_rfid_read()`, `_write()`, `_emulate_start()`, `_emulate_stop()` |
| Files | | `kode_sys_file_status()`, `_list()`, `_read()`, `_write()`, `_create_folder()`, `_delete()`, `_rename()` |
| Settings | `kode_sys_nvs_get()` | |
| Network | `kode_sys_wifi_connect()`, `_disconnect()`, `_status()`, `kode_sys_mqtt_*()` | `kode_sys_wifi_scan()`, `kode_sys_net_lookup()`, `kode_sys_net_ping()`, `kode_sys_http_get()`, `kode_sys_http_post()`, `kode_sys_udp_send()` |
| Bluetooth | `kode_sys_ble_set_name()`, `_set_data()`, `_set_value()`, `_notify()`, `_enabled()`, `_service_fault()`, `KODE_SYS_BLE_SERVICE()`, and `kode_sys_ble_subscribe()` for `BLE_APP_WRITE_SIG` and `BLE_APP_LINK_SIG` | `kode_sys_ble_scan()` |
| Battery and power | `kode_sys_battery_read()`, `kode_sys_power_sleep()`, `_off()`, `_boost()`, `_charge()`, `_charge_limit()` | |
| Firmware images | `kode_sys_flash()` | |
| The expansion header J3 | `kode_sys_exp_input()`, `_output()`, `_adc()`, `_pwm()`, `_irq()`, `_ctrl()`, `_info()` and others, and `kode_sys_exp_power()` for the module's supply, whose end `EXP_POWER_SIG` announces | |

### The name of your app

Add this line to your code, outside of any function. It gives your app a name and categories:

```c
KODE_APP_MANIFEST("My App", "demo,tool");
```

The first text is the display name, which the launcher shows. The second is a list of up to four categories, separated by commas. kodeOS shows one tab for each category in use. The line is optional. Without it, the launcher shows the project name. The data is stored at the start of the app file, so the running app uses no RAM for it.

A person can change the categories of an installed app from the console: `app_cat name="<project>" set="a,b"`.

### The identity of the board

`kode_device_id()` (and `kode_sys_device_id()`) returns the serial number of the board as text, for example `"KODE78YR5EEV0XS"`. It has no separators, so it fits in an MQTT topic, a file name or a URL. It returns an empty text on a board that cannot read its factory MAC address.

### Small settings from outside

`kode_setting_set()` and `kode_setting_get()` store small texts under a key, from a task. An Active Object reads them with `kode_sys_nvs_get()`. They are the way to give your app a value from outside without a new build: a token, an account name, a server address.

A person types `nvs_set key="pat" value="<the token>"` on the kodeOS console one time. After that, your app reads the value at each start with `kode_setting_get("pat")`, or `kode_sys_nvs_get("pat", buf, sizeof(buf))` from an Active Object. If nothing is stored under the key, `kode_setting_get()` returns `""` and `kode_error()` says so, and `kode_sys_nvs_get()` returns `KODE_DENIED`. Here the result means "not found". It is not a fault.

A key has up to 15 characters and a value up to 191. **Nothing stored there is secret.** The flash is not encrypted, the console reads every value, and all the apps on the board share the store.

### The expansion header J3

J3 has fourteen positions, numbered 1 to 14 on the connector. These are the numbers that `kode_pin_*()` and the bus calls take. Each position goes through the routing matrix of the chip, so any position can carry any digital signal or any bus, and each has 68 Ω in series. An analog input is not routed: `kode_pin_read_analog()` reads positions 8 to 12 and 14 only, the ones wired to the chip's ADC.

**A bus.** The standard layer has one I2C bus, one SPI bus and one UART for the whole app. The tasks of the app share them. The UART's two directions are apart: a task can write while another task waits in a read.

| Bus | Calls | Limits |
|---|---|---|
| I2C | `kode_i2c_begin(sda, scl, hz)`, `kode_i2c_probe()`, `_write()`, `_read()`, `_write_read()`, `_end()` | 10 kHz to 400 kHz. Within a transfer the wire stays silent at most `KODE_I2C_TIMEOUT_MS` (100 ms) per command segment, and a probe tries twice |
| SPI | `kode_spi_begin(sck, miso, mosi, cs, hz, mode)`, `kode_spi_transfer()`, `_end()` | 100 kHz to 10 MHz, mode 0 to 3, up to 64 bytes in one transfer |
| UART | `kode_uart_begin(tx, rx, baud)`, `kode_uart_write()`, `_read()`, `_end()` | 1,200 to 921,600 baud, 8 data bits, no parity, 1 stop bit. What arrives waits in a ring of 1 KB |

A begin lends its pins to the bus and takes the controller. While the bus holds a pin, `kode_pin_*()` on it fails with "pin N is lent to i2c". An end gives the pins back, at high impedance. The calls wait, so they belong in a task. An Active Object takes a controller with `kode_sys_exp_ctrl()` and drives the ESP-IDF driver itself. During a flash write, a UART loses the bytes after the 128 that its FIFO holds, unless the image sets `CONFIG_UART_ISR_IN_IRAM`. The example `exp_bus` reads a temperature sensor over I2C.

**A pin that changes.** The event `pin_<n>_changed`, for `n` from 1 to 14, says that the level of pin n changed. Its value is the new level, 0 or 1. A listener or a waiter for the event makes the pin an input with no pull and attaches its interrupt. The level is read `KODE_PIN_SETTLE_MS` (30 ms) after an edge, so a contact that bounces gives one event, at the level where it settled. While anything listens, the pin is read and never driven: `kode_pin_write()`, `kode_pin_write_pwm()`, `kode_pin_read_analog()` and a bus begin on it fail with "pin N is listened to".

**The supply of the module.** `kode_pin_power_3v3(on)` switches the 3.3 V on pins 2 and 19 of the connector, and waits until it is switched, up to `KODE_PIN_POWER_MS` (300 s). The switch is a pin of the C5, so the call links `AO_Radio`, and it returns within milliseconds when the link is up and the radio idle. Otherwise it waits for the job the radio is on, never for one asked after it: a Wi-Fi sweep takes about 40 s and up to 90 s, and the boot's search for a known network takes that sweep and then up to 20 s for each remembered network that refuses the board. With no link to the C5, "on" resets the C5 for 10 ms, and "off" brings the link up first, as a Wi-Fi scan does: nothing is switched on in Settings, and the C5 stays up for the rest of the boot, at about 85 mA. The first call starts the standard layer's Active Object, as the first event call does, at level 3. The state of the supply when an app starts is not promised (see [The board: things that surprise](#the-board-things-that-surprise)), so an app asks for the state that it needs. From an Active Object, `kode_sys_exp_power(on)` asks the same, and `AO_Radio` publishes `EXP_POWER_SIG` when the switch is over. Its `rc` is `KODE_OK` when the supply is as asked, `KODE_IO_ERROR` when the C5 refused, and `KODE_DENIED` when the link would not come up or went down: the supply then switches at the next link-up.

### Touch, brightness and local time

- **Touch.** Three events follow the finger: `touch_pressed`, `touch_moved` and `touch_released`. Each one carries the point in its `x` and `y`, in pixels of the glass from the top-left corner (410 x 502). The moves of a drag are combined: at most one `touch_moved` waits for the listeners, and a newer point takes its place. So a drag never fills the queue of events, and the button events still arrive. `kode_touch_read(&x, &y)` says whether a finger is down now, and where. Name the events with `KODE_EVENT_TOUCH_PRESSED`, `KODE_EVENT_TOUCH_MOVED` and `KODE_EVENT_TOUCH_RELEASED`, which link `AO_Touch`. An Active Object calls `kode_sys_touch_subscribe(me)` and receives `TOUCH_EVT_SIG`.
- **Brightness.** `kode_display_set_brightness(pct)`, or `kode_sys_screen_brightness(pct)` from an Active Object, sets the panel: 0 for dark, or 5 to 100 per cent. It holds while the app runs and is never stored, so the brightness of the user comes back in kodeOS. At 0 the panel and the touch still run.
- **Local time.** `kode_time_local(&t)`, or `kode_sys_clock_local(&t)` from an Active Object, reads the wall clock in the zone of the board: year, month, day, hour, minute, second and weekday. The zone is a setting of the system, `config key="zone"`, a POSIX TZ string such as `"CET-1CEST,M3.5.0,M10.5.0/3"`. kodeOS and every app use the same one, and it is UTC while none is stored. `kode_time_unix()` stays in UTC seconds.

The example `display_lvgl` shows the local time, the point of the finger and the brightness.

### Scans

- `kode_wifi_scan(out, cap)` sweeps both Wi-Fi bands, in about 38 s, and returns the networks, the strongest first. When the link to the C5 is down, it brings the link up first. The request `kode_sys_wifi_scan(me)` posts one `WIFI_SCAN_ENTRY_SIG` for each network, then the answer, `WIFI_SCAN_RSP_SIG`.
- `kode_ble_scan(out, cap, ms)` listens for Bluetooth devices for 0.5 s to 30 s. The request `kode_sys_ble_scan(me, ms)` posts one `BLE_SCAN_ENTRY_SIG` for each device, then the answer, `BLE_SCAN_RSP_SIG`.

The radio runs one scan at a time, of either kind. A scan that finds the radio busy does not wait: the boot's own search for a known network, another scan, a self-test and an install all keep it busy. `kode_wifi_scan()` and `kode_ble_scan()` then fail at once and `kode_error()` says that the radio is busy. A request is answered at once with `rc` `KODE_NO_RESOURCES`. Ask again later.

### Bluetooth for your app

An app turns Bluetooth on in its `sdkconfig.defaults` with `CONFIG_KODEOS_RADIO=y`, `CONFIG_BT_ENABLED=y` and `CONFIG_KODEOS_BLE=y`, as the example `ble_peripheral` does. The board then stays on the air, and an app adds two things to it:

- **A name and maker data**, in the scan response that a phone reads when it looks: `kode_ble_set_name()` and `kode_ble_set_manufacturer_data()`. They are not stored.
- **One GATT service of its own**, declared outside any function with `KODE_BLE_SERVICE(uuid, chars)`. GATT is the way that Bluetooth Low Energy offers values to a phone. The service has up to 4 characteristics, the values of the service, and each one can be read, written or notified. A value has up to 244 bytes. `kode_ble_set_value()` sets what a read gets. `kode_ble_notify()` sets it and sends it to the phone that subscribed. A write arrives as the event `ble_written`: its source is the name of the characteristic, its value the number of bytes, and its text the bytes themselves. `ble_connected` and `ble_disconnected` say when a phone comes and goes.

The security is the one of the console. With console pairing and the passkey on, which is the default, every characteristic needs an encrypted, authenticated and bonded link. An app refuses a new pairing, so the phone bonds in kodeOS first. Bluetooth on and off is the choice of the user, in Settings. While it is off, every `kode_ble_*()` call fails and says so. Every call links `AO_Radio`, so the C5 runs. The extended layer has the same calls: `kode_sys_ble_set_name()`, `_set_data()`, `_set_value()` and `_notify()` are commands, `KODE_SYS_BLE_SERVICE()` declares the service, and `kode_sys_ble_subscribe(me)` brings the writes (`BLE_APP_WRITE_SIG`) and the link (`BLE_APP_LINK_SIG`). The notifications share the link with the console, and no throughput is promised. The example `ble_peripheral` notifies a counter and takes a colour for the LED.

## The build options

An option is a setting of the build. Run `idf.py menuconfig` to see the options, or write an option in `sdkconfig.defaults`, as the examples do. The options are in two menus inside Component config: **Kode Dot** holds the role of the firmware and the power states, and **kodeOS active objects** holds the rest. ESP-IDF keeps the values of a configured project in its `sdkconfig`, and a value there beats a new default. So delete `sdkconfig` when you move to a new SDK version.

**The link decides what starts.** An option decides what is built. Then:

- `AO_Log`, `AO_Boot`, `AO_Power`, `AO_IoExp` and `AO_Buttons` always start. With `KODEOS_CONSOLE` on, so do `AO_Console` and `AO_Autotest`.
- With `KODE_LOWPOWER` on, `AO_LowPower` always starts too: the core owners join its rounds, and the board rests on idle time whether the app asks or not. Only the owners that started are asked; a parked part is already at rest. With `KODE_SCHEDULER` on, `AO_Scheduler` always starts, like the console: its entries run by the minute with no call of the app.
- Any other Active Object starts only when the app calls one of its functions, directly or through a module that it uses. Subscribing to the signal of an owner, or publishing one, does not start it.
- Some owners bring others. Sound and infrared bring `AO_Radio`, because the enable of the speaker amplifier and the infrared parts are on the ESP32-C5. `AO_Radio` brings `AO_Storage`, and `AO_Net` in a build with `KODEOS_NET`.
- An owner that the app does not use puts its part to rest at startup, and keeps the part, so `ps` shows it as `parked(AO_<name>)`. Without `AO_Radio`, the C5 is held in reset. An app with sound and no `AO_Radio` resets the C5 once and leaves it running, for the amplifier; a sound in the first 1.5 s after that reset is silent. `KODEOS_C5_RUN` does the same for an app that talks to the C5 by its own road, and `ps` shows `running for the app`.
- A console command whose owner is not in the app answers `>err code=5 msg=absent`. The commands and the `config` keys of an absent device do not exist in the app.
- In the role of kodeOS (`KODEOS_BOOT_ROLE_OS`), `kode_init()` starts every Active Object that the build compiles.

| Option | What it adds | Default |
|---|---|---|
| `KODEOS_BOOT_ROLE_APP` / `KODEOS_BOOT_ROLE_OS` | what this firmware is: an app, or kodeOS | app |
| `KODEOS_CONSOLE` | the console | off in an app, on in kodeOS |
| `KODEOS_APPMGR` | the Active Object that writes the `apps` region. The console brings it | off in an app, on in kodeOS |
| `KODEOS_SCREENSHOT`, `KODEOS_MIRROR` | a copy of the screen for screenshots, and the live mirror | off in an app, on in kodeOS |
| `KODEOS_RADIO` | the link to the ESP32-C5 | off in an app, on in kodeOS |
| `KODEOS_WIFI`, `KODEOS_BLE` | Wi-Fi and Bluetooth; they need the radio, and `KODEOS_BLE` also needs NimBLE, the Bluetooth stack of ESP-IDF, which an app turns on with `CONFIG_BT_ENABLED=y` | off in an app, on in kodeOS |
| `KODEOS_BLE_CONSOLE` | the console over Bluetooth | off in an app, on in kodeOS |
| `KODEOS_NET`, `KODEOS_MQTT` | the network services and the MQTT client | off in an app, on in kodeOS |
| `KODEOS_NFC` | `AO_Nfc`, the owner of the NFC reader, and the `kode_nfc_*()` calls | off in an app, on in kodeOS |
| `KODEOS_C5_HOLD` | `AO_Radio` holds the C5 in reset while neither Wi-Fi nor Bluetooth is wanted | off |
| `KODEOS_C5_RUN` | without `AO_Radio`, the core leaves the C5 running from boot for an app that talks to it by its own road, such as its UART. The C5 then draws current all the time | off |
| `KODEOS_CHANNEL` | the Kode channel: the app joins it whenever there is Wi-Fi, so the account sees the board online. It needs `KODEOS_MQTT` and `KODEOS_CONSOLE`, and it starts the network and the radio. It holds the one MQTT client, so an app that uses `kode_mqtt_*` or `kode_sys_mqtt_*` leaves it off | off in an app, on in kodeOS |
| `KODE_LOWPOWER` | `AO_LowPower`: the board's AWAKE, ASLEEP and OFF states, `kode_power_off()`, `kode_sys_power_sleep()` and `kode_sys_power_off()`. It needs the radio | off |
| `KODE_SCHEDULER` | `AO_Scheduler`: cron entries that run console lines, and the `cron` command. It takes priority level 16 | off |
| `KHAL_CONSOLE_DOCK` | the console through the charging base. It is in the menu **khal: console ports** | off |

`KODEOS_BOOT_ROLE_APP` is the role of an app: `AO_Boot` does not write otadata at startup, as [The boot model](#the-boot-model) explains. Keep it.

An app starts with the least: no console, no radio, no screenshots, no NFC. It turns on in its `sdkconfig.defaults` what it uses, and nothing of kodeOS that it never calls goes into its image. An option that is on makes the app file bigger: the network code alone adds about 395 kB. `KODEOS_NET` needs `KODEOS_WIFI`, and `KODEOS_WIFI` needs `KODEOS_RADIO`. The examples `ir_remote` and `ble_peripheral` turn the radio on, and only `network` and `mqtt` turn the network code on.

The screens of kodeOS are not in this SDK. An app that draws with the screen calls of `kode.h` lists `kode_screen` in its `main/idf_component.yml`. An app that drives LVGL from an Active Object of its own lists `kscreen`. An app that does not draw never builds LVGL.

## The console of your app

With `CONFIG_KODEOS_CONSOLE=y`, your app has its own console. While your app runs, kodeOS does not run. So the console that answers is built from your copy of `kaos`: your `ps`, your `tasks`, your `log_level`. Every example except `arduino_hello` keeps the console on, because it is the cheapest way to see what the app does.

**Over Bluetooth.** With `KODEOS_BLE_CONSOLE`, the same console also works over a Bluetooth service. This option needs `KODEOS_BLE`, so it exists only in a build with NimBLE. Two console commands turn it on, `radio_up` and then `ble_up`. In an app that uses the radio, `KODEOS_BLE_AT_BOOT` brings Bluetooth up at every boot. A client pairs in kodeOS, at its first connection. With `KODEOS_BLE_PAIRING_PASSKEY`, which is on by default, the board picks six digits, the screen of kodeOS shows them and the client types them. An app refuses a new pairing: it shows no code and hangs up, because pairing a new phone belongs to kodeOS. A phone that kodeOS already paired reconnects to the app with no new pairing, because its keys stay in the C5 and its token in the NVS of the P4.

**Through the charging base.** `KHAL_CONSOLE_DOCK` puts the same console on the charging base, as a USB serial port (`303a:4001`, "Kode Dot"). Opening that port does not restart the board. The option uses the USB OTG controller of the P4, so an app that needs USB for itself leaves it off.

Never connect the USB-C cable and the base to a computer at the same time. The 5 V of the board would reach the computer through the base.

**A long list must be sent in pieces.** Each line that the console prints is one event. The system has a fixed number of free events, and `AO_Log` sends the lines out. Suppose your Active Object has a higher priority than `AO_Log` and prints a long list in one step. The list can use all the free events before `AO_Log` sends the lines, and the lines after that are lost. Print a few lines. Then start a timer of 10 ms (ten ticks: a QP tick is 1 ms) and continue in the next step. Before each piece, ask `kode_sys_console_congested()` (in `kode_sys.h`). The command `ps` shows the number of lost lines as `log_dropped=`.

`help` lists the commands of the console first, then the commands of the owners that the app uses. The exact form of commands and answers is in the page "The console protocol" of the documentation.

## Use the SDK from your own project

A project outside the kodeOS tree gets the SDK through the ESP-IDF component manager, the tool of ESP-IDF that downloads components. Your project names one dependency:

```yaml
# main/idf_component.yml
dependencies:
  kodediy/kode: "==0.1.0"
```

The component manager downloads it from the ESP-IDF component registry, at [components.espressif.com/components/kodediy/kode](https://components.espressif.com/components/kodediy/kode). The registry also lists the examples of this repository under `kodediy/kode`.

The other components arrive with it, at the same version: `kode_sys`, `kaos`, `kqp`, `khal`, `kdrv`, `klog`, `kbsp`, `kdefs` and QP/C. `kode_screen`, `kscreen` and `kui` arrive only when the app names one of them, at the same version. Each component lists the other components that it needs in its own `idf_component.yml`, each at its own version exactly. So your project lists only `kode`, and `kode_screen` or `kscreen` when it draws. Pin `kode` exactly too (`==`), so a new release reaches your app only when you move the pin.

**ESP-IDF version:** v6.1. The folder `tests/target` builds every component for the chip: `idf.py -C tests/target build`.

### The two board facts that your project writes itself

Everything about the board is in `kbsp`: the flash, the PSRAM, the console, the wiring of the C5. Your project reads those settings and never copies them. Two lines cannot come from `kbsp`, because the build needs them before it has fetched `kbsp`.

1. **The chip.** `set(IDF_TARGET esp32p4)` before `project()` in `CMakeLists.txt`. Without it, ESP-IDF chooses `esp32` on a clean checkout and keeps that choice.
2. **The partition table.** In `sdkconfig.defaults`: `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="managed_components/kodediy__kbsp/partitions.csv"`. Your app runs from the `apps` region, so it must be built with this table and no other.

The settings file of the board, `sdkconfig.board`, has a timing problem. ESP-IDF reads the settings files before the component manager fetches `kbsp`, so on a clean checkout the file is not there yet. The `CMakeLists.txt` of every example solves this. When the file is missing, it runs one configure in a folder of its own, only to fetch `kbsp`. Then it goes on with the real build. Copy that `CMakeLists.txt` and do not write your own.

Never copy the content of `sdkconfig.board` into your project. A copy with a wrong PSRAM setting builds without error, and the board then stops at boot with no message.

### Internal RAM is the small memory

The chip has 640 KB of internal RAM and 32 MB of PSRAM. Three things can use only internal RAM. The first is the stack of every task. The second is the memory that DMA needs for its descriptors, the small blocks of data that tell DMA what to copy. The third is the data that an interrupt reads while the flash is being written. With every option on, the SDK takes about 350 KB of it at link time. Link time is the moment when the linker joins the code into one image. So the rest must stay free for those things.

`sdkconfig.board` sets the rule: internal RAM is taken by name. A plain `malloc()` takes PSRAM, whatever its size (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`). The buffers of esp-hosted and the memory of NimBLE come from PSRAM too. The SDK checks these options when it compiles. A project that does not read `sdkconfig.board` stops with an `#error` that names the file.

What this means for your app:

- A buffer that you take with `malloc()` or with `new` is in PSRAM. You need to do nothing.
- A global or a `static` variable is in internal RAM, because the linker puts it there. Do not declare a large buffer that way. Take it with `malloc()` when you need it.
- Every task that you create has its stack in internal RAM.
- The console command `ps` prints four numbers of the internal pool: `int_free` (free now), `int_max` (the largest free block), `int_low` (the lowest free since boot) and `alloc_fail` (the allocations that the heap refused). Read `int_low` after your app has done its heaviest work.
- `test component=mem` judges two of those numbers, `int_low` and `alloc_fail`. It passes when the pool never had less than 64 KB free and the heap refused nothing.

`tools/check_ram.sh <your.elf> <budget file>` checks what a build puts in internal RAM at link time. `tests/target/ram_budget.txt` is the budget of the SDK itself, and a model for the file of your project.

### The layer rules are not for your app

A script, `tools/check_layers.sh`, checks which headers each layer of this SDK includes. For example, `esp_*` headers stay under `kdrv`, and `qpc.h` stays in `kqp`. The script reads the folder `components/` only.

Your app does not have to follow these rules. It can include `freertos/task.h`, call `esp_*` functions and open a socket, like any ESP-IDF project.

### An app that serves TCP

lwIP runs on the P4, so an app can listen on a port like any ESP-IDF project. The SDK does not set any `CONFIG_LWIP_*` option. An app that serves several clients at once sets them in its own `sdkconfig.defaults`. These are the values of KodeCraft, a Minecraft server for up to 8 players, measured on a Kode Dot with 6 clients:

```
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384
CONFIG_LWIP_TCP_WND_DEFAULT=16384
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

- `CONFIG_LWIP_MAX_SOCKETS` counts every socket of the firmware: the listening one, one per client and the SDK's own. The default, 10, leaves little room.
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` puts lwIP's buffers in PSRAM first. With it, internal RAM never went below 160 KB free with 6 clients (`ps`, `int_low`).
- Run the server loop on a task of its own, not in an Active Object: it waits on sockets. KodeCraft pins it to core 1 with a 24 KB stack in internal RAM.
- Heavy work in that loop is the thing to watch, not the network. A firmware built with the template's default `-Og` runs it noticeably slower. `CONFIG_COMPILER_OPTIMIZATION_PERF=y` builds with `-O2`.
- Power save is on by default. The stored setting `wifi_ps` turns it off for lower latency: `config key="wifi_ps" value=off` on the console. It holds from then on, on every connection.

## The boot model

The flash holds three copies of kodeOS and one region for apps:

| Partition | What it holds |
|---|---|
| `factory` | kodeOS, the rescue copy. Written only over the cable |
| `ota_0`, `ota_1` | kodeOS. An update goes to the one that is not running |
| `apps` | the installed apps. Each app runs from the place where it is stored |
| `app_pending` | the mailbox: a computer writes a new app here |

Two separate blocks of data decide what starts:

- **otadata** says which copy of kodeOS starts. Only kodeOS writes it.
- **The boot request** is a small block of data in the `apps` region. It names one app, by its address and size, and it has a state.

kodeOS starts an app in this way. It writes a boot request and resets the board. The bootloader reads the request and checks the image. Then it changes the state of the request with one write of 4 bytes, and it starts the app from its place. The state decides what the bootloader does at the next reset.

A boot request has one of two kinds when kodeOS writes it:

- **One start**, state `NEW`. The launcher, the console command `app_run` and an install over the console write this kind. The console command for an install over the console is `install src="terminal" dst=app run=on`, and the tool `kcli.py` of the kodeOS repository uses it. The bootloader changes the state to `TRIED` before it starts the app. So the next reset of any kind starts kodeOS: `kode_exit()` or `kode_sys_exit_to_os()`, the bottom button held for 600 ms, a crash, a watchdog reset, and a power-off and power-on. The bottom button resets the board only when the option `KODEOS_HOME_HOLD` is on. It is on by default.
- **Start again**, state `ALWAYS`. Only kodeOS writes this kind. It does so when it installs an app that a computer left in the mailbox, with `esptool write-flash 0x1A00000 ...` or with the upload of the Arduino IDE. The bootloader changes the state to `ARMED` and starts the app.

After the first start, the request is `ARMED`. The bootloader then starts the same app again after these resets:

- a program opens the serial port;
- a debugger resets the chip;
- the app restarts itself, with `esp_restart()`, a crash, `kode_exit()`, `kode_sys_exit_to_os()` or the bottom button held;
- the chip wakes from deep sleep.

The bootloader starts kodeOS instead after these resets: a power-on reset, a hardware watchdog reset, and a new app in the mailbox.

So "after any reset the board starts kodeOS" is true for an app that the launcher or the console started. It is not true for an app that came through the mailbox. The bottom button and the left button held together for about one second cause a power-on reset. So this combination returns to kodeOS from any app.

`kode_sys_exit_to_os()` asks `AO_Boot` for a plain reset and returns; `kode_exit()` asks the same and does not return. `AO_Boot` leaves the network, parks the C5 and restarts the chip within 500 ms (`KODE_LEAVE_BUDGET_MS`), and writes nothing. So it returns to kodeOS only after an app of the first kind. After an app of the second kind, it starts the same app again.

The role of a firmware decides what `AO_Boot` does with otadata. With `KODEOS_BOOT_ROLE_APP`, `AO_Boot` does not write otadata at startup: it does not confirm the running image, and it does not clear otadata. This role also adds a flash-write guard to an app that runs from the `apps` region. The guard makes ESP-IDF refuse a write into the image of the app itself, and it makes `esp_ota_get_next_update_partition()` return no partition. An app has no kodeOS partition to write: a call of `esp_ota_begin()` in an app aborts.

### The partition table

This section is for a reader who writes to the flash with esptool or changes the layout of the flash. To write an app, you do not need it. The table is `components/kbsp/partitions.csv`.

| Name | Type | Address | Size | What it holds |
|---|---|---|---|---|
| `nvs` | data | `0xB000` | 64 KB | the settings store |
| `otadata` | data | `0x1B000` | 8 KB | which kodeOS starts |
| `phy_init` | data | `0x1D000` | 4 KB | radio calibration data |
| `factory` | app | `0x20000` | 4 MB | kodeOS, the rescue copy |
| `ota_0` | app | `0x420000` | 4 MB | kodeOS |
| `ota_1` | app | `0x820000` | 4 MB | kodeOS |
| `coredump` | data | `0xC20000` | 128 KB | the state of the last crash of kodeOS |
| `apps` | data, subtype `0x40` | `0xC40000` | 11.75 MB | the installed apps |
| `storage` | data, FAT | `0x1800000` | 2 MB | the files of the user |
| `app_pending` | data, subtype `0x41` | `0x1A00000` | 6 MB | the mailbox |

The table itself is at `0x8000`.

The `apps` region crosses the 16 MB address. A flash chip normally uses addresses of 3 bytes, which reach 16 MB, and the flash of this board is 32 MB. This works because the board is configured for 32-bit flash addresses (`CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH`). ESP-IDF marks that option as experimental.

`apps` is a data partition with its own subtype, so the `esp_ota_*` functions cannot write it. Only `kdrv_apps.c` writes it. An image is placed on a 64 KB boundary, so an app uses its size rounded up to 64 KB.

### Three commands that erase a block of data

Each command erases one block of data and nothing else. The board resets when the erase ends.

```bash
# otadata: the next boot starts the factory copy of kodeOS
python -m esptool --chip esp32p4 -p PORT --after hard-reset erase-region 0x1B000 0x2000

# the boot request: the next boot starts kodeOS, not an app
python -m esptool --chip esp32p4 -p PORT --after hard-reset erase-region 0xC42000 0x1000

# the mailbox: an app that you wrote there is not installed
python -m esptool --chip esp32p4 -p PORT --after hard-reset erase-region 0x1A00000 0x1000
```

**Result:** esptool reports that the region is erased, and the board resets. At the next start, the board does what the comment above the command says.

## The board: things that surprise

This section is for a reader who debugs the hardware or a driver. An app that only calls `kode.h` or `kode_sys.h` does not need it. The full pin map, with comments, is `components/kbsp/board/include/kbsp_pins.h`.

- **No LED is on a GPIO.** The LED is a KTD2026 chip on I2C, at `0x30`. Its register 0 must be `0x18`. With the reset value, the chip switches itself off when the bus lines go low, and the bus is shared.
- **The P4 powers the microSD card.** `SD_PWR` is GPIO22, active low. `kdrv_sd` drives it before each mount, so a card works with the radio off. If `sd_mount` fails with a card in the slot, check GPIO22 and the transistor Q1 first.
- **The ESP32-C5 switches the speaker amplifier.** The enable pin of the amplifier is IO4 of the C5. The firmware of the C5 raises it at boot, so sound works with `KODEOS_RADIO=n`: an app with sound and no radio resets the C5 once at startup and leaves it running, and is silent for about 1.5 s after that reset.
- **The infrared parts are on the C5.** An infrared call goes over the link, so it needs `KODEOS_RADIO`, and an app that uses infrared brings `AO_Radio`. The first infrared request that finds the link down waits while `AO_Radio` brings it up.
- **The ESP32-C5 switches the supply of the expansion module.** J3's +3.3V_EXT (pins 2 and 19) comes from the regulator U18, whose enable is IO5 of the C5, with a pull-up (R72). So the rail is on whenever the C5 does not drive IO5 low: after every boot of the C5, and while the C5 is held in reset. kodeOS drives it low when its link comes up, but a C5 parked for the jump to an app lets go of IO5 again, so an app's start state is not promised: it asks for the state it needs. An app switches it with `kode_pin_power_3v3()` or `kode_sys_exp_power()`; from the console it is `exp_power state=on|off`, and `c5 field="pwr_ext"` reads the pin.
- **A firmware without this SDK switches the module's supply itself.** An Arduino sketch, for example, cannot count on the module being off at its start either. Any reset of the C5 switches the supply on, because the C5 lets go of IO5 and the pull-up wins. With no link to the C5, the sketch holds GPIO23 (`C5_EN`) low for 10 ms and then high. Bringing the link up (esp-hosted, which the WiFi uses) resets the C5 too. With the link up, the pin is driven through esp-hosted's GPIO calls: `esp_hosted_cp_gpio_config()` with IO5 as an output, then `esp_hosted_cp_gpio_set_level(5, 1)` for on or `0` for off. The supply goes off again when kodeOS next boots.
- **The radio brings lwIP.** With `KODEOS_RADIO=y` and `KODEOS_NET=n`, the image still holds lwIP, because the link library needs it. `KODEOS_RADIO=n` removes it.
- **Pin P7 of the expander is the supply of the panel.** Only `AO_IoExp` writes the expander, and `AO_Display` asks it for P7. From the console, `ioexp field="c7" value=1` switches the screen off.
- **The battery percentage does not come from the fuel gauge.** `AO_Power` computes it from the voltage of the cell, with a table of the battery in `components/kbsp/board/include/kbsp_cell.h`. The table turns a voltage into a percentage. The current in `POWER_EVT_SIG` is positive when it flows into the cell.

## Where the rest is

This file covers the SDK: the API, the layers, the Active Objects, the boot model and the board. [MIGRATION.md](MIGRATION.md) moves an app from version 3 of the API to version 4. The detailed documentation is at [docs.kode.diy](https://docs.kode.diy), in its SDK and kodeOS sections: one page for each driver and each Active Object, the console protocol with every command, and the tools.

Every component, and every module folder inside one, has a short README that links its page on docs.kode.diy. `components/kdrv/rfal/README.md` is the NFC library's own, from STMicroelectronics.

Some comments cite Kode-Dot-Val, Kode's board-validation repository, as the origin of ported code. That repository is not public.

## Licence

The SDK is free software. Its licence is the GNU General Public License, version 3 or later. The file `LICENSE` holds the full text.

Two parts of this repository are code of other owners: QP/C of Quantum Leaps, and RFAL of STMicroelectronics. RFAL is not under the GNU General Public License. The file `NOTICE.md` says which licence applies to each part.
