# CODA Frame Builder — Container Build and Deployment

Build the image, publish it to Docker Hub, then pull and run it on Perlmutter.

The image is self-contained: `coda-fb`, `evio_event_parser`, the ET library and
tools, E2SAR, and the Boost/gRPC/protobuf stack. Nothing needs to be installed
on the target machine.

| Component | Source | Default |
|---|---|---|
| Boost 1.89.0, gRPC 1.74.1, protobuf, Abseil | E2SAR `e2sar-deps` package, under `/usr/local` | `0.4.0rc1` / `ubuntu-22.04` |
| ET | built from `JeffersonLab/et` | `v16.6.0` |
| E2SAR | built from `JeffersonLab/E2SAR` (submodules + LFS) | `v0.4.0rc1` |
| `coda-fb`, `evio_event_parser` | this repository | working tree |

---

## 1. Build the image

```bash
# from the repository root
./docker/build.sh --tag coda-fb:v1.0.0
```

Or with plain Docker:

```bash
docker build -f docker/Dockerfile -t coda-fb:v1.0.0 \
  --build-arg E2SAR_REF=v0.4.0rc1 \
  --build-arg ET_REF=v16.6.0 .
```

### Architecture matters

The image targets **`linux/amd64`**, because the `e2sar-deps` package is
published for amd64 only. Perlmutter is AMD EPYC (x86_64), so this is the right
target — but it means you cannot practically build on an Apple Silicon Mac,
where Docker falls back to QEMU and the E2SAR compile runs well over an hour.

Build on an x86_64 machine, or attach buildx to a remote amd64 builder:

```bash
docker buildx create --name amd --driver docker-container --platform linux/amd64
docker buildx use amd
./docker/build.sh --tag coda-fb:v1.0.0
```

### Build options

```
-t, --tag TAG              Image tag (default: coda-fb:latest)
    --platform PLATFORM    Target platform (default: linux/amd64)
    --e2sar-ref REF        E2SAR git ref (default: v0.4.0rc1)
    --e2sar-deps-ver VER   Dependency bundle version (default: 0.4.0rc1)
    --et-ref REF           ET git ref (default: v16.6.0)
    --buildtype TYPE       meson buildtype (default: release)
    --save FILE            Save image to a gzipped tarball
    --push REGISTRY        Tag into REGISTRY and push
    --no-cache             Build without the layer cache
```

Moving `--e2sar-ref` to a newer release may require also moving
`--e2sar-deps-ver` and fixing compile errors in `src/coda-fb.cpp` if the
`Reassembler` / `recvEvent` API changed. The defaults are the versions coda-fb
currently compiles against.

**There is no final `v0.4.0` tag upstream.** The E2SAR repository has
`v0.4.0a1` and `v0.4.0rc1` plus a `v0.4.0-wip` branch; `v0.4.0rc1` is the
newest release candidate and is what these defaults pin. Re-pin once a final
tag is published.

E2SAR 0.4.0 requires **Boost exactly 1.89.0** and **gRPC 1.74.1**, both supplied
by the `e2sar-deps` bundle. Keep `--e2sar-deps-ver` in step with `--e2sar-ref`;
mixing a 0.2.x bundle with a 0.4.0 source tree will not link.

---

## 2. Push to Docker Hub

Log in once per machine:

```bash
docker login
# Username: <DOCKERHUB_USER>
# Password: <a Docker Hub access token, not your account password>
```

Then tag into your namespace and push:

```bash
docker tag coda-fb:v1.0.0 <DOCKERHUB_USER>/coda-fb:v1.0.0
docker push <DOCKERHUB_USER>/coda-fb:v1.0.0
```

`build.sh` does the tag-and-push in one step:

```bash
./docker/build.sh --tag coda-fb:v1.0.0 --push <DOCKERHUB_USER>
# builds, tags as <DOCKERHUB_USER>/coda-fb:v1.0.0, pushes
```

Push a `latest` alias too if you want Perlmutter pulls to track the newest build:

```bash
docker tag coda-fb:v1.0.0 <DOCKERHUB_USER>/coda-fb:latest
docker push <DOCKERHUB_USER>/coda-fb:latest
```

**Use immutable version tags for anything you run in a batch job.** Pulling
`:latest` on Perlmutter days apart can silently give you two different binaries
in the same campaign.

If the repository is private, you will need to authenticate on Perlmutter as
well — see the `podman-hpc login` step below.

---

## 3. Pull the image on Perlmutter

Perlmutter does not run Docker. It provides **podman-hpc** (preferred) and
**Shifter**. Pull on a **login node**; the image is then available to compute
nodes.

### podman-hpc (recommended)

```bash
ssh perlmutter.nersc.gov

# only needed for a private Docker Hub repository
podman-hpc login docker.io

podman-hpc pull <DOCKERHUB_USER>/coda-fb:v1.0.0
podman-hpc images
```

Pulled images are converted to a squashed format automatically and are usable
from compute nodes with no special Slurm flags. If you instead *built* an image
locally on Perlmutter, run `podman-hpc migrate <image>` to make it available to
jobs.

### Shifter (alternative)

```bash
shifterimg -v pull docker:<DOCKERHUB_USER>/coda-fb:v1.0.0
shifterimg images
```

Shifter needs the image named on the job (`--image=...`), unlike podman-hpc.

---

## 4. Run interactively on Perlmutter

### The URI coda-fb needs

`coda-fb --uri` expects the **instance URI** returned when the load balancer is
reserved, not the base control-plane URI. Reserving is a separate step that
happens wherever you manage the LB — commonly a JLab host or your own
workstation, not a Perlmutter login node. Perlmutter only needs the resulting
instance URI.

Bring it across and keep it out of the job script, since it carries an instance
token:

```bash
# on Perlmutter, from the instance URI produced wherever you reserved
export EJFAT_URI='ejfat://<instance-token>@<lb-host>:<port>/lb/<id>?data=...&sync=...'

# or stage it in a private file and source it from the job
install -m 600 /dev/stdin ~/.ejfat_uri <<< "$EJFAT_URI"
export EJFAT_URI=$(cat ~/.ejfat_uri)
```

Two things to watch when the reservation is made off-site:

- **The reservation has to outlive the queue wait.** A Perlmutter job can sit
  queued for a long time; if the LB reservation expires before the job starts,
  `coda-fb` fails at registration. Reserve for longer than you think you need,
  or reserve indefinitely — E2SAR 0.3.0 made `lbadm -d '00:00:00'` the default
  for that.
- **The receiver address is decided on Perlmutter, not at reservation time.**
  `RECEIVER_IP` is whatever compute node Slurm gives you, which is why the batch
  example below computes it at run time rather than baking it in.

E2SAR ships worked Perlmutter/Slurm examples in `scripts/zero_to_hero/` with
notes in `docs/RunningSlurmOnPerlmutter.md`. They reserve from a login node,
which differs from an off-site workflow, but the job-orchestration side still
applies.

Get an interactive allocation first — do not run the receiver on a login node:

```bash
salloc --nodes 1 --qos interactive --time 01:00:00 \
       --constraint cpu --account <mxxxx>
```

### Interactive shell inside the container

Useful for checking the build, inspecting output files, or running
`evio_event_parser` by hand:

```bash
podman-hpc run --rm -it \
  --net host \
  --group-add keep-groups \
  -v $SCRATCH/coda-fb:/data \
  <DOCKERHUB_USER>/coda-fb:v1.0.0 bash
```

Inside the shell:

```bash
coda-fb --help
evio_event_parser /data/frames_thread0_file0000.evio --verbose
ip addr show            # find the address to register with the control plane
```

Any argument that is not a flag and names an executable is run directly instead
of `coda-fb` — `bash`, `evio_event_parser`, `et_start`, `et_monitor`, `ip`, `nc`.

### Interactive run of coda-fb itself

Arguments starting with `-` are passed to `coda-fb`; environment variables are
translated into its flags. Ctrl+C shuts it down cleanly.

```bash
podman-hpc run --rm -it \
  --net host \
  --group-add keep-groups \
  -v $SCRATCH/coda-fb:/data \
  -e EJFAT_URI='ejfat://token@cp-host:18347/lb/1?data=10.0.0.5:10000' \
  -e RECEIVER_IP=10.0.0.5 \
  -e THREADS=4 \
  -e EXPECTED_STREAMS=3 \
  -e FB_OUTPUT_DIR=/data \
  -e VERBOSE_FRAMES=1 \
  <DOCKERHUB_USER>/coda-fb:v1.0.0
```

### Shifter equivalent

```bash
salloc --nodes 1 --qos interactive --time 01:00:00 \
       --constraint cpu --account <mxxxx> \
       --image=docker:<DOCKERHUB_USER>/coda-fb:v1.0.0 \
       --volume="$SCRATCH/coda-fb:/data"

shifter /bin/bash
shifter /usr/local/bin/entrypoint.sh --help
```

---

## 5. Run non-interactively (batch)

Save as `coda-fb.sbatch`:

```bash
#!/bin/bash
#SBATCH --job-name=coda-fb
#SBATCH --nodes=1
#SBATCH --qos=regular
#SBATCH --constraint=cpu
#SBATCH --account=<mxxxx>
#SBATCH --time=04:00:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
# Deliver SIGTERM to the job 120 s before the time limit so coda-fb can
# deregister from the load balancer and flush its files.
#SBATCH --signal=B:TERM@120

set -euo pipefail

IMAGE=<DOCKERHUB_USER>/coda-fb:v1.0.0
OUTDIR=$SCRATCH/coda-fb/$SLURM_JOB_ID
mkdir -p "$OUTDIR"

# The address the EJFAT load balancer will send UDP to.
RECEIVER_IP=$(hostname -I | awk '{print $1}')
echo "Receiving on $RECEIVER_IP, writing to $OUTDIR"

podman-hpc run --rm \
  --name coda-fb-$SLURM_JOB_ID \
  --net host \
  --group-add keep-groups \
  -v "$OUTDIR":/data \
  -e EJFAT_URI="$EJFAT_URI" \
  -e RECEIVER_IP="$RECEIVER_IP" \
  -e RECEIVER_PORT=10000 \
  -e THREADS=4 \
  -e EXPECTED_STREAMS=3 \
  -e FB_THREADS=2 \
  -e FB_OUTPUT_DIR=/data \
  -e SHUTDOWN_GRACE=60 \
  "$IMAGE" &

CID=$!
trap 'echo "caught TERM, stopping container"; podman-hpc stop -t 90 coda-fb-$SLURM_JOB_ID || true' TERM
wait $CID
```

Submit and monitor:

```bash
# the instance URI from your LB reservation (see section 4)
export EJFAT_URI=$(cat ~/.ejfat_uri)
sbatch coda-fb.sbatch

squeue --me
tail -f coda-fb-<jobid>.out
scancel <jobid>          # triggers the clean shutdown path
```

Keep secrets out of the script: `EJFAT_URI` carries an instance token, so export
it in your shell (as above) or read it from a mode-0600 file, rather than
committing it.

### Shifter batch equivalent

```bash
#!/bin/bash
#SBATCH --nodes=1
#SBATCH --qos=regular
#SBATCH --constraint=cpu
#SBATCH --account=<mxxxx>
#SBATCH --time=04:00:00
#SBATCH --image=docker:<DOCKERHUB_USER>/coda-fb:v1.0.0
#SBATCH --volume="/pscratch/sd/<u>/<user>/coda-fb:/data"

srun shifter /usr/local/bin/entrypoint.sh \
  --uri "$EJFAT_URI" \
  --ip "$(hostname -I | awk '{print $1}')" \
  --threads 4 --enable-framebuild=1 \
  --expected-streams 3 --fb-output-dir /data
```

---

## Perlmutter-specific caveats

**Reachability is the thing to verify first.** `coda-fb` registers
`RECEIVER_IP` with the EJFAT control plane, and the load balancer then sends UDP
straight to that address, across ports `RECEIVER_PORT .. RECEIVER_PORT+THREADS-1`.
A Perlmutter compute node has to be routable from the load balancer for any of
this to work, and that is a site networking question, not a container one —
confirm the path with NERSC and the EJFAT operators before scheduling a long run.
Test with `THREADS=1` and `VERBOSE_REASSEMBLE=1` and watch whether the statistics
counters move off zero.

**`--net host` is required, not optional.** Without it the container advertises
an address the load balancer cannot reach, and the port range is awkward to map.

**Root is squashed.** Both podman-hpc and Shifter run the container as *you*,
not root. The image's default `/data` is inside the read-only image, so it must
be volume-mounted to a writable NERSC filesystem. The entrypoint checks this and
fails with a specific message rather than letting `coda-fb` die.

**Write to `$SCRATCH`, not the image.** Use `$SCRATCH` (Lustre) for EVIO output;
`$HOME` is small and not intended for job output. Under Shifter the image is
mounted read-only, so an unmounted output path fails outright.

**`/tmp`, `/etc`, `/var` are system-reserved under Shifter** and are overwritten
by the runtime. Do not put `ET_FILE` in `/tmp` under Shifter — use a
`perNodeCache` volume or a mounted path.

**ET is awkward on Perlmutter.** ET is shared-memory based. Connecting to an
external ET system (`ET_HOST` / `ET_PORT`) needs routable access to it; running
`et_start` inside the container needs `--ipc=host` and a writable shared path.
File output to `$SCRATCH` is the straightforward mode here — ET is still built
into the image because it gates *all* frame building, but you do not have to use
it. For a file-only run leave `ET_FILE` unset.

**Slurm sends SIGTERM.** `coda-fb` installs a handler for SIGINT only; the
entrypoint translates SIGTERM into SIGINT so the load-balancer deregistration in
`ctrlCHandler` actually runs. Use `#SBATCH --signal=B:TERM@120` so you get
advance warning before the time limit, and give `podman-hpc stop -t 90` more time
than `SHUTDOWN_GRACE` (default 60 s). If `coda-fb` has not exited by then the
entrypoint escalates to SIGKILL and logs that deregistration may not have
completed.

---

## Two ways to pass parameters

The entrypoint accepts either style, and both keep the SIGTERM→SIGINT
translation and the ET lifecycle:

**Environment variables** (used by the podman-hpc and Docker examples above).
Variables are translated into `coda-fb` flags; anything you append after the
image name is added to the end of the command line.

**Raw flags.** If you pass `--uri` yourself, the command line is authoritative
and the environment is not consulted at all. This is the natural style for
Shifter and plain `srun`:

```bash
shifter /usr/local/bin/entrypoint.sh \
  --uri "$EJFAT_URI" --ip 10.0.0.5 --threads 4 \
  --enable-framebuild=1 --expected-streams 3 --fb-output-dir /data
```

`--help` works with no parameters set at all, in either style:

```bash
podman-hpc run --rm <DOCKERHUB_USER>/coda-fb:v1.0.0 --help
```

---

## Parameters

Every variable maps to a `coda-fb` flag documented in the
[top-level README](../README.md#options). Unset variables are not passed, so
`coda-fb`'s own defaults apply. Flags after the image name are appended verbatim
and override the environment.

**Required**

| Variable | Flag | Notes |
|---|---|---|
| `EJFAT_URI` | `--uri` | The **instance** URI returned when the load balancer is reserved, not the base control-plane URI. Carries an instance token. |
| `RECEIVER_IP` | `--ip` | Or `AUTO_IP=1` for `--autoip`. Exactly one of the two. |

**Receiver**

| Variable | Flag | Default |
|---|---|---|
| `RECEIVER_PORT` | `--port` | `10000` |
| `THREADS` | `--threads` | `1` |
| `CORES` | `--cores` | unset — **overrides `THREADS`**, e.g. `CORES="4 5 6 7"` |
| `NUMA_NODE` | `--numa` | unset |
| `BUFSIZE` | `--bufsize` | unset (3 MB) |
| `RECV_TIMEOUT` | `--timeout` | unset (500 ms) |
| `REPORT_INTERVAL` | `--report-interval` | unset (5000 ms) |
| `PREFER_IPV6` | `--ipv6` | `0` |
| `TLS_NOVALIDATE` | `--novalidate` | `0` |

**Mode and aggregation**

| Variable | Flag | Default |
|---|---|---|
| `ENABLE_FRAMEBUILD` | `--enable-framebuild` | `1` |
| `EXPECTED_STREAMS` | `--expected-streams` | `1` |
| `FB_THREADS` | `--fb-threads` | `1` |
| `FRAME_TIMEOUT` | `--frame-timeout` | unset (1000 ms) |
| `FRAMENUMBER_SLOP` | `--framenumber-slop` | unset (0) |

**Output**

| Variable | Flag | Default |
|---|---|---|
| `FB_OUTPUT_DIR` | `--fb-output-dir` | `/data` — set **empty** to disable file output |
| `FB_OUTPUT_PREFIX` | `--fb-output-prefix` | unset (`frames`) |
| `ET_FILE` | `--et-file` | unset (ET output disabled) |
| `ET_HOST` | `--et-host` | unset (broadcast discovery) |
| `ET_PORT` | `--et-port` | unset |
| `ET_EVENT_SIZE` | `--et-event-size` | unset (2 MB) |
| `OUTPUT_DIR` | `--output-dir` | required when `ENABLE_FRAMEBUILD=0` |
| `FILE_PREFIX` | `--prefix` | unset (`events`) |
| `FILE_EXTENSION` | `--extension` | unset (`.bin`) |

**Container-only**

| Variable | Default | Meaning |
|---|---|---|
| `RUN_ET_START` | `0` | Start an ET system in the container before `coda-fb`. |
| `ET_START_NEVENTS` | `1000` | `et_start -n` |
| `ET_START_EVENT_SIZE` | `2097152` | `et_start -s` |
| `ET_START_WAIT` | `30` | Seconds to wait for the ET file to appear. |
| `SHUTDOWN_GRACE` | `60` | Seconds allowed for clean shutdown before SIGKILL. |
| `EXTRA_ARGS` | unset | Raw flags appended to the `coda-fb` command line. |

A full annotated template is in
[`coda-fb.env.example`](coda-fb.env.example); pass it with
`--env-file` on a machine that supports it.

---

## Validating output

`evio_event_parser` ships in the image, so output can be checked without
installing anything:

```bash
podman-hpc run --rm -v $SCRATCH/coda-fb:/data <DOCKERHUB_USER>/coda-fb:v1.0.0 \
  evio_event_parser /data/frames_thread0_file0000.evio --verbose
```

Exit code `0` means the file is valid EVIO-6. Add `--fadc-verbose` to decode
FADC250 hits.

---

## Running elsewhere with plain Docker

On an ordinary Linux host with Docker:

```bash
docker run -d --name coda-fb --network host \
  -v /data/frames:/data \
  -e EJFAT_URI='ejfat://token@cp-host:18347/lb/1?data=10.0.0.5:10000' \
  -e RECEIVER_IP=10.0.0.5 -e THREADS=4 -e EXPECTED_STREAMS=3 \
  <DOCKERHUB_USER>/coda-fb:v1.0.0

docker logs -f coda-fb
docker stop -t 90 coda-fb
```

`docker-compose.yml` in this directory sets the stop grace period and log
rotation. For hosts with no registry access, ship a tarball instead:

```bash
./docker/build.sh --tag coda-fb:v1.0.0 --save coda-fb-v1.0.0.tar.gz
scp coda-fb-v1.0.0.tar.gz daq-host:/tmp/
ssh daq-host 'gunzip -c /tmp/coda-fb-v1.0.0.tar.gz | docker load'
```

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `ERROR: RECEIVER_IP is required` | Neither `RECEIVER_IP` nor `AUTO_IP=1` set. |
| `ERROR: frame building needs at least one output` | `FB_OUTPUT_DIR` and `ET_FILE` both empty. |
| `ERROR: FB_OUTPUT_DIR '/data' is not writable by uid N` | Output path not volume-mounted, or owned by another uid. Mount `$SCRATCH`; add `--group-add keep-groups` for CFS. |
| Statistics stay at zero | Missing `--net host`, `RECEIVER_IP` not routable from the load balancer, or the port range is blocked. |
| `et_start exited before creating ...` | Missing `--ipc=host`, or `ET_FILE`'s directory not writable. Under Shifter, `/tmp` is reserved. |
| `Frame builder: NOT COMPILED` | Image built without ET. The build should fail first — rebuild with `--no-cache`. |
| Job killed with no final statistics | Stop timeout shorter than `SHUTDOWN_GRACE`, or no `--signal=B:TERM@120`. |
| `podman-hpc` image missing in a job | Built locally without `podman-hpc migrate`. |
| `exec format error` | Image built for arm64. Rebuild with `--platform linux/amd64`. |

More detail: `-e VERBOSE_FRAMES=1` (frame building) or
`-e VERBOSE_REASSEMBLE=1` (reassembly-only).
