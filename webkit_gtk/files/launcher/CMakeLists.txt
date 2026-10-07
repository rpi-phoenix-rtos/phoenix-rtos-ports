# webkit-browser: the static multi-call WebKitGTK program of Phoenix-RTOS (UI + WebProcess +
# NetworkProcess in one ELF). Added to the WebKit build by webkit_wpe patch 0006 (shared) through
# -DPHOENIX_BROWSER_DIR=<this directory>; modelled on webkit_wpe's files/launcher/CMakeLists.txt.
#
# The UI is WebKit's MiniBrowser/gtk window (BSD-2-Clause: BrowserWindow, BrowserTab,
# BrowserDownloadsBar, BrowserSearchBox, BrowserSettingsDialog, BrowserCellRendererVariant),
# compiled from the WebKit tree, with our own main (webkit-browser.cpp) instead of MiniBrowser's
# main.c. Patch webkit-gtk/0102 gives that window two build-time hooks: BROWSER_DEFAULT_URL (new
# tabs and Home) and BROWSER_ENTRY_TO_URI (what the location entry's text loads).
#
# Everything a static program needs beyond WebKit's own link interface (Mesa's EGL/GLES archives,
# the GTK 3 closure, glib-networking's module, the wayland_phoenix compat library and the --wrap
# options) is appended to every C++ link by ../build-gtk.sh (CMAKE_CXX_STANDARD_LIBRARIES).

set(MiniBrowserGtk_DIR "${TOOLS_DIR}/MiniBrowser/gtk")

set(WebKitBrowser_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/webkit-browser.cpp

    ${MiniBrowserGtk_DIR}/BrowserCellRendererVariant.c
    ${MiniBrowserGtk_DIR}/BrowserDownloadsBar.c
    ${MiniBrowserGtk_DIR}/BrowserSearchBox.c
    ${MiniBrowserGtk_DIR}/BrowserSettingsDialog.c
    ${MiniBrowserGtk_DIR}/BrowserTab.c
    ${MiniBrowserGtk_DIR}/BrowserWindow.c
)

set(WebKitBrowser_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${MiniBrowserGtk_DIR}
    ${WebKitGTK_FRAMEWORK_HEADERS_DIR}
    ${WebKitGTK_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${CMAKE_SOURCE_DIR}/Source
)

set(WebKitBrowser_PRIVATE_DEFINITIONS
    GDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_22
    BROWSER_DEFAULT_URL="file:///usr/share/webkit-browser/start.html"
    BROWSER_ENTRY_TO_URI=phoenix_browser_entry_to_uri
)

# libwebkit2gtk is static on Phoenix (patch webkit-gtk/0101): the program links the frameworks it
# is made of itself, as WebKit's own WebKitWebProcess/WebKitNetworkProcess do there.
set(WebKitBrowser_FRAMEWORKS
    bmalloc
    WTF
    JavaScriptCore
    PAL
    WebCore
    WebKit
)

set(WebKitBrowser_PRIVATE_LIBRARIES
    GTK::GTK
)

set(WebKitBrowser_OUTPUT_NAME webkit-browser)

WEBKIT_EXECUTABLE_DECLARE(WebKitBrowser)
WEBKIT_EXECUTABLE(WebKitBrowser)
WEBKIT_ADD_TARGET_CXX_FLAGS(WebKitBrowser -Wno-unused-parameter)
# MiniBrowser's C sources: as Tools/MiniBrowser/gtk/CMakeLists.txt builds them
set_source_files_properties(
    ${MiniBrowserGtk_DIR}/BrowserCellRendererVariant.c
    ${MiniBrowserGtk_DIR}/BrowserDownloadsBar.c
    ${MiniBrowserGtk_DIR}/BrowserSearchBox.c
    ${MiniBrowserGtk_DIR}/BrowserSettingsDialog.c
    ${MiniBrowserGtk_DIR}/BrowserTab.c
    ${MiniBrowserGtk_DIR}/BrowserWindow.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-deprecated-declarations"
)

install(TARGETS WebKitBrowser DESTINATION "${EXEC_INSTALL_DIR}")

# The export table: the symbols the injected bundle (dlopen()ed by every web process) binds to.
# Both linker options are needed: without --no-dynamic-linker ld creates no dynamic sections for a
# program that links no shared library. --require-defined turns a listed symbol that the link does
# not define into a link error instead of a dlopen() failure on the target.
set(WebKitBrowser_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/webkit-browser.exports")
file(STRINGS "${WebKitBrowser_EXPORTS}" _webkit_browser_export_lines REGEX "^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*;")
set(_webkit_browser_export_options)
foreach (_line IN LISTS _webkit_browser_export_lines)
    string(REGEX REPLACE "^[ \t]*([A-Za-z_][A-Za-z0-9_]*).*" "\\1" _symbol "${_line}")
    list(APPEND _webkit_browser_export_options "LINKER:--require-defined=${_symbol}")
endforeach ()
target_link_options(WebKitBrowser PRIVATE
    "LINKER:--no-dynamic-linker"
    "LINKER:--dynamic-list=${WebKitBrowser_EXPORTS}"
    ${_webkit_browser_export_options}
)
set_property(TARGET WebKitBrowser APPEND PROPERTY LINK_DEPENDS "${WebKitBrowser_EXPORTS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WebKitBrowser_EXPORTS}")

# WebKit's built-in resources are GResource bundles compiled into libwebkit2gtk: nothing calls
# their <name>_get_resource() (lookups go through g_resources_lookup_data()), so from an ARCHIVE
# ld would never pull them in and their registering constructors would never run (the WPE build's
# B6 soak: "g_bytes_get_data: assertion 'bytes != NULL' failed"). Naming each one keeps it. The
# Web Inspector's bundle stays out: no developer extras in this browser.
set(WebKitBrowser_GRESOURCE_BUNDLES
    WebKitResourcesGResourceBundle
    ModernMediaControlsGResourceBundle
)
if (ENABLE_PDFJS)
    list(APPEND WebKitBrowser_GRESOURCE_BUNDLES
        PdfJSGResourceBundle
        PdfJSGResourceBundleExtras
    )
endif ()
foreach (_bundle IN LISTS WebKitBrowser_GRESOURCE_BUNDLES)
    target_link_options(WebKitBrowser PRIVATE "LINKER:--require-defined=${_bundle}_get_resource")
endforeach ()
