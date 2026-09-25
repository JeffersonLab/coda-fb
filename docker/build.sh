#!/usr/bin/env bash
#
# Build (and optionally publish) the CODA Frame Builder container image.
#
#   ./docker/build.sh                                  # build coda-fb:latest
#   ./docker/build.sh --tag coda-fb:v1.0.0
#   ./docker/build.sh --save coda-fb.tar.gz            # tarball for scp/docker load
#   ./docker/build.sh --push registry.example.org/daq  # build and push
#   ./docker/build.sh --e2sar-ref v0.4.0rc1 --et-ref v16.6.0
#
# Run from the repository root (or anywhere - the script locates it itself).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()  { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

TAG="coda-fb:latest"
PLATFORM="linux/amd64"
SAVE_FILE=""
PUSH_REGISTRY=""
NO_CACHE=""
PROGRESS="auto"

# Defaults match the versions coda-fb is developed against.
E2SAR_REF="v0.4.0rc1"
E2SAR_DEPS_VER="0.4.0rc1"
E2SAR_DEPS_DISTRO="ubuntu-22.04"
ET_REF="v16.6.0"
BUILDTYPE="release"

usage() {
    sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    cat <<USAGE

Options:
  -t, --tag TAG              Image tag (default: ${TAG})
      --platform PLATFORM    Target platform (default: ${PLATFORM})
      --e2sar-ref REF        E2SAR git ref to build (default: ${E2SAR_REF})
      --e2sar-deps-ver VER   E2SAR dependency bundle version (default: ${E2SAR_DEPS_VER})
      --e2sar-deps-distro D  Dependency bundle distro (default: ${E2SAR_DEPS_DISTRO})
      --et-ref REF           ET git ref to build (default: ${ET_REF})
      --buildtype TYPE       coda-fb meson buildtype (default: ${BUILDTYPE})
      --save FILE            Save the image to a gzipped tarball for transfer
      --push REGISTRY        Tag into REGISTRY and docker push
      --no-cache             Build without the layer cache
      --progress MODE        Docker build progress output (auto|plain|tty)
  -h, --help                 This message
USAGE
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--tag)                TAG="$2"; shift 2 ;;
        --platform)              PLATFORM="$2"; shift 2 ;;
        --e2sar-ref)             E2SAR_REF="$2"; shift 2 ;;
        --e2sar-deps-ver)        E2SAR_DEPS_VER="$2"; shift 2 ;;
        --e2sar-deps-distro)     E2SAR_DEPS_DISTRO="$2"; shift 2 ;;
        --et-ref)                ET_REF="$2"; shift 2 ;;
        --buildtype)             BUILDTYPE="$2"; shift 2 ;;
        --save)                  SAVE_FILE="$2"; shift 2 ;;
        --push)                  PUSH_REGISTRY="$2"; shift 2 ;;
        --no-cache)              NO_CACHE="--no-cache"; shift ;;
        --progress)              PROGRESS="$2"; shift 2 ;;
        -h|--help)               usage; exit 0 ;;
        *) fail "unknown option: $1 (try --help)" ;;
    esac
done

command -v docker > /dev/null 2>&1 || fail "docker not found on PATH"

# The E2SAR dependency bundle is published for amd64 only, so the image must be
# built for linux/amd64 regardless of the host. On Apple Silicon and other arm64
# hosts that means emulation, which is slow for the E2SAR compile.
HOST_ARCH="$(uname -m)"
if [[ "$PLATFORM" == "linux/amd64" && ( "$HOST_ARCH" == "arm64" || "$HOST_ARCH" == "aarch64" ) ]]; then
    warn "Host is ${HOST_ARCH} but the image targets linux/amd64 (the e2sar-deps"
    warn "package is amd64-only). Docker will emulate via QEMU and the E2SAR"
    warn "compile can take well over an hour."
    warn "Faster: run this script on an x86_64 host, or point buildx at a remote"
    warn "amd64 builder:  docker buildx create --name amd --driver docker-container \\"
    warn "                  --platform linux/amd64 && docker buildx use amd"
fi

info "Building ${TAG}"
info "  platform:     ${PLATFORM}"
info "  E2SAR ref:    ${E2SAR_REF}  (deps ${E2SAR_DEPS_VER} / ${E2SAR_DEPS_DISTRO})"
info "  ET ref:       ${ET_REF}"
info "  buildtype:    ${BUILDTYPE}"
info "  context:      ${PROJECT_DIR}"

BUILD_ARGS=(
    --build-arg "E2SAR_REF=${E2SAR_REF}"
    --build-arg "E2SAR_DEPS_VER=${E2SAR_DEPS_VER}"
    --build-arg "E2SAR_DEPS_DISTRO=${E2SAR_DEPS_DISTRO}"
    --build-arg "ET_REF=${ET_REF}"
    --build-arg "CODA_FB_BUILDTYPE=${BUILDTYPE}"
)

# Prefer buildx when available: it handles --platform properly.
if docker buildx version > /dev/null 2>&1; then
    BUILDER=(docker buildx build --load --platform "${PLATFORM}")
else
    warn "docker buildx not available; falling back to legacy builder"
    BUILDER=(docker build)
fi

"${BUILDER[@]}" \
    ${NO_CACHE} \
    --progress "${PROGRESS}" \
    -f "${PROJECT_DIR}/docker/Dockerfile" \
    -t "${TAG}" \
    "${BUILD_ARGS[@]}" \
    "${PROJECT_DIR}"

info "Built ${TAG}"
docker image inspect "${TAG}" --format '  size: {{.Size}} bytes, arch: {{.Architecture}}' || true

if [[ -n "$SAVE_FILE" ]]; then
    info "Saving image to ${SAVE_FILE}"
    docker save "${TAG}" | gzip > "${SAVE_FILE}"
    info "Transfer and load on the remote host with:"
    echo "    scp ${SAVE_FILE} user@host:/tmp/"
    echo "    ssh user@host 'gunzip -c /tmp/$(basename "${SAVE_FILE}") | docker load'"
fi

if [[ -n "$PUSH_REGISTRY" ]]; then
    REMOTE_TAG="${PUSH_REGISTRY%/}/${TAG##*/}"
    info "Tagging ${TAG} as ${REMOTE_TAG} and pushing"
    docker tag "${TAG}" "${REMOTE_TAG}"
    docker push "${REMOTE_TAG}"
    info "Pushed ${REMOTE_TAG}"
fi
