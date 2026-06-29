# CortexLink Speaker Device

Azure AI Speech TTS actuator for the CortexLink smart home system.

Connects to the MQTT broker, registers as a device, and converts text to
speech via Azure Cognitive Services. Plays audio through the Linux speaker.

## Requirements

- Python 3.10+
- Azure Speech service subscription (key + region)
- Linux with ALSA or PulseAudio (`alsa-utils` or `pulseaudio-utils`)

## Quick Start

```bash
# 1. Install dependencies
pip install -r requirements.txt

# 2. Configure Azure credentials
export AZURE_SPEECH_KEY="your-azure-speech-key"
export AZURE_SPEECH_REGION="eastasia"

# 3. Run
python3 main.py --debug
```

## Configuration

Settings are loaded from `config.json` (created with defaults on first run).
All values can be overridden via environment variables.

### config.json

```json
{
    "mqtt": {
        "host": "localhost",
        "port": 1883,
        "username": "",
        "password": ""
    },
    "device": {
        "uuid": "00000000-0000-0000-0000-000000000003",
        "name": "AI Speaker",
        "location": "",
        "user_param": ""
    },
    "azure": {
        "speech_key": "",
        "speech_region": "eastasia",
        "default_voice": "zh-CN-XiaoxiaoNeural",
        "default_rate": "0%",
        "default_volume": 50
    },
    "audio": {
        "player": "auto"
    }
}
```

### Environment Variables

| Variable | Config Key |
|---|---|
| `AZURE_SPEECH_KEY` | `azure.speech_key` |
| `AZURE_SPEECH_REGION` | `azure.speech_region` |
| `MQTT_HOST` | `mqtt.host` |
| `MQTT_PORT` | `mqtt.port` |
| `MQTT_USERNAME` | `mqtt.username` |
| `MQTT_PASSWORD` | `mqtt.password` |

## Actions

### `speak` — Convert text to speech and play

| Param | Type | Required | Default | Description |
|---|---|---|---|---|
| `text` | str | Yes | — | Text content to speak |
| `voice` | str | No | `zh-CN-XiaoxiaoNeural` | Azure voice name |
| `rate` | str | No | `0%` | Speaking rate (`+10%`, `-20%`) |
| `volume` | int | No | `50` | Volume 0-100 |

### `stop` — Stop current playback

No parameters. Returns `OK` even if nothing is playing.

## Lua Usage Example

```lua
-- Speak a greeting
do_action("00000000-0000-0000-0000-000000000003", "speak", {
    text = "你好，欢迎回家！",
    voice = "zh-CN-XiaoxiaoNeural",
    volume = 60
})

-- Stop playback
do_action("00000000-0000-0000-0000-000000000003", "stop", {})
```

## Response Codes

| Code | Name | Description |
|---|---|---|
| 0 | `OK` | Success |
| 4 | `INVALID_REQUEST` | JSON parse failure |
| 5 | `MISSING_FIELD` | Required `text` parameter missing |
| 20 | `ACTION_NOT_FOUND` | Unknown action ID |
| 22 | `ACTION_FAILED` | TTS synthesis or playback failed |
| 23 | `DEVICE_BUSY` | Currently speaking, retry later |

## Command Line

```
usage: main.py [-h] [--config CONFIG] [--debug]

options:
  --config CONFIG  Path to config.json (default: config.json next to script)
  --debug          Enable debug logging
```

## Supported Voices

Some common zh-CN voices:
- `zh-CN-XiaoxiaoNeural` (female, cheerful)
- `zh-CN-YunxiNeural` (male, narration)
- `zh-CN-XiaoyiNeural` (female, chat)
- `zh-CN-YunjianNeural` (male, sports)
- `zh-CN-XiaochenNeural` (female, calm)

Full voice list: https://learn.microsoft.com/en-us/azure/ai-services/speech-service/language-support
