#!/bin/sh
# musl-gcc, but reporting musl's multiarch name: Python's configure checks that
# it matches the platform triplet (musl-gcc runs the host gcc, which says
# x86_64-linux-gnu).
if [ "$1" = "--print-multiarch" ]; then
    echo x86_64-linux-musl
    exit 0
fi
exec "${MUSL_CC:-musl-gcc}" "$@"
