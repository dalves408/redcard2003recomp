"""Configure the upstream host template for the verified RedCard USA XBE."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
template = root / 'tools/xboxrecomp/templates/new-game'
cmake = (template / 'CMakeLists.txt').read_text(encoding='utf-8')
cmake = cmake.replace('project(your_game_recomp C)', 'project(redcard_recomp C)')
cmake = cmake.replace('project(redcard_recomp C)', 'project(redcard_recomp C)\nif(MSVC)\n    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")\nendif()')
cmake = cmake.replace('    src/main.c\n', '    src/main.c\n    src/redcard_runtime.c\n')
cmake = cmake.replace('    src/redcard_runtime.c\n', '    src/redcard_runtime.c\n    src/redcard_debug.c\n')
cmake = cmake.replace('    src/redcard_debug.c\n', '    src/redcard_debug.c\n    src/redcard_d3d11.c\n')
cmake = cmake.replace('    src/redcard_runtime.c\n', '    src/redcard_runtime.c\n    src/redcard_crt.c\n')
cmake += '''
include("${CMAKE_CURRENT_SOURCE_DIR}/scripts/renderer-tests.cmake")
target_link_libraries(${PROJECT_NAME} PRIVATE d3dcompiler)
target_include_directories(${PROJECT_NAME} PRIVATE "${XBOXRECOMP_DIR}/src/kernel")
target_link_libraries(${PROJECT_NAME} PRIVATE pdh)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox-debug-kit/include")
target_compile_definitions(xbox_video PRIVATE REDCARD_WINDOW_HOOKS=1)
option(REDCARD_DEBUGKIT "Enable Xbox Debug Kit diagnostics" ON)
if(REDCARD_DEBUGKIT)
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox-debug-kit/cmake/xrdbg.cmake")
    xrdbg_enable(${PROJECT_NAME} STDERR_BUFFER 65536)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/scripts/debug-source-scope.cmake")
'''
cmake = cmake.replace('${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp', '${CMAKE_CURRENT_SOURCE_DIR}/tools/xboxrecomp')
cmake = cmake.replace('    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp\n', '    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp\n    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen\n')
(root / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
main = (template / 'src/main.c').read_text(encoding='utf-8')
main = main.replace('YOUR_GAME_NAME', 'RedCard 20-03')
main = main.replace('    setvbuf(stderr, NULL, _IONBF, 0);', '    setvbuf(stderr, NULL, _IONBF, 0);\n    extern void redcard_runtime_init(void);\n    redcard_runtime_init();')
main = main.replace('    redcard_runtime_init();', '    redcard_runtime_init();\n    if (strstr(lpCmdLine, "--input-check")) {\n        extern int redcard_input_check(void);\n        return redcard_input_check();\n    }')
main = main.replace('#define YOUR_GAME_ENTRY_POINT   0x00000000', '#define YOUR_GAME_ENTRY_POINT   0x001D5723')
main = main.replace(r'game\\Your Game Title\\default.xbe', r'game_files\\default.xbe')
main = main.replace(r'game\\Your Game Title', 'game_files')
main = main.replace('extern void xbe_entry_point(void);', '''/* Resolve the verified guest entry through the generated dispatch table. */
typedef void (*redcard_entry_t)(void);
extern redcard_entry_t recomp_lookup(uint32_t xbox_va);
extern int recomp_dispatch_init(void);''')
main = main.replace('    xbe_entry_point();', '''    redcard_entry_t entry = recomp_lookup(YOUR_GAME_ENTRY_POINT);
    if (!entry) {
        fprintf(stderr, "Missing recompiled entry point\\n");
        return 1;
    }
    entry();''')
main = main.replace('    setvbuf(stdout, NULL, _IONBF, 0);', '    extern void redcard_debug_init(void);\n    if (!strstr(lpCmdLine, "--input-check")) redcard_debug_init();\n#ifndef XRDBG\n    setvbuf(stdout, NULL, _IONBF, 0);')
main = main.replace('    setvbuf(stderr, NULL, _IONBF, 0);', '    setvbuf(stderr, NULL, _IONBF, 0);\n#endif')
main = main.replace('    /* Cleanup */', '    extern void recomp_exit_hook_run(const char *why);\n    recomp_exit_hook_run("guest returned");\n    /* Cleanup */')
main = main.replace('Title ID:    0x00000000', 'Title ID:    0x4D57000F').replace('Entry point: 0x00000000', 'Entry point: 0x001D5723')
main = main.replace('if (!strstr(lpCmdLine, "--input-check")) redcard_debug_init();', 'if (!strstr(lpCmdLine, "--input-check") && !strstr(lpCmdLine, "--runtime-check")) redcard_debug_init();')
main = main.replace('    g_xbox_mem_offset = xbox_GetMemoryOffset();', '    g_xbox_mem_offset = xbox_GetMemoryOffset();\n    if (strstr(lpCmdLine, "--runtime-check")) {\n        extern int redcard_crt_check(void);\n        int result=redcard_crt_check();\n        fflush(stdout);fflush(stderr);\n        return result;\n    }')
(root / 'src/main.c').write_text(main, encoding='utf-8')
