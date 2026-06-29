"""CortexLink Speaker Device — MQTT-connected Azure TTS actuator.

Connects to the MQTT broker, registers as an actuator device,
and handles 'speak' and 'stop' actions by synthesizing text
to speech via Azure AI Speech and playing on the Linux speaker.
"""

import json
import logging
import os
import sys
import threading
import uuid as uuid_mod

import paho.mqtt.client as mqtt

from audio_player import AudioPlayer
from azure_tts import AzureTTS

logger = logging.getLogger(__name__)

# DeviceRespCode mapping (must match C++ enum in device_resp_code.h)
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

# Heartbeat interval in seconds
_HEARTBEAT_INTERVAL = 12


class SpeakerDevice:
    """MQTT speaker device with Azure TTS capability."""

    def __init__(self, config: dict):
        """Initialize the speaker device.

        Args:
            config: Full configuration dict (mqtt, device, azure, audio sections).
        """
        self._config = config

        mqtt_cfg = config["mqtt"]
        dev_cfg = config["device"]
        azure_cfg = config["azure"]
        audio_cfg = config.get("audio", {})

        # Device identity
        self._uuid = dev_cfg.get("uuid", "00000000-0000-0000-0000-000000000003")
        self._dev_name = dev_cfg.get("name", "AI Speaker")
        self._location = dev_cfg.get("location", "")
        self._user_param = dev_cfg.get("user_param", "")

        # Azure TTS defaults
        self._default_voice = azure_cfg.get("default_voice", "zh-CN-XiaoxiaoNeural")
        self._default_rate = azure_cfg.get("default_rate", "0%")
        self._default_volume = azure_cfg.get("default_volume", 50)

        # MQTT client
        client_id = f"speaker-{self._uuid[:8]}"
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

        # Services
        self._tts = AzureTTS(
            speech_key=azure_cfg["speech_key"],
            speech_region=azure_cfg["speech_region"],
        )
        self._player = AudioPlayer(preferred=audio_cfg.get("player", "auto"))

        # State
        self._shutdown = threading.Event()
        self._heartbeat_thread: threading.Thread | None = None
        self._speak_lock = threading.Lock()
        self._speaking = False
        self._stop_playback = threading.Event()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def run(self) -> None:
        """Run the device main loop (blocks until shutdown)."""
        logger.info(
            "Starting speaker device: uuid=%s name='%s'",
            self._uuid,
            self._dev_name,
        )

        # Connect to MQTT broker
        logger.info("Connecting to MQTT broker %s:%d", self._mqtt_host, self._mqtt_port)
        self._client.connect(self._mqtt_host, self._mqtt_port, keepalive=30)

        # Start MQTT network loop (background thread)
        self._client.loop_start()

        # Start heartbeat thread
        self._heartbeat_thread = threading.Thread(
            target=self._heartbeat_loop,
            daemon=True,
            name="heartbeat",
        )
        self._heartbeat_thread.start()

        # Wait for shutdown signal
        self._shutdown.wait()

        # Graceful shutdown
        logger.info("Shutting down speaker device...")
        self._stop_playback.set()

        if self._player.is_playing():
            self._player.stop()

        self._client.loop_stop()
        self._client.disconnect()
        logger.info("Speaker device stopped")

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

        # Subscribe to action/config/resp topics
        action_topic = f"device/{self._uuid}/action/#"
        config_topic = f"device/{self._uuid}/config"
        m2s_topic = f"device/{self._uuid}/resp/m2s"

        client.subscribe(action_topic, qos=1)
        client.subscribe(config_topic, qos=1)
        client.subscribe(m2s_topic, qos=1)

        logger.info("Subscribed: %s, %s, %s", action_topic, config_topic, m2s_topic)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        """Called when MQTT connection is lost."""
        if reason_code != 0:
            logger.warning("MQTT disconnected (rc=%d), auto-reconnecting...", reason_code)
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
            elif topic == f"device/{self._uuid}/config":
                self._handle_config(payload)
            elif topic == f"device/{self._uuid}/resp/m2s":
                self._handle_m2s_response(payload)
            else:
                logger.debug("Unhandled topic: %s", topic)
        except Exception:
            logger.exception("Error handling message on topic=%s", topic)

    # ------------------------------------------------------------------
    # Device Registration
    # ------------------------------------------------------------------

    def _register_device(self) -> None:
        """Publish device properties to broadcast/online."""
        registration = {
            "dev_id": self._uuid,
            "dev_name": self._dev_name,
            "dev_type": "actuator",
            "dev_subtype": "speaker",
            "location": self._location,
            "user_param": self._user_param,
            "actions": [
                {
                    "act_id": "speak",
                    "act_name": "Speak",
                    "desc": "Synthesize text to speech via Azure TTS and play through speaker",
                    "params": [
                        {
                            "p_name": "text",
                            "desc": "Text content to speak",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                        {
                            "p_name": "voice",
                            "desc": "Azure TTS voice name (e.g. zh-CN-XiaoxiaoNeural)",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                        {
                            "p_name": "rate",
                            "desc": "Speaking rate adjustment (e.g. +10%, -20%)",
                            "p_type": "str",
                            "range": ["", ""],
                            "unit": "",
                        },
                        {
                            "p_name": "volume",
                            "desc": "Volume level (0-100)",
                            "p_type": "int",
                            "range": ["0", "100"],
                            "unit": "",
                        },
                    ],
                    "pre_cond": "",
                },
                {
                    "act_id": "stop",
                    "act_name": "Stop",
                    "desc": "Stop current speech playback",
                    "params": [],
                    "pre_cond": "",
                },
            ],
            "event": [],
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
        params = msg.get("params", [])

        if act_id == "speak":
            self._handle_speak(msg_id, params)
        elif act_id == "stop":
            self._handle_stop(msg_id)
        else:
            logger.warning("Unknown action: %s", act_id)
            self._send_s2m(msg_id, _RESP_ACTION_NOT_FOUND)

    def _handle_speak(self, msg_id: str, params: list) -> None:
        """Handle the 'speak' action."""
        # Extract text param (required)
        text = _get_param(params, "text")
        if not text:
            logger.warning("speak: missing 'text' parameter")
            self._send_s2m(msg_id, _RESP_MISSING_FIELD)
            return

        text = str(text)

        # Check if already speaking
        with self._speak_lock:
            if self._speaking:
                logger.warning("speak: device busy")
                self._send_s2m(msg_id, _RESP_DEVICE_BUSY)
                return
            self._speaking = True

        # Extract optional params
        voice = str(_get_param(params, "voice") or self._default_voice)
        rate = str(_get_param(params, "rate") or self._default_rate)
        try:
            volume = int(_get_param(params, "volume") or self._default_volume)
        except (ValueError, TypeError):
            volume = self._default_volume

        logger.debug("speak: text='%s' voice='%s' rate='%s' volume=%d",
                     text, voice, rate, volume)

        # Acknowledge immediately
        self._send_s2m(msg_id, _RESP_OK)

        # Synthesize and play in background thread
        thread = threading.Thread(
            target=self._synthesize_and_play,
            args=(text, voice, rate, volume),
            daemon=True,
            name="tts-playback",
        )
        thread.start()

    def _handle_stop(self, msg_id: str) -> None:
        """Handle the 'stop' action."""
        self._stop_playback.set()

        if self._player.is_playing():
            self._player.stop()
            logger.info("Playback stopped by stop action")
        else:
            logger.debug("stop: nothing playing")

        self._send_s2m(msg_id, _RESP_OK)

        with self._speak_lock:
            self._speaking = False

    def _synthesize_and_play(
        self, text: str, voice: str, rate: str, volume: int
    ) -> None:
        """Synthesize text to WAV and play it (runs in background thread)."""
        wav_path = None
        try:
            logger.info("Synthesizing: '%s' (voice=%s, rate=%s, vol=%d)", text, voice, rate, volume)

            if not self._player.is_available():
                logger.error("No audio player available, cannot play TTS output")
                return

            wav_path = self._tts.synthesize_to_wav(
                text=text,
                voice=voice,
                rate=rate,
                volume=volume,
            )

            if wav_path is None:
                logger.error("TTS synthesis failed")
                return

            logger.debug("TTS output file: %s", wav_path)

            # Reset stop event before playing
            self._stop_playback.clear()

            ok = self._player.play(wav_path, stop_event=self._stop_playback)
            if ok:
                logger.info("Playback completed")
            else:
                logger.info("Playback stopped or failed")

        except Exception:
            logger.exception("Error in synthesize_and_play")
        finally:
            if wav_path:
                try:
                    os.unlink(wav_path)
                except OSError:
                    pass
            with self._speak_lock:
                self._speaking = False

    # ------------------------------------------------------------------
    # Config Handling
    # ------------------------------------------------------------------

    def _handle_config(self, payload: str) -> None:
        """Handle incoming device config update."""
        try:
            msg = json.loads(payload)
        except json.JSONDecodeError:
            logger.warning("Invalid JSON in config payload")
            return

        if "dev_name" in msg:
            self._dev_name = msg["dev_name"]
            logger.info("Config: dev_name = '%s'", self._dev_name)
        if "location" in msg:
            self._location = msg["location"]
            logger.info("Config: location = '%s'", self._location)
        if "user_param" in msg:
            self._user_param = msg["user_param"]
            logger.info("Config: user_param updated")

    def _handle_m2s_response(self, payload: str) -> None:
        """Handle host-to-device response (m2s)."""
        try:
            msg = json.loads(payload)
        except json.JSONDecodeError:
            return

        resp_code = msg.get("resp", -1)
        resp_msg_id = msg.get("msg_id", "")
        logger.debug("m2s response: msg_id=%s resp=%d", resp_msg_id, resp_code)

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

    def _send_s2m(self, msg_id: str, resp_code: int) -> None:
        """Send a device-to-host response on resp/s2m."""
        payload = json.dumps({
            "msg_id": msg_id,
            "resp": resp_code,
        })
        topic = f"device/{self._uuid}/resp/s2m"
        try:
            self._client.publish(topic, payload, qos=1)
            logger.debug("s2m sent: msg_id=%s resp=%d", msg_id, resp_code)
        except Exception:
            logger.exception("Failed to send s2m response")


# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------

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
