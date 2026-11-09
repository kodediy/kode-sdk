# kscreen: LVGL on the glass, driven from one Active Object of the app's own.
# The display, the flush handed to AO_Display, the touch feed and the render
# pass - what every firmware that draws would otherwise write again.
#
# kode_sys and kui are public because kscreen.h speaks their types: QActive,
# QTimeEvt and the panel's events from kode_sys.h, lv_display_t from LVGL,
# which kui brings. kdrv, khal and kbsp are private: the flush completion is
# kdrv_display's semaphore, the clock and the PSRAM heap are khal's, and the
# glass's size is a board fact.
#
# Not WHOLE_ARCHIVE: the owner calls it, so the linker is asked for it.
idf_component_register(
    SRCS "src/kscreen.c"
    INCLUDE_DIRS "include"
    REQUIRES kode_sys kui
    PRIV_REQUIRES kdrv khal kbsp
)
