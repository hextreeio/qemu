#!/bin/bash
docker build -t tinyhook .
docker run -it --rm -v `pwd`:/qemu tinyhook /bin/bash -c './configure --enable-linux-user --disable-system --disable-brlapi --disable-gtk --disable-libiscsi --disable-libnfs --disable-pa --disable-rbd --disable-sdl --disable-snappy --disable-vnc --disable-gio --disable-tools && make -j$(nproc) && make install'
