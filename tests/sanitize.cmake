# AddressSanitizer and UndefinedBehaviorSanitizer for the code under test.
#
# Included by each host project's main/CMakeLists.txt, after
# idf_component_register(). It does nothing unless KODE_SANITIZE is set in the
# environment, which `tools/host_tests.sh --sanitize` does and which builds in
# its own build_san directory so the plain build is never disturbed.
#
# Only the `main` component is instrumented: that is every SDK source under
# test, the fakes and the cases. ESP-IDF's own components stay as they are,
# because instrumenting them trips ASan on the linker-section arrays that
# esp_system walks at start-up (esp_sys_init_fn_*), which is IDF's pattern and
# not ours. The link flag is PUBLIC so it reaches the executable.
if(DEFINED ENV{KODE_SANITIZE})
    set(_kode_san -fsanitize=address,undefined -fno-sanitize-recover=undefined
                  -fno-omit-frame-pointer -g)
    target_compile_options(${COMPONENT_LIB} PRIVATE ${_kode_san})
    target_link_options(${COMPONENT_LIB} PUBLIC -fsanitize=address,undefined)
endif()
