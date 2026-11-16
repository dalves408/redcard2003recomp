add_executable(redcard_d3d11_smoke EXCLUDE_FROM_ALL tests/d3d11_smoke.c src/redcard_d3d11.c "${XBOXRECOMP_DIR}/src/kernel/nv2a_vsh_interp.c")
target_include_directories(redcard_d3d11_smoke PRIVATE "${XBOXRECOMP_DIR}/src/kernel" "${XBOXRECOMP_DIR}/src")
target_link_libraries(redcard_d3d11_smoke PRIVATE d3d11 dxgi dxguid d3dcompiler)
add_executable(redcard_audio_pages EXCLUDE_FROM_ALL tests/audio_pages.c)
target_include_directories(redcard_audio_pages PRIVATE "${XBOXRECOMP_DIR}/src/apu")

# Standalone runtime regressions share the patched sources but stay out of game builds.
enable_testing()
foreach(check nv2a_vsh nv2a_combiner nv2a_ffp_backend)
    add_subdirectory("${XBOXRECOMP_DIR}/tests/${check}" "${CMAKE_BINARY_DIR}/checks/${check}" EXCLUDE_FROM_ALL)
endforeach()
add_test(NAME redcard-hardware-rendering COMMAND redcard_d3d11_smoke)
# The smoke test reads guest memory right after each present sync. The game's
# pipelined readback (d8f8c80) finishes that copy one flip later, so every check
# read 0; test the synchronous path it is written for.
set_tests_properties(redcard-hardware-rendering PROPERTIES ENVIRONMENT "RECOMP_SYNC_PIPELINE=0")
add_test(NAME redcard-audio-pages COMMAND redcard_audio_pages)
add_test(NAME redcard-input COMMAND redcard_recomp --input-check)
add_test(NAME redcard-crt COMMAND redcard_recomp --runtime-check)
set_tests_properties(redcard-input redcard-crt PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

enable_language(CXX)
add_executable(input_map_test EXCLUDE_FROM_ALL "${XBOXRECOMP_DIR}/tests/input_map/test_main.c")
add_executable(input_map_cpp EXCLUDE_FROM_ALL "${XBOXRECOMP_DIR}/tests/input_map/cpp_smoke.cpp")
target_link_libraries(input_map_test PRIVATE xbox_input_map)
target_link_libraries(input_map_cpp PRIVATE xbox_input_map)
add_test(NAME input_map_test COMMAND input_map_test)
add_test(NAME input_map_cpp COMMAND input_map_cpp)

add_custom_target(redcard_checks DEPENDS redcard_d3d11_smoke redcard_audio_pages nv2a-vsh-test nv2a-vsh-decoded-test nv2a-combiner-test nv2a-ffp-backend-test nv2a-present-sync-test input_map_test input_map_cpp)
