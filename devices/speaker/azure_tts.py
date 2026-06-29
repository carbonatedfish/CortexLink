"""Azure AI Speech TTS wrapper.

Synthesizes text to speech using Azure Cognitive Services Speech SDK.
Outputs WAV files for playback by AudioPlayer.
"""

import logging
import os
import tempfile

logger = logging.getLogger(__name__)


class AzureTTS:
    """Azure Text-to-Speech synthesizer.

    Uses the Azure Speech SDK to convert text to speech audio,
    writing the result to a temporary WAV file.
    """

    def __init__(self, speech_key: str, speech_region: str):
        """Initialize Azure TTS client.

        Args:
            speech_key: Azure Speech service subscription key.
            speech_region: Azure region (e.g. "eastasia").
        """
        self._speech_key = speech_key
        self._speech_region = speech_region

        # Lazy import so import errors only happen when actually used
        self._speechsdk = None

    def _get_sdk(self):
        """Lazy-load the Azure Speech SDK."""
        if self._speechsdk is None:
            import azure.cognitiveservices.speech as speechsdk
            self._speechsdk = speechsdk
        return self._speechsdk

    def synthesize_to_wav(
        self,
        text: str,
        voice: str = "zh-CN-XiaoxiaoNeural",
        rate: str = "0%",
        volume: int = 50,
    ) -> str | None:
        """Synthesize text to a WAV file.

        Args:
            text: Text to synthesize.
            voice: Azure voice name (e.g. "zh-CN-XiaoxiaoNeural").
            rate: Speaking rate adjustment (e.g. "+10%", "-20%").
            volume: Volume level 0-100.

        Returns:
            Path to the temporary WAV file, or None on failure.
            Caller is responsible for deleting the file after use.
        """
        speechsdk = self._get_sdk()

        speech_config = speechsdk.SpeechConfig(
            subscription=self._speech_key,
            region=self._speech_region,
        )
        speech_config.speech_synthesis_voice_name = voice
        speech_config.set_speech_synthesis_output_format(
            speechsdk.SpeechSynthesisOutputFormat.Riff16Khz16BitMonoPcm
        )

        # Build SSML for fine-grained control
        ssml = (
            '<speak version="1.0" xmlns="http://www.w3.org/2001/10/synthesis"'
            ' xmlns:mstts="http://www.w3.org/2001/mstts"'
            f' xml:lang="zh-CN">'
            f'<voice name="{voice}">'
            f'<prosody rate="{rate}" volume="{volume}">'
            f"{text}"
            f"</prosody>"
            f"</voice>"
            f"</speak>"
        )

        # Create temp WAV file
        fd, temp_path = tempfile.mkstemp(suffix=".wav", prefix="cortexlink_tts_")
        os.close(fd)

        try:
            logger.debug("TTS: synthesizing text_len=%d voice='%s' rate='%s' volume=%d",
                         len(text), voice, rate, volume)
            audio_config = speechsdk.audio.AudioOutputConfig(filename=temp_path)
            synthesizer = speechsdk.SpeechSynthesizer(
                speech_config=speech_config,
                audio_config=audio_config,
            )

            result = synthesizer.speak_ssml(ssml)

            if result.reason == speechsdk.ResultReason.SynthesizingAudioCompleted:
                logger.debug(
                    "TTS synthesis completed: %d chars → %s",
                    len(text),
                    temp_path,
                )
                return temp_path

            elif result.reason == speechsdk.ResultReason.Canceled:
                cancellation = result.cancellation_details
                logger.error(
                    "TTS synthesis canceled: reason=%s, error=%s",
                    cancellation.reason,
                    cancellation.error_details,
                )
                _cleanup_temp(temp_path)
                return None

            else:
                logger.error(
                    "TTS synthesis unexpected result: reason=%s", result.reason
                )
                _cleanup_temp(temp_path)
                return None

        except Exception as e:
            logger.error("TTS synthesis exception: %s", e)
            _cleanup_temp(temp_path)
            return None


def _cleanup_temp(path: str) -> None:
    """Remove a temporary file, ignoring errors."""
    try:
        os.unlink(path)
    except OSError:
        pass
