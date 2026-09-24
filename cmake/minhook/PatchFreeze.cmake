set(_TSUKUYOMI_MINHOOK_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_tsukuyomi_replace_once text begin_marker end_marker replacement out_var)
    string(FIND "${text}" "${begin_marker}" _begin)
    if(_begin EQUAL -1)
        message(FATAL_ERROR "")
    endif()
    string(SUBSTRING "${text}" ${_begin} -1 _rest)
    string(FIND "${_rest}" "${end_marker}" _end_rel)
    if(_end_rel EQUAL -1)
        message(FATAL_ERROR "")
    endif()

    string(LENGTH "${begin_marker}" _begin_len)
    math(EXPR _after_begin "${_begin} + ${_begin_len}")
    string(SUBSTRING "${text}" ${_after_begin} -1 _tail)
    string(FIND "${_tail}" "${begin_marker}" _again)
    if(NOT _again EQUAL -1)
        message(FATAL_ERROR "")
    endif()
    math(EXPR _end "${_begin} + ${_end_rel}")
    string(SUBSTRING "${text}" 0 ${_begin} _head)
    string(SUBSTRING "${text}" ${_end} -1 _foot)
    set(${out_var} "${_head}${replacement}${_foot}" PARENT_SCOPE)
endfunction()

function(tsukuyomi_patch_minhook_freeze in_file out_file)
    file(READ "${in_file}" _src)
    string(REPLACE "\r\n" "\n" _src "${_src}")
    file(READ "${_TSUKUYOMI_MINHOOK_PATCH_DIR}/frozen_threads.inc" _types)
    file(READ "${_TSUKUYOMI_MINHOOK_PATCH_DIR}/freeze.inc" _funcs)
    string(REPLACE "\r\n" "\n" _types "${_types}")
    string(REPLACE "\r\n" "\n" _funcs "${_funcs}")

    _tsukuyomi_replace_once("${_src}"
        "// Suspended threads for Freeze()/Unfreeze().\n"
        "//-------------------------------------------------------------------------\n// Global Variables:"
        "${_types}\n" _src)

    _tsukuyomi_replace_once("${_src}"
        "//-------------------------------------------------------------------------\nstatic BOOL EnumerateThreads(PFROZEN_THREADS pThreads)"
        "//-------------------------------------------------------------------------\nstatic MH_STATUS EnableHookLL(UINT pos, BOOL enable)"
        "${_funcs}" _src)

    set(_old "")
    if(EXISTS "${out_file}")
        file(READ "${out_file}" _old)
    endif()
    if(NOT _old STREQUAL _src)
        file(WRITE "${out_file}" "${_src}")
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${in_file}"
        "${_TSUKUYOMI_MINHOOK_PATCH_DIR}/frozen_threads.inc"
        "${_TSUKUYOMI_MINHOOK_PATCH_DIR}/freeze.inc"
        "${_TSUKUYOMI_MINHOOK_PATCH_DIR}/PatchFreeze.cmake")
endfunction()
