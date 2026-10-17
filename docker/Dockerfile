FROM debian:trixie-slim
RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
        bc \
        bison \
        build-essential \
        bzip2 \
        ca-certificates \
        cmake \
        cpio \
        file \
        flex \
        g++ \
        g++-multilib \
        gawk \
        gcc \
        gcc-multilib \
        git \
        libgnutls28-dev \
        libncurses-dev \
        libssl-dev \
        lzip \
        make \
        patch \
        perl \
        python3 \
        python3-dev \
        python3-pyelftools \
        python3-setuptools \
        rsync \
        swig \
        unzip \
        wget \
        xz-utils \
        zstd && \
    rm -rf /var/lib/apt/lists/*
