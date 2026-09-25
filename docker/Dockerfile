# syntax=docker/dockerfile:1
#
# CODA Frame Builder - self-contained runtime image
#
# Three-stage build:
#   base    - Ubuntu 22.04 + build tools + E2SAR dependency bundle (Boost, gRPC,
#             protobuf, Abseil) installed under /usr/local by the upstream .deb
#   build   - compiles ET, E2SAR and coda-fb, all installed into /usr/local
#   runtime - slim image carrying only /usr/local plus runtime shared libraries
#
# Build:  docker build -f docker/Dockerfile -t coda-fb:latest .
# See docker/README.md for deployment and runtime parameters.

# ---------------------------------------------------------------------------
# Stage: base
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS base

# E2SAR publishes a dependency bundle (Boost 1.89.0 with Boost.URL and
# Boost.Log, gRPC 1.74.1, protobuf and Abseil) per release and distro, under
# /usr/local. coda-fb's meson.build explicitly searches /usr/local/lib64,
# /usr/local/lib and /usr/local/include, so this is the layout it expects.
ARG E2SAR_DEPS_VER=0.4.0rc1
ARG E2SAR_DEPS_DISTRO=ubuntu-22.04
ARG E2SAR_DEPS_URL=https://github.com/JeffersonLab/E2SAR/releases/download/E2SAR-${E2SAR_DEPS_VER}-main-${E2SAR_DEPS_DISTRO}/e2sar-deps_${E2SAR_DEPS_VER}_amd64.deb

ENV DEBIAN_FRONTEND=noninteractive \
    LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64 \
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig \
    PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
    BOOST_ROOT=/usr/local

RUN apt-get -yq update && apt-get -yq install --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        libtool \
        cmake \
        ninja-build \
        pkg-config \
        git \
        git-lfs \
        wget \
        ca-certificates \
        python3-pip \
        python3-dev \
        protobuf-compiler \
        libglib2.0-dev \
        libssl-dev \
        libsystemd-dev \
        libre2-dev \
        libnuma-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# E2SAR unconditionally builds its pybind11 bindings (src/meson.build does a
# plain subdir('pybind') with no option to disable it), so the Python
# development files and pybind11 headers are build requirements even though
# coda-fb only consumes the C++ library. Without python3-dev, meson fails at
# src/pybind/meson.build with "Python dependency not found".
RUN pip3 install --no-cache-dir meson pybind11

# coda-fb's meson.build adds -L/usr/local/lib64 and an rpath entry for it.
# Create it so the rpath resolves even when the deps land in /usr/local/lib.
RUN mkdir -p /usr/local/lib64

RUN wget -q -O /tmp/e2sar-deps.deb "${E2SAR_DEPS_URL}" \
    && dpkg -i /tmp/e2sar-deps.deb \
    && rm -f /tmp/e2sar-deps.deb \
    && ldconfig

# ---------------------------------------------------------------------------
# Stage: build
# ---------------------------------------------------------------------------
FROM base AS build

# --- ET -------------------------------------------------------------------
# ET gates ALL frame building in coda-fb, not just ET output: meson only
# compiles e2sar_reassembler_framebuilder.cpp (and defines ENABLE_FRAME_BUILDER)
# when the ET library is found. Pure C, no JDK installed so the optional
# et_jni target is skipped. INSTALL_DIR forces the /usr/local layout instead of
# ET's default $CODA/Linux-x86_64.
ARG ET_REPO=https://github.com/JeffersonLab/et.git
ARG ET_REF=v16.6.0

RUN git clone --depth 1 --branch "${ET_REF}" "${ET_REPO}" /src/et \
    && cmake -S /src/et -B /src/et/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DINSTALL_DIR=/usr/local \
    && cmake --build /src/et/build --target install --parallel "$(nproc)" \
    && ldconfig \
    && test -f /usr/local/include/et.h \
    && test -x /usr/local/bin/et_start

# --- E2SAR ----------------------------------------------------------------
# Pinned by ref so the image matches the API that coda-fb compiles against.
# There is no final v0.4.0 tag upstream; v0.4.0rc1 is the latest candidate.
#
# Three things matter in the clone below:
#  1. The ref is checked out BEFORE submodules are initialised. Passing
#     --recurse-submodules to git clone resolves submodules against the DEFAULT
#     branch's .gitmodules, not the tag's, and on main those entries are
#     git@github.com: SSH URLs.
#  2. insteadOf rewrites any remaining SSH URL to HTTPS. A container has no SSH
#     key, and git fails with "cannot run ssh: No such file or directory".
#  3. Only udplbd2 is initialised: E2SAR's meson.build reads
#     ../udplbd2/proto/loadbalancer/loadbalancer.proto to generate the gRPC
#     stubs. docs and wiki are documentation repositories the build never uses.
#
# git-lfs is not exercised: .gitattributes only marks *.tar.gz as LFS and
# nothing under that filter is needed to compile.
ARG E2SAR_REPO=https://github.com/JeffersonLab/E2SAR.git
ARG E2SAR_REF=v0.4.0rc1

RUN git config --global url."https://github.com/".insteadOf "git@github.com:" \
    && git clone "${E2SAR_REPO}" /src/e2sar \
    && cd /src/e2sar \
    && git checkout "${E2SAR_REF}" \
    && git submodule update --init --recursive udplbd2 \
    && test -f udplbd2/proto/loadbalancer/loadbalancer.proto

# The first setup plus the -std=c++11 strip is the upstream workaround from
# E2SAR's own Dockerfile.cli; the second setup regenerates with it applied.
RUN cd /src/e2sar \
    && meson setup -Dpkg_config_path="${PKG_CONFIG_PATH}" --prefix /usr/local build \
    && sed -i 's/-std=c++11//g' build/build.ninja \
    && meson setup --reconfigure -Dpkg_config_path="${PKG_CONFIG_PATH}" --prefix /usr/local build \
    && meson compile -C build -j "$(nproc)" \
    && meson install -C build \
    && ldconfig \
    && pkg-config --exists e2sar

# --- coda-fb --------------------------------------------------------------
ARG CODA_FB_BUILDTYPE=release

COPY meson.build meson_options.txt /src/coda-fb/
COPY src/ /src/coda-fb/src/

# coda-fb's meson.build treats a prefix of exactly /usr/local as "no prefix
# given" and then falls back to $CODA/Linux-x86_64/bin, or to $HOME/.local/bin
# when CODA is unset. Neither is under /usr/local, which is all the runtime
# stage copies. Pass a non-sentinel prefix so the plain "custom prefix" branch
# is taken, then place the binaries in /usr/local/bin next to everything else.
ARG CODA_FB_PREFIX=/opt/coda-fb

RUN cd /src/coda-fb \
    && env -u CODA meson setup builddir \
        --buildtype="${CODA_FB_BUILDTYPE}" \
        --prefix "${CODA_FB_PREFIX}" \
        -Dpkg_config_path="${PKG_CONFIG_PATH}" \
    && meson compile -C builddir -j "$(nproc)" \
    && meson install -C builddir \
    && install -m 0755 "${CODA_FB_PREFIX}/bin/coda-fb" /usr/local/bin/coda-fb \
    && install -m 0755 "${CODA_FB_PREFIX}/bin/evio_event_parser" /usr/local/bin/evio_event_parser \
    && ldconfig

# Fail the build here rather than at deploy time if the frame builder was not
# compiled in (missing ET) or a shared library cannot be resolved.
# test -x comes first on purpose: ldd on a missing file writes to stderr and
# prints nothing on stdout, so "! ldd ... | grep -q 'not found'" succeeds for a
# binary that was never installed.
RUN grep -q 'ENABLE_FRAME_BUILDER' /src/coda-fb/builddir/compile_commands.json \
        || (echo "FATAL: ENABLE_FRAME_BUILDER not defined - ET was not detected" >&2; exit 1) \
    && test -x /usr/local/bin/coda-fb \
    && test -x /usr/local/bin/evio_event_parser \
    && ! ldd /usr/local/bin/coda-fb | grep -q 'not found' \
    && ! ldd /usr/local/bin/evio_event_parser | grep -q 'not found'

# ---------------------------------------------------------------------------
# Stage: runtime
# ---------------------------------------------------------------------------
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64 \
    PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin

RUN apt-get -yq update && apt-get -yq install --no-install-recommends \
        ca-certificates \
        libglib2.0-0 \
        libssl3 \
        libsystemd0 \
        libre2-9 \
        libc-ares2 \
        libnuma1 \
        zlib1g \
        iproute2 \
        iputils-ping \
        netcat-openbsd \
        tini \
    && rm -rf /var/lib/apt/lists/*

# Boost, gRPC, protobuf, Abseil, ET, E2SAR, coda-fb and evio_event_parser all
# live under /usr/local by construction.
COPY --from=build /usr/local /usr/local
RUN ldconfig

COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Verify the runtime image can actually resolve everything it needs.
RUN test -x /usr/local/bin/coda-fb \
    && test -x /usr/local/bin/evio_event_parser \
    && ! ldd /usr/local/bin/coda-fb | grep -q 'not found' \
    && ! ldd /usr/local/bin/evio_event_parser | grep -q 'not found' \
    && coda-fb --help > /dev/null \
    && evio_event_parser --help > /dev/null

# Default location for .evio output; mount a volume over it.
RUN mkdir -p /data
VOLUME ["/data"]

LABEL org.opencontainers.image.title="CODA Frame Builder" \
      org.opencontainers.image.description="EJFAT frame aggregator for Jefferson Lab CODA DAQ" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.source="https://github.com/JeffersonLab/coda-fb"

# tini reaps the et_start child and forwards signals; the entrypoint converts
# SIGTERM into the SIGINT that coda-fb actually handles.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
