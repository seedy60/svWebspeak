# svWebspeak

An NVDA synthesizer driver for **SoftVoice**, the formant speech synthesizer that
shipped with [pwWebSpeak](https://en.wikipedia.org/wiki/PwWebSpeak) in the late
1990s.

The engine (`SVCTL32.DLL`, dated 18 August 1997) is a 32-bit i386 library. Modern
NVDA is 64-bit and cannot load it in-process, so svWebspeak runs the engine in a
small dedicated host process and streams the rendered audio back to NVDA over a
loopback socket. NVDA plays the audio through its own `WavePlayer`, so output
device selection, audio ducking and speech cancellation all behave normally.

All 20 SoftVoice personalities are available, with a selectable sample rate up
to 22050 Hz.

## Requirements

- NVDA 2026.1 or later (64-bit)
- Windows 10/11 x64
- Your own copy of the SoftVoice engine files (see below)
- The SoftVoice registration key in the registry (see below)

## The engine files are not included

`SVctl32.DLL`, `SVENG32.DLL` and `Svspan32.dll` are proprietary SoftVoice Inc. /
Productivity Works binaries. They are **not** redistributable and are therefore
not in this repository.

If you have a pwWebSpeak installation, the installer placed them in the Windows
system directory. Copy these three files into
`addon/synthDrivers/svWebspeak/` before building:

| File | Size | Purpose |
|---|---|---|
| `SVctl32.DLL` | 94208 | Control layer — the API this driver calls |
| `SVENG32.DLL` | 327168 | English language data |
| `Svspan32.dll` | 67072 | Spanish language data (optional) |

`pwWebSpeak32\INSTALL.LOG` records where the installer put them —
`C:\WINDOWS\SYSTEM\` on the original Windows 9x install.

## Registration is required

An unregistered SoftVoice engine does not refuse to speak. It **silently
truncates every utterance to a single 16 KB buffer** — about 0.7 seconds. This
is the single most confusing failure mode of this engine, so it is worth being
explicit about.

`SVRegister` needs two things, both under
`HKLM\SOFTWARE\SoftVoice\ProdWorks`:

| Value | Type | Meaning |
|---|---|---|
| `SV_SSIL` | DWORD | Set to `1` |
| `SV_KEY` | DWORD | The registration number your pwWebSpeak install uses |

Because the host is 32-bit, Windows redirects it to
`HKLM\SOFTWARE\WOW6432Node\SoftVoice\ProdWorks`, so both locations should be
written. Copy `svWebspeak-register.example.reg`, substitute your own `SV_KEY`,
and import it with administrator rights:

```
reg import svWebspeak-register.reg
```

`SV_KEY` is **not** compiled into this project and is not in the repository —
it belongs to your own licensed copy of the software. The driver reads it from
the registry at runtime. If it is missing, the driver says so on load and
speech will be clipped to under a second.

To recover the value from your own installation, look at the arguments
`pwspeech.dll` passes to `_SVRegister@20`; it is the fourth argument, pushed as
an immediate at the call site.

## Building

Requires Visual Studio 2022 with the **x86** build tools (the host must be
32-bit to match the engine) and Python 3 for packaging.

```
build.cmd
```

This builds `svwebspeak-host.exe`, copies it into the add-on tree, and produces
`svWebspeak.nvda-addon`. Install that through NVDA's Add-on Store, or
*Tools → Add-on store → Install from external source*.

## Settings

Available in NVDA's Speech settings, and in the settings ring:

| Setting | Notes |
|---|---|
| Voice | 20 personalities (see below) |
| Rate | Maps to the engine's 20–500 range; 50% is its natural speed |
| Pitch | **Relative** to the selected voice — 50% is that voice's own pitch |
| Volume | 0–100 |
| Inflection | Maps to `SVSetF0Range` |
| Capital pitch change | Honoured via inline `PitchCommand` (see below) |
| Sample rate | 8000, 11025 or 22050 Hz (default). Applied on the next utterance |

Pitch is deliberately relative. Each personality is a complete preset with its
own pitch, so applying one absolute value to all of them would flatten Child,
Colossus and Choir Boy into the same voice.

NVDA implements "capital pitch change" by putting a `PitchCommand` inline in
the speech sequence. The engine's pitch applies to a whole `SVTTS` call, so the
driver splits the sequence at each pitch change and renders the pieces
separately, reporting completion only after the last one. Measured: a capital
spoken with a +30 offset comes out at 132 Hz against 101 Hz for the same
letter unmodified.

### Voices

Index order matters — it is the **reverse** of the order the names are stored in
the DLL, which is easy to get wrong and results in every voice being mislabelled.
Verified by measuring each one:

| # | Voice | F0 | | # | Voice | F0 |
|---|---|---|---|---|---|---|
| 0 | Male | 104 Hz | | 10 | Martian | 140 Hz |
| 1 | Female | 218 Hz | | 11 | Colossus | 66 Hz |
| 2 | Large Male | 87 Hz | | 12 | Fast Fred | 144 Hz |
| 3 | Child | 401 Hz | | 13 | Old Woman | 311 Hz |
| 4 | Giant Male | 72 Hz | | 14 | Munchkin | 105 Hz |
| 5 | Mellow Female | 210 Hz | | 15 | Troll | 129 Hz |
| 6 | Mellow Male | 119 Hz | | 16 | Nerd | 168 Hz |
| 7 | Crisp Male | 137 Hz | | 17 | Milktoast | 139 Hz |
| 8 | The Fly | 339 Hz | | 18 | Tipsy | 132 Hz |
| 9 | Robotoid | 90 Hz | | 19 | Choir Boy | 432 Hz |

## How it works

```
NVDA (64-bit)                        svwebspeak-host.exe (32-bit)
  synthDriver                                host
      |  CMD_SPEAK / CMD_STOP / CMD_PARAM       |
      |---------------- TCP loopback ---------->|
      |                                          SVTTS()  -> SVCTL32.DLL
      |                                          waveOutWrite hooked
      |<--- EVT_AUDIO (PCM) / EVT_DONE ---------|
   WavePlayer
```

The host redirects the engine's `waveOutWrite` import so rendered audio is
captured instead of reaching the sound card, and synthesises the buffer-done
notification the engine expects. That has a useful side effect: with nothing
waiting on a sound card, the engine renders roughly **460× faster than real
time** — a 10-second utterance is produced in about 23 ms.

### Notes for anyone doing similar work

Several details of this engine are non-obvious and cost real time to work out:

- **`SVNarrate` speaks phonemes, not text.** `SVTTS` is the text entry point.
  `SVTTS` with flag `0x80` returns the phoneme transcription instead of
  speaking, which is a handy way to inspect the front end.
- **`SVSetPersonality` resets rate, pitch, volume and inflection** to that
  voice's preset values. Any prosody must be re-applied afterwards.
- **The engine's window is top-level**, not message-only. The host must keep
  pumping messages and must never block its main thread, or every
  `SendMessage(HWND_BROADCAST, ...)` on the desktop stalls — which shows up as
  misdirected mouse clicks and modifier keys that appear stuck.
- **Engine parameter ranges** (probed via return code 7010, "parameter out of
  range"): rate 20–500, pitch 10–2000, volume 0–100, inflection 0–500.
  `SVSetPitch(v)` yields an F0 of roughly `1.16 * v`.
- The `pwWebSpeak32` folder does **not** contain the engine. Everything in it is
  VB6 application scaffolding; the synthesizer went to the Windows system
  directory.

- **The word-index scheduler is a dead end for text-to-speech.** The periodic
  timer callback at `0x12850` walks a 100-entry table at `ctx+0xa4` and posts
  `PostMessageA(ctx[0], ctx[0x20], entry.word, &entry+4)`, where `ctx[0]` is
  `SVOpenSpeech`'s arg2. Passing a real window there does make the engine
  address it, but the table is **never populated** by `SVTTS` (measured: zero
  entries after an eight-word sentence) and `ctx[0x20]`, the message it would
  post, stays zero — nothing in the DLL appears to set it. `SVTextToPhon`
  compares `{` and `}`, but braces in input text are simply spoken aloud
  rather than parsed as embedded marks. So word-level index reporting is not
  available through this engine; indexes are reported per utterance instead.

`tools/svprobe.c` is the standalone diagnostic harness used to work all of this
out. It loads the DLL directly, verifies the exports, reports the error table
and can render to a WAV file. `tools/svindex.c` is the harness for the index
scheduler above: it passes a window as arg2, intercepts the engine's
`PostMessageA` calls and dumps the scheduler table.

## Credits

- SoftVoice synthesizer © 1994–1997 SoftVoice, Inc.
- pwWebSpeak by The Productivity Works, Inc.
- NVDA driver by Andre Louis

This project contains no SoftVoice code — only an NVDA driver that calls the
engine's published entry points.
