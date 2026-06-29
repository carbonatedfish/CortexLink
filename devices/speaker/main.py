#!/usr/bin/env python3
"""CortexLink Speaker Device — Azure TTS actuator via MQTT.

Usage:
    python3 main.py [--config <path>] [--debug]

Configuration is loaded from config.json (in the same directory by default).
Azure credentials can be set via environment variables:
    AZURE_SPEECH_KEY, AZURE_SPEECH_REGION, MQTT_HOST, MQTT_PORT
"""

import argparse
import json
import logging
import os
import signal
import sys
from pathlib import Path

from speaker_device import SpeakerDevice

logger = logging.getLogger(__name__)

_DEFAULT_CONFIG = {
    "mqtt": {
        "host": "localhost",
        "port": 1883,
        "username": "",
        "password": "",
    },
    "device": {
        "uuid": "00000000-0000-0000-0000-000000000003",
        "name": "AI Speaker",
        "location": "",
        "user_param": "",
    },
    "azure": {
        "speech_key": "",
        "speech_region": "eastasia",
        "default_voice": "zh-CN-XiaoxiaoNeural",
        "default_rate": "0%",
        "default_volume": 50,
    },
    "audio": {
        "player": "auto",
    },
}

# Environment variable overrides
_ENV_OVERRIDES = {
    ("mqtt", "host"): "MQTT_HOST",
    ("mqtt", "port"): "MQTT_PORT",
    ("mqtt", "username"): "MQTT_USERNAME",
    ("mqtt", "password"): "MQTT_PASSWORD",
    ("azure", "speech_key"): "AZURE_SPEECH_KEY",
    ("azure", "speech_region"): "AZURE_SPEECH_REGION",
}


def _create_default_config(path: str) -> None:
    """Write default config to a file, creating parent dirs if needed."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(_DEFAULT_CONFIG, f, indent=2, ensure_ascii=False)
    logger.info("Default config created at %s", path)


def _load_config(path: str) -> dict:
    """Load config from JSON file, creating default if missing."""
    if not os.path.exists(path):
        _create_default_config(path)

    with open(path, "r", encoding="utf-8") as f:
        config = json.load(f)

    # Ensure all sections exist
    for section in _DEFAULT_CONFIG:
        if section not in config:
            config[section] = {}
        for key, default_val in _DEFAULT_CONFIG[section].items():
            if key not in config[section]:
                config[section][key] = default_val

    return config


def _apply_env_overrides(config: dict) -> dict:
    """Override config values from environment variables."""
    for (section, key), env_var in _ENV_OVERRIDES.items():
        if env_var in os.environ:
            value = os.environ[env_var]
            if section not in config:
                config[section] = {}
            if env_var == "MQTT_PORT":
                config[section][key] = int(value)
            else:
                config[section][key] = value
            logger.debug("Config override: %s.%s = ***", section, key)
    return config


def _validate_config(config: dict) -> None:
    """Validate required config fields. Exits on failure."""
    azure_key = config.get("azure", {}).get("speech_key", "")
    azure_region = config.get("azure", {}).get("speech_region", "")

    if not azure_key:
        logger.error(
            "Azure speech_key is required. "
            "Set in config.json or via AZURE_SPEECH_KEY env var."
        )
        sys.exit(1)

    if not azure_region:
        logger.error(
            "Azure speech_region is required. "
            "Set in config.json or via AZURE_SPEECH_REGION env var."
        )
        sys.exit(1)


def main():
    """Entry point for the speaker device."""
    parser = argparse.ArgumentParser(
        description="CortexLink Speaker Device — Azure TTS via MQTT"
    )
    parser.add_argument(
        "--config",
        default=None,
        help="Path to config.json (default: config.json next to this script)",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Enable debug logging",
    )
    args = parser.parse_args()

    # Determine config path
    if args.config:
        config_path = args.config
    else:
        config_path = os.path.join(os.path.dirname(__file__), "config.json")

    # Setup logging early for startup messages
    level = logging.DEBUG if args.debug else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    # Load and validate config
    logger.info("Loading config from %s", config_path)
    config = _load_config(config_path)
    config = _apply_env_overrides(config)
    _validate_config(config)

    # Create and run device
    device = SpeakerDevice(config)

    # Signal handling for graceful shutdown
    def _on_signal(signum, frame):
        logger.info("Received signal %d, shutting down...", signum)
        device.shutdown()

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    # Run (blocks until shutdown)
    device.run()


if __name__ == "__main__":
    main()
