# coda-fb tests

```bash
meson setup builddir
meson test -C builddir --print-errorlogs
```

Tests are not built by `meson compile` (`build_by_default: false`); `meson test` builds what it
needs. None of them needs EJFAT infrastructure: the end-to-end tests run the real `coda-fb` binary
on loopback with the control plane switched off on both sides.

| Test | Needs | Covers |
|---|---|---|
| `pipeline_stats` | C++17 | Counter/gauge exactness under 8 concurrent writers, histogram `le` bucket boundaries and concurrent observations, quantile interpolation, overhead of an update (printed; asserted < 1 µs). |
| `frame_sender` | C++17 | With a mock transport: every frame submitted exactly once from 4 concurrent builders (no duplicates, contents intact when sent); the buffer stays alive after the builder drops it and is freed after release; blocking on the frame and byte limits; oversized frame admitted alone; `drop` policy drops after the timeout and counts it; queue-full retries do not duplicate and are bounded; permanent failures are rejected without retry; event number mapping and the reserved 0; shutdown drain, deadline-bounded waits, abandoned frames and late completions; offered = submitted + dropped + rejected under contention. |
| `metrics_render` | C++17 | Prometheus text format: HELP/TYPE before samples, `coda_fb_` prefix, `_total` exactly on counters, valid and escaped labels, no duplicate series, all 43 families; values and ns→s / bytes units; histogram buckets cumulative with `+Inf == _count`; disabled stages and missing E2SAR sources omitted; last-activity timestamps only once they happened; stable start time; rendering while 4 threads update the counters. |
| `metrics_endpoint` | Boost | Real sockets: `/metrics` 200 with the Prometheus content type, query string ignored, `/` 200, other paths 404, POST 405; a throwing renderer gives 500; a client that never sends a request neither blocks other scrapes nor stays connected past the read timeout; 200 concurrent scrapes from 8 threads; busy port and bad bind address reported; `stop()` closes the port and is idempotent. |
| `uri_config` | E2SAR | Receive and send URIs parsed independently; per-role requirements (data, sync, token, LB id, IPv6); the same-LB guard; no token in any error message or description, including E2SAR's own parse errors. |
| `cli_startup` | E2SAR, ET | The real binary: `--help`; required/conflicting/deprecated URI options; validation of each send option; a bad `--metrics-bind` and a busy `--metrics-port` fail before any registration; clean, non-zero exit when sender registration fails (nothing else started, no worker registration), when the frame builder fails after the sender started, and when worker registration fails after sender and builder started; no token in any output. |
| `forwarding_e2e_incomplete` | E2SAR, ET | The real binary between an upstream E2SAR segmenter (2 ROC streams, 40 frames) and a downstream E2SAR reassembler standing in for LB #2. Every forwarded frame is reassembled and compared **byte for byte** with the record coda-fb wrote to its `.evio` file; exactly 40 arrive, none twice, frames span several packets, event number = frame number + offset, data id as configured. A 41st frame missing one stream never completes and must not be sent (counted as discarded at shutdown). SIGINT shutdown exits 0, drains, and the final statistics agree. `/metrics` is scraped from the live process and must agree too (submitted, released, offered, complete frames, events, readiness, E2SAR families present, no token or URI); the scrape is saved as `metrics.txt` and passes `promtool check metrics`. |
| `forwarding_e2e_partial-skip` | E2SAR, ET | As above, but the 41st frame times out as a partial frame: written to the file, not forwarded with `--send-partial-frames=0`, and counted. |

Run under sanitizers with `-Db_sanitize=address,undefined` (all tests). ThreadSanitizer cannot run
under x86 emulation; `frame_sender`, `pipeline_stats`, `metrics_render` and `metrics_endpoint`
have no E2SAR dependency and can be built natively (the endpoint test needs Boost headers), e.g.:

```bash
clang++ -std=c++17 -O1 -g -fsanitize=thread -Isrc -Itest test/test_frame_sender.cpp src/frame_sender.cpp -o tsan_fs && ./tsan_fs
clang++ -std=c++17 -O1 -g -fsanitize=thread -Isrc -Itest -I/opt/homebrew/include \
    test/test_metrics_endpoint.cpp src/metrics_endpoint.cpp src/metrics_render.cpp -o tsan_ep && ./tsan_ep
```

To lint a scrape yourself: `curl -s localhost:9465/metrics | promtool check metrics`.

## Validation that needs live EJFAT load balancers

The loopback tests bypass the control planes, so they cannot check sender and worker
registration, sync packets, or the LB data path. With two reserved LB instances (LB #1 to receive
from, LB #2 to send to) and their instance URIs:

1. **Downstream consumer.** On a host routable from LB #2, start a receiver registered with LB #2
   that writes each event to a file, e.g. E2SAR's `e2sar_perf --recv` (or a second coda-fb in
   reassembly-only mode: `coda-fb --recv-ejfat-uri "$LB2_URI" --ip <host> --output-dir /data/lb2`,
   which concatenates the received frames into `/data/lb2/events.bin`).
2. **coda-fb.** Start it with both URIs and file output as the reference copy:
   `coda-fb --recv-ejfat-uri "$LB1_URI" --send-ejfat-uri "$LB2_URI" --ip <host> --enable-framebuild=1 --expected-streams <n> --fb-output-dir /data/fb`.
   Check the startup log: sender registered on LB #2, then frame builder, then worker on LB #1.
   `lbadm --status` on LB #2 should list the sender address, on LB #1 the worker.
3. **Source.** Send ROC data to LB #1 as in production (or with `e2sar_perf --send`).
4. **Compare.** Strip the 56-byte EVIO file header from `/data/fb/frames_thread0_file0000.evio`
   and compare its records with the downstream output (same order with `--fb-threads 1` and one
   downstream worker; otherwise match records by the frame number in the time-slice segment, word
   19). `evio_event_parser` validates both sides.
5. **Shutdown.** Stop coda-fb with SIGINT. Expect `All forwarded frames were released by E2SAR`,
   `Abandoned at shutdown: 0`, and the sender address gone from LB #2's status.
6. **Failure paths.** Repeat step 2 with a revoked or wrong LB #2 token: coda-fb must exit
   non-zero before registering on LB #1. Throttle LB #2 (e.g. `--send-rate 0.1`) to see *Blocked*
   grow with `--send-overflow=block`, and *Dropped: overflow* with `--send-overflow=drop`.

Only the loopback tests above have been run as part of this change; this procedure has not been
run against live load balancers.
