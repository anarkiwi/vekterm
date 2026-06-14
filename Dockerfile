# Multi-stage build for vekterm.
#
# The build stage compiles the pure C99 receiver, runs the unit tests + the
# PiTrex backend syntax check, and links a static binary. The runtime stage is
# a bare `scratch` image holding just that binary.
#
# This builds the hardware-independent (stub) server: it decodes frames and
# reports them, which is exactly what you want in a container (CI, `--dry-run`,
# piping frames from a sender). Driving a real Vectrex needs libpitrex and GPIO
# access on the Pi itself — see docs/DEPLOY.md, not Docker.
#
#   docker build -t vekterm .
#   docker run --rm vekterm --selftest
#   printf '...frame bytes...' | docker run --rm -i vekterm --dry-run --input -

# ---- build + test -------------------------------------------------------
FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Run the full check suite, then statically link the server so the runtime image
# needs no libc. The in-build --selftest confirms the static binary actually runs.
RUN make check \
    && make LDFLAGS=-static vekterm \
    && ./vekterm --selftest

# ---- minimal runtime ----------------------------------------------------
FROM scratch AS runtime

COPY --from=build /src/vekterm /vekterm
# Run unprivileged by default (the container's job is to decode, not to touch
# hardware). Override with `--user 0` + `--device` if reading a real serial port.
USER 65534:65534
ENTRYPOINT ["/vekterm"]
CMD ["--help"]
