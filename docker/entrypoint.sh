#!/usr/bin/env bash
#
# CODA Frame Builder container entrypoint.
#
# Translates environment variables into coda-fb command-line flags, optionally
# starts a local ET system first, and converts the SIGTERM that "docker stop"
# sends into the SIGINT that coda-fb actually installs a handler for. Without
# that translation coda-fb would be SIGKILLed and would never deregister its
# worker from the EJFAT load balancer.
#
# Any arguments passed to the container are appended verbatim to the coda-fb
# command line, so they override or extend anything set through the environment.
#
# Escape hatches: if the first argument is an executable other than coda-fb
# (bash, evio_event_parser, et_start, et_monitor, ...) it is exec'd directly.

set -euo pipefail

log()  { echo "[entrypoint] $*"; }
warn() { echo "[entrypoint] WARNING: $*" >&2; }
die()  { echo "[entrypoint] ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Escape hatch: run some other command in this image
# ---------------------------------------------------------------------------
if [[ $# -gt 0 ]] && [[ "$1" != -* ]] && [[ "$1" != "coda-fb" ]]; then
    if command -v "$1" > /dev/null 2>&1; then
        log "running: $*"
        exec "$@"
    fi
    die "unknown command '$1' (pass flags starting with '-' to coda-fb, or a valid executable name)"
fi
[[ ${1:-} == "coda-fb" ]] && shift

# --help / -h anywhere on the command line goes straight to coda-fb, so the
# image is self-documenting without having to satisfy any required parameter.
for _a in "$@"; do
    case "$_a" in
        -h|--help) exec coda-fb --help ;;
    esac
done

# Raw mode: if the caller passes --uri/-u themselves, the command line is
# authoritative and the environment is not consulted for coda-fb flags. This is
# how container runtimes that pass flags rather than environment variables
# (Shifter, plain srun) are expected to invoke the image. RUN_ET_START and
# SHUTDOWN_GRACE still apply, and SIGTERM is still translated to SIGINT.
RAW_MODE=0
for _a in "$@"; do
    case "$_a" in
        -u|--uri|--uri=*) RAW_MODE=1; break ;;
    esac
done

# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------
# Required
EJFAT_URI="${EJFAT_URI:-}"

# Receiver network
RECEIVER_IP="${RECEIVER_IP:-}"
AUTO_IP="${AUTO_IP:-0}"
RECEIVER_PORT="${RECEIVER_PORT:-10000}"
THREADS="${THREADS:-1}"
CORES="${CORES:-}"
NUMA_NODE="${NUMA_NODE:-}"
BUFSIZE="${BUFSIZE:-}"
RECV_TIMEOUT="${RECV_TIMEOUT:-}"
PREFER_IPV6="${PREFER_IPV6:-0}"
TLS_NOVALIDATE="${TLS_NOVALIDATE:-0}"

# Mode
ENABLE_FRAMEBUILD="${ENABLE_FRAMEBUILD:-1}"

# Frame builder aggregation
EXPECTED_STREAMS="${EXPECTED_STREAMS:-1}"
FB_THREADS="${FB_THREADS:-1}"
FRAME_TIMEOUT="${FRAME_TIMEOUT:-}"
FRAMENUMBER_SLOP="${FRAMENUMBER_SLOP:-}"

# Frame builder file output
# Only defaulted when *unset*: FB_OUTPUT_DIR= (explicitly empty) disables file
# output, which is what an ET-only deployment wants.
FB_OUTPUT_DIR="${FB_OUTPUT_DIR-/data}"
FB_OUTPUT_PREFIX="${FB_OUTPUT_PREFIX:-}"

# ET output
ET_FILE="${ET_FILE:-}"
ET_HOST="${ET_HOST:-}"
ET_PORT="${ET_PORT:-}"
ET_EVENT_SIZE="${ET_EVENT_SIZE:-}"

# Local ET system (et_start) managed by this container
RUN_ET_START="${RUN_ET_START:-0}"
ET_START_NEVENTS="${ET_START_NEVENTS:-1000}"
ET_START_EVENT_SIZE="${ET_START_EVENT_SIZE:-2097152}"
ET_START_WAIT="${ET_START_WAIT:-30}"
ET_START_ARGS="${ET_START_ARGS:-}"

# Reassembly-only output
OUTPUT_DIR="${OUTPUT_DIR:-}"
FILE_PREFIX="${FILE_PREFIX:-}"
FILE_EXTENSION="${FILE_EXTENSION:-}"

# Diagnostics
VERBOSE_FRAMES="${VERBOSE_FRAMES:-0}"
VERBOSE_REASSEMBLE="${VERBOSE_REASSEMBLE:-0}"
REPORT_INTERVAL="${REPORT_INTERVAL:-}"
EXTRA_ARGS="${EXTRA_ARGS:-}"

# Seconds to let coda-fb shut down cleanly after SIGINT before escalating to
# SIGKILL. Keep docker stop -t larger than this or Docker will kill us first.
SHUTDOWN_GRACE="${SHUTDOWN_GRACE:-60}"

is_true() {
    case "$1" in
        1|[tT]rue|[tT]RUE|[yY]es|[yY]ES|[oO]n|[oO]N|TRUE|YES|ON) return 0 ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
if [[ "$RAW_MODE" == "0" ]]; then

[[ -n "$EJFAT_URI" ]] || die "EJFAT_URI is required (the EJFAT control-plane URI).
  Example: -e EJFAT_URI='ejfat://token@cp-host:18347/lb/1?data=10.0.0.5:10000'"

if is_true "$AUTO_IP"; then
    [[ -z "$RECEIVER_IP" ]] || die "set RECEIVER_IP or AUTO_IP=1, not both"
else
    [[ -n "$RECEIVER_IP" ]] || die "RECEIVER_IP is required (or set AUTO_IP=1).
  This address is registered with the EJFAT control plane and the load balancer
  sends UDP directly to it, so it must be an address of the host as seen from
  the load balancer. Run the container with --network host."
fi

if is_true "$ENABLE_FRAMEBUILD"; then
    if [[ -z "$ET_FILE" && -z "$FB_OUTPUT_DIR" ]]; then
        die "frame building needs at least one output: set ET_FILE and/or FB_OUTPUT_DIR"
    fi
    if [[ -n "$FB_OUTPUT_DIR" ]]; then
        mkdir -p "$FB_OUTPUT_DIR" 2>/dev/null || true
        [[ -d "$FB_OUTPUT_DIR" ]] || die "FB_OUTPUT_DIR '$FB_OUTPUT_DIR' does not exist and could not be created"
        [[ -w "$FB_OUTPUT_DIR" ]] || die "FB_OUTPUT_DIR '$FB_OUTPUT_DIR' is not writable by uid $(id -u).
  Mount it with matching ownership, or run the container with --user $(id -u):$(id -g)."
    fi
else
    [[ -n "$OUTPUT_DIR" ]] || die "reassembly-only mode (ENABLE_FRAMEBUILD=0) requires OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR" 2>/dev/null || true
    [[ -d "$OUTPUT_DIR" && -w "$OUTPUT_DIR" ]] || die "OUTPUT_DIR '$OUTPUT_DIR' is not a writable directory"
fi

fi  # end of RAW_MODE=0 validation

if is_true "$RUN_ET_START"; then
    [[ -n "$ET_FILE" ]] || die "RUN_ET_START=1 requires ET_FILE (the ET memory-mapped file, e.g. /tmp/et_sys_pagg)"
fi

# ---------------------------------------------------------------------------
# Optional: start a local ET system
# ---------------------------------------------------------------------------
ET_PID=""

start_et() {
    is_true "$RUN_ET_START" || return 0

    local et_cmd=(et_start -f "$ET_FILE" -n "$ET_START_NEVENTS" -s "$ET_START_EVENT_SIZE")
    [[ -n "$ET_PORT" ]] && et_cmd+=(-p "$ET_PORT")
    # shellcheck disable=SC2206
    [[ -n "$ET_START_ARGS" ]] && et_cmd+=($ET_START_ARGS)

    log "starting ET system: ${et_cmd[*]}"
    "${et_cmd[@]}" &
    ET_PID=$!

    local waited=0
    while [[ ! -e "$ET_FILE" ]]; do
        if ! kill -0 "$ET_PID" 2>/dev/null; then
            wait "$ET_PID" || true
            die "et_start exited before creating '$ET_FILE'.
  A local ET system needs shared memory: run with --ipc=host and --shm-size=2g,
  and make sure $(dirname "$ET_FILE") is writable."
        fi
        if (( waited >= ET_START_WAIT )); then
            die "timed out after ${ET_START_WAIT}s waiting for ET file '$ET_FILE'"
        fi
        sleep 1
        waited=$((waited + 1))
    done
    log "ET system ready at '$ET_FILE' (pid $ET_PID)"
}

stop_et() {
    [[ -n "$ET_PID" ]] || return 0
    if kill -0 "$ET_PID" 2>/dev/null; then
        log "stopping ET system (pid $ET_PID)"
        kill -TERM "$ET_PID" 2>/dev/null || true
        wait "$ET_PID" 2>/dev/null || true
    fi
    ET_PID=""
    return 0
}

# Make sure a locally started ET system is never orphaned, including on the
# early-exit paths in start_et (a stale et_start would keep holding the
# memory-mapped file and its TCP port).
trap stop_et EXIT

# ---------------------------------------------------------------------------
# Assemble the coda-fb command line
# ---------------------------------------------------------------------------
args=()

if [[ "$RAW_MODE" == "1" ]]; then
    log "raw mode: --uri given on the command line, environment not consulted"
    args=("$@")
else

args=(--uri "$EJFAT_URI")

if is_true "$AUTO_IP"; then
    args+=(--autoip)
else
    args+=(--ip "$RECEIVER_IP")
fi
args+=(--port "$RECEIVER_PORT")

# --cores overrides --threads in coda-fb, so never send both.
if [[ -n "$CORES" ]]; then
    # shellcheck disable=SC2206
    args+=(--cores $CORES)
    [[ "$THREADS" == "1" ]] || warn "CORES is set, so THREADS=$THREADS is ignored by coda-fb"
else
    args+=(--threads "$THREADS")
fi

[[ -n "$NUMA_NODE" ]]       && args+=(--numa "$NUMA_NODE")
[[ -n "$BUFSIZE" ]]         && args+=(--bufsize "$BUFSIZE")
[[ -n "$RECV_TIMEOUT" ]]    && args+=(--timeout "$RECV_TIMEOUT")
[[ -n "$REPORT_INTERVAL" ]] && args+=(--report-interval "$REPORT_INTERVAL")

is_true "$PREFER_IPV6"     && args+=(--ipv6)
is_true "$TLS_NOVALIDATE"  && args+=(--novalidate)

if is_true "$ENABLE_FRAMEBUILD"; then
    args+=(--enable-framebuild=1)
    args+=(--expected-streams "$EXPECTED_STREAMS")
    args+=(--fb-threads "$FB_THREADS")
    [[ -n "$FRAME_TIMEOUT" ]]     && args+=(--frame-timeout "$FRAME_TIMEOUT")
    [[ -n "$FRAMENUMBER_SLOP" ]]  && args+=(--framenumber-slop "$FRAMENUMBER_SLOP")
    [[ -n "$FB_OUTPUT_DIR" ]]     && args+=(--fb-output-dir "$FB_OUTPUT_DIR")
    [[ -n "$FB_OUTPUT_PREFIX" ]]  && args+=(--fb-output-prefix "$FB_OUTPUT_PREFIX")
    [[ -n "$ET_FILE" ]]           && args+=(--et-file "$ET_FILE")
    [[ -n "$ET_HOST" ]]           && args+=(--et-host "$ET_HOST")
    [[ -n "$ET_PORT" ]]           && args+=(--et-port "$ET_PORT")
    [[ -n "$ET_EVENT_SIZE" ]]     && args+=(--et-event-size "$ET_EVENT_SIZE")
else
    args+=(--enable-framebuild=0)
    args+=(--output-dir "$OUTPUT_DIR")
    [[ -n "$FILE_PREFIX" ]]    && args+=(--prefix "$FILE_PREFIX")
    [[ -n "$FILE_EXTENSION" ]] && args+=(--extension "$FILE_EXTENSION")
fi

is_true "$VERBOSE_FRAMES"     && args+=(--verbose-frames)
is_true "$VERBOSE_REASSEMBLE" && args+=(--verbose-reassemble)

# shellcheck disable=SC2206
[[ -n "$EXTRA_ARGS" ]] && args+=($EXTRA_ARGS)
args+=("$@")

fi  # end of environment-driven assembly

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
CODA_FB_PID=""
WATCHDOG_PID=""
SHUTTING_DOWN=0

shutdown() {
    [[ "$SHUTTING_DOWN" == "1" ]] && return 0
    SHUTTING_DOWN=1
    [[ -n "$CODA_FB_PID" ]] || return 0
    kill -0 "$CODA_FB_PID" 2>/dev/null || return 0

    # coda-fb installs a handler for SIGINT only (signal(SIGINT, ctrlCHandler)).
    # That handler deregisters the worker from the EJFAT load balancer before
    # any blocking shutdown call, drains the builder threads and prints final
    # statistics -- so SIGTERM must be translated, and the result given time.
    log "forwarding shutdown to coda-fb as SIGINT (pid $CODA_FB_PID)"
    kill -INT "$CODA_FB_PID" 2>/dev/null || true

    # Escalate if it wedges, so the container always exits rather than waiting
    # for Docker to SIGKILL the whole thing.
    (
        sleep "$SHUTDOWN_GRACE"
        if kill -0 "$CODA_FB_PID" 2>/dev/null; then
            warn "coda-fb still running ${SHUTDOWN_GRACE}s after SIGINT; sending SIGKILL."
            warn "LB deregistration may not have completed."
            kill -KILL "$CODA_FB_PID" 2>/dev/null || true
        fi
    ) &
    WATCHDOG_PID=$!
}

trap shutdown TERM INT

start_et

log "exec: coda-fb ${args[*]}"
coda-fb "${args[@]}" &
CODA_FB_PID=$!

# The first wait is interrupted when a trapped signal arrives; keep waiting
# until coda-fb is actually reaped so its clean shutdown can finish.
set +e
wait "$CODA_FB_PID"
rc=$?
while kill -0 "$CODA_FB_PID" 2>/dev/null; do
    wait "$CODA_FB_PID"
    rc=$?
done
set -e

if [[ -n "$WATCHDOG_PID" ]]; then
    kill "$WATCHDOG_PID" 2>/dev/null || true
    wait "$WATCHDOG_PID" 2>/dev/null || true
fi

stop_et

log "coda-fb exited with status $rc"
exit "$rc"
