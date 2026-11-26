# kode, kode_screen, kui and kscreen are the only names: every other
# component arrives behind them, exactly as it does in an app that draws, and
# kode_sys.h reaches kaos and kqp through kode_sys's public requirements.
# kscreen is compiled, not started: this project runs no Active Object of its
# own, and the screen calls are linked, never made.
#
# The variants `minimal`, `minimal_noradio` and `minimal_ble`
# (KODE_TARGET_VARIANT) build minimal.c instead: the LED and the buttons on an
# app's default configuration, the image that says what an app using two
# owners links (tools/check_starts.sh --absent); the second with the radio
# compiled out, which builds the C5's reset line alone (kdrv_c5_hold.c); the
# third with Bluetooth on and no BLE console, which builds the Bluetooth
# calls with no pairing type in kdrv_ble.h.
#
# The variant `arduino` builds main.c with arduino_port.c: the console's cable
# port given by the app, as Arduino's core gives it. Whole, as the core links a
# sketch: only khal, linked after main, calls the port's two functions.
if("$ENV{KODE_TARGET_VARIANT}" MATCHES "^minimal")
    idf_component_register(
        SRCS "minimal.c"
        INCLUDE_DIRS "."
        REQUIRES kode
    )
elseif("$ENV{KODE_TARGET_VARIANT}" STREQUAL "arduino")
    idf_component_register(
        SRCS "main.c" "arduino_port.c"
        INCLUDE_DIRS "."
        REQUIRES kode kode_screen kui kscreen
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        REQUIRES kode kode_screen kui kscreen
    )
endif()
