FROM debian/eol:jessie
RUN apt-get update -qq && \
    apt-get install -y -qq --force-yes gcc gcc-arm-none-eabi binutils-arm-none-eabi make bc python && \
    rm -rf /var/lib/apt/lists/*
