# svWebspeak

An NVDA driver for **SoftVoice**, the speech synthesizer that shipped with [pwWebSpeak](https://www.talkinginterfaces.org/artifacts/pwwebspeak/) in the mid to late
1990s.

The engine (`SVCTL32.DLL`, dated 18 August 1997) is a 32-bit i386 library. Modern
NVDA is 64-bit and cannot load it in-process, so svWebspeak runs the engine in a
small dedicated host process and streams the rendered audio back to NVDA over a
loopback socket. NVDA plays the audio through its own `WavePlayer`, so output
device selection, audio ducking and speech cancellation all behave normally.

All 20 SoftVoice personalities are available, with a selectable sample rate up
to 22050 Hz.

## Requirements

- NVDA 2019.3 or later
- Windows 10/11 x64
- The SoftVoice registration key in the registry (see below)

## Registration is required

An unregistered SoftVoice engine does not refuse to speak. It **silently
truncates every utterance to a single 16 KB buffer** — about 0.7 seconds. This
is the single most confusing failure mode of this engine, so it is worth being
explicit about.

A registry file, sv_license.reg, is included in the releases. Simply hit Enter on the file and answer yes to the prompts that appear. Please do this before installing the add-on.

## Building

Requires Visual Studio 2022 with the **x86** build tools (the host must be
32-bit to match the engine) and Python 3 for packaging.

```
build.cmd
```

This builds `svwebspeak-host.exe`, copies it into the add-on tree, and produces
`svWebspeak.nvda-addon`. Install that through NVDA's Add-on Store, or
*Tools → Add-on store → Install from external source*.

SVctl32.dll, sveng32.dll and svspan32.dll must be present in addon/synthDrivers/svWebspeak.

svspan32.dll is the Spanish language data. The host opens the engine with both
the English and Spanish language bits, so whichever data DLLs are present
become selectable; drop svspan32.dll and the add-on quietly falls back to
English only.

## Settings

Available in NVDA's Speech settings, and in the settings ring:

| Setting | Notes |
|---|---|
| Voice | Lets you switch between SoftVoice's 20 voice profiles. |
| Rate | Maps to the engine's 20–500 range; 50% is its natural speed |
| Pitch | **Relative** to the selected voice — 50% is that voice's own pitch |
| Volume | 0–100 |
| Inflection | Maps to `SVSetF0Range` |
| Sample rate | 8000, 11025 or 22050 Hz (default). Applied on the next utterance |
| Language | English or Spanish, switched live without restarting the engine |

Pitch is deliberately relative. Each personality is a complete preset with its
own pitch, so applying one absolute value to all of them would flatten Child,
Colossus and Choir Boy into the same voice.

The 20 voices are the same in either language: a personality is a set of
formant parameters held in SVCTL32, while the language DLL supplies the
text-to-phoneme rules. Only the pronunciation changes.

## Credits

- SoftVoice synthesizer © 1994–1997 SoftVoice, Inc.
- pwWebSpeak by The Productivity Works, Inc.

This project contains no SoftVoice code — only an NVDA driver that calls the
engine's published entry points.
