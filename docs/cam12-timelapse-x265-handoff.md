# Cam12 x265 timelapse handoff

> Historical operational snapshot from July 2026. Paths, hashes, active experiments, and
> resource readings below describe that handoff point and are not the current production state.

Last updated: 2026-07-26 20:04 UTC / 2026-07-27 03:04 Asia/Bangkok.

## Current state

- Repository: `adameat/vibe-motion`
- Pull request: <https://github.com/adameat/vibe-motion/pull/18>
- Branch: `agent/efficient-timelapse-baichuan-pin`
- Relevant commit: `e159fbf` (`Fix timelapse FPS metadata and add x265 overrides`)
- Production host: `camka`
- Production binary: `/usr/local/bin/vibe-motion`
- Production binary SHA-256:
  `447d272160a09c7fc94078e4e2c7d94049a788ce3aae38d5955658c849feaa43`

A 24-hour experiment is currently running for **cam12 only**. The other cameras
use the global safe timelapse profile.

The selected cam12 profile is:

```conf
timelapse_quality 20
timelapse_keyframe_interval 120
timelapse_preset slow
timelapse_threads 2
timelapse_b_frames 4
timelapse_pixel_format yuv420p10le
timelapse_x265_params keyint=3600:min-keyint=3600:scenecut=0:open-gop=1:b-adapt=2:b-pyramid=1:ref=4:me=star:subme=4:merange=57:rc-lookahead=10:ctu=64:min-cu-size=8:max-tu-size=32:rd=4:rdoq-level=2:rect=1:temporal-mvp=1:weightp=1:aq-mode=3:aq-strength=0.9:qg-size=32:cutree=1:sao=1:deblock=0,0:psy-rd=2.0:psy-rdoq=1.0:wpp=1
```

`timelapse_quality 20` maps to x265 CRF 40.

The experiment started at 2026-07-26 18:58 UTC and should restore the safe
camera config after 24 hours, at approximately 2026-07-27 18:58 UTC
(2026-07-28 01:58 Asia/Bangkok).

At the time of this handoff:

- `vibe-motion` PID is unchanged;
- all 8 cameras are connected;
- reconnects: 0;
- reported camera errors: 0;
- 65 complete one-minute samples have an average process CPU of 260.47%, or
  about 2.60 CPU cores;
- maximum `VmRSS`: 6,330,680 KiB, or 6.04 GiB;
- minimum `MemAvailable`: 9,514,456 KiB, or 9.07 GiB.

This is an intermediate result after about 65 minutes, not the completed
24-hour conclusion.

## Required timelapse semantics

The intended behavior is:

1. Accept approximately one decoded timelapse frame per wall-clock second. When
   idle keyframe-only decoding is active, the actual cadence is bounded by the
   camera keyframe cadence.
2. Timestamp the stored timelapse stream as 30 fps.
3. One real hour therefore becomes about 120 seconds of playback: approximately
   30x acceleration.

The production global config now has:

```conf
timelapse_interval 1
timelapse_fps 30
```

The earlier config used `timelapse_fps 1`, which produced genuine 1 fps output
files. Merely changing the encoder frame rate was not sufficient for every
short Matroska file: the stream rate metadata also needed to be populated.

`src/media.cpp` now sets both:

```cpp
impl_->stream->avg_frame_rate = impl_->encoder->framerate;
impl_->stream->r_frame_rate = impl_->encoder->framerate;
```

Closed production files from all eight cameras were verified with
`r_frame_rate=30/1` and `avg_frame_rate=30/1`.

The selected cam12 GOP is 3,600 output frames:

- 3,600 frames / 30 fps = 120 seconds of output video;
- at approximately one captured frame per wall second, this is one wall hour;
- an hourly file therefore normally contains one keyframe at its start.

`timelapse_keyframe_interval` is expressed in output-video seconds. It is not a
frame count and it is not a wall-clock interval.

## Code changes

Commit `e159fbf` adds:

- explicit Matroska/stream frame-rate metadata;
- `timelapse_pixel_format`, accepting `yuv420p` or `yuv420p10le`;
- `timelapse_x265_params`, passed through only to `libx265`;
- validation preventing Main10/x265-only options from being used with another
  encoder;
- propagation of the per-camera overrides through the runtime and HTTP status;
- regression coverage for a Main10 timelapse with nominal and average 30 fps.

Relevant files:

- `include/vibe_motion/config.hpp`
- `include/vibe_motion/media.hpp`
- `src/config.cpp`
- `src/media.cpp`
- `src/runtime.cpp`
- `src/http.cpp`
- `tests/config_test.cpp`
- `tests/media_test.cpp`

The deployment build used Clang 22 and FFmpeg 8.1.2. Before deployment:

- the full build succeeded;
- `format-check` succeeded;
- CTest passed 11/11 tests.

## Safe global profile

When cam12 has no per-camera overrides, all cameras inherit:

```conf
timelapse_codec hevc
timelapse_quality 20
timelapse_keyframe_interval 3600
timelapse_preset ultrafast
timelapse_threads 1
timelapse_b_frames 4
```

This is Main/yuv420p unless overridden. Its configured GOP is 108,000 output
frames, but hourly files contain only about 3,600 frames, so they still have
only their initial keyframe.

After a safe-profile restart, total process RSS has typically been about
4.7-5.0 GiB and CPU about 1.4 cores, depending on scene and camera activity.

## Full handoff profile and the original OOM

The first cam12 experiment applied the full recommended profile:

- Main10;
- `slow`;
- CRF 40;
- `rc-lookahead=60`;
- `bframes=8`;
- `ref=4`;
- 2 frame threads.

On the former 8 GiB VM it caused two OOM kills. The kernel reported anonymous
RSS of 7,619,404 KiB and 7,679,212 KiB.

This was not caused by the long GOP being stored in memory. The large memory
users are x265 input/reconstructed surfaces and analysis state, especially:

- 10-bit 2560x1920 frame surfaces;
- `rc-lookahead`;
- reference and B-frame surfaces;
- concurrent frame encodes;
- memory retained by the allocator after an encoder is closed.

The VM now has 16 GiB RAM and no swap.

## Independent live parameter matrix

The live base for this matrix was Main10, `slow`, CRF 40, lookahead 10,
4 B-frames, `ref=2`, and 2 threads. Each row changed only the named setting for
about ten minutes.

The CPU windows occurred sequentially, so scene changes add noise. Memory
deltas are more stable. The clean `ref=4` and repeated control windows were run
back-to-back without crossing an hourly rotation.

| Variant | Process CPU | Max RSS | RSS delta vs repeated control | Interpretation |
| --- | ---: | ---: | ---: | --- |
| Repeated control | 248.89% | 5.58 GiB | baseline | Clean comparison baseline |
| `rc-lookahead=20` | 255.31% | 5.90 GiB | +0.32 GiB | Moderate analysis-memory cost |
| `rc-lookahead=40` | 249.99% | 6.57 GiB | +0.99 GiB | Dominant tested memory cost |
| `bframes=8` | 265.23% | 5.79 GiB | +0.21 GiB | Highest measured CPU; no useful size gain below |
| `ref=4`, clean | 258.12% | 5.56 GiB | effectively zero | About +3.7% CPU; small compression gain |
| `frame-threads=1` | 256.58% | 5.54 GiB | -0.04 GiB | Negligible memory saving and lower throughput |

An earlier control window measured 220.53% CPU but was marked invalid because
seven Baichuan cameras briefly reconnected near minute eight. It must not be
used as the clean control.

The first `ref=4` window crossed the 18:00 hourly rotation. Its 5.91 GiB maximum
was therefore confounded by encoder close/reopen behavior and was superseded by
the clean result above.

Raw live results on `camka`:

```text
/var/tmp/cam12-x265-matrix-20260726T170328Z.tsv
/var/tmp/cam12-x265-matrix-20260726T171859Z.tsv
/var/tmp/cam12-x265-matrix-files-20260726T171859Z.tsv
/var/tmp/cam12-x265-matrix-20260726T181919Z.tsv
/var/tmp/cam12-x265-matrix-files-20260726T181919Z.tsv
```

The individual camera configs remain on `camka`:

```text
/tmp/cam12-matrix-lookahead10.conf
/tmp/cam12-matrix-lookahead20.conf
/tmp/cam12-matrix-lookahead40.conf
/tmp/cam12-matrix-bframes8.conf
/tmp/cam12-matrix-ref4.conf
/tmp/cam12-matrix-frame-threads1.conf
```

## Same-source compression comparison

Sequential live files contain different scenes, so they cannot establish
compression efficiency. The same decoded 655-frame cam12 fragment was therefore
encoded locally with every variant.

All rows below use Main10, `slow`, CRF 40, and identical source frames. Payload
is compressed video-packet bytes without Matroska overhead.

| Variant | Payload bytes | Delta vs control | SSIM |
| --- | ---: | ---: | ---: |
| Control: lookahead 10, B4, ref2 | 914,311 | baseline | 0.977089 |
| `rc-lookahead=20` | 1,008,604 | +10.31% | 0.981212 |
| `rc-lookahead=40` | 1,044,642 | +14.25% | 0.982537 |
| `bframes=8` | 914,379 | +0.01% | 0.977075 |
| `ref=4` | 902,809 | -1.26% | 0.977136 |
| `frame-threads=1` | 914,461 | +0.02% | 0.977057 |

Interpretation:

- Lookahead 20/40 spends more bytes at the same CRF and produces measurably
  higher quality. It is a quality/RAM trade, not a storage saving at CRF 40.
- Eight B-frames produced no practical compression or quality benefit on this
  timelapse source.
- `ref=4` was the only tested parameter with a small storage win, slightly
  higher SSIM, no measurable RAM penalty, and an acceptable CPU cost.
- One frame thread did not reduce storage or CPU in the live process enough to
  justify its throughput penalty.

This is why `ref=4` was selected while lookahead stayed at 10 and B-frames at 4.

### Safe versus selected encoder

On the same source:

| Profile | Payload bytes | SSIM |
| --- | ---: | ---: |
| Safe Main/ultrafast, CRF 40 | 704,724 | 0.934386 |
| Selected Main10/slow/ref4, CRF 40 | 902,809 | 0.977136 |
| Selected Main10/slow/ref4, CRF 42 | 706,164 | 0.970478 |
| Selected Main10/slow/ref4, CRF 44 | 553,833 | 0.955909 |

At almost exactly the safe file size, CRF 42 improves SSIM from 0.9344 to
0.9705. This demonstrates the compression-efficiency benefit of Main10/slow.

The 24-hour production experiment intentionally remains at the handoff's
recommended CRF 40 so that CRF is not changed at the same time as the other
parameters.

## Comparison with the old spider archive

The old spider conversion is in:

```text
/home/alexey/moth/archive_one_timelapse.sh
```

Its relevant command is:

```sh
ffmpeg -i "$FNAME" -an $OPTIONS -c:v libx265 -crf 36 -preset superfast "$DSTFNAME"
```

For a 30 fps source it sets:

```sh
OPTIONS='-vf setpts=30*PTS -r 1'
```

Therefore spider deliberately produced **1 fps output files**. The new
vibe-motion files keep the sampled frames at 30 fps and obtain 30x playback
acceleration without rewriting them to 1 fps.

An old archived cam12 example:

```text
/cam/archive/timelapse/cam12/2026/07/cam12-20260723180000.mkv
```

has:

- HEVC Main/yuv420p;
- 2560x1920;
- 1 fps;
- 3,600 seconds of playback;
- 149,749,749 bytes;
- 332,777 bit/s;
- six keyframes, exactly 600 output seconds apart.

A new selected-profile production hour:

```text
/cam/timelapse/cam12/2026/07/cam12-20260726190001.mkv
```

has:

- HEVC Main10/yuv420p10le;
- 2560x1920;
- exactly 30 fps nominal and average;
- 3,535 frames;
- 117.833 seconds of playback;
- 5,715,811 bytes;
- 388,061 bit/s;
- one keyframe.

Playback bitrate is misleading across these files because their playback
durations differ by about 30x. Per wall-clock hour, this example is about
26 times smaller than the old archive. This is an indicative night-hour
comparison, not a controlled same-scene result.

The exact spider command was also replayed on the same 655 frames used above:

| Profile | File bytes | SSIM |
| --- | ---: | ---: |
| Spider: Main, 1 fps, CRF 36, superfast | 5,481,174 | 0.989510 |
| New: Main10, 30 fps, CRF 36, slow/ref4 | 1,399,240 | 0.986712 |
| New production choice: Main10, 30 fps, CRF 40, slow/ref4 | 910,505 | 0.977136 |

At the same CRF, the new profile is about 3.9 times smaller with a small SSIM
reduction. At production CRF 40 it is about six times smaller than the spider
command on these frames, with a larger but still bounded quality reduction.

## Hourly-rotation memory behavior

The long GOP itself is not accumulating an hour of frames in RAM. However, RSS
has a step after an hourly writer is closed and a new writer is opened.

Observed during the current day run:

- before the first rotation: 5,794,116 KiB;
- two minutes after the first rotation: 6,136,196 KiB;
- first step: about +326 MiB;
- before the second rotation: 6,203,220 KiB;
- two minutes after the second rotation: 6,329,888 KiB;
- second step: about +121 MiB.

The second step is smaller, so a linear hourly leak is not yet established.
Likely explanations are allocator arena retention/fragmentation or x265/FFmpeg
buffers that are freed logically but not immediately returned to the kernel.

The 24-hour run is intended to distinguish:

- bounded allocator high-water retention that reaches a plateau;
- a repeatable per-rotation leak;
- scene-dependent x265 working-set variation.

Possible follow-up experiments if RSS continues to step upward:

1. Call glibc `malloc_trim(0)` after the timelapse writer is fully closed and
   measure RSS immediately before and after.
2. Reuse an encoder across file boundaries if container rotation can be
   separated from codec lifetime.
3. As an operational fallback, restart the process after a bounded number of
   rotations. This is less desirable than identifying the retained allocation.

Do not call the behavior a confirmed leak until the 24-hour samples are
reviewed.

## Current 24-hour monitor

Transient systemd unit:

```text
vibe-motion-cam12-ref4-day.service
```

Monitor script:

```text
/tmp/vibe-motion-cam12-day-monitor.sh
```

Minute samples:

```text
/var/tmp/cam12-ref4-day-20260726T185626Z.tsv
```

Progress log:

```text
/var/tmp/cam12-ref4-day-progress-20260726T185626Z.log
```

Safe cam12 backup:

```text
/etc/vibe-motion/camera12.conf.before-day-ref4-20260726T185626Z
```

Protections:

- runtime `MemoryMax=13G` on `vibe-motion.service`;
- monitor guard at process RSS 12 GiB;
- monitor guard at `MemAvailable` 2 GiB;
- automatic safe-config restore if the PID changes or a memory guard fires;
- automatic safe-config restore and service restart after 24 hours;
- runtime memory limit is removed during cleanup.

Useful read-only checks:

```sh
sudo systemctl status vibe-motion-cam12-ref4-day --no-pager -l
sudo tail -n 20 /var/tmp/cam12-ref4-day-progress-20260726T185626Z.log
sudo tail -n 20 /var/tmp/cam12-ref4-day-20260726T185626Z.tsv
sudo systemctl show vibe-motion -p MainPID -p MemoryCurrent -p MemoryMax
curl -fsS http://127.0.0.1:8892/status.json | jq
```

To stop the experiment early, stop the monitor unit:

```sh
sudo systemctl stop vibe-motion-cam12-ref4-day
```

Its signal trap restores the safe cam12 config, restarts `vibe-motion`, and
removes the runtime memory limit.

After automatic or manual cleanup, verify:

```sh
sudo systemctl is-active vibe-motion
sudo systemctl show vibe-motion -p MemoryMax
sudo grep -E '^[[:space:]]*timelapse_(pixel_format|x265_params)' \
  /etc/vibe-motion/camera12.conf
```

Expected safe state:

- `vibe-motion` is active;
- `MemoryMax=infinity`;
- camera12.conf contains no `timelapse_pixel_format` or
  `timelapse_x265_params` override;
- status JSON reports 8 connected cameras and zero errors.

## Production backups

Known binary/config backups:

```text
/usr/local/bin/vibe-motion.before-cam12-profile-20260726T104213Z
/usr/local/bin/vibe-motion.before-fps-metadata-20260726T111541Z
/etc/vibe-motion/camera12.conf.before-cam12-profile-20260726T104213Z
/etc/vibe-motion/camera12.conf.before-day-ref4-20260726T185626Z
```

## Recommended next steps

1. Let the current monitor complete unless a guard restores safe mode first.
2. Summarize RSS immediately before and after every top-of-hour rotation.
3. Check whether the RSS steps shrink to zero, plateau, or remain linear.
4. Compare the completed hourly cam12 files by:
   - bytes per captured frame;
   - wall-hour storage;
   - nominal and average 30 fps;
   - keyframe count;
   - scene/light conditions.
5. If memory plateaus safely, decide whether to keep CRF 40 or move to CRF 42:
   CRF 42 matched the safe file size with much better same-source SSIM.
6. If memory grows every hour, test `malloc_trim(0)` after writer close before
   enabling this profile for more cameras.
7. Do not enable lookahead 40, B-frames 8, or the full original profile on all
   cameras based on these results. Lookahead 40 is the clearest RAM multiplier,
   and B8 did not provide a compression benefit on the controlled fragment.
