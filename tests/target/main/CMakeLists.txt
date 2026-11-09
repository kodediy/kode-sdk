# kode, kode_screen, kui and kscreen are the only names: every other
# component arrives behind them, exactly as it does in an app that draws, and
# kode_sys.h reaches kaos and kqp through kode_sys's public requirements.
# kscreen is compiled, not started: this project runs no Active Object of its
# own, and the screen calls are linked, never made.
#
# The variants `minimal` and `minimal_noradio` (KODE_TARGET_VARIANT) build
# minimal.c instead: the LED and the buttons on an app's default configuration,
# the image that says what an app using two owners links
# (tools/check_starts.sh --absent); the second with the radio compiled out,
# which builds the C5's reset line alone (kdrv_c5_hold.c).
if("$ENV{KODE_TARGET_VARIANT}" MATCHES "^minimal")
    idf_component_register(
        SRCS "minimal.c"
        INCLUDE_DIRS "."
        REQUIRES kode
    )
else()
    idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        REQUIRES kode kode_screen kui kscreen
    )
endif()
