# vekterm build factory.
#
# Builds everything in Docker so you need no local cross-toolchain, and exports
# the deployable artifacts:
#
#   docker build --target artifacts --output type=local,dest=out .
#     -> out/kernel.img   boots straight into vekterm on a PiTrex (baremetal)
#     -> out/vekterm.img  flashable SD-card image
#
# libpitrex is vendored in third_party/libpitrex, so there is no clone and no
# build-time patching. `make docker` runs exactly this. See docs/DEPLOY.md.

FROM debian:trixie-slim AS build

# gcc-arm-none-eabi + newlib: baremetal ARM cross-compiler (produces kernel.img).
# build-essential: host gcc for the unit tests. mtools+fdisk+curl: SD image.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        build-essential \
        make \
        ca-certificates \
        curl \
        mtools \
        fdisk \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# 1. Host unit tests (system gcc) — fail the build if the protocol decode breaks.
RUN make check

# 2. Cross-compile the baremetal receiver against the vendored libpitrex and
#    assemble the flashable SD image: `make image` builds kernel.img then
#    vekterm.img. Calibrate via --build-arg VEKTERM_CFLAGS='-DVT_...'.
ARG RPI_FW_REF=stable
ARG VEKTERM_CFLAGS=
RUN RPI_FW_REF="${RPI_FW_REF}" make image EXTRA_CFLAGS="${VEKTERM_CFLAGS}"

# ---- export stage: nothing but the artifacts ----------------------------
FROM scratch AS artifacts
COPY --from=build /build/kernel.img /kernel.img
COPY --from=build /build/kernel7.img /kernel7.img
COPY --from=build /build/vekterm.img /vekterm.img
