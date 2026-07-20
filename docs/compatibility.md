# Motion compatibility target

This project was independently implemented from configuration syntax,
documentation, and observed external behavior. No Motion implementation source
is included or copied.

## Implemented profile

| Area | Options / behavior |
| --- | --- |
| Configuration | ordered `camera` includes, global inheritance, last value wins, quoted values, aliases, unknown-option diagnostics |
| Sources | RTMP and RTSP, ONVIF Media profile/stream discovery, TCP transport option, input dictionary parameters, timeout and reconnect, full/keyframe/automatic decode modes |
| Detection | grayscale background difference, `noise_level`, `noise_tune`, `threshold`, `threshold_tune`, `despeckle_filter`, `lightswitch`, PGM masks, `minimum_motion_frames` |
| Event lifecycle | local and/or ONVIF PullPoint motion triggers, `event_gap`, compressed pre-capture ring, frame-counted post-capture, `movie_all_frames`, H.264/HEVC passthrough or fixed-codec transcoding, best event JPEG |
| Files | `%` expansion, snapshots, MP4/MKV passthrough movies, hourly MPEG-4 or HEVC timelapse in compatible containers, automatic parent directories |
| Hooks | event start/end, picture save, movie start/end; asynchronous argv execution and exit logging |
| HTTP | one global listener, read-only codec/status fields, latest JPEG, per-camera multipart MJPEG, H.264/HEVC fragmented-MP4 passthrough or fixed-codec transcoding |
| Operations | foreground/systemd operation, structured logs, signal shutdown, HUP reload, per-camera failure isolation |

## Explicitly excluded

- V4L2, libcamera, and all other local capture devices
- SQL/database writes and queries
- sound detection
- video loopback devices
- mutable webcontrol configuration
- TLS and HTTP authentication in the current profile

## Known fidelity limits

- `noise_tune` uses a bounded sampled-median estimate with hysteresis, while
  `threshold_tune` uses a bounded rolling changed-pixel estimate. Both are
  behavior-compatible rather than exact copies of Motion's tuning routines.
- `locate_motion_mode preview` with `locate_motion_style redbox` is rendered on
  best-event JPEGs. `text_*` overlays and HTTP `stream_preview_scale` are parsed
  but are not rendered/applied yet.
- Only `picture_output best`, unlimited H.264/HEVC passthrough or transcoded
  MP4/MKV event movies, and hourly MPEG-4/HEVC timelapse with configurable VBR quality,
  fixed bitrate, encoder selection, and keyframe interval
  are accepted. Other parsed output modes fail config validation instead of
  silently behaving differently.
- The read-only HTTP routes are behavior-oriented; this is not a byte-for-byte
  clone of Motion's HTML interface or combined camera grid.
- ONVIF SOAP authentication supports HTTP Digest and WS-Security UsernameToken
  PasswordDigest, with automatic fallback. Event subscriptions are unfiltered
  at the camera and motion topics are filtered locally using
  `onvif_motion_topics` for better vendor compatibility.

An excluded option that is active is never silently treated as implemented.
Config validation reports it, while blank database options and known legacy
options are accepted with a diagnostic for drop-in parsing of the current file.

## Observable hook ordering

For a new event the daemon calls event-start, opens the movie, then calls
movie-start. At the event boundary it writes the best picture and calls
picture-save, calls event-end, finalizes the movie, and finally calls movie-end.
Snapshots are independent and also call picture-save.

Supported script arguments include `%t`, `%v`, strftime tokens, `%o`, `%D`,
`%N`, `%f`, `%n`, and `%$`. The expander also supports the filename and event
tokens documented in the example configuration.
