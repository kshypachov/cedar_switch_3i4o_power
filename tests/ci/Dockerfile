# Linux build environment for the simulated test tier.
#
# Zephyr's POSIX architecture (native_sim) does not build on macOS, and the
# development hosts here are macOS. Rather than weaken the test strategy to
# whatever runs natively, the sim tier runs in this container under Colima.
#
# Deliberately minimal: native_sim compiles with the container's own gcc, so
# the Zephyr SDK is not needed. That keeps the image around 1 GB instead of
# the ~10 GB of zephyrprojectrtos/ci, which matters when every developer and
# every CI job pulls it.
#
# Base is trixie rather than bookworm for two hard requirements: Zephyr needs
# CMake >= 3.28 (bookworm ships 3.25) and Python >= 3.12 (bookworm ships 3.11).
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        device-tree-compiler \
        file \
        git \
        gperf \
        ninja-build \
        python3 \
        python3-dev \
        python3-venv \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Python tooling in its own venv: Debian marks the system interpreter as
# externally managed, and keeping the list here rather than pointing at the
# mounted tree makes the image reproducible from this context alone.
ENV VIRTUAL_ENV=/opt/zvenv
RUN python3 -m venv "$VIRTUAL_ENV"
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# Mirrors zephyr/scripts/requirements-base.txt plus what twister needs, minus
# the parts only used against real hardware (pyocd and friends) and the heavy
# SBOM and vision extras.
RUN pip install --no-cache-dir --upgrade pip \
    && pip install --no-cache-dir \
        anytree \
        canopen \
        colorama \
        intelhex \
        jsonschema \
        junitparser \
        natsort \
        packaging \
        ply \
        psutil \
        pyelftools \
        pykwalify \
        pyserial \
        pytest \
        PyYAML \
        semver \
        tabulate \
        tqdm \
        west

# native_sim compiles with the container's own gcc; announcing that keeps
# Zephyr from hunting for an SDK this image deliberately does not carry.
ENV ZEPHYR_TOOLCHAIN_VARIANT=host

WORKDIR /ws
