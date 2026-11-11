# kaos - k + AOs. Every Active Object in kodeOS, one component, because "is
# it an AO?" is the only question that decides whether something belongs
# here, and splitting them across components made that look like a layer
# distinction when it was really just a directory.
#
# The optional ones are compile-time (see Kconfig), not runtime: ESP-IDF links
# what it compiles.
#
# AO_Gui is not here any more. It lives in kgui, which requires the SDK rather
# than being required by it - which is the only way a headless firmware can
# avoid LVGL, since a requirement cannot be gated on a Kconfig value that does
# not exist yet when requirements are resolved.
set(srcs
    "core/src/kaos_claim.c"
    # What kode_init() calls for an owner the app did not link: out of every
    # owner's file, so naming a park never links its owner.
    "core/src/kaos_park.c"
    # kaos_linked(): the one test of a weak reference to an owner.
    "core/src/kaos_linked.c"
    # The doors every owner keeps for kode_sys's requests.
    "core/src/kaos_ask.c"
    "log/src/log_ao.c"
    "storage/src/storage_ao.c"
    "boot/src/boot_ao.c"
    # The time zone grammar AO_Boot asks before it stores a zone.
    "boot/src/boot_zone.c"
    "led/src/led_mgr.c"
    "ioexp/src/ioexp_ao.c"
    "buttons/src/buttons_ao.c"
    "ir/src/ir_ao.c"
    # AO_Rfid. Unconditional, like every other device owner: the 125 kHz LF
    # front end is on the Antenna PCB, so something has to own it.
    "rfid/src/rfid_ao.c"
    # AO_Audio. Unconditional, like every other device owner: the codec is on
    # the board, so something has to own it.
    "audio/src/audio_ao.c"
    # AO_Imu and AO_Haptic. Unconditional for the same reason: both parts are
    # on the board, so something has to own them.
    "imu/src/imu_ao.c"
    "haptic/src/haptic_ao.c"
    "power/src/power_ao.c"
    "display/src/display_ao.c"
    "touch/src/touch_ao.c"
    # The touch test's record, which the console writes with: its own file,
    # so the console does not link AO_Touch into an app that never uses it.
    "touch/src/touch_record.c"
    # The registry of the app's variables and functions, kept here because
    # AO_Console is what reads them and kode sits above kaos. Compiled always -
    # it has no hardware, and of QP only a setting's owner - and it refuses
    # every registration in a build without the console, so a table nobody
    # could reach is never held.
    "core/src/kregistry.c"
    # kode_config_post() and kode_config_answer(), a setting's two doors.
    "core/src/kode_config.c"
)
set(priv_reqs klog kbsp)
set(priv_incs
    "console/src"
    "core/src"
    "display/src"
    "net/src"
    "scheduler/src")

if(CONFIG_KODEOS_APPMGR)
    list(APPEND srcs "appmgr/src/appmgr_ao.c")
endif()
if(CONFIG_KODEOS_NFC)
    # AO_Nfc. Off, the reader is the app's: nothing here starts or parks it.
    list(APPEND srcs "nfc/src/nfc_ao.c")
    if(CONFIG_KODEOS_CONSOLE)
        list(APPEND srcs "nfc/src/nfc_verbs.c")
    endif()
endif()
if(CONFIG_KODE_LOWPOWER)
    # AO_LowPower: the board's three states and the round that enters them.
    list(APPEND srcs "lowpower/src/lowpower_ao.c")
    # The summary of the last rest: a ring of gauge samples over khal_nvs.
    list(APPEND srcs "lowpower/src/lowpower_last.c")
    # The time's rectangle for the glass at rest: arithmetic over the
    # renderer's glyph strip, no panel, so the host suite compiles it alone.
    list(APPEND srcs "display/src/display_clock.c")
else()
    # The facade calls exist either way, so an app's source compiles; with the
    # option off they refuse.
    list(APPEND srcs "lowpower/src/lowpower_none.c")
endif()
if(CONFIG_KODE_SCHEDULER)
    # AO_Scheduler: the table of cron entries and the lines it runs.
    list(APPEND srcs "scheduler/src/scheduler_ao.c")
    # Its time expression: plain C, no device, read by AO_Scheduler alone.
    list(APPEND srcs "scheduler/src/sched_cron.c")
else()
    # The doors exist either way, so an app's source compiles; with the
    # option off they refuse.
    list(APPEND srcs "scheduler/src/scheduler_none.c")
endif()
if(CONFIG_KODEOS_AUTOTEST)
    # AO_Autotest: the self-test rows and the walk over them. It owns no
    # device, so unlike the owners above it can be left out.
    list(APPEND srcs "autotest/src/autotest_ao.c")
    list(APPEND srcs "autotest/src/autotest_psram.c")
    # The self-test rows each owner registers from its start, under the same
    # option: without AO_Autotest no row table is compiled or linked.
    list(APPEND srcs "core/src/krows.c")
endif()
if(CONFIG_KODEOS_CONSOLE)
    list(APPEND srcs "console/src/console_ao.c")
    # The file verbs' bodies. Split out of console_ao.c, which was already the
    # largest source in the tree: what stays there is a handler per verb,
    # because the Console struct and the in-flight machinery are private to
    # it, and what moves here is everything below cmd_begin() - which is also
    # what the host suite can then compile on its own.
    list(APPEND srcs "console/src/console_fs.c")
    # The grammar: splitting a line, reading and writing a quoted string.
    # Pure text, so the host suite compiles it on its own and compares bytes.
    list(APPEND srcs "console/src/console_grammar.c")
    # The owners' verbs, each beside its owner and registered by its start,
    # so a device's verbs come in with the AO the app links and not before.
    list(APPEND srcs
        "core/src/kverbs.c"
        "led/src/led_verbs.c"
        "touch/src/touch_verbs.c"
        "audio/src/audio_verbs.c"
        "haptic/src/haptic_verbs.c"
        "ir/src/ir_verbs.c"
        "rfid/src/rfid_verbs.c"
        "display/src/display_verbs.c"
        "storage/src/storage_verbs.c")
    if(CONFIG_KODEOS_APPMGR)
        list(APPEND srcs "appmgr/src/appmgr_verbs.c")
    endif()
    if(CONFIG_KODE_LOWPOWER)
        list(APPEND srcs "lowpower/src/lowpower_verbs.c")
    endif()
    if(CONFIG_KODE_SCHEDULER)
        list(APPEND srcs "scheduler/src/scheduler_verbs.c")
    endif()
endif()
if(CONFIG_KODEOS_RADIO)
    list(APPEND srcs "radio/src/radio_ao.c")
    # The Wi-Fi frames door: its asker, its request and what it posts.
    list(APPEND srcs "radio/src/radio_frames.c")
    if(CONFIG_KODEOS_CONSOLE)
        list(APPEND srcs "radio/src/radio_verbs.c")
    endif()
    if(CONFIG_KODEOS_WIFI)
        # The remembered networks live in the C5's vault (kode_wire.h);
        # AO_Radio asks it through kdrv_c5svc.
        if(CONFIG_KODEOS_NET)
            list(APPEND srcs "net/src/net_ao.c")
            # The Kode channel's text: the api's answers, the bodies it is
            # sent, the verdict on an error and the back-off. Pure, so the
            # host suite compiles it on its own.
            list(APPEND srcs "net/src/net_channel.c")
            if(CONFIG_KODEOS_CONSOLE)
                list(APPEND srcs "net/src/net_verbs.c")
            endif()
        endif()
    endif()
endif()
if(CONFIG_KODE_TEST_DOT)
    # The screen analysis of the end-of-line test: the camera's photos judged
    # in plain C, no RTOS and no IDF, so the host suite in tests/host_tdan
    # compiles the same three files on its own.
    list(APPEND srcs
        "test_dot/analysis/src/tdan_geom.c"
        "test_dot/analysis/src/tdan_pix.c"
        "test_dot/analysis/src/tdan_judge.c")
    list(APPEND priv_incs "test_dot/analysis/include" "test_dot/analysis/src")
    # AO_TestDot itself: the AO, its worker's jobs, the camera part and the
    # verbs. Dormant from boot; the cable wakes it.
    list(APPEND srcs
        "test_dot/src/test_dot_ao.c"
        "test_dot/src/test_dot_worker.c"
        "test_dot/src/test_dot_cam.c"
        "test_dot/src/test_dot_jpeg.c")
    list(APPEND priv_incs "test_dot/src")
    if(CONFIG_KODEOS_CONSOLE)
        list(APPEND srcs "test_dot/src/test_dot_verbs.c")
    endif()
    # No fused multiply-add across statements: GCC on the P4 fuses by default
    # (gnu17 means -ffp-contract=fast), so the same photo would give other
    # floats here than on the host. Off, the analysis answers the same on
    # every compiler.
    set_source_files_properties(
        "test_dot/analysis/src/tdan_geom.c"
        "test_dot/analysis/src/tdan_pix.c"
        "test_dot/analysis/src/tdan_judge.c"
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "core/include"
    PRIV_INCLUDE_DIRS ${priv_incs}
    # kdefs is public: kode_variables.h includes kode_defs.h, so anything that
    # includes the public header needs it on its own include path.
    REQUIRES kqp khal kdrv kdefs
    PRIV_REQUIRES ${priv_reqs}
)
