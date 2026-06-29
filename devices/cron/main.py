#!/usr/bin/env python3
"""CortexLink Cron Virtual Device — cron scheduler via MQTT.

Replaces the C++ CronScheduler in-process component. Runs as a standalone
Python process that connects to the MQTT broker and provides cron scheduling
services to the CortexLink rule engine.

Usage:
    python3 main.py [--config <path>] [--debug]

Configuration is loaded from config.json (in the same directory by default).
MQTT credentials can be set via environment variables:
    MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD
"""

import argparse
import json
import logging
import os
import signal
import sys

from cron_device import CronDevice

logger = logging.getLogger(__name__)

_DEFAULT_CONFIG = {
    "mqtt": {
        "host": "localhost",
        "port": 1883,
        "username": "",
        "password": "",
    },
    "device": {
        "uuid": "00000000-0000-0000-0000-000000000001",
        "name": "Cron Timer",
        "location": "",
        "user_param": "",
    },
}

# Environment variable overrides
_ENV_OVERRIDES = {
    ("mqtt", "host"): "MQTT_HOST",
    ("mqtt", "port"): "MQTT_PORT",
    ("mqtt", "username"): "MQTT_USERNAME",
    ("mqtt", "password"): "MQTT_PASSWORD",
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

    # Ensure all sections exist with defaults
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


def main():
    """Entry point for the cron device."""
    parser = argparse.ArgumentParser(
        description="CortexLink Cron Virtual Device — cron scheduler via MQTT"
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

    # Setup logging
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

    # Create and run device
    device = CronDevice(config)

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
