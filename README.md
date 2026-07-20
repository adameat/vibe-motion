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

The daemon requires Clang 22, libcurl and libxml2 development headers, and a coherent
FFmpeg development installation containing `libavformat`, `libavcodec`, `libavutil`, and
`libswscale`. On this development host that FFmpeg installation lives under `/usr/local`.

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

Deployment builds can opt in to CPU-specific code generation with
`-DVIBE_ENABLE_NATIVE_OPTIMIZATION=ON` and to Clang ThinLTO with
`-DVIBE_ENABLE_LTO=ON`. Both options are disabled by default so ordinary builds
remain portable. Native binaries should only be moved to machines with a
compatible CPU. LTO and sanitizers intentionally cannot be enabled in the same
build directory; keep separate deployment and sanitizer builds.

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
MJPEG routes such as `/1/mjpg` and `/1/mjpg/stream`. It is read-only; mutating
Motion web control actions and database features are out of scope.

## ONVIF cameras

`onvif_url` is the camera's Device Service endpoint, commonly
`http://camera/onvif/device_service`. When it is present, `vibe-motion` discovers the Media
service, gets the available profiles, and uses `GetStreamUri` instead of `netcam_url`.
`onvif_profile` optionally selects an exact profile `Name` (a token is also accepted); without
it the profile with the largest `width × height` is selected. `width` and `height` may be
omitted for ONVIF cameras and then come from that profile. Explicit dimensions remain an
analysis-scaling override. A camera file can use `width auto` and `height auto` to undo
dimensions inherited from the main file.

ONVIF event polling runs independently from successful stream discovery, so it can connect while
media discovery is retrying. Both features use `onvif_url`; configuring it still makes the ONVIF
`GetStreamUri` result the camera's media source, so event-only ONVIF alongside a separate
`netcam_url` is not supported:

```conf
onvif_url http://192.0.2.20/onvif/device_service
onvif_userpass user:password
onvif_auth auto
onvif_events on
motion_detection off
decode_frames auto
onvif_motion_topics Motion,MotionAlarm,CellMotionDetector,PeopleDetect,VehicleDetect,DogCatDetect,FaceDetect
```

`onvif_events on` creates a PullPoint subscription without a server-side topic filter and
locally accepts the comma-separated topic fragments in `onvif_motion_topics`. This works
with common topics such as `RuleEngine/CellMotionDetector/Motion` and
`VideoSource/MotionAlarm`, plus Reolink smart topics such as `PeopleDetect`, `VehicleDetect`,
`DogCatDetect`, and `FaceDetect`, while allowing other vendor topics to be added. Boolean
`IsMotion`, `Motion`, `State`, `Alarm`, or `LogicalState` data items drive the event state.
Multiple rules/sources are tracked separately and combined by OR.

Set `onvif_log_events on` while commissioning a camera to write every received notification
as a single structured JSON object at info level. The record includes the raw topic, whether
it matched a configured motion topic, whether a boolean state was found, UTC time, operation,
Source/Key/Data items, and the serialized raw `NotificationMessage` XML subtree. This captures
vendor-specific nested elements without logging the SOAP request's WS-Security header. Treat
the payload as potentially sensitive camera metadata and disable this diagnostic option after
commissioning. Unmatched notifications are diagnostic only and never trigger a motion event.

`motion_detection on` (the default) keeps local pixel analysis enabled. With ONVIF events
also enabled, either source can trigger an event. Set it to `off` when the camera's own
analytics should be the only trigger; video decoding, snapshots, recording, timelapse, and
streaming continue normally. `event_gap` still controls how long recording remains active
after the external state becomes false.

`decode_frames` controls decoder CPU independently from compressed passthrough recording:

- `all` is the compatibility-preserving default and decodes every video frame;
- `keyframes` always decodes keyframes only;
- `auto` uses keyframe-only decoding while idle when ONVIF events are enabled and local
  `motion_detection` is off. An HTTP snapshot or MJPEG consumer requests full decoding; the
  camera worker switches after the next decoded keyframe without reconnecting and returns to
  keyframe-only decoding five seconds after the last consumer leaves.

Every compressed packet still enters the pre-capture ring and an active passthrough movie.
Snapshots, event pictures, and timelapse use decoded keyframes while idle, so their maximum
idle cadence is limited by the camera's GOP/keyframe interval. The JSON status exposes the
configured, requested, and active decode modes plus the latest observed keyframe interval.

The JSON status reports subscription health, aggregate motion state, profile token, event
count, last topic, UTC time, the last event's ONVIF Source/Data metadata, and separate
media-discovery and event-subscription errors. `onvif_auth auto`
first tries HTTP Digest and falls back to WS-Security UsernameToken PasswordDigest for cameras
such as Reolink; `digest` and `wsse` force either mode. HTTPS certificates are verified by
default; `onvif_tls_verify off` is available for explicitly trusted cameras with self-signed
certificates. ONVIF device connections bypass environment HTTP proxies.

See `examples/camera-onvif.conf` for a complete camera file.

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
- MP4/MKV passthrough begins from the latest buffered keyframe and rebases
  packet timestamps. `movie_codec copy` preserves the camera codec. A fixed
  `movie_codec h264|hevc` decodes and re-encodes the same packet stream;
  `movie_encoder libx265` can require x265. `movie_quality`, `movie_bitrate`,
  and `movie_keyframe_interval` control that encoder. HEVC in MP4 is tagged
  as `hvc1`.
- `timelapse_container mkv` is recommended for hourly timelapse output. The
  Motion-compatible `mpeg4` value writes AVI; MPEG Program Stream is not used
  for MPEG-4 timelapses.
- `timelapse_codec mpeg4|hevc` selects the encoded codec independently of the
  container. `timelapse_encoder libx265` requests x265 explicitly; an empty
  encoder selects libx265 first for HEVC and otherwise uses FFmpeg's default.
  HEVC supports MKV and MP4, while the legacy `mpeg4` container means MPEG-4
  Part 2 in AVI.
- `timelapse_quality 1..100` enables variable-bitrate encoding; `0`
  preserves bitrate mode. In bitrate mode, `timelapse_bitrate 0` keeps the
  resolution-derived default and a positive value selects an explicit number
  of bits per second. `timelapse_keyframe_interval` sets the maximum distance
  between keyframes in output-video seconds; its default is 10.
- `stream_codec mjpeg` keeps only the Motion-compatible MJPEG routes.
  `stream_codec copy` enables packet-based fragmented-MP4 passthrough at
  `/<camera>/video.mp4`. A fixed `stream_codec h264|hevc` creates one
  transcoder per active web client, controlled by `stream_encoder`,
  `stream_quality`, `stream_bitrate`, and `stream_keyframe_interval`. Both
  modes reuse the existing camera connection. HEVC playback depends on
  OS/browser codec support; Safari on Apple platforms is commonly supported,
  while other browsers vary.
- Detection is behavior-compatible, not pixel-identical to Motion. Thresholds
  should be tuned against representative clips before migration.

See [docs/compatibility.md](docs/compatibility.md) for the implemented option
surface and remaining limits.
