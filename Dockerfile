FROM alpine:3.19
USER root

# Install build dependencies
RUN apk add --no-cache \
    bash bc bison bzip2 ca-certificates coreutils findutils flex \
    gcc g++ git make meson ninja pkgconf sed tar wget xz \
    musl-dev linux-headers \
    # Libraries for QEMU
    glib-dev glib-static \
    pixman-dev \
    dtc-dev \
    zlib-dev zlib-static \
    # Python build dependencies
    openssl-dev openssl-libs-static \
    libffi-dev \
    bzip2-dev bzip2-static \
    xz-dev \
    readline-dev readline-static \
    sqlite-dev sqlite-static \
    ncurses-dev ncurses-static \
    expat-dev expat-static \
    # System Python for QEMU build tools
    python3 python3-dev py3-pip \
    # Rust
    curl

# Install Rust
RUN curl https://sh.rustup.rs -sSf | bash -s -- -y
RUN echo 'source $HOME/.cargo/env' >> $HOME/.bashrc

# Build libffi static library (not available in Alpine repos)
WORKDIR /tmp
RUN wget https://github.com/libffi/libffi/releases/download/v3.4.4/libffi-3.4.4.tar.gz && \
    tar xzf libffi-3.4.4.tar.gz && \
    cd libffi-3.4.4 && \
    ./configure --prefix=/usr --disable-shared --enable-static CFLAGS="-fPIC" && \
    make -j$(nproc) && \
    make install && \
    cd .. && rm -rf libffi-3.4.4*

# Build Python 3.10 from source with static library
WORKDIR /tmp
RUN wget https://www.python.org/ftp/python/3.10.13/Python-3.10.13.tar.xz && \
    tar xf Python-3.10.13.tar.xz && \
    cd Python-3.10.13 && \
    ./configure \
        --prefix=/opt/python-static \
        --disable-shared \
        CFLAGS="-fPIC" && \
    make -j$(nproc) && \
    make install && \
    cd .. && \
    rm -rf Python-3.10.13 Python-3.10.13.tar.xz

# Install Python packages needed for QEMU build using system Python
RUN pip3 install tomli pycotap --break-system-packages

# Create pkg-config file for static Python embedding
RUN rm -f /opt/python-static/lib/pkgconfig/python3-embed.pc \
          /opt/python-static/lib/pkgconfig/python-3.10-embed.pc && \
    cat > /opt/python-static/lib/pkgconfig/python3-embed.pc << 'PKGEOF'
prefix=/opt/python-static
exec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include/python3.10

Name: Python
Description: Embed Python into your application (static)
Version: 3.10.13
Cflags: -I${includedir}
Libs: -L${libdir} -l:libpython3.10.a -lpthread -lm -lz -lffi -lutil
PKGEOF

# Create the symlink to match expected names
RUN ln -s python3-embed.pc /opt/python-static/lib/pkgconfig/python-3.10-embed.pc

# Set PKG_CONFIG_PATH but keep system Python for build tools
ENV PKG_CONFIG_PATH="/opt/python-static/lib/pkgconfig"

WORKDIR /qemu
