# Xvexa: an X server that shows its screen in a window on the Vexa desktop.
# tools/build-x11.sh copies this directory into xorg-server's hw/kdrive/vexa.
xvexa_server = executable(
    'Xvexa',
    ['xvexa.c'],
    include_directories: [
        inc,
        include_directories('../src')
    ],
    dependencies: common_dep,
    link_with: [
        libxserver_main,
        kdrive,
        libxserver_fb,
        libxserver,
        libxserver_config,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd,
    ],
    install: true,
)
