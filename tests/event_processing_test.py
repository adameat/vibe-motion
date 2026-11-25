"""End-to-end event processing against a disposable MySQL 8 and a loopback Bot API."""
import contextlib
import datetime
import email.policy
import getpass
import http.server
import json
from pathlib import Path
import shutil
import signal
import socket as network_socket
import subprocess
import sys
import tempfile
import threading
import time
from email.parser import BytesParser

BINARY = Path(sys.argv[1]).resolve()
SCHEMA = Path(__file__).parent / "fixtures" / "event-schema.sql"


def until(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out at {predicate.__code__.co_firstlineno}")


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run(root, stack):
    responses = []
    attempts = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            self.end_headers()
            process = subprocess.Popen(
                ["ffmpeg", "-nostdin", "-loglevel", "error", "-re", "-f", "lavfi",
                 "-i", "testsrc2=size=160x120:rate=10", "-c:v", "libx264", "-threads", "1",
                 "-preset", "ultrafast", "-g", "10", "-f", "mpegts", "-"],
                stdout=subprocess.PIPE,
            )
            try:
                while chunk := process.stdout.read(1316):
                    self.wfile.write(chunk)
            except (ConnectionError, BrokenPipeError):
                pass
            finally:
                stop(process)
                process.stdout.close()

        def do_POST(self):
            prefix, method = self.path.rsplit("/", 1)
            assert prefix == "/bot123:test"
            data = self.rfile.read(int(self.headers["Content-Length"]))
            mime = BytesParser(policy=email.policy.default).parsebytes(
                f"Content-Type: {self.headers['Content-Type']}\r\n\r\n".encode() + data)
            parts = {p.get_param("name", header="content-disposition"): p.get_payload(decode=True)
                     for p in mime.iter_parts()}
            assert parts["chat_id"] == b"-123"
            attempts.append(method)
            code = 200
            if len(attempts) == 1:
                code = 429
                result = {"ok": False, "parameters": {"retry_after": 1}}
            else:
                result = {"ok": True, "result": True if method == "deleteMessage" else
                          {"message_id": 101 if method == "sendVideo" else 100}}
                responses.append((method, parts))
            body = json.dumps(result).encode()
            self.send_response(code)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    stack.callback(server.server_close)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    stack.callback(server.shutdown)
    url = f"http://127.0.0.1:{server.server_port}"
    datadir = root / "mysql"
    socket = root / "test.sock"
    mysql = ["mysql", "--no-defaults", f"--socket={socket}", "-uroot", "-BN"]
    mysqld = ["mysqld", "--no-defaults", f"--datadir={datadir}",
              f"--user={getpass.getuser()}", f"--log-error={root / 'mysql.log'}"]
    subprocess.run([*mysqld, "--initialize-insecure"], check=True)
    db = subprocess.Popen([*mysqld, f"--socket={socket}", "--skip-networking", "--mysqlx=OFF",
                           f"--pid-file={root / 'mysql.pid'}"])
    stack.callback(stop, db)
    until(lambda: socket.exists() and subprocess.run([*mysql, "-e", "select 1"],
          capture_output=True).returncode == 0)
    subprocess.run(mysql, input="CREATE DATABASE motion; USE motion;\n" + SCHEMA.read_text() +
                   "INSERT INTO cams(id,tz) VALUES(1,'Asia/Bangkok');", text=True, check=True)
    def sql(query):
        return subprocess.check_output([*mysql, "motion", "-e", query], text=True).strip()

    (root / "client.cnf").write_text(f"[client]\nuser=root\nsocket={socket}\n")
    (root / "token").write_text("123:test\n")
    common = (Path(__file__).parent / "fixtures" / "runtime" / "motion.conf").read_text()
    common = "\n".join(line for line in common.splitlines() if not line.startswith(
        ("log_file", "webcontrol_port", "on_", "target_dir", "camera ")))
    (root / "motion.conf").write_text(common + f"""
target_dir {root}
event_db_options_file {root}/client.cnf
event_database motion
event_processing on
event_retention_days 0
event_video_options -vf "scale=480:-2,setpts=PTS/6,setsar=1:1" -c:v libx264 -preset veryfast -an -crf 28 -profile:v baseline -r 30
event_telegram on
event_link_prefix https://example.invalid/e/
telegram_token_file {root}/token
telegram_chat_id -123
telegram_api_url {url}
camera camera.conf
""")
    (root / "camera.conf").write_text(f"camera_id 1\ncamera_name cam01\ncamera_url {url}/live.ts\nmedia_transport direct\n")
    args = [str(BINARY), "-n", "-c", str(root / "motion.conf")]
    subprocess.run([*args, "--check-config", "--strict-config"], check=True)
    def start(*extra):
        process = subprocess.Popen([*args, *extra])
        stack.callback(stop, process)
        return process
    conf = root / "motion.conf"
    enabled = conf.read_text()
    conf.write_text(enabled.replace("event_processing on", "event_processing off")
                    .replace("event_database motion", ""))
    capture = start()
    until(lambda: (root / "snapshot" / "cam01" / "lastsnap.jpg").exists())
    assert not (root / ".vibe-events").exists()
    conf.write_text(enabled)
    capture.send_signal(signal.SIGHUP)
    until(lambda: any(method == "editMessageMedia" for method, _ in responses))
    before = sql("SELECT COUNT(*) FROM responses")
    conf.write_text(enabled.replace("event_processing on", "event_processing off"))
    capture.send_signal(signal.SIGHUP)
    def has_managed_worker():
        children = Path(f"/proc/{capture.pid}/task/{capture.pid}/children").read_text().split()
        for child in children:
            try:
                if b"--event-worker" in Path(f"/proc/{child}/cmdline").read_bytes().split(b"\0"):
                    return True
            except FileNotFoundError:
                pass
        return False
    until(lambda: not has_managed_worker())
    assert int(before) > 0
    stop(capture)
    conf.write_text(enabled)
    worker = start("--event-worker")
    until(lambda: any(method == "deleteMessage" for method, _ in responses))
    assert attempts.count("sendPhoto") == 2
    assert sql("SELECT COUNT(*) FROM events") == "1"
    assert sql("SELECT message_id FROM events WHERE end_time IS NOT NULL") == "101"
    assert sql("SELECT count(*) FROM timelapses WHERE status='ok'") == "1"
    assert sql("SELECT count(*) FROM files WHERE status='ok'") == "4"
    assert (root / "snapshot" / "cam01" / "lastsnaps.jpg").is_file()
    by_method = dict(responses)
    assert b"LIVE" in by_method["sendPhoto"]["caption"]
    assert json.loads(by_method["editMessageMedia"]["media"])["media"] == "attach://photo"
    assert b"https://example.invalid/e/1" in by_method["sendVideo"]["caption"]
    assert b"ftyp" in by_method["sendVideo"]["video"][:16]
    assert by_method["deleteMessage"]["message_id"] == b"100"
    video = sql("SELECT path FROM files WHERE type='media.video'")
    stream = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams",
                                                  "-of", "json", video]))["streams"][0]
    assert (stream["codec_name"], stream["width"]) == ("h264", 480)

    stop(worker)

    # Resume a conversion queued by the old worker, process external
    # pictures across midnight, and expire a worker-only camera with delivery disabled.
    original = Path(sql("SELECT path FROM files WHERE type='motion.movie'"))
    source = root / "legacy movie.mkv"
    shutil.copyfile(original, source)
    encoded = root / "legacy movie-video.mp4"
    retired = root / "retired.jpg"
    retired.write_bytes(b"old")
    active_picture = root / "active-event.jpg"
    active_picture.write_bytes(b"open event")
    writing_movie = root / "still-writing.mkv"
    writing_movie.write_bytes(b"recording")
    stamp = datetime.datetime.now()
    external = root / "ext" / "cam1" / stamp.strftime("%Y/%m/%d") / stamp.strftime("upload_cam_%Y%m%d%H%M%S.jpg")
    external.parent.mkdir(parents=True)
    shutil.copyfile(root / "snapshot" / "cam01" / "lastsnaps.jpg", external)
    # Camera 13 uploaded just before midnight. Its event closed after midnight,
    # while the worker was stopped; there is no upload directory for today.
    midnight = stamp.replace(hour=0, minute=0, second=0, microsecond=0)
    previous = midnight - datetime.timedelta(seconds=1)
    late_upload = (root / "ext" / "cam13" / previous.strftime("%Y/%m/%d")
                   / previous.strftime("upload_cam_%Y%m%d%H%M%S.jpg"))
    late_upload.parent.mkdir(parents=True)
    late_upload.write_bytes(b"previous day's upload")
    sql(f"""
        INSERT INTO cams(id,max_event_days,last_external) VALUES(13,1,'{midnight}');
        INSERT INTO events(id,cam_id,start_time,end_time) VALUES
            (10,1,now()-INTERVAL 1 HOUR,now()-INTERVAL 59 MINUTE),
            (11,13,now()-INTERVAL 3 DAY,now()-INTERVAL 3 DAY),
            (12,13,'{previous}','{midnight}'),
            (13,13,now()-INTERVAL 5 DAY,NULL),
            (14,13,now()-INTERVAL 4 DAY,now()-INTERVAL 3 DAY);
        INSERT INTO files VALUES
            (10,'motion.movie','{source}','ok'), (10,'media.video','{encoded}','converting'),
            (11,'motion.picture','{retired}','ok'),
            (13,'motion.picture','{active_picture}','ok'),
            (14,'motion.movie','{writing_movie}','writing');
        INSERT INTO jobs(command,touch_event_id,touch_file_type,sender,status)
            VALUES('obsolete encoder command',10,'media.video','{network_socket.gethostname().split('.')[0]}','new');
    """)
    conf = root / "motion.conf"
    conf.write_text(conf.read_text().replace("event_telegram on", "event_telegram off")
                    .replace("event_retention_days 0", "event_retention_days -1\nevent_worker_camera_ids 13"))
    sent = len(attempts)
    start("--event-worker")
    until(lambda: sql("SELECT count(*) FROM jobs") == "0")
    # The two oldest events are ineligible. Maintenance should skip both
    # and remove the next expired event in the same pass.
    until(lambda: sql("SELECT count(*) FROM events WHERE id IN (11,13,14)") == "2")
    assert sql("SELECT id FROM events WHERE id IN (11,13,14) ORDER BY id").splitlines() == ["13", "14"]
    assert sql("SELECT status FROM files WHERE event_id=14") == "writing"
    assert active_picture.exists() and writing_movie.exists()
    until(lambda: sql("SELECT count(*) FROM files WHERE type='external.picture'") == "2")
    destination = (root / "events" / "cam13" / previous.strftime("%Y/%m")
                   / previous.strftime("cam13-%Y%m%d%H%M%S-ext.jpg"))
    assert sql("SELECT path FROM files WHERE event_id=12 AND type='external.picture'") == str(destination)
    assert destination.read_bytes() == b"previous day's upload" and not late_upload.exists()
    assert sql("SELECT last_external FROM cams WHERE id=13") == str(midnight)
    assert encoded.stat().st_size and not retired.exists() and not external.exists()
    assert len(attempts) == sent


if __name__ == "__main__":
    # MySQL and MariaDB have distinct database bootstrap commands.
    version = subprocess.check_output(["mysqld", "--version"], text=True) if shutil.which("mysqld") else ""
    if "8." not in version or not all(shutil.which(x) for x in ("mysql", "ffmpeg", "ffprobe")):
        sys.exit(77)
    with tempfile.TemporaryDirectory() as temp, contextlib.ExitStack() as stack:
        run(Path(temp), stack)
