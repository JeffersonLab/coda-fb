#!/usr/bin/env bash
#
# CLI and startup-failure tests for coda-fb. Needs no EJFAT infrastructure:
# control-plane failures are provoked with ports 18001/18002 on 127.0.0.1, where
# nothing is expected to listen.
#
# Usage: cli_startup_test.sh <path to coda-fb>

set -u
CODA_FB="$1"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
OUT=""
RC=0

TOKEN_R="RECVsecretTOKEN111"
TOKEN_S="SENDsecretTOKEN222"
RECV_URI="ejfat://${TOKEN_R}@127.0.0.1:18001/lb/11?data=127.0.0.1&sync=127.0.0.1:19010"
SEND_URI="ejfat://${TOKEN_S}@127.0.0.1:18002/lb/22?data=127.0.0.1:22100&sync=127.0.0.1:19011"

run() {  # run coda-fb with a hard time limit, capture output and exit code
    OUT=$(timeout 60 "$CODA_FB" "$@" 2>&1)
    RC=$?
}
ok()   { echo "ok:   $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL: $1"; echo "$OUT" | tail -20 | sed 's/^/      | /'; fail=$((fail + 1)); }
expect_fail() {  # expect_fail <description> <required substring>
    if [[ $RC -ne 0 && $RC -ne 124 && "$OUT" == *"$2"* ]]; then ok "$1"; else bad "$1 (rc=$RC, wanted '$2')"; fi
}
expect_absent() {
    if [[ "$OUT" != *"$2"* ]]; then ok "$1"; else bad "$1 (found '$2')"; fi
}
no_tokens() {
    if [[ "$OUT" != *"$TOKEN_R"* && "$OUT" != *"$TOKEN_S"* ]]; then ok "$1: no token in output"; else bad "$1: token leaked"; fi
}

# --- help and option parsing -------------------------------------------------------------
run --help
if [[ $RC -eq 0 && "$OUT" == *"--recv-ejfat-uri"* && "$OUT" == *"--send-ejfat-uri"* && "$OUT" == *"--send-overflow"* ]]; then
    ok "--help documents both URIs and the send options"
else
    bad "--help"
fi

run --ip 127.0.0.1 --enable-framebuild=1 --fb-output-dir "$WORK/f"
expect_fail "receive URI is required" "--recv-ejfat-uri is required"

run --recv-ejfat-uri "$RECV_URI" --uri "$RECV_URI" --ip 127.0.0.1 --output-dir "$WORK"
expect_fail "--uri and --recv-ejfat-uri are mutually exclusive" "not both"

# the deprecated alias is parsed as the receive URI: we get past it to the send URI check
run --uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --send-ejfat-uri "ejfat://${TOKEN_S}@127.0.0.1:18002/lb/22?data=bad"
expect_fail "--uri still works, with a deprecation warning" "--uri/-u is deprecated"
expect_fail "  ...and the send URI is validated separately" "--send-ejfat-uri: not a valid EJFAT URI"
no_tokens "invalid send URI"

run --recv-ejfat-uri "ejfat://${TOKEN_R}@127.0.0.1:18001/lb/11?data=nonsense" --ip 127.0.0.1 --output-dir "$WORK"
expect_fail "invalid receive URI names its option" "--recv-ejfat-uri: not a valid EJFAT URI"
no_tokens "invalid receive URI"

run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --output-dir "$WORK" --send-ejfat-uri "$SEND_URI"
expect_fail "forwarding requires frame building" "requires --enable-framebuild=1"

run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 \
    --send-ejfat-uri "ejfat://${TOKEN_S}@127.0.0.1:18002/lb/22?data=127.0.0.1:22100"
expect_fail "send URI needs a sync address with the control plane" "no IPv4 sync= address"
no_tokens "send URI without sync"

run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 \
    --send-ejfat-uri "ejfat://${TOKEN_S}@127.0.0.1:18001/lb/11?data=127.0.0.1:22100&sync=127.0.0.1:19011"
expect_fail "sending to the receive LB is refused" "same load balancer instance"
no_tokens "same LB"

run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --send-ejfat-uri "$SEND_URI" --send-event-offset 0
expect_fail "--send-event-offset 0 is refused" "--send-event-offset must be >= 1"
run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --send-ejfat-uri "$SEND_URI" --send-overflow maybe
expect_fail "--send-overflow is validated" "'block' or 'drop'"
run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --send-ejfat-uri "$SEND_URI" --send-queue-frames 0
expect_fail "--send-queue-frames is validated" "--send-queue-frames must be between"

run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1
expect_fail "frame building needs some output" "Forwarding to a second load balancer"

run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --output-dir "$WORK" --metrics-bind not-an-ip --metrics-port 9465
expect_fail "--metrics-bind is validated" "invalid --metrics-bind address"

# a busy metrics port fails before anything is registered
python3 -c 'import socket,time; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 23999)); s.listen(); time.sleep(30)' &
BLOCKER=$!
sleep 1
run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --fb-output-dir "$WORK/fm" \
    --send-ejfat-uri "$SEND_URI" --send-sender-ip 127.0.0.1 --metrics-port 23999
kill $BLOCKER 2>/dev/null; wait $BLOCKER 2>/dev/null
expect_fail "busy --metrics-port fails startup" "cannot listen for metrics on 127.0.0.1:23999"
expect_absent "  ...before registering as a sender" "Registering sender"

# --- startup failures unwind cleanly -------------------------------------------------------
# 1. Sender registration fails (no control plane at 127.0.0.1:18002): nothing else starts.
run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --fb-output-dir "$WORK/f1" \
    --send-ejfat-uri "$SEND_URI" --send-sender-ip 127.0.0.1
expect_fail "sender registration failure exits non-zero" "unable to register as a sender"
expect_absent "  ...before the frame builder starts" "Initializing frame builder"
expect_absent "  ...and before registering as a worker on the receive LB" "Registering worker"
no_tokens "sender registration failure"

# 2. Frame builder fails after the sender started: the sender is torn down.
run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --fb-output-dir /proc/coda-fb-cannot-create \
    --send-ejfat-uri "$SEND_URI" --send-nocp --send-warmup 0 --send-bufsize 200000
expect_fail "frame builder failure after sender init exits non-zero" "Failed to start frame builder"
expect_absent "  ...without registering as a worker" "Registering worker"

# 3. Worker registration on the receive LB fails after sender and builder started.
run --recv-ejfat-uri "$RECV_URI" --ip 127.0.0.1 --enable-framebuild=1 --fb-output-dir "$WORK/f3" \
    --send-ejfat-uri "$SEND_URI" --send-nocp --send-warmup 0 --send-bufsize 200000 --bufsize 200000
expect_fail "receive registration failure exits non-zero" "Failed to prepare receiver"
expect_fail "  ...and stops the frame builder it had started" "Stopping frame builder"
no_tokens "receive registration failure"

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
