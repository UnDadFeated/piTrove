"""Exercise the production controller against isolated media and settings."""
import base64
import datetime
import http.client
import json
import os
from pathlib import Path
import re
import socket
import struct
import sqlite3
import subprocess
import sys
import tempfile
import time

KEY = "fixture-controller-key"


# Two lossless 320×180 solid-color WebP fixtures.
SYNTHETIC_MEDIA = (
    "UklGRiQAAABXRUJQVlA4TBgAAAAvP8EsAAdQs840s/8BAEX6/58i+p/6/wM=",
    "UklGRiQAAABXRUJQVlA4TBgAAAAvP8EsAAfQmZo1s/8BAEX6/58i+p/6/wM=",
)


def tiff_fixture():
    width, height = 32, 24
    bits_offset, pixels_offset = 134, 140
    entries = [
        (256, 4, 1, width), (257, 4, 1, height), (258, 3, 3, bits_offset),
        (259, 3, 1, 1), (262, 3, 1, 2), (273, 4, 1, pixels_offset),
        (277, 3, 1, 3), (278, 4, 1, height), (279, 4, 1, width * height * 3), (284, 3, 1, 1),
    ]
    return (b"II" + struct.pack("<HIH", 42, 8, len(entries)) +
            b"".join(struct.pack("<HHII", *entry) for entry in entries) +
            struct.pack("<IHHH", 0, 8, 8, 8) + bytes((24, 160, 80)) * width * height)


def common_image_fixtures():
    jpeg = base64.b64decode(
        "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYxLjE5LjEwMQD/2wBDAAgEBAQEBAUFBQUFBQYGBgYGBgYGBgYH"
        "BwcICAgHBwcGBgcHCAgICAkJCQgICAgJCQoKCgwMCwsODg4RERT/xABMAAEBAAAAAAAAAAAAAAAAAAAABwEBAQAAAAAA"
        "AAAAAAAAAAAABQcQAQAAAAAAAAAAAAAAAAAAAAARAQAAAAAAAAAAAAAAAAAAAAD/wAARCAAQABADASIAAhEAAxEA"
        "/9oADAMBAAIRAxEAPwCOAL+Lf//Z"
    )
    png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAACXBIWXMAAAABAAAAAQBPJcTWAAAAFUlEQVR4"
        "nGNgYPhLIhrVMKph2GoAAISu/QH+5zsIAAAAAElFTkSuQmCC"
    )
    gif = (b"GIF89a" + struct.pack("<HHBBB", 5, 4, 0x80, 0, 0) + b"\x00\x00\x00\xff\xff\xff"
           + b"," + struct.pack("<HHHHB", 2, 1, 1, 1, 0) + b"\x02\x02\x44\x01\x00;")
    bmp = (struct.pack("<2sIHHI", b"BM", 78, 0, 0, 54)
           + struct.pack("<IiiHHIIiiII", 40, 4, -2, 1, 24, 0, 24, 0, 0, 0, 0)
           + bytes((255, 0, 0)) * 8)
    bottom_up = bytearray(bmp)
    bottom_up[22:26] = struct.pack("<i", 2)
    return {
        "fixture.jpg": (jpeg, "16x16"),
        "fixture.png": (png, "16x16"),
        "offset-fixture.gif": (gif, "5x4"),
        "fixture.bmp": (bottom_up, "4x2"),
        "top-down-fixture.bmp": (bmp, "4x2"),
        "tiff-fixture.tiff": (tiff_fixture(), "32x24"),
    }


def verify(port, cache, version):
    def request(path, key=None, *, source="127.0.0.1", host=None, method="GET", body=None):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5, source_address=(source, 0))
        headers = {}
        if key:
            headers["Authorization"] = f"Bearer {key}"
        if host:
            headers["Host"] = host
        if body is not None:
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    deadline = time.monotonic() + 60
    while True:
        try:
            status, headers, payload = request("/api/status")
            if status == 200:
                break
        except OSError:
            pass
        if time.monotonic() >= deadline:
            raise RuntimeError("The production controller did not come up")
        time.sleep(1)

    status, _, dashboard = request("/")
    assert status == 200 and f"piTrove {version}</span>".encode() in dashboard
    assert not json.loads(payload).get("filename")
    assert headers["X-Content-Type-Options"] == "nosniff"
    assert request("/", host="untrusted.example")[0] == 400
    for path in ("/api/news", "/api/stocks", "/api/config", "/api/chores", "/api/google_photos/picker"):
        assert request(path)[0] == 401, path
    assert request("/google_photos_setup")[0] == 200
    status, _, payload = request("/google_photos_setup", KEY, method="POST",
                                 body="client_id=fixture-client&client_secret=fixture-secret")
    assert status == 200 and "photospicker.mediaitems.readonly" in json.loads(payload)["authorization_url"]
    settings = json.loads(request("/api/settings", KEY)[2])
    assert settings["gcalendar_ical_url_set"] is True and "gcalendar_ical_url" not in settings
    assert request("/api/settings/update?transition_delay=nan", KEY)[0] == 400
    assert request("/api/settings/update?gcalendar_ical_url=https://evil.test/x.ics", KEY)[0] == 400
    assert request("/api/settings/update?play_just_photos=1&play_just_videos=1", KEY)[0] == 400
    assert request("/api/settings/update?timezone=Mars/Olympus", KEY)[0] == 400
    assert request("/api/settings/update?news_local_query=hello%0Aworld", KEY)[0] == 400
    assert "revision" in json.loads(request("/api/config", KEY)[2])
    board = json.loads(request("/api/chores", KEY)[2])
    assert "today" in board and not board["people"]
    assert request("/api/chores/people/save", KEY, method="POST", body="name=Fixture&color=%23336699")[0] == 200

    status, _, payload = request("/api/screenshot", KEY)
    assert status == 200 and payload.startswith(b"\x89PNG"), status
    for _ in range(5):
        assert request("/api/status", "fixture-invalid")[0] == 401
    assert request("/api/status", KEY)[0] == 429
    assert request("/api/config", KEY)[0] == 429
    assert request("/api/config", KEY, source="127.0.0.2")[0] == 200
    assert request("/api/status")[0] == 200
    with sqlite3.connect(cache / "cache.db") as database:
        plan = database.execute(
            "EXPLAIN QUERY PLAN SELECT path,type FROM cache WHERE bad=0 AND preprocessed=0 LIMIT 20"
        ).fetchall()
        assert any("idx_cache_needs_preprocess" in row[3] for row in plan), plan


def smoke(image):
    cmake = (Path(__file__).resolve().parents[1] / "CMakeLists.txt").read_text()
    version = re.search(r"project\(piTrove VERSION (\d+\.\d+\.\d+)", cmake).group(1)
    with tempfile.TemporaryDirectory(prefix="pitrove-controller-") as directory:
        root = Path(directory)
        for name in ("config", "media", "cache", "logs", "screenshots"):
            (root / name).mkdir()
        with socket.socket() as server:
            server.bind(("127.0.0.1", 0))
            port = server.getsockname()[1]
        (root / "config/config.toml").write_text(f"""
[paths]
media_dir = "/app/media"
cache_dir = "/app/cache"
log_dir = "/app/logs"
[display]
resolution = 640,360
fullscreen = 0
bg_style = "pattern"
[slideshow]
transition_delay = 10
transition_effect = "none"
bias_lighting = 1
edge_glow_shadow = 1
[scan]
window_days = 0
[remote]
http_enabled = 1
http_port = {port}
api_key = "{KEY}"
[infopanels]
enabled = 1
left_panel_enabled = 1
[gcalendar]
enabled = 0
ical_url = "https://example.invalid/private-fixture/basic.ics"
[stockstreamer]
enabled = 0
[news]
enabled = 0
[keepalive]
enabled = 0
""", encoding="utf-8")
        for i, media in enumerate(SYNTHETIC_MEDIA):
            filename = f"{datetime.date.today():%Y%m%d}_family-fixture-{i}.webp"
            (root / "media" / filename).write_bytes(base64.b64decode(media))

        fixtures = common_image_fixtures()
        for name, (data, _) in fixtures.items():
            (root / "media" / name).write_bytes(data)
        rejected = bytearray(fixtures["fixture.png"][0])
        rejected[16:20] = struct.pack(">I", 20000)
        (root / "media/oversized-fixture.png").write_bytes(rejected)

        command = [
            "docker", "run", "--detach", "--init", "--network", "host", "--cpus", "0.75",
            "--user", f"{os.getuid()}:{os.getgid()}", "-e", "PITROVE_LOCK_PATH=/tmp/pitrove-smoke.lock",
            "-e", "SDL_VIDEO_DRIVER=offscreen", "-e", "SDL_AUDIO_DRIVER=dummy",
            "-e", "SDL_RENDER_DRIVER=software", "-e", "LIBGL_ALWAYS_SOFTWARE=1",
        ]
        for name in ("config", "media", "cache", "logs", "screenshots"):
            suffix = ":ro" if name == "media" else ""
            command += ["-v", f"{root / name}:/app/{name}{suffix}"]
        container = subprocess.check_output(command + [image], text=True).strip()
        try:
            verify(port, root / "cache", version)
            for name, (_, dimensions) in fixtures.items():
                decoded = subprocess.check_output(["docker", "exec", container, "/app/piTrove",
                                                   "--selftest-media", "/app/media/" + name], text=True)
                assert f"{dimensions} RGBA" in decoded, (name, decoded)
            rejected = subprocess.run(["docker", "exec", container, "/app/piTrove",
                                       "--selftest-media", "/app/media/oversized-fixture.png"],
                                      capture_output=True, text=True)
            assert rejected.returncode == 1 and "Could not decode image" in rejected.stderr
            subprocess.run(["docker", "exec", container, "/app/scripts/healthcheck.sh"], check=True)
        except Exception:
            subprocess.run(["docker", "logs", "--tail", "60", container], check=False)
            raise
        finally:
            subprocess.run(["docker", "stop", "--time", "15", container], check=False, stdout=subprocess.DEVNULL)
            subprocess.run(["docker", "rm", "-f", container], check=False, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    smoke(sys.argv[1])
    print("Production controller, access key isolation, chores, screenshot, SQLite index, Picker entry point, and image decoding passed")
