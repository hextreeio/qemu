FROM ubuntu
USER root
RUN apt-get update
RUN apt-get install --no-install-recommends -y bash bc bison bzip2 ca-certificates findutils flex gcc git libc6-dev libfdt-dev libffi-dev libglib2.0-dev libpixman-1-dev locales make meson ninja-build pkgconf python3 python3-venv sed tar python3-dev

RUN apt-get install -y curl
RUN curl https://sh.rustup.rs -sSf | bash -s -- -y

RUN echo 'source $HOME/.cargo/env' >> $HOME/.bashrc

WORKDIR /qemu
