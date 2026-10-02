# wpe-browser: the static multi-call WPE WebKit program of Phoenix-RTOS (UI + WebProcess +
# NetworkProcess in one ELF). Added to the WebKit build by patch 0006 through
# -DPHOENIX_BROWSER_DIR=<this directory>; modelled on Tools/MiniBrowser/wpe/CMakeLists.txt.
#
# What a static program needs beyond WebKit's own link interface -- Mesa's EGL/GLES archives
# (epoxy resolves through eglGetProcAddress), glib-networking's OpenSSL module, the
# wayland_phoenix compat library (memfd_create over shmsrv), the static closure of every
# dependency and the --wrap options -- is appended to every C++ link by ../build-wpe.sh
# (CMAKE_CXX_STANDARD_LIBRARIES), so that it comes after all of WebCore's libraries.

set(WPEBrowser_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/wpe-browser.cpp
)

set(WPEBrowser_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${DERIVED_SOURCES_WPE_API_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
    ${WPEPlatform_DERIVED_SOURCES_DIR}
    ${WEBKIT_DIR}/WPEPlatform
)

# libWPEWebKit is static on Phoenix (patch 0006): the program links the frameworks it is made
# of (bmalloc, WTF, JavaScriptCore, WebCore: OBJECT libraries; PAL) itself, as WebKit's own
# WPEWebProcess/WPENetworkProcess do there.
set(WPEBrowser_FRAMEWORKS
    bmalloc
    WTF
    JavaScriptCore
    PAL
    WebCore
    WebKit
)

set(WPEBrowser_PRIVATE_LIBRARIES
    PNG::PNG
)

set(WPEBrowser_OUTPUT_NAME wpe-browser)

WEBKIT_EXECUTABLE_DECLARE(WPEBrowser)
WEBKIT_EXECUTABLE(WPEBrowser)

install(TARGETS WPEBrowser DESTINATION "${EXEC_INSTALL_DIR}")

# The export table: the symbols the shared objects wpe-browser loads may bind to (libphoenix's
# dlopen(); the injected bundle and web process extensions). Both linker options are needed:
# without --no-dynamic-linker ld creates no dynamic sections for a program that links no shared
# library, and --dynamic-list alone then does nothing. --require-defined makes a listed symbol
# that the link does not define an error instead of a dlopen() failure on the target.
set(WPEBrowser_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/wpe-browser.exports")
file(STRINGS "${WPEBrowser_EXPORTS}" _wpe_browser_export_lines REGEX "^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*;")
set(_wpe_browser_export_options)
foreach (_line IN LISTS _wpe_browser_export_lines)
    string(REGEX REPLACE "^[ \t]*([A-Za-z_][A-Za-z0-9_]*).*" "\\1" _symbol "${_line}")
    list(APPEND _wpe_browser_export_options "LINKER:--require-defined=${_symbol}")
endforeach ()
target_link_options(WPEBrowser PRIVATE
    "LINKER:--no-dynamic-linker"
    "LINKER:--dynamic-list=${WPEBrowser_EXPORTS}"
    ${_wpe_browser_export_options}
)
set_property(TARGET WPEBrowser APPEND PROPERTY LINK_DEPENDS "${WPEBrowser_EXPORTS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WPEBrowser_EXPORTS}")
