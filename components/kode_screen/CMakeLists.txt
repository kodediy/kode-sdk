# kode_screen: the standard layer's screen calls (kode_screen_new() and the rest,
# declared in kode.h). An app that draws names it; kode does not, so an app
# that never draws downloads no LVGL.
#
# Two halves. kode_widget.c, kode_canvas.c, kode_png.c and kode_compass.c run in the
# app's tasks: they post, draw, read PNG files and guide the compass.
# kode_screen.c runs on the standard layer's Active Object, drives kscreen and
# is the only file that calls LVGL. They meet
# kode through kode_internal.h, the table the Active Object takes, the post
# it offers and the call prologue.
#
# kode is public because an app that names this component writes against
# kode.h. kscreen and kui are private: the screen is built with them, and an
# app that draws through kode.h never names an LVGL type.
idf_component_register(
    SRCS "src/kode_widget.c" "src/kode_canvas.c" "src/kode_png.c" "src/kode_compass.c" "src/kode_screen.c"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES kode
    PRIV_REQUIRES kscreen kui kbsp kdrv khal klog kqp
)

# kode_internal.h compiles only where this is defined: here, privately.
target_compile_definitions(${COMPONENT_LIB} PRIVATE KODE_INTERNAL_ALLOWED)
