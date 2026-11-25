# Event compression and Telegram on pearl

The capture process writes ordered event records under `target_dir/.vibe-events/<camera_id>/`. The recorder starts a lower-priority child process of the same executable to consume them. It restarts the child if it exits and applies configuration changes on SIGHUP. Only the newest pending periodic snapshot is retained. Separate threads handle MySQL intake, FFmpeg, Telegram, and periodic maintenance. Movies keep their original recording. The worker publishes a sibling `-video.mp4` after FFmpeg finishes successfully.

## Configuration

Put shared options **before** the first `camera` include in the main configuration. A camera file can override the media, retention, and Telegram options. This example uses pearl’s existing camera rows for credentials and routing.

```conf
target_dir /cam
event_db_options_file /etc/vibe-motion/event-db.cnf
event_database motion
event_processing on
event_compression on
event_telegram on
event_link_prefix https://xenohome.com/e/
event_timezone Asia/Bangkok
event_retention_days 2
# Also maintain the historical camera omitted from the capture includes.
event_worker_camera_ids 13
```

Set `event_database` to the actual schema name. Supply the existing database connection in a MySQL option file readable by the service user. For example:

```ini
[client]
host=database.example
user=motion
password=replace-with-the-database-password
```

| Option | Behavior |
| --- | --- |
| `event_processing` | Enables event intake and processing for this camera. Defaults to off. Both processes read the same configuration. |
| `event_spool_dir` | Global absolute queue directory. Defaults to `target_dir/.vibe-events`. The service user needs write access. |
| `event_worker_camera_ids` | Additional comma-separated camera IDs for worker-only processing, using main-file defaults. Enabled capture cameras are included automatically. |
| `event_compression` | Enables conversion of closed event movies. Defaults to on for processed cameras. |
| `event_video_options` | FFmpeg output arguments. Quoting groups arguments containing spaces. An empty setting uses `cams.video_options`. |
| `event_snapshot_width`, `event_thumbnail_width` | Width of the small live JPEG and the event thumbnail, respectively. Defaults are 480 and 90 pixels. |
| `event_telegram` | Enables delivery, live edits, and Telegram message removal for the camera. Defaults to off. |
| `telegram_token_file`, `telegram_chat_id` | Override `cams.bot_id` and `cams.chat_id` independently. The file contains the token on one line. Use an absolute path. |
| `telegram_api_url` | Bot API server. Defaults to `https://api.telegram.org`. |
| `telegram_updates_per_minute` | Global rolling budget for media sends and edits, including requests recorded by the legacy sender. Defaults to 15. Zero disables this budget. HTTP 429 also backs off according to Telegram’s response. |
| `event_timezone`, `event_link_prefix` | Caption timezone and optional event-page URL prefix. An empty timezone uses `cams.tz`. The URL ends with the database event ID. |
| `event_retention_days` | Deletes expired event media, chat messages, and database rows. The default of -1 uses `cams.max_event_days`. Zero disables expiry. |

To move the encoder profile out of MySQL, set the shared arguments in the main file:

```conf
event_video_options -vf "scale=480:-2,setpts=PTS/6,setsar=1:1" -c:v libx264 -preset veryfast -an -crf 28 -profile:v baseline -r 30
```

For cameras 4 and 10, add `-x264-params keyint=500` to the full value in each camera file. The worker uses FFmpeg for JPEG derivatives as well. Existing `picture_output`, `snapshot_interval`, and movie settings still control which source media the recorder produces.

The event queue survives a worker restart. If a Telegram upload succeeds and the connection drops before its response arrives, retrying can post a duplicate message.

## Migration

1. Install the standard build and its runtime libraries. Event processing uses the FFmpeg executable, the MySQL client’s authentication support, and system timezone data.
2. Use the existing MySQL schema and camera IDs. The service user needs read/write access to the existing `cams`, `events`, `files`, `jobs`, `responses`, `traces`, and `timelapses` tables and to the media tree under `target_dir`. On pearl, provision the queue before cutover with `install -d -o motion -g motion -m 0770 /cam/.vibe-events`. Set up the database option file and check the proposed main configuration with `vibe-motion -c /etc/vibe-motion/motion.conf --check-config`.
3. At cutover, stop capture cleanly so open movies close. Drain in-flight legacy jobs and sends. Stop the old event-compression service and any periodic job for the migrated cameras. Remove the five `on_*` callbacks that invoke `motion.py` from the migrated configuration. For a one-camera pilot, exclude its ID from the legacy periodic list and drain that camera’s legacy work before enabling its new worker.
4. Start the recorder with the new configuration. It launches the event worker when any camera has `event_processing on` or `event_worker_camera_ids` lists additional cameras. To turn processing off, disable it for every camera, clear the extra IDs, and reload the recorder. The media-maintenance and timelapse-archive timers have separate responsibilities.
5. Check a live event, its final video and link, and the original recording. For rollback, stop capture, run `vibe-motion --event-worker -c /etc/vibe-motion/motion.conf` separately to drain pending records and uploads, then stop it and restore the original callbacks and legacy worker configuration.

The worker recognizes closed movies and pending video conversions already present in the database. It rebuilds FFmpeg arguments from the camera profile. Web clients continue to see the same event IDs, paths, snapshot fields, and Telegram message IDs. The periodic worker also matches external camera uploads by timestamp and expires old events according to each camera's retention setting.
