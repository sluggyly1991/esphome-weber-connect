#!/usr/bin/env python3
"""Weber Connect ingress panel: connectivity status, magic pairing, phone handoff.

Runs the BLE telemetry bridge in the background and serves a small web UI over
Home Assistant ingress. All hub management (scan, pair, handoff, forget) happens
here with one tap; the add-on configuration stays minimal.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import signal
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from weber_ble_pair import (
    build_pairing_summary,
    load_or_create_pairing_keys,
    pair_once,
)
from weber_ble_pair import write_json_atomic as write_json_private
from weber_ble_scan import scan as ble_scan
from weber_status_bridge import (
    VERSION,
    build_state,
    load_pairing_summary,
    mqtt_publish,
    read_status_once,
    release_ble_connection,
    write_json_atomic,
)


LOGGER = logging.getLogger("weber_connect_panel")

BRIDGE_MESSAGE_VERSION = 10
PAIR_MESSAGE_VERSION = 11
LISTEN_SECONDS = 8.0
BLE_TIMEOUT = 20.0
PAIR_LISTEN_SECONDS = 90.0
SCAN_SECONDS = 20.0
MAX_PROBES = 4

DEFAULT_SETTINGS = {
    "address": None,
    "poll_seconds": 30,
    "handoff_minutes": 15,
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def clamp(value: int, low: int, high: int) -> int:
    return max(low, min(high, value))


class HubController:
    """Owns hub state and serializes every BLE operation."""

    def __init__(
        self,
        data_dir: Path,
        mqtt: dict[str, Any] | None,
    ) -> None:
        self.data_dir = data_dir
        self.settings_file = data_dir / "settings.json"
        self.summary_file = data_dir / "pairing_summary.json"
        self.key_file = data_dir / "pairing_keys.json"
        self.status_file = data_dir / "latest_status.json"

        self.mqtt = mqtt
        self.settings = dict(DEFAULT_SETTINGS)
        self.summary: dict[str, Any] | None = None

        self.scanning = False
        self.pairing = False
        self.candidates: list[dict[str, Any]] = []
        self.setup_error: str | None = None

        self.handoff_active = False
        self.handoff_until: float | None = None
        self._handoff_token = 0

        self.last_read_at: str | None = None
        self.last_read_ok = False
        self.last_error: str | None = None
        self.last_state: dict[str, Any] = {}
        self.mqtt_published_at: str | None = None
        self.mqtt_error: str | None = None

        self._ble_lock = asyncio.Lock()
        self._wake = asyncio.Event()

        self._load()

    # -- persistence ---------------------------------------------------------

    def _load(self) -> None:
        if self.settings_file.exists():
            try:
                stored = json.loads(self.settings_file.read_text(encoding="utf-8"))
                self.settings.update({k: stored[k] for k in DEFAULT_SETTINGS if k in stored})
            except (OSError, ValueError) as exc:
                LOGGER.warning("Could not read settings: %r", exc)
        if self.summary_file.exists():
            try:
                self.summary = load_pairing_summary(self.summary_file)
            except (OSError, ValueError) as exc:
                LOGGER.warning("Could not read pairing summary: %r", exc)

    def _save_settings(self) -> None:
        write_json_atomic(self.settings_file, self.settings)

    # -- derived state -------------------------------------------------------

    @property
    def address(self) -> str | None:
        if self.settings.get("address"):
            return self.settings["address"]
        if self.summary:
            return (self.summary.get("hub") or {}).get("ble_address")
        return None

    @property
    def paired(self) -> bool:
        return self.summary is not None and bool(self.address)

    def _can_bridge(self) -> bool:
        return self.paired and not self.handoff_active and not self.scanning and not self.pairing

    def state(self) -> str:
        if self.pairing:
            return "pairing"
        if self.scanning:
            return "scanning"
        if not self.paired:
            return "setup"
        if self.handoff_active:
            return "handoff"
        if not self.last_read_at:
            return "connecting"
        return "online" if self.last_read_ok else "offline"

    async def snapshot(self) -> dict[str, Any]:
        remaining = None
        if self.handoff_active and self.handoff_until is not None:
            remaining = max(0, int(self.handoff_until - time.time()))
        return {
            "version": VERSION,
            "state": self.state(),
            "paired": self.paired,
            "address": self.address,
            "hub": (self.summary or {}).get("hub"),
            "probes": self.last_state.get("probes", []),
            "probe_count": self.last_state.get("probe_count", 0),
            "last_read_at": self.last_read_at,
            "last_error": self.last_error,
            "setup_error": self.setup_error,
            "scanning": self.scanning,
            "pairing": self.pairing,
            "candidates": self.candidates,
            "handoff": {
                "active": self.handoff_active,
                "remaining_seconds": remaining,
                "auto_resume": self.handoff_until is not None,
            },
            "mqtt": {
                "configured": bool(self.mqtt and self.mqtt.get("host")),
                "published_at": self.mqtt_published_at,
                "error": self.mqtt_error,
            },
            "settings": {
                "poll_seconds": self.settings["poll_seconds"],
                "handoff_minutes": self.settings["handoff_minutes"],
            },
        }

    # -- actions -------------------------------------------------------------

    async def start_scan(self) -> dict[str, Any]:
        if self.scanning or self.pairing:
            return {"ok": False, "error": "Another hub operation is already running."}
        asyncio.get_running_loop().create_task(self._scan_task())
        return {"ok": True}

    async def _scan_task(self) -> None:
        self.scanning = True
        self.setup_error = None
        self.candidates = []
        try:
            async with self._ble_lock:
                if self.address:
                    await asyncio.to_thread(release_ble_connection, self.address)
                result = await ble_scan(SCAN_SECONDS, include_all=False, stop_on_weber=False)
            self.candidates = [
                {
                    "address": row.get("address"),
                    "name": row.get("local_name") or row.get("name") or "Weber Hub",
                    "rssi": row.get("rssi"),
                }
                for row in result.get("weber_candidates", [])
            ]
            if not self.candidates:
                self.setup_error = (
                    "No hub found. Make sure the hub is powered on, awake, and close "
                    "to your Home Assistant Bluetooth adapter, then try again."
                )
        except Exception as exc:
            LOGGER.error("Scan failed: %r", exc)
            self.setup_error = f"Bluetooth scan failed: {exc}"
        finally:
            self.scanning = False
            self._wake.set()

    async def pair(self, address: str | None) -> dict[str, Any]:
        if self.scanning or self.pairing:
            return {"ok": False, "error": "Another hub operation is already running."}
        asyncio.get_running_loop().create_task(self._pair_task(address))
        return {"ok": True}

    def _log_pair_events(self, result: dict[str, Any]) -> None:
        """Log whatever the hub sent during a failed pairing attempt."""
        events = result.get("events") or []
        if not events:
            LOGGER.warning("Hub sent no notifications at all during pairing")
            return
        for event in events:
            decoded = event.get("decoded") or {}
            envelope = decoded.get("envelope") or {}
            candidate = envelope.get("body_plain_candidate") or {}
            LOGGER.warning(
                "Pairing event source=%s type=%s hex=%s",
                event.get("source"),
                candidate.get("type_name") or "UNDECODED",
                event.get("hex"),
            )

    async def _pair_task(self, address: str | None) -> None:
        self.pairing = True
        self.setup_error = None
        try:
            async with self._ble_lock:
                if not address:
                    result = await ble_scan(SCAN_SECONDS, include_all=False, stop_on_weber=True)
                    candidates = result.get("weber_candidates", [])
                    if not candidates:
                        raise RuntimeError(
                            "No hub found nearby. Wake the hub and keep it close, then try again."
                        )
                    address = candidates[0]["address"]
                keys = load_or_create_pairing_keys(
                    path=self.key_file,
                    display_name="Home Assistant",
                    companion_id=None,
                    companion_public_key=None,
                    reset_key=False,
                )
                args = SimpleNamespace(
                    address=address,
                    version=PAIR_MESSAGE_VERSION,
                    timeout=BLE_TIMEOUT,
                    write_without_response=False,
                    listen_seconds=PAIR_LISTEN_SECONDS,
                )
                result = await pair_once(args, keys)
                response = result.get("pairing_response")
                if not response:
                    self._log_pair_events(result)
                    raise RuntimeError(
                        "The hub did not confirm pairing. When the hub beeps, "
                        "press the button on the hub, then try again."
                    )
                if response.get("status") != "CONFIRMED":
                    raise RuntimeError(f"The hub declined pairing ({response.get('status')}).")
                summary = build_pairing_summary(
                    address=address,
                    keys=keys,
                    pairing_response=response,
                    hub_name="Weber Connect Hub",
                    hub_serial=None,
                    hub_model="Connect Hub",
                    hub_software_revision=None,
                    hub_wifi_mac=None,
                )
                write_json_private(self.summary_file, summary, mode=0o600)
                self.summary = summary
                self.settings["address"] = address
                self._save_settings()
                self.last_read_at = None
                self.last_read_ok = False
                self.last_error = None
                LOGGER.info("Paired with hub at %s", address)
        except Exception as exc:
            LOGGER.error("Pairing failed: %r", exc)
            self.setup_error = str(exc)
        finally:
            self.pairing = False
            self._wake.set()

    async def handoff(self, minutes: int | None) -> dict[str, Any]:
        if not self.paired:
            return {"ok": False, "error": "No hub is paired yet."}
        if minutes is None:
            minutes = self.settings["handoff_minutes"]
        minutes = clamp(int(minutes), 0, 240)
        self.handoff_active = True
        self._handoff_token += 1
        token = self._handoff_token
        self._wake.set()
        async with self._ble_lock:
            await asyncio.to_thread(release_ble_connection, self.address)
        if minutes > 0:
            self.handoff_until = time.time() + minutes * 60
            asyncio.get_running_loop().create_task(self._auto_resume(token, self.handoff_until))
            LOGGER.info("Hub handed off to the phone app; auto-resume in %s minutes", minutes)
        else:
            self.handoff_until = None
            LOGGER.info("Hub handed off to the phone app until manually resumed")
        return {"ok": True}

    async def _auto_resume(self, token: int, until: float) -> None:
        await asyncio.sleep(max(0.0, until - time.time()))
        if self.handoff_active and self._handoff_token == token:
            LOGGER.info("Handoff window ended; reconnecting to hub")
            self.handoff_active = False
            self.handoff_until = None
            self._wake.set()

    async def resume(self) -> dict[str, Any]:
        self.handoff_active = False
        self.handoff_until = None
        self._handoff_token += 1
        self._wake.set()
        return {"ok": True}

    async def forget(self) -> dict[str, Any]:
        """Forget the hub locally. Pairing keys are kept so re-pairing is instant."""
        self.handoff_active = False
        self.handoff_until = None
        self.summary = None
        self.settings["address"] = None
        self._save_settings()
        self.last_state = {}
        self.last_read_at = None
        self.last_read_ok = False
        self.last_error = None
        self.candidates = []
        try:
            self.summary_file.unlink(missing_ok=True)
        except OSError as exc:
            LOGGER.warning("Could not remove pairing summary: %r", exc)
        self._wake.set()
        return {"ok": True}

    async def update_settings(self, payload: dict[str, Any]) -> dict[str, Any]:
        if "poll_seconds" in payload:
            self.settings["poll_seconds"] = clamp(int(payload["poll_seconds"]), 10, 3600)
        if "handoff_minutes" in payload:
            self.settings["handoff_minutes"] = clamp(int(payload["handoff_minutes"]), 0, 240)
        self._save_settings()
        self._wake.set()
        return {"ok": True, "settings": dict(self.settings)}

    # -- bridge loop ---------------------------------------------------------

    async def _read_cycle(self) -> None:
        async with self._ble_lock:
            if not self._can_bridge():
                return
            try:
                result = await read_status_once(
                    address=self.address,
                    companion_id=self.summary["companion_id"],
                    version=BRIDGE_MESSAGE_VERSION,
                    listen_seconds=LISTEN_SECONDS,
                    timeout=BLE_TIMEOUT,
                    write_without_response=False,
                )
            except Exception as exc:
                self.last_read_ok = False
                self.last_error = f"Could not reach the hub: {exc}"
                self.last_read_at = utc_now()
                LOGGER.warning("Read failed: %r", exc)
                return

        latest = result.get("latest_status")
        self.last_read_at = utc_now()
        if not latest:
            self.last_read_ok = False
            self.last_error = "Connected, but the hub sent no probe status."
            return

        self.last_read_ok = True
        self.last_error = None
        state = build_state(
            self.summary,
            latest,
            self.address,
            connected=bool(result.get("connected")),
            max_probes=MAX_PROBES,
        )
        self.last_state = state
        write_json_atomic(self.status_file, state)
        await self._publish(state)

    async def _publish(self, state: dict[str, Any]) -> None:
        if not (self.mqtt and self.mqtt.get("host")):
            return
        args = SimpleNamespace(
            topic_prefix="weber_connect",
            discovery_prefix="homeassistant",
            discovery=True,
            retain=True,
            poll_seconds=self.settings["poll_seconds"],
            max_probes=MAX_PROBES,
            mqtt_host=self.mqtt.get("host"),
            mqtt_port=int(self.mqtt.get("port") or 1883),
            mqtt_username=self.mqtt.get("username") or None,
            mqtt_password=self.mqtt.get("password") or None,
        )
        try:
            await asyncio.to_thread(mqtt_publish, args, state, self.summary)
            self.mqtt_published_at = utc_now()
            self.mqtt_error = None
        except Exception as exc:
            self.mqtt_error = str(exc)
            LOGGER.error("MQTT publish failed: %r", exc)

    async def run(self) -> None:
        while True:
            self._wake.clear()
            if self._can_bridge():
                await self._read_cycle()
                try:
                    await asyncio.wait_for(self._wake.wait(), timeout=self.settings["poll_seconds"])
                except asyncio.TimeoutError:
                    pass
            else:
                await self._wake.wait()


class PanelRequestHandler(BaseHTTPRequestHandler):
    controller: HubController | None = None
    loop: asyncio.AbstractEventLoop | None = None
    index_file: Path | None = None

    def log_message(self, format: str, *args: Any) -> None:
        LOGGER.debug("http: " + format, *args)

    def _call(self, coro: Any) -> Any:
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout=60)

    def _send_json(self, payload: dict[str, Any], status: int = 200) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _route(self) -> str:
        return self.path.split("?", 1)[0].rstrip("/").rsplit("/", 1)[-1]

    def do_GET(self) -> None:  # noqa: N802 (http.server API)
        if self._route() == "status":
            self._send_json(self._call(self.controller.snapshot()))
            return
        if self.index_file and self.index_file.exists():
            body = self.index_file.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self._send_json({"error": "panel UI is missing"}, status=404)

    def do_POST(self) -> None:  # noqa: N802 (http.server API)
        length = int(self.headers.get("Content-Length") or 0)
        payload: dict[str, Any] = {}
        if length:
            try:
                payload = json.loads(self.rfile.read(length).decode("utf-8"))
            except ValueError:
                self._send_json({"ok": False, "error": "invalid JSON body"}, status=400)
                return
        controller = self.controller
        actions = {
            "scan": lambda: controller.start_scan(),
            "pair": lambda: controller.pair(payload.get("address")),
            "handoff": lambda: controller.handoff(payload.get("minutes")),
            "resume": lambda: controller.resume(),
            "forget": lambda: controller.forget(),
            "settings": lambda: controller.update_settings(payload),
        }
        action = actions.get(self._route())
        if action is None:
            self._send_json({"ok": False, "error": "unknown action"}, status=404)
            return
        try:
            result = self._call(action())
        except Exception as exc:
            LOGGER.error("Action %s failed: %r", self._route(), exc)
            self._send_json({"ok": False, "error": str(exc)}, status=500)
            return
        self._send_json(result, status=200 if result.get("ok") else 400)


def load_mqtt(args: argparse.Namespace) -> dict[str, Any] | None:
    if not args.mqtt_host:
        return None
    mqtt: dict[str, Any] = {"host": args.mqtt_host, "port": args.mqtt_port}
    if args.mqtt_credentials_file and args.mqtt_credentials_file.exists():
        try:
            credentials = json.loads(args.mqtt_credentials_file.read_text(encoding="utf-8"))
            mqtt["username"] = credentials.get("username")
            mqtt["password"] = credentials.get("password")
        except (OSError, ValueError) as exc:
            LOGGER.warning("Could not read MQTT credentials: %r", exc)
    return mqtt


async def serve(args: argparse.Namespace) -> int:
    args.data_dir.mkdir(parents=True, exist_ok=True)
    controller = HubController(
        data_dir=args.data_dir,
        mqtt=load_mqtt(args),
    )

    loop = asyncio.get_running_loop()
    task = asyncio.current_task()
    stop_requested = False

    def request_stop(signum: int) -> None:
        nonlocal stop_requested
        stop_requested = True
        LOGGER.info(
            "Received %s; disconnecting from hub and shutting down",
            signal.Signals(signum).name,
        )
        task.cancel()

    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, request_stop, sig)
        except (NotImplementedError, RuntimeError):
            pass

    handler = type(
        "BoundPanelHandler",
        (PanelRequestHandler,),
        {"controller": controller, "loop": loop, "index_file": args.static_dir / "index.html"},
    )
    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    LOGGER.info("Weber Connect panel listening on port %s", args.port)

    try:
        await controller.run()
    except asyncio.CancelledError:
        if not stop_requested:
            raise
    finally:
        httpd.shutdown()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Weber Connect ingress panel and bridge.")
    parser.add_argument("--port", type=int, default=8099)
    parser.add_argument("--data-dir", type=Path, default=Path("/data/weber-connect-bridge"))
    parser.add_argument("--static-dir", type=Path, default=Path(__file__).parent / "static")
    parser.add_argument("--mqtt-host", default=None)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-credentials-file", type=Path, default=None)
    parser.add_argument("--log-level", default="info", choices=["debug", "info", "warning", "error"])
    args = parser.parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    return asyncio.run(serve(args))


if __name__ == "__main__":
    raise SystemExit(main())
