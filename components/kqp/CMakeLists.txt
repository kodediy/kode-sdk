# kqp - QP/C integration: signals, pools, pub/sub startup, and the two things
# the framework needs from the machine (the tick and Q_onError). Those are
# framework support, not board support, which is why they are not in kbsp.
set(KQP_SRCS
    "src/kqp.c"
    "src/kqp_isr.c"
    "src/kqp_self.c"
    "src/kqp_timer.c"
    "src/kqp_worker.c"
    "src/kqp_bsp.c"
    "src/kqp_tick.c")
# The Active Object watchdog (its judge and its supervisor) exists only when
# the switch is on.
if(CONFIG_KQP_AO_WATCH)
    list(APPEND KQP_SRCS "src/kqp_watch_judge.c" "src/kqp_watch.c")
endif()

idf_component_register(
    SRCS ${KQP_SRCS}
    INCLUDE_DIRS "include"
    # khal is public rather than private for a mechanical reason: the public
    # header kqp_signals.h includes khal_sync.h, and a public header that
    # includes another component's header makes that component public too.
    # It includes it because the blocking request events carry a khal_sem_t
    # (`done`, signalled by the owner once the answer is written), and khal's
    # is the only semaphore type kqp is allowed to name - rule 1 keeps
    # freertos/ below kdrv, so SemaphoreHandle_t is not in this component's
    # vocabulary.
    REQUIRES qpc khal
    PRIV_REQUIRES kdefs klog
)
