# xrdbg is host glue. Keep its macros/includes off the 58 MB of generated game
# code, which does not use them, so debug-kit updates need no full recompile.
get_target_property(_rc_defs ${PROJECT_NAME} COMPILE_DEFINITIONS)
if(_rc_defs)
    list(REMOVE_ITEM _rc_defs XRDBG=1 XRDBG_STDERR_BUFFER=65536)
    set_property(TARGET ${PROJECT_NAME} PROPERTY COMPILE_DEFINITIONS "${_rc_defs}")
endif()
get_target_property(_rc_opts ${PROJECT_NAME} COMPILE_OPTIONS)
if(_rc_opts)
    list(REMOVE_ITEM _rc_opts /Zi)
    set_property(TARGET ${PROJECT_NAME} PROPERTY COMPILE_OPTIONS "${_rc_opts}")
endif()
get_target_property(_rc_incs ${PROJECT_NAME} INCLUDE_DIRECTORIES)
list(REMOVE_ITEM _rc_incs "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox-debug-kit/include")
set_property(TARGET ${PROJECT_NAME} PROPERTY INCLUDE_DIRECTORIES "${_rc_incs}")
set_property(SOURCE src/main.c src/redcard_debug.c src/redcard_videoclip.c APPEND PROPERTY INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox-debug-kit/include")
if(REDCARD_DEBUGKIT)
    set_property(SOURCE src/main.c src/redcard_debug.c src/redcard_videoclip.c
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox-debug-kit/src/xrdbg.c"
        APPEND PROPERTY COMPILE_DEFINITIONS XRDBG=1 XRDBG_STDERR_BUFFER=65536)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox-debug-kit/src/xrdbg.c"
        APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox-debug-kit/include")
endif()
