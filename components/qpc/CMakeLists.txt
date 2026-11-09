# QP/C v8.1.4 (vendored from https://github.com/QuantumLeaps/qpc, GPL-3.0)
# plus the FreeRTOS port adapted for ESP-IDF SMP (see port/qf_port.c).
#
# Layering rule (kodeOS invariant 1): ONLY the kqp/kbsp components may
# depend on this component. Never include qpc.h above kqp.

idf_component_register(
    SRCS
        "port/qf_port.c"
        "qpc/src/qf/qep_hsm.c"
        "qpc/src/qf/qep_msm.c"
        "qpc/src/qf/qf_act.c"
        "qpc/src/qf/qf_defer.c"
        "qpc/src/qf/qf_dyn.c"
        "qpc/src/qf/qf_mem.c"
        "qpc/src/qf/qf_ps.c"
        "qpc/src/qf/qf_qact.c"
        "qpc/src/qf/qf_qeq.c"
        "qpc/src/qf/qf_qmact.c"
        "qpc/src/qf/qf_time.c"
    INCLUDE_DIRS
        "qpc/include"
        "port"
    REQUIRES
        freertos
)
