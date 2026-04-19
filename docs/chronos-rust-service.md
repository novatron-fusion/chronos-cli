# Chronos 2.1-HD Rust Service — Design Summary

On-camera Rust service that replaces the current
camera → TIFF → LabVIEW → HDF5 → NAS pipeline with a single-stage
direct path: camera DRAM → raw binary on tmpfs → chunked HDF5 on NAS,
coordinated via Zenoh.

## Context

### Hardware constraints

- **Camera:** Kron Technologies Chronos 2.1-HD
- **Network:** 1 GbE only — hard limit at ~110 MB/s sustained
- **Internal storage:** 8/16/32 GB RAM ring buffer (2.7/5.5/11 s at full res)
- **OS:** Embedded Linux on ARMv7 SoC
- **Architecture:** FPGA captures to DRAM; CPU handles readout/rendering

### The 1 GbE reality

Full-res acquisition rates are tens of GB/s into DRAM. The 1 GbE port
cannot stream real-time; the camera is burst-record-to-RAM, then offload.
A 32 GB buffer takes roughly **5 minutes to offload** over gigabit
regardless of software pipeline. This fits shot-based operation
(100 ms – 10 s shots with inter-shot idle time) but must be planned into
the shot cycle.

### Current problems to solve

- Per-frame TIFF files create massive filesystem metadata overhead
- LabVIEW is an unnecessary hop in the data path
- Separate NAS copy step adds latency before analysis can start
- Four separate I/O operations per frame on the critical path

## Camera software architecture

Three separate projects provide control on the camera:

| Project       | Role                                    | D-Bus Service |
|---------------|-----------------------------------------|---------------|
| pychronos     | FPGA/sensor/recording control           | `ca.krontech.chronos.control` |
| chronos-cli   | **DRAM readout, file save, display**    | `ca.krontech.chronos.video` |
| chronos-http  | REST API layered on top of both         | — |

> Key insight from the `chronos-cli/src/pipeline/README.md`:
> The `cam-pipeline` daemon has direct DRAM access and can save raw
> pixel data as a **single contiguous binary file** via the `recordfile`
> method with `format="y16"` or `format="y12b"`.
> No need to fork chronos-cli, no HTTP pulls, no shared memory plumbing.

## Architecture

```
         Zenoh shot control
       (nfg/n1/control/shot/*)
                 │
                 ▼
       ┌──────────────────────┐
       │  Rust service        │
       │  (on camera)         │
       └──────────────────────┘
         │         │         │
         │ zbus    │ zbus    │ mmap + zstd
         ▼         ▼         ▼
    control     video    tmpfs raw file
   (recording)  (save)        │
                              ▼
                        NFS/SMB to
                        mounted NAS
                        (HDF5 file)
                              │
                              ▼
                      Zenoh notify:
              nfg/n1/shot/<id>/camera_fast/path
```

## Key formats — `recordfile` options

From the pipeline README:

| Format        | Use for science? | Notes |
|---------------|------------------|-------|
| `h264`/`x264` | ❌ | Lossy compression |
| `dng`         | ❌ | Directory of per-frame files |
| `tiff`        | ❌ | Debayered, many files |
| `tiffraw`     | ❌ | Raw but still many files |
| **`y16`**     | ✅ | **Single file, raw 16-bit padded, recommended** |
| **`y12b`**    | ✅ | Single file, packed 12-bit (tighter) |

For Chronos 2.1-HD at 12-bit sensor depth, `y12b` saves 25% disk/wire
bandwidth vs `y16` at the cost of slightly more complex unpacking.
Start with `y16` for simplicity, switch to `y12b` if offload time matters.

## Service flow

1. Service starts, opens D-Bus connection, subscribes to Zenoh
   `nfg/n1/control/shot/state`
2. On shot arm: configure recording via `ca.krontech.chronos.control`
   (resolution, frame rate, exposure, trigger mode)
3. On shot trigger: recording runs in FPGA/DRAM autonomously
4. On shot end:
   - Subscribe to `eof` signal on `ca.krontech.chronos.video`
   - Call `recordfile(format="y16", filename="/tmp/shot_N.y16",
     start=0, length=totalFrames)`
   - Await `eof` signal (synchronous completion notification)
5. Process raw file:
   - `mmap` the file from tmpfs
   - Stream frames into chunked HDF5 on mounted NAS with Zstd+shuffle
   - Per-shot metadata (fps, t0, shot_id, exposure) as HDF5 attributes
6. Publish availability to Zenoh:
   `nfg/n1/shot/<id>/camera_fast/path` → `/mnt/nas/shots/N/camera.hdf5`
7. Delete tmpfs file, ready for next shot

## Why this works cleanly

- **One process, one language** — no LabVIEW, no Python, no cross-language pipeline
- **No forks of upstream** — uses documented D-Bus methods only
- **tmpfs as zero-copy IPC** — cam-pipeline's existing save path is our
  "shared memory"; writes land in RAM, Rust mmap reads from RAM
- **Synchronous via `eof` signal** — no polling, no race conditions
- **Decoupled from network writes** — HDF5 write to NAS happens after
  camera DRAM is free; next shot can arm while HDF5 finalizes
- **Single HDF5 file output** — no per-shot conversion step
- **Off-camera deployment possible** — same binary can run on a gateway
  PC using chronos-http instead of local D-Bus (config flag)

## Rust crate stack

| Concern                    | Crate                                 |
|----------------------------|---------------------------------------|
| D-Bus client (async)       | `zbus`                                |
| Zenoh pub/sub              | `zenoh`                               |
| Memory mapping             | `memmap2`                             |
| HDF5 output                | `hdf5-metno` (actively maintained fork) |
| Zstd compression for HDF5  | via `hdf5-metno` blosc plugin         |
| Zero-copy byte casting     | `bytemuck`                            |
| Array views                | `ndarray`                             |
| Async runtime              | `tokio`                               |

## Cross-compilation

- **Target triple:** `armv7-unknown-linux-gnueabihf`
- **Recommended approach for iteration:** build directly on the camera
  (install rustup; slower builds but zero toolchain setup)
- **Recommended for CI:** use the `cross` tool (Docker-based,
  handles HDF5 library cross-linking)
- **HDF5 dependency:** needs ARM build of `libhdf5` at link time

## Deployment

- Single static binary, ~10–20 MB with HDF5 linked
- systemd service on the camera
- Configuration via TOML: Zenoh router address, NAS mount path,
  default recording parameters, key-expression prefix
- Monitored via Zenoh — publishes own health/status to
  `nfg/n1/diag/camera_fast/status`

## Integration with novaxc / Zenoh

### Subscribed keys

- `nfg/n1/control/shot/state` — shot lifecycle (arm/trigger/end/abort)
- `nfg/n1/control/camera_fast/config` — runtime parameter overrides

### Published keys

- `nfg/n1/diag/camera_fast/status` — service state, last shot id,
  frames captured, offload progress
- `nfg/n1/diag/camera_fast/preview` — optional JPEG preview
  (every Nth frame, downsampled) for operator display
- `nfg/n1/shot/<id>/camera_fast/path` — NAS path of completed HDF5
- `nfg/n1/shot/<id>/camera_fast/metadata` — shot parameters,
  frame count, timestamps, calibration reference

### What does NOT go over Zenoh

- Bulk frame data — stays on the filesystem path
- Raw binary y16 blobs — stays on tmpfs, never network-visible
- Full-resolution video — only the HDF5 file path is published;
  analysis nodes open it directly from NAS

## HDF5 output structure

```
/mnt/nas/shots/<shot_id>/camera_fast.hdf5
├── /frames                          (uint16, chunked, zstd+shuffle)
│   ├── attrs/shot_id
│   ├── attrs/frame_rate_hz
│   ├── attrs/t0_ns                  (TAI ns from PTP)
│   ├── attrs/exposure_us
│   ├── attrs/resolution             (hRes, vRes)
│   └── attrs/sensor_bit_depth
├── /metadata
│   ├── attrs/camera_serial
│   ├── attrs/fpga_version
│   └── attrs/calibration_ref
└── attrs/shot_id, attrs/created_utc
```

**Chunk shape:** `(16, vRes, hRes)` — 16 frames per chunk gives good
compression ratio without excessive chunk overhead.
**Compression:** Zstd level 3 with shuffle filter — ~2–3× ratio on
typical scientific imagery, minimal CPU cost.
**SWMR:** optional — enable if analysis nodes want to read
while NAS write is still in progress.

## Open questions / future work

- Measure actual offload time for `y16` vs `y12b` at target resolution
- Verify jumbo frames end-to-end on 1 GbE path (camera, switch, NAS)
- Decide whether to expose live preview at 1 Hz via Zenoh for HMI
- Evaluate running the same service on an off-camera gateway PC
  (would use chronos-http remote API instead of local D-Bus)
- Integration with PTP timestamping (sof/eof signals carry status
  but may need additional PTP correlation from novaxc)
- Multi-camera deployment — one service instance per camera,
  distinct Zenoh key-expression prefixes

## References

- Chronos pipeline daemon: `chronos-cli/src/pipeline/README.md`
- Chronos control API: `https://support.krontech.ca/`
- pychronos: `github.com/krontech/pychronos`
- chronos-cli: `github.com/krontech/chronos-cli`
- zbus: `docs.rs/zbus`
- hdf5-metno: `crates.io/crates/hdf5-metno`



# Plan Rust Service:

# Minimal on-camera Rust service — MVP

## Context

Novatron's current camera data path is camera → TIFF → LabVIEW → HDF5 → NAS. The goal (per [chronos-rust-service.md](c:/Git/chronos-cli/chronos-rust-service.md)) is to collapse that into a single on-camera Rust service. This MVP is a deliberately narrow slice of that design: **only the storage offload leg**. No shot-state coordination, no status/preview publishing, no config overrides over Zenoh.

Workflow:
1. Hardware triggers recording — camera captures into DRAM autonomously (nothing for this service to do).
2. After the shot, something (novaxc or operator) publishes a **shot-folder path** to a Zenoh key.
3. This service reacts: it queries the camera's current recording config over D-Bus, calls `recordfile` to spill DRAM to a y16 blob on tmpfs, then converts that blob to a chunked Zstd-compressed HDF5 file inside the shot folder on NAS.

No other Zenoh input. No status output. One input, one output file.

## Scope boundaries (what's NOT in this MVP)

- No Zenoh status/diag publishing (`nfg/n1/diag/...`)
- No preview stream
- No runtime config overrides via Zenoh
- No y12b (add later; see "Follow-ups")
- No SWMR, no on-NAS partial reads mid-write
- No multi-camera support

## Location & target

- New crate at [c:/Git/chronos-cli/rust/camera_fast/](c:/Git/chronos-cli/rust/camera_fast/) (subdir of chronos-cli as confirmed).
- Target: `armv7-unknown-linux-gnueabihf`. Deployment on the camera as a systemd service. Cross-compile with `cross` during iteration, or `cargo build` directly on the camera for quick edits.

## Service behavior

Single async task loop:

1. **Startup**
   - Load TOML config (Zenoh router addr, Zenoh key expression, tmpfs staging dir, D-Bus bus addr).
   - Open zbus session bus connection.
   - Open Zenoh session; declare subscriber on the configured key.
2. **On each Zenoh sample**
   - Decode payload as UTF-8 — this is the **shot folder path** (e.g. `/mnt/nas/shots/42/`).
   - Ensure the folder exists (it should already; error if not).
   - Query `ca.krontech.chronos.control` over D-Bus for: `resolution` (hRes, vRes), `frameRate`, `exposurePeriod`, `sensorBitDepth`, `totalFrames`, `cameraSerial`. Use `get(as)` method returning `a{sv}`.
   - Build a tmpfs blob path, e.g. `<tmpfs_dir>/shot_<unix_ns>.y16`.
   - Subscribe to the `eof` signal on `ca.krontech.chronos.video`.
   - Call `recordfile` on `ca.krontech.chronos.video` with args `{format: "y16", filename: <tmpfs_path>, start: 0, length: totalFrames}`.
   - Await the matching `eof` signal (or an error variant). Timeout guard: generous (e.g. totalFrames × 1s / min-expected-fps × 2).
   - `mmap` the tmpfs file read-only.
   - Create `<shot_folder>/camera_fast.hdf5`. Create a `/frames` dataset with shape `(totalFrames, vRes, hRes)`, dtype `uint16`, chunk shape `(16, vRes, hRes)`, Zstd level 3 with shuffle filter.
   - Stream frames: for each chunk of 16 frames, take a `&[u16]` view via `bytemuck::cast_slice` over the mmap slice, write the slab into HDF5. No per-frame allocations.
   - Write attrs on `/frames`: `shot_id` (from folder basename), `frame_rate_hz`, `exposure_us`, `resolution` (hRes, vRes), `sensor_bit_depth`, `t0_ns` (current TAI ns via CLOCK_TAI — PTP correlation is a follow-up).
   - Write attrs on `/metadata`: `camera_serial`, `fpga_version` (from D-Bus).
   - Write root attrs: `shot_id`, `created_utc`.
   - Flush + close HDF5.
   - `munmap` and `unlink` the tmpfs file.
   - Log success; continue loop.
3. **On any error in handling a sample**
   - Log with enough detail to debug (shot folder, stage of failure).
   - Do NOT delete the tmpfs blob on failure — leave it for inspection.
   - Continue listening. A failed shot doesn't kill the service.

## File layout of the new crate

```
rust/camera_fast/
├── Cargo.toml
├── config.example.toml
├── systemd/camera_fast.service
└── src/
    ├── main.rs            # tokio entrypoint, config load, Zenoh subscribe loop
    ├── config.rs          # TOML structs, defaults
    ├── dbus.rs            # zbus proxies for .control and .video, typed wrappers around get() and recordfile()
    ├── pipeline.rs         # the per-shot pipeline: recordfile → eof → mmap → HDF5
    └── hdf5_writer.rs     # dataset/attr creation, chunked write loop
```

Keep `main.rs` under ~80 lines — orchestration only. Real logic in `offload.rs` so it's unit-testable with a fake shot folder and a pre-made raw blob on disk.

## Crate dependencies

From the design doc — pinned to known-compatible versions at implementation time:

| Concern | Crate |
|---|---|
| Async runtime | `tokio` (features: rt-multi-thread, macros, fs, signal) |
| D-Bus client | `zbus` (async) |
| Zenoh | `zenoh` |
| mmap | `memmap2` |
| HDF5 | `hdf5-metno` (with `zstd` feature) |
| Zero-copy byte cast | `bytemuck` |
| TOML config | `serde`, `toml` |
| Logging | `tracing`, `tracing-subscriber` |
| Errors | `anyhow` (bin-level), `thiserror` (lib-level if we split) |

## Critical files to reference (do not modify in MVP)

- [chronos-rust-service.md](c:/Git/chronos-cli/chronos-rust-service.md) — authoritative design doc; HDF5 layout and attrs mirror it.
- [src/pipeline/raw.c](c:/Git/chronos-cli/src/pipeline/raw.c) — confirms y16 byte layout: `hres × vres × 2` bytes per frame, little-endian, pure concatenation, no header.
- [src/pipeline/dbus-video.c:341](c:/Git/chronos-cli/src/pipeline/dbus-video.c) — `recordfile` method signature and async completion via `eof` signal.
- [src/api/ca.krontech.chronos.video.xml](c:/Git/chronos-cli/src/api/ca.krontech.chronos.video.xml) — D-Bus interface definition; use to generate zbus proxies (or hand-write).
- [src/api/ca.krontech.chronos.control.xml](c:/Git/chronos-cli/src/api/ca.krontech.chronos.control.xml) — control D-Bus interface for reading resolution / fps / bit depth / totalFrames.
- [src/pipeline/README.md](c:/Git/chronos-cli/src/pipeline/README.md) — `recordfile` argument semantics (format, filename, start, length) and `eof` signal description.

## Configuration (TOML)

```toml
# config.example.toml
[zenoh]
connect = ["tcp/novaxc-router.local:7447"]
shot_key = "nfg/n1/shot/+/folder"   # subscribe key; payload is the folder path

[staging]
tmpfs_dir = "/tmp"                  # raw y16 blob goes here between recordfile and HDF5

[dbus]
# default session/system bus; override only if needed for testing off-camera
```

Keep config flat; add keys only when something actually varies.

## Verification

Local / off-camera:
1. `cargo build --target armv7-unknown-linux-gnueabihf` via `cross` — confirms HDF5+zstd link on the target triple.
2. Unit test `offload::write_hdf5_from_raw` with a hand-crafted `(2, 4, 4)` uint16 buffer written to a temp file; reopen the HDF5 with `h5py` and assert values, chunking, and compression filter ID.

On camera (smoke test):
3. `scp` the armv7 binary + config to the camera. Start under systemd (or `./camera_fast --config ...` for a dry run).
4. Record a short shot via pychronos in the usual way, then publish a Zenoh sample:
   ```
   zenoh-pub -k "nfg/n1/shot/test1/folder" -v "/mnt/nas/shots/test1"
   ```
5. Assert `/mnt/nas/shots/test1/camera_fast.hdf5` appears, opens in `h5py`, `/frames` shape matches `(totalFrames, vRes, hRes)`, pixel values match a reference TIFF from the current LabVIEW path.
6. Assert the tmpfs blob is gone on success and present on induced failure (e.g. force the HDF5 write to a read-only NAS path).

Performance gate (informational only for MVP):
7. Time a full-res offload end-to-end; log `recordfile` duration, mmap+HDF5 write duration separately. Design doc ceiling is ~5 min for a 32 GB buffer over 1 GbE — anything slower than that is a bug on our side.

## Follow-ups (explicitly deferred)

- Add y12b support (bit-unpack into the uint16 HDF5 dataset on the fly).
- Publish `nfg/n1/diag/camera_fast/status` with service state and last-shot progress.
- Publish `nfg/n1/shot/<id>/camera_fast/path` on completion so analysis nodes don't poll.
- PTP-correlated `t0_ns` (currently CLOCK_TAI on the camera).
- SWMR mode for NAS-side readers to tail writes.
- systemd hardening (sandbox dirs, restart policy).
