# Refuses an app image too big for the mailbox, before `idf.py flash` writes it.
#
# The mailbox (app_pending) keeps its last sector for kodeOS's intent flag, so
# an image there may reach up to that sector and no further. A bigger app can
# still fit the apps region; it goes in over the console, not through the
# mailbox.
#   usage: cmake -DKBSP_BIN=<app.bin> -DKBSP_MAX=<bytes> -P check_mailbox_size.cmake

if(NOT DEFINED KBSP_BIN OR NOT DEFINED KBSP_MAX)
    message(FATAL_ERROR "kbsp: check_mailbox_size.cmake needs KBSP_BIN and KBSP_MAX")
endif()
if(NOT EXISTS "${KBSP_BIN}")
    message(FATAL_ERROR "kbsp: ${KBSP_BIN} does not exist")
endif()

file(SIZE "${KBSP_BIN}" size)
math(EXPR max "${KBSP_MAX}")
if(size GREATER max)
    math(EXPR over "${size} - ${max}")
    message(FATAL_ERROR
        "kbsp: ${KBSP_BIN} is ${size} bytes and the mailbox holds ${max}, "
        "${over} too many, so idf.py flash cannot write it. Install it over "
        "the console instead, with the kodeOS repository's tools/kcli.py: "
        "`kcli.py -p <port> install ${KBSP_BIN} --dst app`.")
endif()
message(STATUS "kbsp: ${size} of the mailbox's ${max} bytes")
