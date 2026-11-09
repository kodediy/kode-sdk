# kscreen is named for LVGL on the glass - the display, the flush handed to
# AO_Display, the touch feed and the render pass - and it brings kui and LVGL
# with it from main/idf_component.yml.
idf_component_register(
    SRCS "display_lvgl_main.c" "display_lvgl_ao.c"
    INCLUDE_DIRS "."
    REQUIRES kode
    PRIV_REQUIRES kscreen
)
