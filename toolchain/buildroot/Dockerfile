FROM debian/eol:jessie
RUN apt-get update -qq && \
    apt-get install -y -qq --force-yes build-essential gcc g++ make patch perl python python3 \
        wget cpio unzip rsync bc file bzip2 xz-utils ncurses-dev texinfo gawk bison flex \
        libssl-dev ca-certificates git && \
    rm -rf /var/lib/apt/lists/*
