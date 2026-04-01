cmake_minimum_required(VERSION 3.10)

# Enable CMAKE_MSVC_RUNTIME_LIBRARY (requires CMP0091 before project())
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Statically link the MSVC C Runtime so the binary is self-contained
# and does not depend on vcruntime140.dll / ucrtbase.dll being present.
# Must be set before project() so it applies to all targets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(tmux)

# Set C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(TMUX_USE_UTF8PROC "Use utf8proc for UTF-8 handling" OFF)
option(TMUX_USE_PDCURSES "Use PDCurses for terminal handling" OFF)

# Platform detection
if(WIN32)

    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-D_WINSOCK_DEPRECATED_NO_WARNINGS)
    add_definitions(-DPLATFORM_WINDOWS=1)

    # Prioritize our surrogate headers
    include_directories(BEFORE
        ${CMAKE_SOURCE_DIR}/compat/win32
        ${CMAKE_SOURCE_DIR}/compat/win32/sys
        ${CMAKE_SOURCE_DIR}/compat/win32/netinet
        ${CMAKE_SOURCE_DIR}/compat/win32/arpa
    )

    include_directories(${CMAKE_SOURCE_DIR}/compat)
    include_directories(${CMAKE_SOURCE_DIR})
endif()

# Find dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# libevent - required
# Pass -DLIBEVENT_ROOT=<path> to cmake if libevent is not on the system search path.
# Example: cmake -B build -DLIBEVENT_ROOT=C:/Users/you/libevent-install
set(LIBEVENT_ROOT "" CACHE PATH "Path to libevent install prefix (contains lib/ and include/)")

if(LIBEVENT_ROOT)
    find_package(Libevent CONFIG QUIET HINTS "${LIBEVENT_ROOT}" NO_DEFAULT_PATH)
    if(NOT Libevent_FOUND)
        find_package(Libevent CONFIG QUIET HINTS "${LIBEVENT_ROOT}")
    endif()
else()
    find_package(Libevent CONFIG QUIET)
    if(NOT Libevent_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBEVENT libevent>=2.1)
        endif()
    endif()
endif()

if(NOT Libevent_FOUND AND NOT LIBEVENT_FOUND)
    message(FATAL_ERROR
        "libevent not found.\n"
        "Build libevent with -DEVENT__LIBRARY_TYPE=STATIC and pass its install "
        "prefix to cmake:\n"
        "  cmake -B build -DLIBEVENT_ROOT=C:/path/to/libevent-install\n"
        "See docs/windows/BUILDING.md for full instructions.")
endif()

message(STATUS "Found libevent")
if(Libevent_FOUND)
    get_target_property(LIBEVENT_INCLUDE_DIRS libevent::core INTERFACE_INCLUDE_DIRECTORIES)
endif()

# PDCurses (optional, for terminfo)
if(TMUX_USE_PDCURSES)
    find_path(PDCURSES_INCLUDE_DIR curses.h
        HINTS ENV PDCURSES_DIR
        PATH_SUFFIXES include
    )
    find_library(PDCURSES_LIBRARY
        NAMES pdcurses pdcursesmod
        HINTS ENV PDCURSES_DIR
        PATH_SUFFIXES lib
    )
    if(PDCURSES_INCLUDE_DIR AND PDCURSES_LIBRARY)
        message(STATUS "Found PDCurses: ${PDCURSES_LIBRARY}")
        set(HAVE_PDCURSES ON)
    else()
        message(STATUS "PDCurses not found - will use VT passthrough mode")
        set(HAVE_PDCURSES OFF)
    endif()
endif()

# Windows libraries
set(WIN32_LIBRARIES
    ws2_32
    advapi32
    user32
    shell32
    kernel32
)

# Source files from Makefile.am
set(TMUX_SOURCES
    alerts.c
    arguments.c
    attributes.c
    cfg.c
    client.c
    cmd-attach-session.c
    cmd-bind-key.c
    cmd-break-pane.c
    cmd-capture-pane.c
    cmd-choose-tree.c
    cmd-command-prompt.c
    cmd-confirm-before.c
    cmd-copy-mode.c
    cmd-detach-client.c
    cmd-display-menu.c
    cmd-display-message.c
    cmd-display-panes.c
    cmd-find-window.c
    cmd-find.c
    cmd-if-shell.c
    cmd-join-pane.c
    cmd-kill-pane.c
    cmd-kill-server.c
    cmd-kill-session.c
    cmd-kill-window.c
    cmd-list-buffers.c
    cmd-list-clients.c
    cmd-list-keys.c
    cmd-list-panes.c
    cmd-list-sessions.c
    cmd-list-windows.c
    cmd-load-buffer.c
    cmd-lock-server.c
    cmd-move-window.c
    cmd-new-session.c
    cmd-new-window.c
    cmd-paste-buffer.c
    cmd-pipe-pane.c
    cmd-parse.c
    cmd-queue.c
    cmd-refresh-client.c
    cmd-rename-session.c
    cmd-rename-window.c
    cmd-resize-pane.c
    cmd-resize-window.c
    cmd-respawn-pane.c
    cmd-respawn-window.c
    cmd-rotate-window.c
    cmd-run-shell.c
    cmd-save-buffer.c
    cmd-select-layout.c
    cmd-select-pane.c
    cmd-select-window.c
    cmd-send-keys.c
    cmd-server-access.c
    cmd-set-buffer.c
    cmd-set-environment.c
    cmd-set-option.c
    cmd-show-environment.c
    cmd-show-messages.c
    cmd-show-options.c
    cmd-show-prompt-history.c
    cmd-source-file.c
    cmd-suspend-pane.c
    cmd-split-window.c
    cmd-swap-pane.c
    cmd-swap-window.c
    cmd-switch-client.c
    cmd-unbind-key.c
    cmd-wait-for.c
    cmd.c
    colour.c
    control-notify.c
    control.c
    environ.c
    file.c
    format.c
    format-draw.c
    grid-reader.c
    grid-view.c
    grid.c
    hyperlinks.c
    input-keys.c
    input.c
    job.c
    key-bindings.c
    key-string.c
    layout-custom.c
    layout-set.c
    layout.c
    log.c
    menu.c
    mode-tree.c
    names.c
    notify.c
    options-table.c
    options.c
    paste.c
    popup.c
    proc.c
    regsub.c
    resize.c
    screen-redraw.c
    screen-write.c
    screen.c
    server-acl.c
    server-client.c
    server-fn.c
    server.c
    session.c
    spawn.c
    status.c
    style.c
    tty-acs.c
    tty-features.c
    tty-keys.c
    tty-term.c
    tty.c
    utf8-combined.c
    utf8.c
    window-buffer.c
    window-client.c
    window-clock.c
    window-copy.c
    window-customize.c
    window-tree.c
    window.c
    xmalloc.c
)

set(COMPAT_SOURCES
    compat/asprintf.c
    compat/fgetln.c
    compat/freezero.c
    compat/getopt_long.c
    compat/getprogname.c
    compat/reallocarray.c
    compat/recallocarray.c
    compat/strlcat.c
    compat/strlcpy.c
    compat/strndup.c
    compat/strsep.c
    compat/strtonum.c
    compat/unvis.c
    compat/vis.c
    compat/osdep-win32.c
    compat/win32-dirent.c
    compat/win32-io.c
    compat/win32-job.c
    compat/win32-jobctl.c
    compat/win32-session-log.c
    compat/win32-misc.c
    compat/win32-process.c
    compat/win32-pty.c
    compat/win32-regex.c
    compat/win32-signal.c
    compat/win32-socket.c
    compat/win32-stubs.c
    compat/win32-term.c
    compat/win32-wcwidth.c
    compat/win32-clipboard.c
    compat/base64.c
    compat/imsg.c
    compat/imsg-buffer.c
    compat/strcasestr.c
    compat/err.c
    compat/setenv.c
    # compat/closefrom.c  -- conflict with win32-process.c
    compat/getline.c
    compat/explicit_bzero.c
)

# Utility library
add_library(tmux-util STATIC
    ${COMPAT_SOURCES}
    xmalloc.c
)
target_include_directories(tmux-util PUBLIC
    ${CMAKE_SOURCE_DIR}/compat/win32
    ${CMAKE_SOURCE_DIR}/compat
    ${CMAKE_SOURCE_DIR}
    $<TARGET_PROPERTY:libevent::core,INTERFACE_INCLUDE_DIRECTORIES>
)
# Apply force-include to tmux-util (all compat sources need win32.h)
if(MSVC)
    target_compile_options(tmux-util PRIVATE /FI"win32.h")
else()
    target_compile_options(tmux-util PRIVATE -include win32.h)
endif()

# Main executable
add_executable(tmux tmux.c ${TMUX_SOURCES})
target_include_directories(tmux PRIVATE
    $<TARGET_PROPERTY:libevent::core,INTERFACE_INCLUDE_DIRECTORIES>
)
# Apply force-include to tmux (all tmux core sources need win32.h)
if(MSVC)
    target_compile_options(tmux PRIVATE /FI"win32.h")
else()
    target_compile_options(tmux PRIVATE -include win32.h)
endif()
target_link_libraries(tmux
    tmux-util
    libevent::core
    libevent::extra
    ${WIN32_LIBRARIES}
)

if(HAVE_PDCURSES)
    target_link_libraries(tmux ${PDCURSES_LIBRARY})
    target_compile_definitions(tmux PRIVATE HAVE_PDCURSES=1)
endif()

# Signal proxy helper (PERM-02: ConPTY boundary Ctrl+Break relay)
# Standalone Win32 console app — does NOT use libevent or win32.h.
# Force-include is applied per-target above, so this target is clean by default.
add_executable(tmux-signal-proxy compat/win32-signal-proxy/main.c)
target_compile_definitions(tmux-signal-proxy PRIVATE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
