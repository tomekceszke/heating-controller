#!/usr/bin/env python3
"""Serves firmware/web/app.html with mocked API data, for UI work without a device.

Usage: tools/preview.py [--port 8099] [--board 1|2] [--shoot docs/img]

The page goes through home-idf's render_page.py, so it is byte for byte what the firmware embeds.
--shoot saves Live / History / Settings at 390x844 (pixel ratio 2) in headless Chrome and exits.
"""
import argparse
import http.server
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "firmware" / "web"
HOME_IDF = ROOT.parent / "home-idf"
sys.path.insert(0, str(HOME_IDF / "tools"))
import render_page  # noqa: E402

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
NAME = "h-controller"
WIDTH, HEIGHT, SCALE = 390, 844, 2
MIN_VIEWPORT = 500
ZOOM = MIN_VIEWPORT / WIDTH

BOARDS = {
    1: ("Heating 1", "2462abf200dc", [
        ("370000001287ce28", 10, "Heater supply", "Heater", "supply", 54.2),
        ("5900000008f0a828", 11, "Heater return", "Heater", "return", 43.8),
        ("9b00000009029f28", 99, "Outdoor", "Outdoor", "", 11.4),
    ], ["outdoor", "heater supply", "heater return"]),
    2: ("Heating 2", "ec626083a66c", [
        ("b700000014e83928", 21, "Radiators return", "Radiators", "return", 34.1),
        ("2800000008efc028", 22, "Kitchen radiators supply", "Kitchen radiators", "supply", 41.7),
        ("39000000150fc228", 23, "Kitchen radiators return", "Kitchen radiators", "return", 33.2),
        ("7100000008ece828", 31, "Floor supply", "Floor", "supply", 30.6),
        ("a60416586fb5ff28", 32, "Floor return", "Floor", "return", 26.9),
        ("77041469f284ff28", 33, "Hallway floor supply", "Hallway floor", "supply", 29.8),
        ("f10316555c4bff28", 34, "Hallway floor return", "Hallway floor", "return", 26.1),
    ], ["floor supply", "kitchen supply", "floor drop"]),
}

SYSTEM = {
    "version": "2.1.0", "idf": "v5.4.2", "bootloader_idf": "v5.4.2", "partition": "ota_0",
    "pending_verify": False, "reset_reason": "power on", "uptime_s": 93600,
    "up_since": "2026-09-16 21:15", "time_synced": True, "ota_running": False,
    "heap_kb": {"free": 121, "min": 108},
    "wifi": {"connected": True, "rssi": -58, "channel": 6, "bssid": "b0:4e:26:aa:bb:cc"},
}


def status(board):
    label, mac, sensors, captions = BOARDS[board]
    rows = [{"id": i, "label": lb, "group": g, "dir": d, "order": o,
             "c": c, "age_s": 12, "errors": 0, "lost": False}
            for i, o, lb, g, d, c in sensors]
    by_id = {r["id"]: r for r in rows}
    if board == 1:
        values = [by_id["9b00000009029f28"]["c"], by_id["370000001287ce28"]["c"], by_id["5900000008f0a828"]["c"]]
        units = ["°C", "°C", "°C"]
    else:
        values = [by_id["7100000008ece828"]["c"], by_id["2800000008efc028"]["c"],
                  round(by_id["7100000008ece828"]["c"] - by_id["a60416586fb5ff28"]["c"], 1)]
        units = ["°C", "°C", "K"]
    return {
        "device": label, "mac": mac, "sensors_alive": True, "sensors": rows,
        "highlights": [{"caption": c, "unit": u, "value": v, "age_s": 12}
                       for c, u, v in zip(captions, units, values)],
        "mqtt": {"connected": True, "outbox_bytes": 0, "outbox_limit_bytes": 24576,
                 "broker": "mqtt://192.168.11.16:1883"},
        "sample_period_s": 60, "system": SYSTEM,
    }


def events(board):
    now = int(time.time())
    label = BOARDS[board][2][0][2]
    return {"events": [
        {"ts": now - 300, "type": "mqtt", "on": True, "count": 0, "prev_count": 0, "temp_c": None, "detail": ""},
        {"ts": now - 1800, "type": "sensor_back", "on": False, "count": 0, "prev_count": 0,
         "temp_c": 33.4, "detail": label},
        {"ts": now - 2400, "type": "sensor_lost", "on": True, "count": 0, "prev_count": 0,
         "temp_c": None, "detail": label},
        {"ts": now - 93000, "type": "sensors", "on": False, "count": len(BOARDS[board][2]),
         "prev_count": 0, "temp_c": None, "detail": ""},
    ]}


def history(board):
    n = 1440
    out = []
    for idx, (sid, _o, label, _g, _d, base) in enumerate(BOARDS[board][2]):
        series = []
        for i in range(n):
            if i % 173 == 0:
                series.append(None)
                continue
            series.append(int(round((base + 3.5 * math.sin((i / n) * 6.3 + idx) - idx * 0.2) * 10)))
        out.append({"id": sid, "label": label, "t": series})
    return {"period_s": 60, "newest": int(time.time()), "newest_age_s": 14, "series": out}


SHOOT_INJECT = """
<style>
  html { zoom: @ZOOM@; }                       /* headless Chrome clamps the window to 500 px: zoom back to a phone */
  *, *::before, *::after { animation: none !important; transition: none !important; }
</style>
<script>
  // The page polls /api/status every 5 s; under Chrome's virtual clock that loop never lets the shot happen.
  (() => { const real = window.setTimeout; window.setTimeout = (fn, ms, ...rest) => (ms >= 2000 ? 0 : real(fn, ms, ...rest)); })();
</script>
"""


def handler_for(board, shooting=False):
    page = render_page.render((WEB / "app.html").read_text("utf-8"), NAME)
    if shooting:
        page = page.replace("</body>", SHOOT_INJECT.replace("@ZOOM@", f"{ZOOM:.5f}") + "</body>")
    page = page.encode("utf-8")

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send_json(self, obj):
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/api/session":
                return self.send_json({"authenticated": True, "csrf": "preview", "version": "2.1.0"})
            if path == "/api/status":
                return self.send_json(status(board))
            if path == "/api/events":
                return self.send_json(events(board))
            if path == "/api/history":
                return self.send_json(history(board))
            if path == "/manifest.webmanifest":
                return self.send_json(json.loads((WEB / "manifest.webmanifest").read_text()))
            if path == "/apple-touch-icon.png":
                data = (WEB / "apple-touch-icon.png").read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                return self.wfile.write(data)
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)

        def do_POST(self):
            self.send_json({"ok": True})

    return Handler


SCENES = {"live": "live", "history": "history", "settings": "settings"}


def shoot(port, out_dir, board, scene):
    """Chrome writes the file and then hangs on this machine, so it is killed once the image stops growing."""
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"app-{board}-{scene}.png"
    raw = target.with_suffix(".raw.png")
    raw.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory() as profile:
        chrome = subprocess.Popen([
            CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
            f"--user-data-dir={profile}",
            f"--window-size={MIN_VIEWPORT},{round(HEIGHT * ZOOM)}",
            f"--force-device-scale-factor={SCALE}",
            "--virtual-time-budget=4000",
            f"--screenshot={raw}",
            f"http://127.0.0.1:{port}/#{scene}",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline, size = time.time() + 90, -1
        while time.time() < deadline:
            time.sleep(0.5)
            now = raw.stat().st_size if raw.exists() else -1
            if now > 0 and now == size:
                break
            size = now
            if chrome.poll() is not None:
                break
        chrome.kill()
        chrome.wait()
    if not raw.exists():
        raise SystemExit(f"{scene}: Chrome produced no image")
    subprocess.run(["sips", "-z", str(HEIGHT * SCALE), str(WIDTH * SCALE), str(raw), "--out", str(target)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw.unlink()
    print(target)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--board", type=int, default=2, choices=(1, 2))
    ap.add_argument("--shoot")
    ap.add_argument("--scene", default="live", choices=tuple(SCENES))
    args = ap.parse_args()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler_for(args.board, bool(args.shoot)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.port}/"
    if args.shoot:
        shoot(args.port, pathlib.Path(args.shoot), args.board, args.scene)
        server.shutdown()
        return
    print(f"Heating {args.board} mock on {url} (Ctrl-C to stop)")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
