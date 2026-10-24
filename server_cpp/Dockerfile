FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    ghostscript \
    poppler-utils \
    imagemagick \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

RUN sed -i 's/rights="none" pattern="PDF"/rights="read|write" pattern="PDF"/g' /etc/ImageMagick-6/policy.xml || true

WORKDIR /workspace
