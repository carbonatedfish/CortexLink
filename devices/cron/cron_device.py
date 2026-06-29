"""CortexLink Cron Virtual Device — MQTT-connected cron scheduler.

Connects to the MQTT broker as a virtual device with fixed UUID
00000000-0000-0000-0000-000000000001.  Handles add_cron, add_relative_cron,
remove_cron, and list_crons actions.  A background scheduler thread evaluates
cron expressions against UTC+8 time and publishes cron_trigger events via MQTT.

Replaces the C++ CronScheduler class (src/cron/cron_scheduler.cpp).
"""

from __future__ import annotations

import json
import logging
import os
import threading
import time
import uuid as uuid_mod

import paho.mqtt.client as mqtt

import cron_parser

logger = logging.getLogger(__name__)

# =============================================================================
# Constants — must match C++ device_resp_code.h
# =============================================================================

_RESP_OK = 0
_RESP_DEVICE_NOT_FOUND = 1
_RESP_DEVICE_OFFLINE = 2
_RESP_INTERNAL_ERROR = 3
_RESP_INVALID_REQUEST = 4
_RESP_MISSING_FIELD = 5
_RESP_TIMEOUT = 6
_RESP_ACTION_NOT_FOUND = 20
_RESP_INVALID_PARAMS = 21
_RESP_ACTION_FAILED = 22
_RESP_DEVICE_BUSY = 23
_RESP_UNKNOWN_ERROR = 99

# Well-known UUIDs — must match C++ CronScheduler constants
CRON_DEV_UUID = "00000000-0000-0000-0000-000000000001"
CRON_EVT_UUID = "00000000-0000-0000-0000-000000000002"

# Timing
_SCHEDULER_INTERVAL = 30   # seconds between cron evaluations
_HEARTBEAT_INTERVAL = 12   # seconds between heartbeats


# =============================================================================
# CronDevice
# =============================================================================


class CronDevice:
    """MQTT virtual device implementing a cron scheduler.

    Follows the same MQTT protocol as physical devices:
    - Registers via broadcast/online (including cron_trigger event definition)
    - Sends periodic heartbeats on device/{uuid}/heartbeat
    - Subscribes to device/{uuid}/action/# for incoming actions
    - Publishes cron_trigger events to device/{uuid}/event/cron_trigger
    - Sends s2m responses on device/{uuid}/resp/s2m
    """

    def __init__(self, config: dict):
        """Initialize the cron device.

        Args:
            config: Full configuration dict with mqtt and device sections.
        """
        self._config = config

        mqtt_cfg = config.get("mqtt", {})
        dev_cfg = config.get("device", {})

        # Device identity
        self._uuid = dev_cfg.get("uuid", CRON_DEV_UUID)
        self._dev_name = dev_cfg.get("name", "Cron Timer")
        self._location = dev_cfg.get("location", "")
        self._user_param = dev_cfg.get("user_param", "")

        # MQTT client
        client_id = f"cron-{self._uuid[:8]}"
        self._client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
        )
        if mqtt_cfg.get("username"):
            self._client.username_pw_set(
                mqtt_cfg["username"], mqtt_cfg.get("password", "")
            )

        # Enable auto-reconnect
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)

        # Wire callbacks
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

        # MQTT connection params
        self._mqtt_host = mqtt_cfg.get("host", "localhost")
        self._mqtt_port = mqtt_cfg.get("port", 1883)

        # Cron state
        self._entries: list[dict] = []          # list of {id, expr, params, trigger_count}
        self._last_fired_minute: dict[str, int] = {}  # cron_id → unix_minute
        self._lock = threading.Lock()

        # Persistence path
        self._crontab_path = os.path.expanduser("~/.cortexlink/cron/crontab.txt")

        # Shutdown coordination
        self._shutdown = threading.Event()
        self._heartbeat_thread: threading.Thread | None = None
        self._scheduler_thread: threading.Thread | None = None

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def run(self) -> None:
        """Run the device main loop (blocks until shutdown)."""
        logger.info(
            "Starting cron device: uuid=%s name='%s'",
            self._uuid,
            self._dev_name,
        )

        # Ensure ~/.cortexlink/cron/ directory exists
        cron_dir = os.path.dirname(self._crontab_path)
        os.makedirs(cron_dir, exist_ok=True)

        # Load persisted cron jobs
        with self._lock:
            loaded = self._load_crontab()
            logger.info("Loaded %d cron job(s) from %s", loaded, self._crontab_path)

        # Connect to MQTT broker
        logger.info("Connecting to MQTT broker %s:%d", self._mqtt_host, self._mqtt_port)
        self._client.connect(self._mqtt_host, self._mqtt_port, keepalive=30)

        # Start MQTT network loop (background thread)
        self._client.loop_start()

        # Start heartbeat thread
        self._heartbeat_thread = threading.Thread(
            target=self._heartbeat_loop,
            daemon=True,
            name="cron-heartbeat",
        )
        self._heartbeat_thread.start()

        # Start scheduler thread
        self._scheduler_thread = threading.Thread(
            target=self._scheduler_loop,
            daemon=True,
            name="cron-scheduler",
        )
        self._scheduler_thread.start()

        # Wait for shutdown signal
        self._shutdown.wait()

        # Graceful shutdown
        logger.info("Shutting down cron device...")
        self._client.loop_stop()
        self._client.disconnect()
        logger.info("Cron device stopped")

    def shutdown(self) -> None:
        """Signal the device to shut down gracefully."""
        self._shutdown.set()

    # ------------------------------------------------------------------
    # MQTT Callbacks
    # ------------------------------------------------------------------

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        """Called when MQTT connection is established (or re-established)."""
        if reason_code == 0:
            logger.info("MQTT connected (rc=%d)", reason_code)
        else:
            logger.warning("MQTT connect returned rc=%d", reason_code)
            return

        # Register device with host
        self._register_device()

        # Subscribe to action topic
        action_topic = f"device/{self._uuid}/action/#"
        client.subscribe(action_topic, qos=1)
        logger.info("Subscribed: %s", action_topic)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        """Called when MQTT connection is lost."""
        if reason_code != 0:
            logger.warning(
                "MQTT disconnected (rc=%d), auto-reconnecting...", reason_code
            )
        else:
            logger.info("MQTT disconnected cleanly")

    def _on_message(self, client, userdata, msg):
        """Dispatch incoming MQTT messages."""
        topic = msg.topic
        payload = msg.payload.decode("utf-8", errors="replace")

        logger.debug("MQTT recv: topic=%s payload=%s", topic, payload[:200])

        try:
            if topic.startswith(f"device/{self._uuid}/action/"):
                act_id = topic.rsplit("/", 1)[-1]
                self._handle_action(act_id, payload)
            else:
                logger.debug("Unhandled topic: %s", topic)
        except Exception:
            logger.exception("Error handling message on topic=%s", topic)

    # ------------------------------------------------------------------
    # Device Registration
    # ------------------------------------------------------------------

    def _register_device(self) -> None:
        """Publish device properties to broadcast/online.

        Includes the cron_trigger event definition so DeviceManager
        upserts it into the event table (needed for RuleEngine to
        accept cron_trigger events).
        """
        registration = {
            "dev_id": self._uuid,
            "dev_name": self._dev_name,
            "dev_type": "composite",
            "dev_subtype": "",
            "location": self._location,
            "user_param": self._user_param,
            "actions": [
                {
                    "act_id": "add_cron",
                    "act_name": "Add Cron Job",
                    "desc": "Add a cron job with a standard 5-field cron expression",
                    "params": [
                        {
                            "p_name": "expr",
                            "desc": "5-field cron expression (min hour dom month dow)",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                        {
                            "p_name": "params",
                            "desc": "Custom parameters injected into cron_trigger event",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                        {
                            "p_name": "trigger_count",
                            "desc": "Max trigger count (-1=infinite, N=auto-delete after N)",
                            "p_type": "int",
                            "range": ["", ""],
                            "unit": "",
                        },
                    ],
                    "pre_cond": "",
                },
                {
                    "act_id": "add_relative_cron",
                    "act_name": "Add Relative Cron Job",
                    "desc": "Add a one-shot cron job relative to current time",
                    "params": [
                        {
                            "p_name": "offset",
                            "desc": "Time offset (e.g. 30m, 2h, 1d, 1h30m)",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                        {
                            "p_name": "params",
                            "desc": "Custom parameters injected into cron_trigger event",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                        {
                            "p_name": "trigger_count",
                            "desc": "Max trigger count (-1=infinite, default 1)",
                            "p_type": "int",
                            "range": ["", ""],
                            "unit": "",
                        },
                    ],
                    "pre_cond": "",
                },
                {
                    "act_id": "remove_cron",
                    "act_name": "Remove Cron Job",
                    "desc": "Remove a cron job by its ID",
                    "params": [
                        {
                            "p_name": "cron_id",
                            "desc": "Cron job UUID",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                    ],
                    "pre_cond": "",
                },
                {
                    "act_id": "list_crons",
                    "act_name": "List Cron Jobs",
                    "desc": "List all cron jobs",
                    "params": [],
                    "pre_cond": "",
                },
            ],
            "event": [
                {
                    "evt_id": CRON_EVT_UUID,
                    "evt_name": "cron_trigger",
                    "desc": "Cron job trigger event — fired by the Cron virtual device when a cron expression matches",
                    "params": [
                        {
                            "p_name": "cron_id",
                            "desc": "Cron job UUID",
                            "p_type": "str",
                            "unit": "",
                        },
                        {
                            "p_name": "expr",
                            "desc": "Cron expression that triggered",
                            "p_type": "str",
                            "unit": "",
                        },
                        {
                            "p_name": "user_id",
                            "desc": "Optional user ID",
                            "p_type": "str",
                            "unit": "",
                        },
                    ],
                }
            ],
            "data": [],
        }

        payload = json.dumps(registration, ensure_ascii=False)
        topic = "broadcast/online"
        result = self._client.publish(topic, payload, qos=1)
        logger.info(
            "Device registration sent to %s (rc=%d)", topic, result.rc
        )

    # ------------------------------------------------------------------
    # Action Handling
    # ------------------------------------------------------------------

    def _handle_action(self, act_id: str, payload: str) -> None:
        """Route an action to the appropriate handler."""
        logger.info("Action received: act_id=%s", act_id)

        # Parse JSON
        try:
            msg = json.loads(payload)
        except json.JSONDecodeError as e:
            logger.warning("Invalid JSON in action payload: %s", e)
            self._send_s2m("", _RESP_INVALID_REQUEST)
            return

        msg_id = msg.get("msg_id", "")
        action_params = msg.get("params", [])

        if act_id == "add_cron":
            self._handle_add_cron(msg_id, action_params)
        elif act_id == "add_relative_cron":
            self._handle_add_relative_cron(msg_id, action_params)
        elif act_id == "remove_cron":
            self._handle_remove_cron(msg_id, action_params)
        elif act_id == "list_crons":
            self._handle_list_crons(msg_id)
        else:
            logger.warning("Unknown action: %s", act_id)
            self._send_s2m(msg_id, _RESP_ACTION_NOT_FOUND)

    def _handle_add_cron(self, msg_id: str, action_params: list) -> None:
        """Handle the 'add_cron' action."""
        # Extract and validate expr
        expr = _get_param(action_params, "expr")
        if expr is None:
            logger.warning("add_cron: missing expr")
            self._send_s2m(msg_id, _RESP_MISSING_FIELD)
            return
        expr = str(expr)

        # Validate cron expression
        parsed, parse_err = cron_parser.parse(expr)
        if parsed is None:
            logger.warning("add_cron: invalid expr '%s': %s", expr, parse_err)
            self._send_s2m(msg_id, _RESP_INVALID_PARAMS,
                           data={"error": parse_err})
            return

        # Extract trigger_count (default -1 = infinite)
        trigger_count = -1
        tc_raw = _get_param(action_params, "trigger_count")
        if tc_raw is not None:
            try:
                trigger_count = int(tc_raw)
            except (ValueError, TypeError):
                pass

        # Extract custom params
        custom_params = "{}"
        cp_raw = _get_param(action_params, "params")
        if cp_raw is not None and str(cp_raw):
            raw_str = str(cp_raw)
            try:
                parsed_cp = json.loads(raw_str)
                if isinstance(parsed_cp, dict):
                    custom_params = json.dumps(parsed_cp, ensure_ascii=False)
            except json.JSONDecodeError:
                # Not valid JSON, wrap as single "value" param
                custom_params = json.dumps({"value": raw_str}, ensure_ascii=False)

        # Generate cron_id
        cron_id = str(uuid_mod.uuid4())

        # Store entry
        entry = {
            "id": cron_id,
            "expr": expr,
            "params": custom_params,
            "trigger_count": trigger_count,
        }

        with self._lock:
            self._entries.append(entry)
            if not self._save_crontab():
                logger.error("add_cron: failed to save crontab")
                self._entries.pop()
                self._send_s2m(msg_id, _RESP_INTERNAL_ERROR)
                return

        self._send_s2m(msg_id, _RESP_OK, data={"cron_id": cron_id})
        logger.info(
            "add_cron: id=%s expr='%s' trigger_count=%d",
            cron_id, expr, trigger_count,
        )
        logger.debug(
            "add_cron: cron_id=%s expr='%s' trigger_count=%d params=%s",
            cron_id, expr, trigger_count, custom_params,
        )

    def _handle_add_relative_cron(self, msg_id: str, action_params: list) -> None:
        """Handle the 'add_relative_cron' action."""
        # Extract and validate offset
        offset_raw = _get_param(action_params, "offset")
        if offset_raw is None:
            logger.warning("add_relative_cron: missing offset")
            self._send_s2m(msg_id, _RESP_MISSING_FIELD)
            return
        offset_str = str(offset_raw)

        # Parse offset
        offset_secs, offset_err = cron_parser.parse_offset(offset_str)
        if offset_secs is None:
            logger.warning(
                "add_relative_cron: invalid offset '%s': %s",
                offset_str, offset_err,
            )
            self._send_s2m(msg_id, _RESP_INVALID_PARAMS,
                           data={"error": offset_err})
            return

        # Compute absolute cron expression
        expr = cron_parser.make_cron_from_offset(offset_secs)

        # Extract trigger_count (default 1 = one-shot)
        trigger_count = 1
        tc_raw = _get_param(action_params, "trigger_count")
        if tc_raw is not None:
            try:
                trigger_count = int(tc_raw)
            except (ValueError, TypeError):
                pass

        # Extract custom params
        custom_params = "{}"
        cp_raw = _get_param(action_params, "params")
        if cp_raw is not None and str(cp_raw):
            raw_str = str(cp_raw)
            try:
                parsed_cp = json.loads(raw_str)
                if isinstance(parsed_cp, dict):
                    custom_params = json.dumps(parsed_cp, ensure_ascii=False)
            except json.JSONDecodeError:
                custom_params = json.dumps({"value": raw_str}, ensure_ascii=False)

        # Generate cron_id
        cron_id = str(uuid_mod.uuid4())

        # Store entry
        entry = {
            "id": cron_id,
            "expr": expr,
            "params": custom_params,
            "trigger_count": trigger_count,
        }

        with self._lock:
            self._entries.append(entry)
            if not self._save_crontab():
                logger.error("add_relative_cron: failed to save crontab")
                self._entries.pop()
                self._send_s2m(msg_id, _RESP_INTERNAL_ERROR)
                return

        self._send_s2m(msg_id, _RESP_OK,
                       data={"cron_id": cron_id, "expr": expr})
        logger.info(
            "add_relative_cron: id=%s offset='%s' expr='%s' trigger_count=%d",
            cron_id, offset_str, expr, trigger_count,
        )

    def _handle_remove_cron(self, msg_id: str, action_params: list) -> None:
        """Handle the 'remove_cron' action."""
        cron_id_raw = _get_param(action_params, "cron_id")
        if cron_id_raw is None:
            logger.warning("remove_cron: missing cron_id")
            self._send_s2m(msg_id, _RESP_MISSING_FIELD)
            return

        cron_id = str(cron_id_raw)
        found = False

        with self._lock:
            for i, entry in enumerate(self._entries):
                if entry["id"] == cron_id:
                    del self._entries[i]
                    found = True
                    break

            self._last_fired_minute.pop(cron_id, None)

            if found:
                self._save_crontab()

        # Idempotent — always return OK
        self._send_s2m(msg_id, _RESP_OK)

        if found:
            logger.info("remove_cron: id=%s", cron_id)
        else:
            logger.debug("remove_cron: id=%s not found (ignored)", cron_id)

    def _handle_list_crons(self, msg_id: str) -> None:
        """Handle the 'list_crons' action."""
        jobs = []

        with self._lock:
            for entry in self._entries:
                job = {
                    "cron_id": entry["id"],
                    "expr": entry["expr"],
                    "trigger_count": entry["trigger_count"],
                }
                # Parse custom params for display
                params_str = entry["params"]
                if params_str and params_str != "{}":
                    try:
                        job["params"] = json.loads(params_str)
                    except json.JSONDecodeError:
                        job["params"] = params_str
                else:
                    job["params"] = {}
                jobs.append(job)

        self._send_s2m(msg_id, _RESP_OK, data={"jobs": jobs})
        logger.debug("list_crons: returned %d job(s)", len(jobs))

    # ------------------------------------------------------------------
    # Scheduler Loop
    # ------------------------------------------------------------------

    def _scheduler_loop(self) -> None:
        """Background thread that evaluates cron expressions against UTC+8 time.

        Wakes every 30 seconds (or immediately when shutdown is signaled).
        Deduplicates: each job fires at most once per unix minute.
        """
        logger.info("Cron scheduler thread started")

        while not self._shutdown.wait(_SCHEDULER_INTERVAL):
            # Get current UTC+8 time
            minute, hour, day, month, dow = cron_parser.get_current_time_utc8()

            # Compute current unix minute for dedup
            current_minute_key = int(time.time() / 60)

            logger.debug("Cron scheduler: evaluating %d entries at %02d:%02d",
                         len(self._entries), hour, minute)

            # Snapshot entries under lock
            with self._lock:
                snapshot = list(self._entries)

            for entry in snapshot:
                entry_id = entry["id"]

                # Dedup: skip if already fired this minute
                with self._lock:
                    last_min = self._last_fired_minute.get(entry_id)
                    if last_min == current_minute_key:
                        continue

                # Parse cron expression
                parsed, _ = cron_parser.parse(entry["expr"])
                if parsed is None:
                    logger.warning(
                        "Cron scheduler: failed to parse expr '%s' for job %s",
                        entry["expr"], entry_id,
                    )
                    continue

                # Check match
                if not cron_parser.matches(parsed, minute, hour, day, month, dow):
                    continue

                # --- Fire! ---

                # Record dedup
                with self._lock:
                    self._last_fired_minute[entry_id] = current_minute_key

                # Build params array for event
                params_array: list[dict] = []

                # System params
                params_array.append({"p_name": "cron_id", "value": entry_id})
                params_array.append({"p_name": "expr", "value": entry["expr"]})

                # Custom params from stored JSON
                params_str = entry["params"]
                if params_str and params_str != "{}":
                    try:
                        custom = json.loads(params_str)
                        for key, val in custom.items():
                            if isinstance(val, str):
                                val_str = val
                            elif isinstance(val, bool):
                                val_str = "true" if val else "false"
                            elif isinstance(val, (int, float)):
                                val_str = str(val)
                            else:
                                val_str = json.dumps(val, ensure_ascii=False)
                            params_array.append({"p_name": key, "value": val_str})
                    except json.JSONDecodeError:
                        logger.warning(
                            "Cron scheduler: failed to parse custom params for job %s",
                            entry_id,
                        )

                # Build event payload
                event_payload = json.dumps({
                    "msg_id": str(uuid_mod.uuid4()),
                    "evt_id": CRON_EVT_UUID,
                    "params": params_array,
                }, ensure_ascii=False)

                event_topic = f"device/{self._uuid}/event/cron_trigger"

                logger.info(
                    "Cron scheduler: firing job %s expr='%s' trigger_count=%d",
                    entry_id, entry["expr"], entry["trigger_count"],
                )

                self._client.publish(event_topic, event_payload, qos=1)

                # Handle trigger count
                trigger_count = entry["trigger_count"]
                if trigger_count > 0:
                    trigger_count -= 1

                    with self._lock:
                        if trigger_count == 0:
                            logger.info(
                                "Cron scheduler: job %s trigger_count reached 0, removing",
                                entry_id,
                            )
                            self._entries = [
                                e for e in self._entries
                                if e["id"] != entry_id
                            ]
                            self._last_fired_minute.pop(entry_id, None)
                        else:
                            for e in self._entries:
                                if e["id"] == entry_id:
                                    e["trigger_count"] = trigger_count
                                    break

                        self._save_crontab()

        logger.info("Cron scheduler thread exited")

    # ------------------------------------------------------------------
    # Crontab Persistence
    # ------------------------------------------------------------------

    def _load_crontab(self) -> int:
        """Load cron jobs from ~/.cortexlink/cron/crontab.txt.

        Format: <uuid>|<expr>|<params_json>|<trigger_count>

        Must be called while holding self._lock.

        Returns:
            Number of entries loaded.
        """
        self._entries.clear()

        if not os.path.exists(self._crontab_path):
            logger.info("No existing crontab at %s", self._crontab_path)
            return 0

        try:
            with open(self._crontab_path, "r", encoding="utf-8") as f:
                lines = f.readlines()
        except OSError as e:
            logger.error("Failed to read crontab: %s", e)
            return 0

        for line_no, line in enumerate(lines, start=1):
            line = line.strip()

            # Skip empty lines and comments
            if not line or line.startswith("#"):
                continue

            # Parse pipe-delimited format
            parts = line.split("|")
            if len(parts) < 4:
                logger.warning(
                    "Crontab line %d malformed (expected 4 fields): %s",
                    line_no, line,
                )
                continue

            entry_id = parts[0]
            expr = parts[1]
            params = parts[2]
            count_str = parts[3]

            try:
                trigger_count = int(count_str)
            except ValueError:
                logger.warning(
                    "Crontab line %d invalid trigger_count '%s', using -1",
                    line_no, count_str,
                )
                trigger_count = -1

            # Validate cron expression on load
            parsed, _ = cron_parser.parse(expr)
            if parsed is None:
                logger.warning(
                    "Crontab line %d has invalid expr '%s', skipping",
                    line_no, expr,
                )
                continue

            self._entries.append({
                "id": entry_id,
                "expr": expr,
                "params": params,
                "trigger_count": trigger_count,
            })

        return len(self._entries)

    def _save_crontab(self) -> bool:
        """Write all cron jobs to crontab.txt.

        Must be called while holding self._lock.

        Returns:
            True on success, False on failure.
        """
        try:
            with open(self._crontab_path, "w", encoding="utf-8") as f:
                for entry in self._entries:
                    f.write(
                        f"{entry['id']}|{entry['expr']}|{entry['params']}|{entry['trigger_count']}\n"
                    )
            return True
        except OSError as e:
            logger.error("Failed to write crontab: %s", e)
            return False

    # ------------------------------------------------------------------
    # Heartbeat
    # ------------------------------------------------------------------

    def _heartbeat_loop(self) -> None:
        """Send periodic heartbeats to keep the device online."""
        topic = f"device/{self._uuid}/heartbeat"
        while not self._shutdown.wait(_HEARTBEAT_INTERVAL):
            if self._client.is_connected():
                payload = json.dumps({"msg_id": str(uuid_mod.uuid4())})
                try:
                    self._client.publish(topic, payload, qos=1)
                    logger.debug("Heartbeat sent")
                except Exception:
                    logger.warning("Heartbeat publish failed")

    # ------------------------------------------------------------------
    # Response Helpers
    # ------------------------------------------------------------------

    def _send_s2m(self, msg_id: str, resp_code: int, data: dict | None = None) -> None:
        """Send a device-to-host response on resp/s2m.

        Args:
            msg_id: The msg_id from the original action request.
            resp_code: DeviceRespCode value.
            data: Optional data dict to include in the response.
        """
        payload_dict: dict = {
            "msg_id": msg_id,
            "resp": resp_code,
        }
        if data is not None:
            payload_dict["data"] = data

        payload = json.dumps(payload_dict, ensure_ascii=False)
        topic = f"device/{self._uuid}/resp/s2m"
        try:
            self._client.publish(topic, payload, qos=1)
            logger.debug("s2m sent: msg_id=%s resp=%d", msg_id, resp_code)
        except Exception:
            logger.exception("Failed to send s2m response")


# =============================================================================
# Helpers
# =============================================================================


def _get_param(params: list, name: str):
    """Extract a parameter value from the CortexLink params array format.

    The params array has the form:
        [{"p_name": "...", "value": ...}, ...]

    Args:
        params: List of param dicts.
        name: Parameter name to look up.

    Returns:
        The parameter value, or None if not found.
    """
    for p in params:
        if isinstance(p, dict) and p.get("p_name") == name:
            return p.get("value")
    return None
