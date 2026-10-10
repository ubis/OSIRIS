FROM debian:trixie

ARG BINUTILS_VER=2.44
ARG GCC_VER=15.2.0
ARG NEWLIB_VER=4.5.0.20241231
ARG TARGET=tic6x-elf
ARG PREFIX=/opt/c6x
ARG JOBS=4

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        build-essential ca-certificates wget xz-utils texinfo bison flex \
        libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev python3 make file >/dev/null && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/tc
ENV PATH=${PREFIX}/bin:$PATH

# binutils
RUN wget -q https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz && \
    tar xf binutils-${BINUTILS_VER}.tar.xz && mkdir b-binutils && cd b-binutils && \
    ../binutils-${BINUTILS_VER}/configure --target=${TARGET} --prefix=${PREFIX} \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng --disable-sim >/dev/null && \
    make -j${JOBS} >/dev/null 2>&1 && make install >/dev/null && \
    cd /tmp/tc && rm -rf binutils-* b-binutils

# GCC (C only) + newlib, single multilib for little-endian C674x
RUN wget -q https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz && \
    wget -q https://sourceware.org/pub/newlib/newlib-${NEWLIB_VER}.tar.gz && \
    tar xf gcc-${GCC_VER}.tar.xz && tar xf newlib-${NEWLIB_VER}.tar.gz && \
    mkdir b-gcc && cd b-gcc && \
    ../gcc-${GCC_VER}/configure --target=${TARGET} --prefix=${PREFIX} \
        --with-arch=c674x --disable-multilib --enable-languages=c \
        --with-newlib --with-headers=/tmp/tc/newlib-${NEWLIB_VER}/newlib/libc/include \
        --disable-nls --disable-shared --disable-threads --disable-libssp \
        --disable-libquadmath --disable-libgomp --disable-libatomic --disable-lto \
        --without-isl >/dev/null && \
    make -j${JOBS} all-gcc >/dev/null 2>&1 && make install-gcc >/dev/null && \
    make -j${JOBS} all-target-libgcc >/dev/null 2>&1 && make install-target-libgcc >/dev/null && \
    cd /tmp/tc && mkdir b-newlib && cd b-newlib && \
    ../newlib-${NEWLIB_VER}/configure --target=${TARGET} --prefix=${PREFIX} \
        --disable-multilib --disable-newlib-supplied-syscalls --disable-nls >/dev/null && \
    make -j${JOBS} >/dev/null 2>&1 && make install >/dev/null && \
    cd / && rm -rf /tmp/tc

RUN ${TARGET}-gcc --version | head -1 && ${TARGET}-ld --version | head -1
WORKDIR /src
