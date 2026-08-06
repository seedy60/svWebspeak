# svWebspeak - SoftVoice synthesizer driver for NVDA.
#
# SVctl32.DLL is a 1997 32-bit i386 library. NVDA 2026.1 is the first 64-bit
# release and cannot load it in-process; 2025.3 and earlier are 32-bit and
# could. The engine runs in svwebspeak-host.exe on every version regardless, so
# there is one code path - and since the engine creates a top-level window and
# is not thread-safe, keeping it out of NVDA's process is worth doing anyway.
# The host captures the engine's PCM and streams it here; playback is done by
# NVDA's own WavePlayer so output device selection, ducking and cancellation
# all behave normally.

import os
import queue
import socket
import struct
import subprocess
import threading
import time

from collections import OrderedDict

import addonHandler
import config
import nvwave
from autoSettingsUtils.driverSetting import DriverSetting
from autoSettingsUtils.utils import StringParameterInfo
from logHandler import log
from speech.commands import IndexCommand, PitchCommand
from synthDriverHandler import (
    SynthDriver,
    VoiceInfo,
    synthDoneSpeaking,
    synthIndexReached,
)

addonHandler.initTranslation()

HOST_EXE = "svwebspeak-host.exe"
CONNECT_TIMEOUT = 10.0
INIT_TIMEOUT = 20.0
# Cap restarts so a permanently broken engine cannot spin forever.
MAX_RECOVERIES = 5

CMD_SPEAK, CMD_STOP, CMD_PARAM, CMD_SHUTDOWN = 1, 2, 3, 4
EVT_AUDIO, EVT_DONE = 1, 2
P_RATE, P_PITCH, P_VOLUME, P_PERSONALITY, P_INFLECTION = 1, 2, 3, 4, 5
P_LANGUAGE = 6

# SVSetLanguage takes the engine's own language bit, and only accepts one
# that was loaded at SVOpenSpeech. The host opens with English|Spanish, so
# whichever data DLLs are present become selectable.
LANGUAGES = ((0x1, "en", "English"), (0x2, "es", "Spanish"))

# Sample rates the engine supports. 22050 is the default because it is the
# best quality it offers; the flags are decoded in the host.
SAMPLE_RATES = ("8000", "11025", "22050")
DEFAULT_SAMPLE_RATE = "22050"

# Per-access tracing. Errors are always logged regardless.
DIAGNOSTICS = False

# The 20 built-in personalities. The engine's index order is the REVERSE of
# the order the names are stored in SVctl32.DLL; verified by measuring each
# one (idx 3 Child = 420 Hz, 8 The Fly = 582 Hz, 11 Colossus = 66 Hz, 12 Fast
# Fred is the fastest, 19 Choir Boy = 326 Hz). Getting this wrong makes every
# voice the wrong voice.
PERSONALITIES = (
    "Male", "Female", "Large Male", "Child", "Giant Male", "Mellow Female",
    "Mellow Male", "Crisp Male", "The Fly", "Robotoid", "Martian", "Colossus",
    "Fast Fred", "Old Woman", "Munchkin", "Troll", "Nerd", "Milktoast",
    "Tipsy", "Choir Boy",
)

# Each personality's own pitch, in engine units. Pitch is applied relative to
# these so a voice keeps its character instead of every voice being forced to
# one pitch, which means a wrong value here detunes that voice at every slider
# position - including the 50% default, where SVSetPitch must be a no-op.
#
# These are the engine's own values, read straight out of SVGetVoiceInfo (the
# 16-bit field at offset 4 of the per-voice block); regenerate with
# tools\svvoice.exe. They are NOT derived from rendered audio: an earlier
# table inferred them as F0/1.16 and was wrong for 18 of the 20 voices, badly
# so for Martian (121 vs 80), The Fly (292 vs 480) and Giant Male (62 vs 45).
# Verified by rendering: SVSetPitch(value below) reproduces each preset to
# 0.0%, so the relative mapping is exactly neutral at 50%.
NATURAL_PITCH = (
    90, 200, 80, 350, 45, 190, 110, 125, 480, 90,
    80, 66, 135, 270, 90, 110, 140, 120, 145, 310,
)

# Accepted engine ranges, probed via the return code (7010 = out of range).
RATE_MIN, RATE_NATURAL, RATE_MAX = 20, 150, 500
PITCH_MIN, PITCH_MAX = 10, 2000
INFLECTION_MAX = 200


class SynthDriver(SynthDriver):
    name = "svWebspeak"
    description = "SoftVoice (pwWebSpeak)"

    supportedSettings = (
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.VolumeSetting(),
        SynthDriver.InflectionSetting(),
        SynthDriver.LanguageSetting(),
        DriverSetting(
            "samplerate",
            # Translators: label for the sample rate setting, with accelerator.
            _("Sample &rate (Hz)"),
            # Exposed in the settings ring so it is reachable from the
            # keyboard without opening the settings dialog.
            availableInSettingsRing=True,
            defaultVal=DEFAULT_SAMPLE_RATE,
            # Translators: label for the sample rate setting.
            displayName=_("Sample rate"),
        ),
    )
    # PitchCommand is how NVDA implements "capital pitch change".
    supportedCommands = {IndexCommand, PitchCommand}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    @classmethod
    def check(cls):
        return os.path.isfile(os.path.join(os.path.dirname(__file__), HOST_EXE))

    def __init__(self):
        super().__init__()
        self._proc = None
        self._conn = None
        self._reader = None
        self._player = None
        self._lock = threading.Lock()
        self._msgId = 0
        self._closing = False
        self._rate = 50
        self._pitch = 50
        self._volume = 100
        self._inflection = 50
        self._voice = "0"
        self._sampleRate = DEFAULT_SAMPLE_RATE
        self._engineRate = 22050
        self._language = "en"
        self._langMask = 0x1
        # Index commands are resolved against how much audio precedes them.
        self._utterId = 0
        self._utterIndexes = {}
        self._recovering = False
        self._recoveries = 0
        # Last pitch actually sent to the engine, so inline pitch changes only
        # cost a message when the value really changes.
        self._enginePitch = None
        self._pendingRateChange = False
        # Cancellation epoch, not an utterance counter. NVDA queues multiple
        # utterances with speak() and only interrupts with cancel(), so every
        # speak between two cancels shares an epoch and all of their audio
        # stays valid.
        self._speechSeq = 0
        self._audioQueue = queue.Queue()
        self._playerThread = threading.Thread(target=self._playerLoop,
                                              daemon=True)
        self._playerThread.start()
        # Which settings the user actually chose, so voice presets are only
        # overridden where that is genuinely wanted.
        self._userSet = set()
        self._startHost()

    # ---------------------------------------------------------------- host

    def _hostPath(self):
        return os.path.join(os.path.dirname(__file__), HOST_EXE)

    def _engineDir(self):
        # The engine DLLs ship beside the host.
        return os.path.dirname(__file__)

    def _startHost(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        server.settimeout(CONNECT_TIMEOUT)
        host, port = server.getsockname()

        cmd = [
            self._hostPath(),
            "--address", "%s:%d" % (host, port),
            "--dir", self._engineDir(),
            "--rate", self._sampleRate,
            "--bits", "16",
        ]
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = subprocess.SW_HIDE
        try:
            # CREATE_NO_WINDOW so launching the host never allocates a console
            # or briefly steals focus from whatever the user is doing.
            self._proc = subprocess.Popen(
                cmd, startupinfo=si,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
        except Exception:
            server.close()
            raise
        try:
            self._conn, _peer = server.accept()
            self._conn.settimeout(None)
        except Exception:
            self._killProc()
            server.close()
            raise RuntimeError("svWebspeak host did not connect")
        server.close()

        # The reader thread is what delivers audio and the init status; without
        # it the driver loads happily and is then permanently silent.
        self._initEvent = threading.Event()
        self._initStatus = None
        self._reader = threading.Thread(target=self._readLoop, daemon=True)
        self._reader.start()

        if not self._initEvent.wait(INIT_TIMEOUT) or self._initStatus != 0:
            status = self._initStatus
            self.terminate()
            if status == -4:
                raise RuntimeError(
                    "svWebspeak: the SoftVoice registration number is missing. "
                    "Add a DWORD named SV_KEY under "
                    r"HKLM\SOFTWARE\SoftVoice\ProdWorks (and under "
                    "WOW6432Node) - see the README. Without it the engine "
                    "clips every utterance to under a second."
                )
            raise RuntimeError(
                "svWebspeak: SoftVoice engine failed to initialise "
                "(status %r). Check that SVctl32.DLL and SVENG32.DLL are "
                "present in the add-on folder." % (status,)
            )

        try:
            output = config.conf["audio"]["outputDevice"]
        except Exception:
            output = config.conf["speech"]["outputDevice"]
        self._diag("host ready, engine rate %d" % self._engineRate)
        self._player = nvwave.WavePlayer(
            channels=1,
            samplesPerSec=self._engineRate,
            bitsPerSample=16,
            outputDevice=output,
        )

    def _scheduleRecover(self):
        if self._closing or self._recovering:
            return
        if self._recoveries >= MAX_RECOVERIES:
            log.error("svWebspeak: host has failed %d times; giving up"
                      % self._recoveries)
            return
        self._recovering = True
        threading.Thread(target=self._recover, daemon=True).start()

    def _recover(self):
        try:
            time.sleep(0.25)
            if self._closing:
                return
            self._recoveries += 1
            log.warning("svWebspeak: host stopped unexpectedly, restarting "
                        "(attempt %d)" % self._recoveries)
            self._shutdownHost()
            try:
                while True:
                    self._audioQueue.get_nowait()
            except queue.Empty:
                pass
            self._startHost()
            # _set_voice re-applies the prosody the user had chosen.
            self._set_voice(self._voice)
        except Exception:
            log.error("svWebspeak: host restart failed", exc_info=True)
        finally:
            self._recovering = False

    def _killProc(self):
        if self._proc and self._proc.poll() is None:
            try:
                self._proc.terminate()
            except Exception:
                pass

    # ---------------------------------------------------------------- wire

    def _send(self, cmd, payload=b""):
        with self._lock:
            self._msgId += 1
            body = b"\x01" + struct.pack("<IH", self._msgId, cmd) + payload
            frame = struct.pack("<I", len(body)) + body
            try:
                self._conn.sendall(frame)
            except Exception:
                if not self._closing:
                    log.error("svWebspeak host connection lost", exc_info=True)

    def _recvExact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self._conn.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("host closed")
            buf += chunk
        return buf

    def _readLoop(self):
        try:
            while True:
                (length,) = struct.unpack("<I", self._recvExact(4))
                payload = self._recvExact(length)
                kind = payload[0]
                if kind == 2:
                    msgId, status = struct.unpack_from("<II", payload, 1)
                    if msgId == 0:
                        rc, rate, _bits, langs = struct.unpack_from(
                            "<iIII", payload, 9)
                        self._engineRate = rate
                        self._langMask = langs
                        self._initStatus = rc
                        self._initEvent.set()
                elif kind == 3:
                    (evt,) = struct.unpack_from("<H", payload, 1)
                    if evt == EVT_AUDIO:
                        seq, n = struct.unpack_from("<II", payload, 3)
                        self._onAudio(seq, payload[11:11 + n])
                    elif evt == EVT_DONE:
                        seq, utt = struct.unpack_from("<II", payload, 3)
                        self._onDone(seq, utt)
        except Exception:
            if not self._closing:
                log.debugWarning("svWebspeak reader stopped", exc_info=True)
                # Losing the host must not leave the driver permanently mute -
                # for a screen reader that is the worst possible failure. Bring
                # it back instead of waiting for the user to switch synths.
                self._scheduleRecover()
            self._initEvent.set()

    # --------------------------------------------------------------- audio

    def _onAudio(self, seq, pcm):
        # WavePlayer.feed() blocks once its buffer is full. Calling it from the
        # socket reader would stop us draining the socket, which back-pressures
        # the host and freezes the thread owning the engine's top-level window
        # - stalling message broadcasts for the whole desktop. So hand off to a
        # player thread and keep reading.
        if not pcm or seq != self._speechSeq:
            # Belongs to an utterance that has since been cancelled or
            # superseded; a bare "cancelled" flag would race with the next
            # speak() clearing it.
            return
        self._audioQueue.put(("audio", seq, pcm))

    def _playerLoop(self):
        while True:
            item = self._audioQueue.get()
            if item is None:
                return
            kind, seq, payload = item
            # Re-check here as well as on receipt: cancel() can land between
            # get() and feed(), and that in-flight block would otherwise play
            # after the user has already interrupted.
            if seq != self._speechSeq:
                continue
            if kind == "done":
                self._finishUtterance(payload)
                continue
            player = self._player
            if not player:
                continue
            try:
                player.feed(payload)
            except Exception:
                log.debugWarning("svWebspeak feed failed", exc_info=True)

    def _outstandingAudio(self):
        """True while more audio for this run of speech is still expected."""
        return bool(self._utterIndexes) or not self._audioQueue.empty()

    def _releasePlayer(self):
        """Drain the player once nothing further is expected.

        WavePlayer.feed() enables NVDA's audio ducker on every call, and only
        idle() or stop() release it again. cancel() calls stop(), which is why
        interrupting speech restored the volume, but an utterance left to
        finish on its own never released the duck and the system stayed
        quiet indefinitely.

        idle() syncs before it releases, so calling it at every utterance
        boundary would drain the buffer between queued utterances and bring
        back the choppy, word-at-a-time delivery. Only sync when nothing is
        outstanding, then check again: sync() blocks, and NVDA can queue more
        speech while it does. This mirrors what oneCore does.
        """
        player = self._player
        if not player or self._outstandingAudio():
            return
        # Best effort only. idle() syncs internally, so a failure here must
        # not skip it - that would leave the duck applied, which is the whole
        # bug this guards against.
        try:
            player.sync()
        except Exception:
            log.debugWarning("svWebspeak: player sync failed", exc_info=True)
        if self._outstandingAudio():
            return
        try:
            player.idle()
        except Exception:
            log.debugWarning("svWebspeak: could not idle the player",
                             exc_info=True)

    def _finishUtterance(self, utt):
        """Report an utterance's indexes, then its completion, in order.

        NVDA holds queued speech until it sees these, so they must fire for
        every utterance. Per-utterance state is essential: NVDA issues several
        speak() calls within a few hundred ms and a single shared slot loses
        all but the last one's indexes, which stalls the queue.
        """
        indexes, isFinal = self._utterIndexes.pop(utt, ((), True))
        for index in indexes:
            synthIndexReached.notify(synth=self, index=index)
        if isFinal:
            # Release the duck before announcing completion, so the volume is
            # already back to normal by the time NVDA acts on it.
            self._releasePlayer()
            synthDoneSpeaking.notify(synth=self)

    def _onDone(self, seq, utt):
        if seq != self._speechSeq:
            return
        # Queue the completion behind that utterance's audio so indexes and
        # the done notification fire in the right order relative to playback.
        self._audioQueue.put(("done", seq, utt))

    # --------------------------------------------------------------- speech

    def _splitByPitch(self, speechSequence):
        """Split a sequence into (pitchPercent, text, indexes) segments.

        The engine's pitch is a property of a whole SVTTS call, so an inline
        pitch change - which is how NVDA implements "capital pitch change" -
        can only be honoured by rendering each stretch separately.
        """
        segments = []
        curText, curIdx = [], []
        curPitch = self._pitch

        def flush(pitch):
            joined = " ".join(t for t in curText if t).strip()
            if joined or curIdx:
                segments.append((pitch, joined, list(curIdx)))
            del curText[:]
            del curIdx[:]

        for item in speechSequence:
            if isinstance(item, str):
                curText.append(item)
            elif isinstance(item, IndexCommand):
                curIdx.append(item.index)
            elif isinstance(item, PitchCommand):
                try:
                    newPitch = int(item.newValue)
                except Exception:
                    newPitch = self._pitch + getattr(item, "offset", 0)
                newPitch = max(0, min(100, newPitch))
                if newPitch != curPitch:
                    flush(curPitch)
                    curPitch = newPitch
        flush(curPitch)

        # A segment with indexes but no text has nothing to render; move its
        # indexes onto the next segment so they still fire, in order.
        merged = []
        carried = []
        for pitch, txt, idx in segments:
            if not txt:
                carried.extend(idx)
                continue
            merged.append((pitch, txt, carried + idx))
            carried = []
        if carried:
            if merged:
                merged[-1] = (merged[-1][0], merged[-1][1],
                              merged[-1][2] + carried)
            else:
                merged.append((self._pitch, "", carried))
        return merged

    def speak(self, speechSequence):
        segments = self._splitByPitch(speechSequence)
        spoken = [s for s in segments if s[1]]
        if not spoken:
            for _pitch, _txt, idx in segments:
                for index in idx:
                    synthIndexReached.notify(synth=self, index=index)
            synthDoneSpeaking.notify(synth=self)
            return

        if self._pendingRateChange:
            self._pendingRateChange = False
            self._diag("applying deferred samplerate %s" % self._sampleRate)
            try:
                self._restart()
            except Exception:
                log.error("svWebspeak: samplerate restart failed", exc_info=True)

        # Do NOT bump the epoch here: that would invalidate audio for
        # utterances NVDA has queued but not yet heard.
        seq = self._speechSeq
        last = len(spoken) - 1
        for n, (pitch, txt, idx) in enumerate(spoken):
            self._applyPitchPercent(pitch)
            self._utterId += 1
            utt = self._utterId
            # Indexes are kept per utterance and reported when that
            # utterance's audio has been fed, so several queued speak() calls
            # cannot lose each other's indexes. Only the final segment reports
            # completion, or NVDA would advance once per segment.
            self._utterIndexes[utt] = (idx, n == last)
            data = txt.encode("mbcs", "replace")
            self._send(CMD_SPEAK,
                       struct.pack("<III", seq, utt, len(data)) + data)

    def cancel(self):
        # Only a cancel invalidates in-flight audio.
        self._speechSeq += 1
        self._utterIndexes.clear()
        # Drop anything still queued or it will play after the stop.
        try:
            while True:
                self._audioQueue.get_nowait()
        except queue.Empty:
            pass
        self._send(CMD_STOP)
        if self._player:
            try:
                self._player.stop()
            except Exception:
                pass

    def pause(self, switch):
        if self._player:
            try:
                self._player.pause(switch)
            except Exception:
                pass

    # ------------------------------------------------------------- settings

    def _applyPitchPercent(self, percent):
        """Set the engine pitch for the next utterance, if it differs."""
        engine = self._pitchToEngine(percent)
        if engine != self._enginePitch:
            self._enginePitch = engine
            self._paramSend(P_PITCH, engine)

    def _paramSend(self, param, value):
        self._send(CMD_PARAM, struct.pack("<Hi", param, int(value)))

    # NVDA swallows AttributeError inside a property getter and reports the
    # setting as unsupported, so every accessor logs what it does. DIAG lines
    # are INFO so they appear without raising NVDA's logging level.
    def _diag(self, msg):
        if DIAGNOSTICS:
            log.info("svWebspeak: " + msg)

    def _get_rate(self):
        try:
            self._diag("get rate -> %r" % (self._rate,))
            return self._rate
        except Exception:
            log.error("svWebspeak: _get_rate failed", exc_info=True)
            raise

    def _rateToEngine(self, value):
        # Piecewise so 50% lands on the engine's natural speed rather than
        # halfway up a 20..500 span, which would read far too fast.
        if value <= 50:
            return RATE_MIN + value * (RATE_NATURAL - RATE_MIN) // 50
        return RATE_NATURAL + (value - 50) * (RATE_MAX - RATE_NATURAL) // 50

    def _pitchToEngine(self, value):
        # Relative to the selected voice: 50% = the voice's own pitch, so
        # Child stays high and Colossus stays deep.
        try:
            natural = NATURAL_PITCH[int(self._voice)]
        except (ValueError, IndexError):
            natural = NATURAL_PITCH[0]
        scaled = int(natural * (0.5 + value / 100.0))
        return max(PITCH_MIN, min(PITCH_MAX, scaled))

    def _set_rate(self, value):
        self._diag("set rate <- %r" % (value,))
        try:
            self._rate = value
            self._userSet.add("rate")
            self._paramSend(P_RATE, self._rateToEngine(value))
        except Exception:
            log.error("svWebspeak: _set_rate failed", exc_info=True)
            raise

    def _get_pitch(self):
        try:
            self._diag("get pitch -> %r" % (self._pitch,))
            return self._pitch
        except Exception:
            log.error("svWebspeak: _get_pitch failed", exc_info=True)
            raise

    def _set_pitch(self, value):
        self._diag("set pitch <- %r" % (value,))
        try:
            self._pitch = value
            self._userSet.add("pitch")
            self._enginePitch = self._pitchToEngine(value)
            self._paramSend(P_PITCH, self._enginePitch)
        except Exception:
            log.error("svWebspeak: _set_pitch failed", exc_info=True)
            raise

    def _get_volume(self):
        self._diag("get volume -> %r" % (self._volume,))
        return self._volume

    def _set_volume(self, value):
        self._diag("set volume <- %r" % (value,))
        self._volume = value
        self._userSet.add("volume")
        self._paramSend(P_VOLUME, max(0, min(100, value)))

    def _get_inflection(self):
        self._diag("get inflection -> %r" % (self._inflection,))
        return self._inflection

    def _set_inflection(self, value):
        self._diag("set inflection <- %r" % (value,))
        self._inflection = value
        self._userSet.add("inflection")
        self._paramSend(P_INFLECTION, value * INFLECTION_MAX // 100)

    def _get_language(self):
        # NVDA calls languageIsSupported(self.language) on every speech
        # sequence; the base class returns None, which crashes
        # normalizeLanguage and aborts language handling for the utterance.
        return self._language

    def _set_language(self, value):
        code = (value or "en").split("_")[0].lower()
        bit = next((b for b, c, _n in LANGUAGES if c == code), None)
        if bit is None or not (self._langMask & bit):
            return
        self._language = code
        self._paramSend(P_LANGUAGE, bit)

    def _get_availableLanguages(self):
        # Only offer what the engine actually loaded; Spanish depends on
        # Svspan32.dll being present next to the host.
        return OrderedDict(
            (code, StringParameterInfo(code, name))
            for bit, code, name in LANGUAGES if self._langMask & bit
        )

    def _get_availableVoices(self):
        # Tag each voice with its language, otherwise NVDA has nothing to
        # match against when it filters voices by language.
        return OrderedDict(
            (str(i), VoiceInfo(str(i), name, self._language))
            for i, name in enumerate(PERSONALITIES)
        )

    def _get_voice(self):
        return self._voice

    def _set_voice(self, value):
        self._diag("set voice <- %r" % (value,))
        if value not in self.availableVoices:
            value = "0"
        self._voice = value
        self._paramSend(P_PERSONALITY, int(value))
        # The preset resets pitch, so nothing cached is valid any more.
        self._enginePitch = None
        # SVSetPersonality loads a complete voice preset and resets rate,
        # pitch, volume and inflection to that personality's own values.
        # Measured: rate=300 then personality=1 renders 2.97s, but
        # personality=1 then rate=300 renders 1.49s. NVDA applies the voice
        # live as the user arrows through the combo, so without this the
        # user's speech rate is silently wiped on every keypress.
        self._applyVoiceParams()

    def _applyVoiceParams(self):
        """Re-send prosody after a voice preset is loaded.

        Only settings the user (or their saved config) actually chose are
        re-sent. Forcing all of them would flatten every personality onto the
        same prosody and destroy the character that distinguishes them.
        """
        self._diag("re-applying prosody after voice change: %s" % self._userSet)
        if "rate" in self._userSet:
            self._paramSend(P_RATE, self._rateToEngine(self._rate))
        if "pitch" in self._userSet:
            self._enginePitch = self._pitchToEngine(self._pitch)
            self._paramSend(P_PITCH, self._enginePitch)
        if "volume" in self._userSet:
            self._paramSend(P_VOLUME, max(0, min(100, self._volume)))
        if "inflection" in self._userSet:
            self._paramSend(P_INFLECTION,
                            self._inflection * INFLECTION_MAX // 100)

    def _get_availableSamplerates(self):
        return OrderedDict(
            (r, StringParameterInfo(r, r)) for r in SAMPLE_RATES
        )

    def _get_samplerate(self):
        self._diag("get samplerate -> %r" % (self._sampleRate,))
        return self._sampleRate

    def _set_samplerate(self, value):
        self._diag("set samplerate <- %r" % (value,))
        value = str(value)
        if value not in SAMPLE_RATES or value == self._sampleRate:
            return
        self._sampleRate = value
        # The engine fixes its format at SVOpenSpeech, so changing it needs a
        # host restart. Tearing the host down here would happen while the
        # settings dialog is still building its controls, which corrupts the
        # driver's state mid-dialog, so defer it to the next utterance.
        self._pendingRateChange = True
        self._diag("samplerate change deferred to next speak")

    def _restart(self):
        """Relaunch the host, keeping the driver itself alive.

        Must NOT go through terminate(): that stops the player thread for
        good, after which audio queues up with nothing feeding WavePlayer and
        speech only returns when NVDA recreates the driver.
        """
        settings = (self._voice, self._rate, self._pitch, self._volume,
                    self._inflection)
        self._shutdownHost()
        # Queued PCM belongs to the old sample rate; playing it through the
        # new device would sound wrong.
        try:
            while True:
                self._audioQueue.get_nowait()
        except queue.Empty:
            pass
        self._startHost()
        (self._voice, self._rate, self._pitch, self._volume,
         self._inflection) = settings
        # A fresh host starts on the engine's default language.
        self._set_language(self._language)
        # _set_voice re-applies prosody itself, so ordering is safe here.
        self._set_voice(self._voice)

    # ------------------------------------------------------------ lifecycle

    def _shutdownHost(self):
        """Stop the host process and audio device, leaving the driver usable."""
        self._closing = True
        try:
            self._send(CMD_SHUTDOWN)
        except Exception:
            pass
        if self._player:
            try:
                self._player.stop()
                self._player.close()
            except Exception:
                pass
            self._player = None
        if self._conn:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None
        self._killProc()
        self._proc = None
        self._closing = False

    def terminate(self):
        self._shutdownHost()
        # Only a real teardown stops the player thread.
        try:
            self._audioQueue.put(None)
        except Exception:
            pass
