cmake_minimum_required(VERSION 3.28)
project(eta_jupyter LANGUAGES CXX)

add_executable(eta_jupyter
    src/eta/jupyter/main_jupyter.cpp
    src/eta/jupyter/eta_interpreter.cpp
    src/eta/jupyter/kernel_config.cpp
    src/eta/jupyter/display.cpp
    src/eta/jupyter/magics.cpp
    src/eta/jupyter/comm/heap_comm.cpp
    src/eta/jupyter/comm/disasm_comm.cpp
    src/eta/jupyter/comm/actors_comm.cpp
    src/eta/jupyter/comm/dag_comm.cpp
    src/eta/jupyter/comm/tensor_comm.cpp
    ../interpreter/src/eta/interpreter/repl_wrap.cpp
)

target_include_directories(eta_jupyter
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../interpreter/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../session/src
)

target_link_libraries(eta_jupyter
    PRIVATE eta_core eta_torch eta_nng eta_stats eta_log
            xeus xeus-zmq nlohmann_json::nlohmann_json
)

target_compile_definitions(eta_jupyter PRIVATE
    ETA_HAS_TORCH=1 ETA_HAS_NNG=1 ETA_HAS_STATS=1
    ETA_STDLIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../stdlib"
    ETA_JUPYTER_SOURCE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

eta_copy_torch_dlls(eta_jupyter)
eta_copy_nng_dlls(eta_jupyter)
eta_copy_log_dlls(eta_jupyter)

# xeus, xeus-zmq, and libzmq are shared in the FetchContent build and must be
# available next to eta_jupyter on Windows.
if(WIN32)
    # Copy in-tree FetchContent DLLs so eta_jupyter is runnable from the build tree.
    foreach(_dep IN ITEMS xeus xeus-zmq libzmq)
        if(TARGET ${_dep})
            add_custom_command(TARGET eta_jupyter POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_dep}>
                    $<TARGET_FILE_DIR:eta_jupyter>
                COMMENT "Copying ${_dep} DLL to eta_jupyter output dir"
            )
        endif()
    endforeach()
    # libuv is a direct runtime dependency of xeus-zmq 4.x. It arrives via
    # find_package (vcpkg / system) so it is an imported target, not in-tree.
    if(TARGET LibUV::LibUV)
        add_custom_command(TARGET eta_jupyter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LibUV::LibUV>
                $<TARGET_FILE_DIR:eta_jupyter>
            COMMENT "Copying libuv (uv.dll) to eta_jupyter output dir"
        )
    endif()
endif()

if(NOT MSVC)
    target_link_libraries(eta_jupyter PRIVATE stdc++ m)
endif()

install(TARGETS eta_jupyter RUNTIME DESTINATION bin)

# ── Bundle xeus / xeus-zmq / libzmq / libuv runtime libs into the install tree ─
#
# These libraries are linked dynamically by eta_jupyter and must be
# co-located with the executable in the release bundle, otherwise launching
# the kernel fails with:
#   * Windows : STATUS_DLL_NOT_FOUND (exit 0xC0000135 / -1073741515)
#   * Linux   : "error while loading shared libraries: libxeus.so.N"
#   * macOS   : "dyld: Library not loaded: @rpath/libxeus.N.dylib"
#
# Required DLLs (Windows):
#   libxeus.dll / xeus.dll        -- xeus core
#   libxeus-zmq.dll / xeus-zmq.dll-- xeus ZMQ transport
#   libzmq-*.dll                  -- ZeroMQ
#   uv.dll                        -- libuv (direct dep of xeus-zmq 4.x)
#   OpenSSL::Crypto (optional)    -- if libzmq built with CURVE
#
# This must work for two build paths:
#   1. FetchContent build (cmake/FetchXeus.cmake) -> non-imported targets
#      named  xeus / xeus-zmq / libzmq.  xeus's own install rules are
#      suppressed (CMAKE_SKIP_INSTALL_RULES) so we copy build artefacts.
#   2. find_package(... CONFIG) build (vcpkg / conda / system) -> imported
#      targets named  xeus::xeus / xeus::xeus-zmq.  libzmq and libuv have
#      no CMake target and must be located on disk.

# Resolve a runtime library to *either* a CMake target name (when the dep
# was built in-tree via FetchContent and we want the install rule to use a
# generator expression) *or* an absolute path on disk (when the dep was
# located via find_package and IMPORTED_LOCATION is known at configure time).
#
# Sets:
#   ${out_target}  -> non-empty target name iff a non-imported target exists
#   ${out_file}    -> absolute path iff an imported target's location was found
# Both empty if no candidate matches.
function(_eta_resolve_runtime_lib out_target out_file)
    set(${out_target} "" PARENT_SCOPE)
    set(${out_file}   "" PARENT_SCOPE)
    set(_candidates ${ARGN})
    foreach(_t IN LISTS _candidates)
        if(NOT TARGET ${_t})
            continue()
        endif()
        get_target_property(_imported ${_t} IMPORTED)
        if(NOT _imported)
            # In-tree (FetchContent) target: defer to generator expression.
            set(${out_target} "${_t}" PARENT_SCOPE)
            return()
        endif()
        # Imported target: resolve a real path now so we can glob siblings.
        foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL DEBUG NOCONFIG)
            get_target_property(_cfg_loc ${_t} IMPORTED_LOCATION_${_cfg})
            if(_cfg_loc)
                set(${out_file} "${_cfg_loc}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
        get_target_property(_loc ${_t} IMPORTED_LOCATION)
        if(_loc)
            set(${out_file} "${_loc}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

_eta_resolve_runtime_lib(_eta_xeus_tgt     _eta_xeus_lib     xeus     xeus::xeus)
_eta_resolve_runtime_lib(_eta_xeus_zmq_tgt _eta_xeus_zmq_lib xeus-zmq xeus::xeus-zmq)
_eta_resolve_runtime_lib(_eta_libzmq_tgt   _eta_libzmq_lib   libzmq)

# OpenSSL::Crypto is a transitive runtime dependency of xeus-zmq.  On clean
# target machines (no system OpenSSL) the .dll/.so is also absent.
set(_eta_openssl_crypto_tgt "")
set(_eta_openssl_crypto_lib "")
if(TARGET OpenSSL::Crypto)
    _eta_resolve_runtime_lib(_eta_openssl_crypto_tgt _eta_openssl_crypto_lib OpenSSL::Crypto)
endif()

# libuv (uv.dll) is a direct runtime dependency of xeus-zmq 4.x.
# It is found by xeus-zmq's own find_package(LibUV) and arrives here as an
# imported target.  Without it, eta_jupyter fails with STATUS_DLL_NOT_FOUND.
set(_eta_libuv_tgt "")
set(_eta_libuv_lib "")
if(TARGET LibUV::LibUV)
    _eta_resolve_runtime_lib(_eta_libuv_tgt _eta_libuv_lib LibUV::LibUV)
endif()

if(WIN32)
    # Windows: DLLs go next to the .exe; loader searches the application
    # directory before PATH so no further configuration is required.

    # 1. In-tree targets -> use $<TARGET_FILE:...> so multi-config works.
    foreach(_t IN ITEMS
            "${_eta_xeus_tgt}"
            "${_eta_xeus_zmq_tgt}"
            "${_eta_libzmq_tgt}"
            "${_eta_openssl_crypto_tgt}"
            "${_eta_libuv_tgt}")
        if(_t)
            install(FILES "$<TARGET_FILE:${_t}>" DESTINATION bin)
        endif()
    endforeach()

    # 2. Imported targets -> install the resolved file path.  Filter to .dll
    #    only because vcpkg's OpenSSL config sometimes reports the .lib import
    #    library in IMPORTED_LOCATION rather than the runtime DLL.
    foreach(_lib IN ITEMS
            "${_eta_xeus_lib}"
            "${_eta_xeus_zmq_lib}"
            "${_eta_libzmq_lib}"
            "${_eta_openssl_crypto_lib}"
            "${_eta_libuv_lib}")
        if(_lib AND _lib MATCHES "\\.dll$")
            install(FILES "${_lib}" DESTINATION bin)
        endif()
    endforeach()

    # 3. vcpkg / conda do not export libzmq as a CMake target.  Locate its DLL
    #    by name in the same directory as xeus.dll (vcpkg installs runtime
    #    DLLs into <triplet>/bin/, with import libs in <triplet>/lib/).
    if(_eta_xeus_lib AND NOT _eta_libzmq_lib AND NOT _eta_libzmq_tgt)
        get_filename_component(_eta_xeus_dir "${_eta_xeus_lib}" DIRECTORY)
        set(_eta_search_dirs "${_eta_xeus_dir}")
        if(EXISTS "${_eta_xeus_dir}/../bin")
            list(APPEND _eta_search_dirs "${_eta_xeus_dir}/../bin")
        endif()
        foreach(_dir IN LISTS _eta_search_dirs)
            file(GLOB _eta_zmq_dlls
                "${_dir}/libzmq*.dll"
                "${_dir}/zmq*.dll")
            foreach(_d IN LISTS _eta_zmq_dlls)
                install(FILES "${_d}" DESTINATION bin)
            endforeach()
            # libsodium is a common libzmq runtime dep when CURVE is enabled.
            file(GLOB _eta_sodium_dlls "${_dir}/libsodium*.dll")
            foreach(_d IN LISTS _eta_sodium_dlls)
                install(FILES "${_d}" DESTINATION bin)
            endforeach()
        endforeach()
    endif()

    # 4. libuv disk-search fallback for vcpkg / conda (no CMake target).
    #    uv.dll is a direct runtime dependency of xeus-zmq 4.x.
    if(_eta_xeus_lib AND NOT _eta_libuv_lib AND NOT _eta_libuv_tgt)
        get_filename_component(_eta_xeus_dir "${_eta_xeus_lib}" DIRECTORY)
        set(_eta_uv_search_dirs "${_eta_xeus_dir}")
        if(EXISTS "${_eta_xeus_dir}/../bin")
            list(APPEND _eta_uv_search_dirs "${_eta_xeus_dir}/../bin")
        endif()
        foreach(_dir IN LISTS _eta_uv_search_dirs)
            file(GLOB _eta_uv_dlls "${_dir}/uv.dll" "${_dir}/libuv*.dll")
            foreach(_d IN LISTS _eta_uv_dlls)
                install(FILES "${_d}" DESTINATION bin)
            endforeach()
        endforeach()
    endif()

    # 5. OpenSSL runtime fallback for package-manager builds where
    #    OpenSSL::Crypto resolves to an import library (.lib) but not the
    #    runtime DLL path at configure time.
    if((NOT _eta_openssl_crypto_tgt) OR
       (_eta_openssl_crypto_lib AND NOT _eta_openssl_crypto_lib MATCHES "\\.dll$"))
        set(_eta_openssl_search_dirs "")
        foreach(_seed IN ITEMS
                "${_eta_xeus_lib}"
                "${_eta_xeus_zmq_lib}"
                "${_eta_openssl_crypto_lib}")
            if(_seed)
                get_filename_component(_seed_dir "${_seed}" DIRECTORY)
                if(EXISTS "${_seed_dir}")
                    list(APPEND _eta_openssl_search_dirs "${_seed_dir}")
                endif()
                if(EXISTS "${_seed_dir}/../bin")
                    list(APPEND _eta_openssl_search_dirs "${_seed_dir}/../bin")
                endif()
            endif()
        endforeach()
        if(DEFINED OPENSSL_ROOT_DIR AND OPENSSL_ROOT_DIR AND EXISTS "${OPENSSL_ROOT_DIR}/bin")
            list(APPEND _eta_openssl_search_dirs "${OPENSSL_ROOT_DIR}/bin")
        endif()
        list(REMOVE_DUPLICATES _eta_openssl_search_dirs)
        foreach(_dir IN LISTS _eta_openssl_search_dirs)
            file(GLOB _eta_openssl_dlls "${_dir}/libcrypto*.dll" "${_dir}/libssl*.dll")
            foreach(_d IN LISTS _eta_openssl_dlls)
                install(FILES "${_d}" DESTINATION bin)
            endforeach()
        endforeach()
    endif()

elseif(APPLE)
    # macOS: ship .dylib siblings into lib/, set rpath relative to executable.
    set_property(TARGET eta_jupyter APPEND PROPERTY
        INSTALL_RPATH "@loader_path/../lib")

    # 1. In-tree targets: use TARGET_FILE + TARGET_SONAME_FILE generator exprs.
    foreach(_t IN ITEMS
            "${_eta_xeus_tgt}"
            "${_eta_xeus_zmq_tgt}"
            "${_eta_libzmq_tgt}"
            "${_eta_openssl_crypto_tgt}"
            "${_eta_libuv_tgt}")
        if(_t)
            install(FILES "$<TARGET_FILE:${_t}>" DESTINATION lib)
            install(FILES "$<TARGET_SONAME_FILE:${_t}>" DESTINATION lib OPTIONAL)
        endif()
    endforeach()

    # 2. Imported targets: glob versioned dylib siblings of the resolved file.
    foreach(_lib IN ITEMS
            "${_eta_xeus_lib}"
            "${_eta_xeus_zmq_lib}"
            "${_eta_libzmq_lib}"
            "${_eta_openssl_crypto_lib}"
            "${_eta_libuv_lib}")
        if(_lib)
            get_filename_component(_dir  "${_lib}" DIRECTORY)
            get_filename_component(_name "${_lib}" NAME_WE)
            file(GLOB _siblings "${_dir}/${_name}*.dylib")
            foreach(_s IN LISTS _siblings)
                install(FILES "${_s}" DESTINATION lib)
            endforeach()
        endif()
    endforeach()

elseif(UNIX)
    # Linux: ship .so plus all SONAME-versioned siblings (lib*.so.N,
    # lib*.so.N.M.K) so the dynamic linker's SONAME lookup resolves.
    # RPATH = $ORIGIN/../lib makes the loader find them next to the binary.
    set_property(TARGET eta_jupyter APPEND PROPERTY
        INSTALL_RPATH "$ORIGIN/../lib")

    # 1. In-tree targets: TARGET_FILE is the real file, TARGET_SONAME_FILE
    #    is the SONAME symlink (e.g. libxeus.so.6 -> libxeus.so.6.0.5).
    #    The dynamic linker resolves by SONAME, so both must be present.
    foreach(_t IN ITEMS
            "${_eta_xeus_tgt}"
            "${_eta_xeus_zmq_tgt}"
            "${_eta_libzmq_tgt}"
            "${_eta_openssl_crypto_tgt}"
            "${_eta_libuv_tgt}")
        if(_t)
            install(FILES "$<TARGET_FILE:${_t}>" DESTINATION lib)
            install(FILES "$<TARGET_SONAME_FILE:${_t}>" DESTINATION lib OPTIONAL)
        endif()
    endforeach()

    # 2. Imported targets: glob versioned .so siblings of the resolved file.
    foreach(_lib IN ITEMS
            "${_eta_xeus_lib}"
            "${_eta_xeus_zmq_lib}"
            "${_eta_libzmq_lib}"
            "${_eta_openssl_crypto_lib}"
            "${_eta_libuv_lib}")
        if(_lib)
            get_filename_component(_dir  "${_lib}" DIRECTORY)
            get_filename_component(_name "${_lib}" NAME_WE)
            file(GLOB _siblings "${_dir}/${_name}.so" "${_dir}/${_name}.so.*")
            foreach(_s IN LISTS _siblings)
                install(FILES "${_s}" DESTINATION lib)
            endforeach()
        endif()
    endforeach()
endif()

install(FILES resources/kernel.json.in DESTINATION share/jupyter/kernels/eta)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/logo-32x32.png")
    install(FILES resources/logo-32x32.png DESTINATION share/jupyter/kernels/eta)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/logo-64x64.png")
    install(FILES resources/logo-64x64.png DESTINATION share/jupyter/kernels/eta)
endif()
