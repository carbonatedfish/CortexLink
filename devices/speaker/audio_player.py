"""Audio player for WAV files on Linux.

Auto-detects available players: paplay, aplay, ffplay.
Supports synchronous playback with stop capability.
"""

import logging
import os
import subprocess
import threading
import time

logger = logging.getLogger(__name__)


class AudioPlayer:
    """Plays WAV audio files using system audio tools."""

    # Detection order: PulseAudio > ALSA > ffmpeg
    _PLAYER_CANDIDATES = ["paplay", "aplay", "ffplay"]
    _FFPLAY_CMD = ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"]

    def __init__(self, preferred: str = "auto"):
        """Initialize player.

        Args:
            preferred: "auto" to auto-detect, or a specific player name.
        """
        self._player_cmd = None
        self._process: subprocess.Popen | None = None
        self._lock = threading.Lock()

        if preferred == "auto":
            self._player_cmd = self._detect_player()
        else:
            self._player_cmd = preferred
            if not self._check_player(preferred):
                logger.warning("Preferred player '%s' not found", preferred)

        if self._player_cmd:
            logger.info("Audio player: %s", self._player_cmd)
        else:
            logger.warning(
                "No audio player found (tried: %s). Audio playback disabled.",
                ", ".join(self._PLAYER_CANDIDATES),
            )

    def _check_player(self, name: str) -> bool:
        """Check if a player executable is available."""
        try:
            subprocess.run(
                ["which", name],
                capture_output=True,
                timeout=3,
            )
            return True
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return False

    def _detect_player(self) -> str | None:
        """Detect the first available audio player."""
        for name in self._PLAYER_CANDIDATES:
            if self._check_player(name):
                return name
        return None

    def is_available(self) -> bool:
        """Return True if an audio player is available."""
        return self._player_cmd is not None

    def play(self, wav_path: str, stop_event: threading.Event | None = None) -> bool:
        """Play a WAV file synchronously.

        Blocks until playback completes or stop_event is set.

        Args:
            wav_path: Path to the WAV file to play.
            stop_event: Optional event to signal early termination.

        Returns:
            True if playback completed successfully, False if stopped or failed.

        Raises:
            RuntimeError: If already playing or no player is available.
        """
        if not self._player_cmd:
            raise RuntimeError("No audio player available")

        with self._lock:
            if self._process is not None and self._process.poll() is None:
                raise RuntimeError("Already playing")
            self._process = None

        if not os.path.exists(wav_path):
            logger.error("WAV file not found: %s", wav_path)
            return False

        # Build command
        if self._player_cmd == "ffplay":
            cmd = self._FFPLAY_CMD + [wav_path]
        else:
            cmd = [self._player_cmd, wav_path]

        logger.debug("Playing: %s", cmd)

        try:
            with self._lock:
                self._process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
        except Exception as e:
            logger.error("Failed to start audio player: %s", e)
            return False

        # Wait for completion or stop signal
        while self._process.poll() is None:
            if stop_event and stop_event.is_set():
                self.stop()
                return False
            time.sleep(0.1)

        rc = self._process.returncode
        with self._lock:
            self._process = None

        if rc != 0:
            logger.warning("Audio player exited with code %d", rc)
            return False

        return True

    def stop(self) -> bool:
        """Stop current playback.

        Returns:
            True if playback was stopped, False if nothing was playing.
        """
        with self._lock:
            proc = self._process
            self._process = None

        if proc is None or proc.poll() is not None:
            return False

        logger.debug("Stopping playback (pid=%d)", proc.pid)
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            logger.warning("Player did not terminate, killing (pid=%d)", proc.pid)
            proc.kill()
            proc.wait(timeout=2)

        return True

    def is_playing(self) -> bool:
        """Return True if audio is currently playing."""
        with self._lock:
            return self._process is not None and self._process.poll() is None
