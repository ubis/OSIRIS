FROM debian:trixie-slim
RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends build-essential gcc g++ make patch perl python3 \
        python3-setuptools python3-dev python3-pyelftools swig libgnutls28-dev libssl-dev \
        wget cpio unzip rsync bc file bzip2 xz-utils libncurses-dev gawk bison flex \
        ca-certificates git lzop && \
    rm -rf /var/lib/apt/lists/*
