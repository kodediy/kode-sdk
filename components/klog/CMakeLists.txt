# klog - the only logging front-end of kodeOS (invariant 7).
idf_component_register(
    SRCS "src/klog.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES log esp_timer
)
