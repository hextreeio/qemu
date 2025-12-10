#!/bin/bash
set -e

docker build -t tinyhook .

# Use -t only if we have a TTY
DOCKER_TTY=""
if [ -t 0 ]; then
    DOCKER_TTY="-it"
fi

docker run $DOCKER_TTY --rm -v `pwd`:/qemu tinyhook /bin/bash -c '
    source $HOME/.cargo/env
    export PKG_CONFIG_PATH="/opt/python-static/lib/pkgconfig:$PKG_CONFIG_PATH"
    
    # Clean previous build
    rm -rf build
    
    ./configure \
        --enable-linux-user \
        --disable-system \
        --disable-brlapi \
        --disable-gtk \
        --disable-libiscsi \
        --disable-libnfs \
        --disable-pa \
        --disable-rbd \
        --disable-sdl \
        --disable-snappy \
        --disable-vnc \
        --disable-gio \
        --disable-tools \
        --disable-docs \
        --disable-werror \
        --static
    
    make -j$(nproc)
    make install
'
