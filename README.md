# vibe-motion

`vibe-motion` is a clean-room C++20 network-camera motion daemon built with the
LLVM toolchain (Clang 22, libc++, libc++abi, and LLD). Its first
compatibility target covers RTMP/RTSP input, motion masks and event grouping,
snapshots, best event pictures, passthrough event movies, hourly timelapse,
hooks, and an integrated MJPEG/status HTTP server.

It deliberately does **not** support local V4L2/libcamera devices or databases.
Unknown Motion options are reported; `--check-config` can be used before a
service migration.

## Build

The daemon requires Clang 22 and a coherent FFmpeg development installation containing
`libavformat`, `libavcodec`, `libavutil`, and `libswscale`.  On this development
host that installation lives under `/usr/local`.

```sh
cmake -S . -B build-clang \
  -DCMAKE_TOOLCHAIN_FILE=cmake/clang-toolchain.cmake \
  -DVIBE_FFMPEG_ROOT=/usr/local
cmake --build build-clang -j
cd build-clang && ctest --output-on-failure
```

The repository includes `.clangd`, `.clang-format`, and `.clang-tidy`. Useful
quality targets are `format-check` and `tidy`. A sanitizer build is configured
with `-DVIBE_ENABLE_SANITIZERS=ON`.

Do not point headers at one FFmpeg installation and libraries at another.  The
CMake configuration intentionally searches only below `VIBE_FFMPEG_ROOT`.

## Run

```sh
./build-clang/vibe-motion --check-config -c examples/motion.conf
./build-clang/vibe-motion --check-config --strict-config -c examples/motion.conf
./build-clang/vibe-motion --dump-effective-config -c examples/motion.conf
./build-clang/vibe-motion -n -c examples/motion.conf
```

`SIGTERM` and `SIGINT` perform a graceful stop. `SIGHUP` validates and reloads
the configuration by replacing camera workers only after the new graph parses
successfully. Network URLs are redacted from logs and config dumps.

The web listener serves a small status page and JSON status plus Motion-style
MJPEG routes such as `/1/mjpg/stream`. It is read-only; mutating Motion web
control actions and database features are out of scope.

## Migration from an existing Motion deployment

The intended deployment sequence is:

1. Copy the existing main and camera config files without changing secrets.
2. Run `vibe-motion --check-config -c /etc/motion/motion.conf`.
3. Resolve every unsupported active-option warning.
4. Run against recorded/test streams and compare the hook ledger and generated
   media before stopping the existing service.
5. Install `packaging/vibe-motion.service` as a separate unit. Never run both
   daemons against the same output filenames during validation.

Hook child supervisors set their Linux process name to `motion` for compatibility
with integrations that inspect parent process names. The daemon executable and
systemd service remain visibly named `vibe-motion`.

## Compatibility notes

- Main options are inherited when each `camera` directive is encountered.
- Legacy `camera_id`/`camera_name`, `rtsp_uses_tcp`, and `lightswitch` aliases
  are accepted. Legacy per-camera `stream_port` is parsed but the current
  Motion 5 deployment ignores it; streaming uses the global web port.
- Event hooks run as argv without a shell. Quoted arguments are preserved and
  metacharacters introduced by filenames cannot execute shell fragments.
- MP4 passthrough begins from the latest buffered keyframe and rebases packet
  timestamps. This is the most important part to validate for every camera
  codec before production cutover.
- Detection is behavior-compatible, not pixel-identical to Motion. Thresholds
  should be tuned against representative clips before migration.

See [docs/compatibility.md](docs/compatibility.md) for the implemented option
surface and remaining limits.
