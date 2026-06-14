# vekterm build factory.
#
# Builds everything in Docker so you need no local cross-toolchain, and exports
# the deployable artifacts:
#
#   docker build --target artifacts --output type=local,dest=out .
#     -> out/kernel.img   boots straight into vekterm on a PiTrex (baremetal)
#     -> out/vekterm.img  flashable SD-card image
#
# `make docker` runs exactly this. See docs/DEPLOY.md.

FROM debian:trixie-slim AS build

# gcc-arm-none-eabi: baremetal ARM cross-compiler (produces kernel.img).
# build-essential: host gcc for the unit tests. mtools+fdisk+curl: SD image.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        build-essential \
        make \
        git \
        ca-certificates \
        curl \
        mtools \
        fdisk \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Pin the pitrex revision for reproducible images:
#   docker build --build-arg PITREX_REF=<sha-or-branch> ...
ARG PITREX_REPO=https://github.com/gtoal/pitrex
ARG PITREX_REF=master
RUN git clone "${PITREX_REPO}" pitrex && git -C pitrex checkout "${PITREX_REF}"

COPY . /build/vekterm

# 1. Host unit tests (system gcc) — fail the build if the protocol decode breaks.
RUN cd vekterm && make check

# 2. Cross-compile the baremetal receiver against libpitrex and assemble the
#    flashable SD image: `make image` builds kernel.img then vekterm.img.
#    Calibrate without editing source via --build-arg VEKTERM_CFLAGS='-DVT_...'.
ARG RPI_FW_REF=stable
ARG VEKTERM_CFLAGS=
RUN cd vekterm && RPI_FW_REF="${RPI_FW_REF}" \
    make image PITREX_DIR=/build/pitrex EXTRA_CFLAGS="${VEKTERM_CFLAGS}"

# ---- export stage: nothing but the artifacts ----------------------------
FROM scratch AS artifacts
COPY --from=build /build/vekterm/kernel.img /kernel.img
COPY --from=build /build/vekterm/vekterm.img /vekterm.img
