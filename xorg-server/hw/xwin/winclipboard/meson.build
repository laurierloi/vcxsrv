srcs_windows_clipboard = [
    'winclipboard.h',
    'textconv.c',
    'image.c',
    'outgoing.c',
    'thread.c',
    'wndproc.c',
    'xevents.c',
]

xwin_clipboard = static_library(
    'XWinclipboard',
    srcs_windows_clipboard,
    include_directories: [inc, include_directories('..')],
    c_args: '-DHAVE_XWIN_CONFIG_H',
    dependencies: [
        dependency('xcb'),
        dependency('xcb-aux'),
        dependency('xcb-icccm'),
        dependency('xcb-xfixes'),
        socket_dep,
    ],
)

srcs_xwinclip = [
    'xwinclip.c',
    'debug.c',
]

executable(
    'xwinclip',
    srcs_xwinclip,
    include_directories: [inc, include_directories('..')],
    c_args: '-DHAVE_XWIN_CONFIG_H',
    dependencies: [dependency('xcb'), dependency('xcb-aux'),
                   dependency('xcb-icccm'), dependency('xcb-xfixes')],
    link_with: xwin_clipboard,
    link_args: ['-lgdi32', '-lpthread', '-lole32', '-lwindowscodecs', '-luuid'],
    install: true,
)

xwinclip_man = configure_file(
    input: 'xwinclip.man',
    output: 'xwinclip.1',
    configuration: manpage_config,
)
install_man(xwinclip_man)

# Explicit test target: never starts a server or accesses the user clipboard as
# part of a normal build. Default runtime uses a separate Windows window station.
executable(
    'clipboard-integration-test',
    'tests/integration-test.c',
    include_directories: [inc, include_directories('..')],
    c_args: '-DHAVE_XWIN_CONFIG_H',
    dependencies: [dependency('xcb'), dependency('xcb-aux'),
                   dependency('xcb-icccm'), dependency('xcb-xfixes')],
    link_with: xwin_clipboard,
    link_args: ['-lgdi32', '-lpthread', '-lole32', '-lwindowscodecs', '-luuid'],
    build_by_default: false,
)
