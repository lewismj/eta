include(FetchContent)

set(ETA_LIBCURL_VERSION "8.13.0" CACHE STRING
    "Pinned libcurl upstream version used by the eta-http sidecar package.")
set(ETA_LIBCURL_GIT_TAG "curl-8_13_0" CACHE STRING
    "Pinned libcurl upstream git tag used by the eta-http sidecar package.")

function(eta_http_fetch_libcurl)
    # Keep the documented v1 surface HTTP-focused.
    set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)

    set(USE_NGHTTP3 OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        set(CURL_ZLIB ON CACHE BOOL "" FORCE)
    else()
        message(STATUS "eta_http: ZLIB not found; building libcurl without zlib support")
        set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    endif()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)

    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
    elseif(APPLE)
        set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
    else()
        set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
        set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
    endif()

    FetchContent_Declare(
        libcurl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG        ${ETA_LIBCURL_GIT_TAG}
        GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(libcurl)

    if(NOT TARGET CURL::libcurl)
        message(FATAL_ERROR
            "Fetched libcurl did not expose the expected CURL::libcurl target.")
    endif()
endfunction()
