FROM ubuntu:20.04

ENV LANG C.UTF-8

RUN apt-get update \
    && apt-get -y upgrade \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y \
         build-essential git cmake

RUN git clone --depth=1 https://github.com/adamstark/Gist.git \
    && cd Gist \
    && mkdir build \
    && cd build \
    && cmake .. \
    && cmake --build .

    # && apt-get autoremove -y \
    # && apt-get clean -y \
    # && rm -rf /var/lib/apt/lists/*

RUN apt-get update \
    && apt-get -y upgrade \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y \
         neovim

WORKDIR /analyser
