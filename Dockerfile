FROM ubuntu
USER root
RUN apt-get update
RUN apt-get install --no-install-recommends -y bash bc bison bzip2 ca-certificates findutils flex gcc git libc6-dev libfdt-dev libffi-dev libglib2.0-dev libpixman-1-dev locales make meson ninja-build pkgconf python3 python3-venv sed tar python3-dev

RUN apt-get install -y curl
RUN curl https://sh.rustup.rs -sSf | bash -s -- -y

RUN echo 'source $HOME/.cargo/env' >> $HOME/.bashrc

WORKDIR /qemu

# Build with: docker build -t tinyhook .
# Run with: docker run -it --rm -v `pwd`:/qemu tinyhook /bin/bash
# Configure:
# RUN ./configure --static --enable-linux-user --disable-system --disable-brlapi --disable-gtk --disable-libiscsi --disable-libnfs --disable-pa --disable-rbd --disable-sdl --disable-snappy --disable-vnc --disable-gio --disable-tools