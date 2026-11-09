# kbsp's part in the project: where `idf.py flash` writes an app.
#
# ESP-IDF's `flash` target writes a bootloader, the partition table, a blank
# otadata and the app at the first app partition, and on the Kode Dot that
# partition is kodeOS's factory slot: flashing an app that way puts the app
# where kodeOS was, and the board then starts the app with nothing to start it
# from. An app goes in through the mailbox instead (app_pending): kodeOS adopts
# it at the next boot, installs it in the apps region and runs it.
#
# So in the app role, kbsp_flash_to_mailbox() runs once the project's
# CMakeLists has been read, and leaves `flash` and `app-flash` one image each:
# the app, at app_pending's offset in the board's partition table. Everything
# that reads those targets follows - `idf.py flash`, the VS Code extension's
# Flash button and its debug launch, which read build/flasher_args.json. kodeOS
# itself is the OS role and keeps ESP-IDF's targets as they are.
# `idf.py erase-flash` is not a flash target and still erases the whole chip.
#
# The same role moves ESP-IDF's size check. ESP-IDF checks an image against the
# smallest app partition, and here those are kodeOS's three 4 MB slots, where an
# app never goes: an app lives in the apps region, so that is the partition it
# is checked against. And `flash` refuses an image the mailbox cannot hold,
# which an install over the console still can.

# The partition an app is flashed to, by its name in partitions.csv.
set(KBSP_MAILBOX_PARTITION "app_pending")
# The apps region's subtype in partitions.csv (KAPPS_PART_SUBTYPE in kapps_fmt.h).
set(KBSP_APPS_SUBTYPE "0x40")
# The most an image in the mailbox can hold: where the intent sector starts
# (KAPPS_PENDING_INTENT_OFF in kapps_fmt.h).
set(KBSP_MAILBOX_IMAGE_MAX "0x5FF000")

if(CONFIG_KODEOS_BOOT_ROLE_APP)
    # ESP-IDF's partition_table component defines the command this replaces; a
    # definition made before it would be the one replaced.
    if(NOT COMMAND partition_table_add_check_size_target)
        message(FATAL_ERROR
            "kbsp: partition_table's project_include.cmake has not run yet, so "
            "the app's size check cannot be moved to the apps region")
    endif()

    # ESP-IDF's partition_table_add_check_size_target(), with an app partition
    # replaced by the apps region. ESP-IDF calls it once, for app_check_size.
    function(partition_table_add_check_size_target target_name)
        cmake_parse_arguments(CMD "" "BINARY_PATH;PARTITION_TYPE;PARTITION_SUBTYPE" "DEPENDS" ${ARGN})
        if(NOT CMD_PARTITION_TYPE STREQUAL "app" OR CMD_PARTITION_SUBTYPE)
            message(FATAL_ERROR
                "kbsp: a size check against ${CMD_PARTITION_TYPE} ${CMD_PARTITION_SUBTYPE} "
                "is not one kbsp knows how to move")
        endif()

        partition_table_get_partition_info(apps
            "--partition-type data --partition-subtype ${KBSP_APPS_SUBTYPE}" "offset")
        if(NOT apps)
            message(FATAL_ERROR
                "kbsp: the partition table has no apps region (data, ${KBSP_APPS_SUBTYPE}), "
                "so this app has nowhere to be installed")
        endif()

        idf_build_get_property(python PYTHON)
        idf_build_get_property(table_bin PARTITION_TABLE_BIN_PATH)
        add_custom_target(${target_name}
            COMMAND ${python} ${PARTITION_TABLE_CHECK_SIZES_TOOL_PATH}
                --offset ${PARTITION_TABLE_OFFSET}
                partition --type data --subtype ${KBSP_APPS_SUBTYPE}
                ${table_bin} ${CMD_BINARY_PATH}
            DEPENDS ${CMD_DEPENDS} partition_table_bin)
    endfunction()
endif()

function(kbsp_flash_to_mailbox)
    if(NOT CONFIG_KODEOS_BOOT_ROLE_APP)
        return()
    endif()

    idf_build_get_property(app_bin PROJECT_BIN)
    partition_table_get_partition_info(mailbox
        "--partition-name ${KBSP_MAILBOX_PARTITION}" "offset")
    if(NOT mailbox)
        message(FATAL_ERROR
            "kbsp: the partition table has no ${KBSP_MAILBOX_PARTITION} partition, "
            "so this app has nowhere to be flashed")
    endif()

    idf_build_get_property(build_dir BUILD_DIR)
    add_custom_target(kbsp_mailbox_check_size
        COMMAND ${CMAKE_COMMAND}
            -DKBSP_BIN=${build_dir}/${app_bin}
            -DKBSP_MAX=${KBSP_MAILBOX_IMAGE_MAX}
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_mailbox_size.cmake
        DEPENDS gen_project_binary)

    foreach(target flash app-flash)
        if(TARGET ${target})
            add_dependencies(${target} kbsp_mailbox_check_size)
            set_property(TARGET ${target} PROPERTY IMAGES "${mailbox} ${app_bin}")
            set_property(TARGET ${target} PROPERTY FLASH_FILE
                "\"${mailbox}\" : \"${app_bin}\"")
            set_property(TARGET ${target} PROPERTY FLASH_ENTRY
                "\"app\" : { \"offset\" : \"${mailbox}\", \"file\" : \"${app_bin}\", \"encrypted\" : \"false\" }")
        endif()
    endforeach()
    message(STATUS "kbsp: idf.py flash writes this app to the mailbox at "
                   "${mailbox}; kodeOS installs it at the next boot")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL kbsp_flash_to_mailbox)
