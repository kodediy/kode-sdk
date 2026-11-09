#!/usr/bin/env bash
#
# The layer cake, checked rather than described.
#
# Twenty rules, all of them about what happens inside this SDK: where ESP-IDF
# stops, where QP/C stops, where the board is described, where stdio stops,
# where the RMT stops, where the expansion header's ADC stops, where the LEDC
# stops, where the C5's RPC stops, where the host's network stack stops,
# where the I2S stops, where the buses stop, where LVGL stops, where the ROM's inflater stops,
# where the camera stops,
# where cJSON stops, where the standard layer stops, who may use its seam, who
# may call the blocking calls it is built from, what the sys header may
# include, and where an Active Object may not wait.
# The one rule not here - one owner per
# device - cannot be checked from outside the running system, so it is checked
# from inside it: every AO claims what it owns at startup and a double claim
# is fatal (kbsp_owners.h).
#
# The LVGL rule confines lv_* to kui, kscreen and kode_screen.c, inside the
# SDK. The root's tools/check_layers.sh carries the matching rule for kgui,
# kodeOS's own consumer above the SDK, and runs this script for everything
# below it.
#
#   usage: tools/check_layers.sh
#   exit:  0 all clear, 1 something crossed a layer
#
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
report() {   # report <rule> <explanation> <matches>
    if [ -n "$3" ]; then
        printf '\n\033[31mFAIL\033[0m  %s\n      %s\n\n%s\n' "$1" "$2" "$3"
        fail=1
    else
        printf '\033[32mok\033[0m    %s\n' "$1"
    fi
}

# Skips build output and third-party sources: qpc and rfal are vendored and
# managed_components are not ours to hold to our own rules. Lines
# that look like prose are skipped too - a comment that mentions
# lv_display_flush_ready() is discussing it, not calling it. C++ sources are
# read with the C ones: a rule that never opens a .cpp passes it.
scan() { grep -rn --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' \
             "$1" components \
             2>/dev/null \
           | grep -v '/build/' \
           | grep -v 'components/qpc/' \
           | grep -v 'components/kdrv/rfal/' \
           | grep -v 'managed_components/' \
           | grep -vE ':[0-9]+: *(\*|//|/\*)' \
           | { [ $# -gt 1 ] && grep -vE "$2" || cat; }; }

# ---- 1. ESP-IDF stops at kdrv --------------------------------------------
# The whole point of khal and kdrv: above them this is portable C that speaks
# kode_status_t. klog is on the list because kbsp, khal and kdrv all log, so it
# has to sit below them and reach ESP-IDF directly.
#
# soc/ and hal/ are on the same list and not covered by the esp_ prefix: the
# register-level headers are named soc/rtc.h and hal/*_ll.h, so a file reaching
# past the drivers into the silicon would have slipped through a rule that only
# looked for esp_. khal_clk_esp.c is the reason this was noticed.
report "esp_*/driver/freertos/soc/hal only in kbsp, khal, kdrv, klog" \
  "Something above kdrv is talking to ESP-IDF directly. Put it behind a driver." \
  "$(scan '#include "\(esp_\|driver/\|freertos/\|soc/\|hal/\)' \
          'components/(kbsp|khal|kdrv|klog)/')"

# ---- 2. QP stops at kqp --------------------------------------------------
# One door to the framework. kqp includes qpc.h, configures it, and re-exports
# it through kqp.h; everyone else gets QP from kqp.h or does not get it at
# all - the AOs in kaos, the facade (kode_sys), kodeOS's own GUI, and anyone
# writing Active Objects of their own through kode_sys.h. kaos used to be on this list
# too, back when kaos.h took the framework raw; nothing was gained by the
# exception, since every file that includes kaos.h already includes kqp.h.
# The same rule is what lets an app link kdrv without dragging a framework in
# with it: below kaos there are no Active Objects to need one.
report "qpc.h only in kqp" \
  "Something outside kqp is reaching past it for the framework." \
  "$(scan '#include "qpc\.h"' 'components/kqp/')"

# ---- 3. The board is described in one place ------------------------------
# Pin numbers and I2C addresses are facts about the Kode Dot. Two copies of a
# hardware fact is how an app once came to declare PSRAM it does not have.
report "pins and I2C addresses defined only in kbsp" \
  "Define it in kbsp_pins.h and use the name everywhere else." \
  "$(scan '^#define KBSP_\(PIN\|I2C_ADDR\|IOEXP_\)' 'components/kbsp/')"

# ---- 4. stdio stops at the storage driver --------------------------------
# One AO opens files. If a second place can fopen(), it has become a second
# reader of the media whether it meant to or not.
report "dirent/stat only in kdrv_storage.c" \
  "AO_Storage owns the media; ask it rather than opening a file yourself." \
  "$(scan '#include <\(dirent\|sys/stat\)\.h>' 'components/kdrv/storage/src/kdrv_storage\.c')"

# ---- 5. No RMT on the P4 ---------------------------------------------------
# Rule 1 already keeps driver/ inside kdrv, and that is not enough here. The
# infrared block - the one thing on this board an RMT channel would drive - is
# wired to the ESP32-C5, and its RMT lives in the C5's firmware (kode_ir.c);
# kdrv_ir.c is that service's client over the link. A file opening an RMT
# channel on the P4 is therefore driving a pin the board does not route, or
# a second owner of one that already has one, and either arrives as an
# include rather than as a claim.
report "driver/rmt_* nowhere on the P4" \
  "The infrared block is the C5's; post to AO_Ir rather than opening a channel." \
  "$(scan '#include "driver/rmt_' 'components/kdrv/ir/src/no_such_file\.c')"

# ---- 5b. The ADC stops at the header's port and the RFID front end -------
# Rule 1 keeps esp_adc inside khal and kdrv; this keeps it in TWO named files.
# Every analog pin exposed on J3 has one owner and one table (khal_exp_esp.c),
# so a third file opening ADC1 for the header would be a second path to a pin
# that already has one. kdrv_rfid_lf.c is the deliberate second holder: its
# adc_continuous handle is on ADC2_CH5 (GPIO54, RFID_RF_IN), not a J3 pin,
# and exists only while an RFID request is in flight. The same per-file
# carve-out shape as rule 5 (rmt -> kdrv_ir) and rule 8 (i2s -> kdrv_es8311).
report "esp_adc/* only in khal_exp_esp.c and kdrv_rfid_lf.c" \
  "The header's pins have one table; ask khal_exp rather than opening the peripheral." \
  "$(scan '#include "esp_adc/' \
          '(components/khal/exp/src/khal_exp_esp|components/kdrv/rfid/src/kdrv_rfid_lf)\.c')"

# ---- 5c. The LEDC has one owner --------------------------------------------
# The P4's eight LEDC channels share four timers, so two files configuring
# them can retune each other's timer without an error on either side.
# khal_pwm hands out channels and timers from one table; its port is the one
# file that may include driver/ledc.h.
report "driver/ledc.h only in khal_pwm_esp.c" \
  "The LEDC has one owner; ask khal_pwm for a channel." \
  "$(scan '#include "driver/ledc\.h' 'components/khal/pwm/src/khal_pwm_esp\.c')"

# ---- 6. The C5 stops at its drivers ---------------------------------------
# Narrower than rule 1 on purpose: esp-hosted, the remote WiFi stack and NimBLE
# are one sequential RPC channel to one owned device, and the files allowed to
# speak it are the ones AO_Radio drives. A second caller is a second claimant
# of the C5 wearing an include for a hat.
#
# Four files, because AO_Radio's worker drives four drivers: the link itself
# (kdrv_radio), the station (kdrv_wifi), the BLE host (kdrv_ble) and the
# coprocessor's own snapshot (kdrv_c5), whose refresh is a chain of RPCs that
# only makes sense on that worker.
#
# The pattern lists NimBLE's four include roots, not just `nimble/`: the host
# API arrives as `host/ble_*.h`, its services as `services/gap|gatt/` and the
# bond store as `store/config/`. A rule that matches only one of the four
# reports success on the three it cannot see.
report "esp_hosted/esp_wifi/nimble only in kdrv_radio, kdrv_wifi, kdrv_ble, kdrv_c5" \
  "The C5 has one owner; post to AO_Radio instead of dialling the RPC yourself." \
  "$(scan '#include "\(esp_hosted\|esp_wifi\|nimble/\|host/ble_\|services/gap/\|services/gatt/\|store/config/\)' \
          'components/kdrv/radio/src/kdrv_(radio|wifi|ble|c5)\.c')"

# ---- 7. The host network stack stops at its driver ------------------------
# Rule 1 does not catch these: lwip/sockets.h, mqtt_client.h and mqtt5_client.h
# do not start with esp_, so a socket opened three layers up would pass every check above.
# It stops here for the same reason stdio stops at kdrv_storage.c - one place
# that can open one, or there are as many network stacks as there are files.
report "lwip/ping/http/tls/mqtt/sntp only in kdrv_net.c" \
  "Post to AO_Net, or call kode.h's network calls from a task - do not open your own socket." \
  "$(scan '#include "\(lwip/\|ping/\|esp_http_client\|esp_tls\|esp_crt_bundle\|mqtt5\?_client\|esp_netif_sntp\|esp_sntp\)' \
          'components/kdrv/radio/src/kdrv_net\.c')"

# ---- 8. The I2S stops at the codec driver ---------------------------------
# Rule 1 already keeps driver/ inside kdrv, and that is not enough here, for
# the reason rule 5 exists for the RMT: the I2S controller is bound to five
# GPIOs that already have an owner, so a second file opening a channel of its
# own is a second owner of five pins - the exact failure the ownership model
# exists to prevent, arriving as an include rather than as a claim. There is
# one controller carrying both directions of one codec; there is no second
# channel to open.
#
# The codec component is on the same list and is not covered by rule 1: its
# headers are esp_codec_dev*.h but also audio_codec_*.h and es8311_codec.h,
# and neither of those starts with esp_. kdrv_es8311.c wraps that component
# and keeps every one of its types inside itself, which is what lets
# kdrv_es8311.h speak kode_status_t and nothing else.
report "driver/i2s_*, esp_codec_dev and audio_codec_* only in kdrv_es8311.c" \
  "The audio path has one owner; post to AO_Audio rather than opening a channel." \
  "$(scan '#include "\(driver/i2s_\|esp_codec_dev\|audio_codec_\|es8311_\)' \
          'components/kdrv/audio/src/kdrv_es8311\.c')"

# ---- 8b. The buses stop at their six drivers ------------------------------
# Rule 1 keeps driver/ inside khal and kdrv, and that is not enough here: J3's
# fourteen positions are all routed through the chip's matrix, so any file
# that can include one of IDF's I2C, SPI or UART drivers - master or slave,
# the legacy I2C driver, the I3C master (whose I2C mode drives I2C devices),
# the SPI bus layer under both roles, the UART's DMA (uhci), and the two that
# bring one in with them (uart_select.h includes uart.h, sdspi_host.h
# includes spi_master.h) - can open a bus on any of them without passing
# through khal_exp's books. Seven files hold such a header, each with one owner behind it and
# each allowed only the header its owner uses: kdrv_exp_bus.c (the buses an
# app runs on J3, on positions and a controller lent first: I2C master, SPI
# master, UART), khal_i2c_esp.c (the board's I2C bus: I2C master),
# kdrv_st25r3916.c (the NFC reader's SPI host: SPI master),
# khal_console_esp.c (the console's UART), kdrv_ov5640.c (the E5 station's
# camera, whose SCCB is J3's I2C, on the whole header and a controller lent
# first: I2C master), kdrv_station_i2c.c (the station's two chips, on that
# same bus: I2C master) and kdrv_dock_loop.c (the E5 station's loop round the
# charging base's two pads, lent by AO_Log's dock console first: UART). An
# eighth file, or another header in
# one of the seven, would be a second path to J3 or a second owner of one of
# the others, arriving as an include rather than as a claim. The I3C, select
# and SD-over-SPI headers have no file at all. Both #include spellings are
# read.
report "IDF's I2C, SPI and UART headers only in their seven drivers, each its own" \
  "A bus on J3 is kode.h's kode_i2c_*, kode_spi_*, kode_uart_*, or kode_sys_exp_lend() and kode_sys_exp_ctrl() first." \
  "$(scan '#include *[<"]driver/\(i2c\|i2c_master\|i2c_slave\|i3c_master\|i3c_master_i2c\|spi_common\|spi_master\|spi_slave\|spi_slave_hd\|sdspi_host\|uart\|uart_select\|uhci\)\.h[>"]' \
          '^components/(kdrv/exp_bus/src/kdrv_exp_bus\.c:[0-9]+:#include "driver/(i2c_master|spi_master|uart)\.h"|khal/i2c/src/khal_i2c_esp\.c:[0-9]+:#include "driver/i2c_master\.h"|kdrv/nfc/src/kdrv_st25r3916\.c:[0-9]+:#include "driver/spi_master\.h"|khal/console/src/khal_console_esp\.c:[0-9]+:#include "driver/uart\.h"|kdrv/ov5640/src/kdrv_ov5640\.c:[0-9]+:#include "driver/i2c_master\.h"|kdrv/station/src/kdrv_station_i2c\.c:[0-9]+:#include "driver/i2c_master\.h"|kdrv/station/src/kdrv_dock_loop\.c:[0-9]+:#include "driver/uart\.h")')"

# ---- 9. LVGL stops at the UI kit and the renderer -------------------------
# The rule is about threads, not files: there is no lock around LVGL, because
# one task drives it (AO_Gui in kodeOS, the app's own renderer elsewhere).
# kscreen is that task's plumbing - the display, the flush, the touch feed,
# the render pass - and kui is a set of builders that task calls, so both run
# on that same thread. Any other SDK file calling lv_* is a second caller and
# very probably a second thread. kodeOS's own screens live above the SDK, in
# kgui, and the root's script checks those. kode_screen.c is the standard
# layer's page and runs on the standard layer's Active Object, the one owner of
# LVGL in an app that draws through kode.h; kode_widget.c beside it runs on the
# app's tasks and stays out.
report "lv_* only in kui, kscreen and kode_screen.c" \
  "Two threads in LVGL is the bug this rule prevents; there is no lock." \
  "$(scan '\blv_[a-z_]*(' '(components/(kui|kscreen)/|components/kode_screen/src/kode_screen\.c)')"

# ---- 10. The ROM's inflater stops at its two callers ----------------------
# Rule 1 does not catch this one, for the reason it misses the network stack:
# miniz.h does not start with esp_. And esp_rom, which ships it, is a
# requirement every component already has, so any file could include it and
# call tinfl_decompress() in the ROM. Two files do. khal_zlib_esp.c is the
# seam: one whole zlib stream into a buffer, behind khal_zlib_decompress(), with the
# decoder's 11 KB of state taken from PSRAM and given back inside the call.
# kdrv_kfw.c is the deliberate second holder, the shape rule 5b has for
# kdrv_rfid_lf.c: it feeds a firmware container's payload to the same routine
# a chunk at a time through a 32 KB window, a payload nobody holds whole and
# so no job for the one-call seam. A third file would be a third place that
# decides where those 11 KB live, and the ROM's own one-call helpers answer
# that with the caller's stack.
report "miniz.h only in khal_zlib_esp.c and kdrv_kfw.c" \
  "The ROM's inflater has one seam; call khal_zlib_decompress() rather than the ROM." \
  "$(scan '#include [<"]miniz\.h[>"]' \
          '(components/khal/zlib/src/khal_zlib_esp|components/kdrv/apps/src/kdrv_kfw)\.c')"

# ---- 10b. The camera stops at its driver ----------------------------------
# The E5 station's camera module sits on J3 and has one driver,
# kdrv_ov5640.c: the sensor component, its SCCB layer and IDF's DVP
# controller are included there and nowhere else. Rule 1 does not catch them
# all, for the reason it misses the ROM's inflater: the sensor's own header is
# ov5640.h, and the rest start with esp_ and so pass anywhere in kdrv. A second
# file holding one would be a second path to fourteen pins that are lent to
# the driver as one loan.
report "esp_cam_sensor, esp_sccb, esp_cam_ctlr and ov5640.h only in kdrv_ov5640.c" \
  "The camera has one driver; call kdrv_ov5640 rather than the sensor component." \
  "$(scan '#include [<"]\(esp_cam_sensor\|esp_sccb_\|esp_cam_ctlr\|ov5640\.h\)' \
          'components/kdrv/ov5640/src/kdrv_ov5640\.c')"

# ---- 11. cJSON stops at Kody's pack ---------------------------------------
# kui_pack.c parses the pack's manifest and checks each Lottie before ThorVG
# sees it, and it may do so only because it scans the depth first without
# recursing: cJSON's parser recurses as deep as the text nests, on the
# caller's stack. A second caller would be a second place feeding cJSON text
# nobody bounded. Rule 1 does not catch it: cJSON.h does not start with esp_.
report "cJSON.h only in kui_pack.c" \
  "Parse JSON through kui_pack, which bounds the depth before cJSON recurses." \
  "$(scan '#include [<"]cJSON\.h[>"]' 'components/kui/pack/src/kui_pack\.c')"

# ---- 12. The standard layer stops at the facade ---------------------------
# components/kode is the layer an app's include lands in (kode.h). It sits
# on kode_sys and what kode_sys drags in, and it names no screen: its REQUIRES
# are kode_sys, kdefs, kaos, kdrv and khal, so a headless app that pins it does
# not download LVGL. This rule keeps the sources honest about that. A quoted
# include in components/kode must name a public header of a layer it requires,
# or one kode_sys brings with it (kode_sys, kdefs, klog, kaos, kqp, kdrv), or
# one of its own; esp_*, driver/, freertos/, soc/, hal/, qpc.h, lv_*,
# kscreen.h and kui.h are none of those,
# so they fail here as they do everywhere above the drivers. The screen calls
# are kode_screen's, a component of its own that an app names when it draws.
#
# khal is admitted by header, three of them and no more: khal_system.h (the
# calling task's one pointer, and PSRAM for what hangs from it), khal_sync.h
# (the semaphores and mutexes a blocking call waits on) and khal_once.h (the
# one-shot the first-use start is elected by). The standard
# layer keeps per-task state and blocks, and those are the only seams that do
# it; the rest of khal (buses, pins, clocks) stays behind kode_sys.
#
# The list is the headers' names as they stand in the tree, found rather than
# typed, so a header added to one of those layers is allowed with no edit.
standard_allowed="$(find components/kode components/kode_sys components/kdefs \
                       components/klog components/kaos components/kqp \
                       components/kdrv -name '*.h' \
                       \( -path '*/include/*' -o -path 'components/kode/*' \) \
                       -not -path '*/build/*' -not -path '*managed_components*' \
                       2>/dev/null \
                  | sed 's|.*/||; s|\.|\\.|g' | sort -u | paste -sd'|' -)"
standard_allowed="${standard_allowed}|khal_system\\.h|khal_sync\\.h|khal_once\\.h"
report "components/kode includes only kode_sys and the layers below it" \
  "The standard layer reaches the system through kode_sys.h; the screen is kode_screen's." \
  "$(scan '#include "' "#include \"(${standard_allowed})\"" \
          | grep '^components/kode/')"

# ---- 12b. The standard layer's seam has two users -------------------------
# kode_internal.h hands kode_screen the standard layer's Active Object and
# its call prologue; anyone else would be drawing on that thread or posting
# into its queue behind the layer's back. Outside this tree the header itself
# refuses to compile unless KODE_INTERNAL_ALLOWED is defined, which only
# those two components do, privately.
report "kode_internal.h only in kode and kode_screen" \
  "The seam is kode_screen's; an app writes against kode.h." \
  "$(scan '#include "kode_internal\.h"' 'components/(kode|kode_screen)/')"

# ---- 12c. The blocking calls are the standard layer's ---------------------
# kode_sys_internal.h holds every sys call that waits for its answer: kode.h is
# built from them, and kode_sys.h, which an Active Object includes, never
# waits. Its folder is public only because ESP-IDF has no include path for
# some consumers alone, so the header is kept here instead: kode_sys defines
# the calls and kode wraps them. Outside this tree the header itself refuses
# to compile unless KODE_SYS_INTERNAL_ALLOWED is defined, which only those two
# components do, privately.
report "kode_sys_internal.h only in kode_sys and kode" \
  "A blocking sys call stalls an Active Object; post through kode_sys.h or call kode.h from a task." \
  "$(scan '#include "kode_sys_internal\.h"' 'components/(kode_sys|kode)/')"

# ---- 12d. kode_sys.h speaks its own types ---------------------------------
# What an app's Active Object includes carries kode_sys_* types and sizes, not
# a driver's: a kdrv_, knfc_ or khal_ header or kaos.h in it would make the
# app's code depend on a layer that is free to change. Each kode_sys_<module>.c
# converts, and a _Static_assert there keeps each mirrored value equal to the driver's.
# The includes are checked by name, not by prefix: the C headers, the two
# app-description headers kdrv carries (kapp_desc.h, kapp_cat.h), the console
# tables kaos carries (kode_variables.h, kode_functions.h, kode_config.h), and
# kqp.h, which brings QP/C and every system event with it (and so khal_sync.h,
# the `done` of the blocking requests). Any other include is a new dependency
# of every app, and is added here in the same commit, where a reviewer sees it.
sys_includes='^(stddef|stdint|stdbool|kode_defs|kode_log|kapp_desc|kode_variables|kode_functions|kode_config|kapp_cat|kqp|kode_init)\.h$'
report "kode_sys.h includes only the headers named here" \
  "A driver type in the sys contract binds the app to the driver; give it a kode_sys_ type and convert in its kode_sys_<module>.c." \
  "$(grep -nE '^#include ' components/kode_sys/include/kode_sys.h \
       | sed -E 's/^([0-9]+):#include [<"]([^>"]+)[>"].*/\1 \2/' \
       | awk -v ok="$sys_includes" '$2 !~ ok { print "components/kode_sys/include/kode_sys.h:" $1 ": " $2 }')"

# ---- 13. A kode.h call is never made from an Active Object ---------------
# kode.h is the blocking layer: its calls wait on a semaphore until the answer
# comes back, and an AO that waits stops serving its queue. A file that builds
# an Active Object (QActive_ctor, Q_STATE_CAST) is code that runs inside one,
# so it must not include kode.h; it speaks kode_sys.h, which posts and
# publishes. The one exception is components/kode/src/kode_standard_ao.c, the
# standard layer's own AO: it is the layer, not a user of it, and its handlers
# never block. Only components is scanned, like every rule above: the host
# tests compile kode_standard_ao.c a second time and start spy AOs in files that
# include kode.h on purpose, and they are not code that ships.
ao_files="$(scan '\(QActive_ctor(\|Q_STATE_CAST(\)' \
            | grep -v '^components/kode/src/kode_standard_ao\.c:' \
            | cut -d: -f1 | sort -u)"
ao_hits=""
for f in $ao_files; do
    hit="$(grep -n '#include "kode\.h"' "$f" | grep -vE '^[0-9]+: *(\*|//|/\*)' \
             | sed "s|^|$f:|")"
    [ -n "$hit" ] && ao_hits="${ao_hits}${hit}"$'\n'
done
report "kode.h never in a file that builds an Active Object" \
  "A kode.h call blocks; an AO that blocks stops serving its queue. Use kode_sys.h inside an AO." \
  "${ao_hits%$'\n'}"

if [ "$fail" -eq 0 ]; then
    printf '\n\033[32mAll layer rules hold.\033[0m\n'
else
    printf '\n\033[31mThe cake has a hole in it.\033[0m\n'
fi
exit "$fail"
