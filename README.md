# svWebspeak

An NVDA driver for SoftVoice, the speech synthesizer that shipped with the [pwWebSpeak](https://www.talkinginterfaces.org/artifacts/pwwebspeak/) talking web browser in the mid to late
1990s.

The engine (`SVCTL32.DLL`, dated 18 August 1997) is a 32-bit i386 library.
NVDA 2026.1 is the first 64-bit release and cannot load it in-process; 2025.3
and earlier are 32-bit and could. svWebspeak always runs the engine in a small
dedicated host process anyway, and streams the rendered audio back to NVDA over
a loopback socket, so there is a single code path on every supported version.
NVDA plays the audio through its own `WavePlayer`, so output device selection,
audio ducking and speech cancellation all behave normally.

All 20 SoftVoice personalities are available, with a selectable sample rate up
to 22050 Hz.

## Requirements

- NVDA 2019.3 or later
- Windows 10/11 x64
- The SoftVoice registration key in the registry (see below)

## Registration is required

The build of SoftVoice this add-on uses requires a license to work fully. An unlicensed SoftVoice does not refuse to speak. It silently
truncates every utterance to a single 16 KB buffer; about 0.7 seconds. To guard against this, the add-on checks for the license at runtime. If it is not found, NVDA will refuse to load the synthesizer. No one wants to hear audio being cut off constantly.

A registry file, sv_license.reg, is included in the [releases](https://github.com/seedy60/svWebspeak/releases). Simply hit Enter on the file and answer yes to the prompts that appear. Please do this before installing the add-on.

## Building

Requires Visual Studio 2022 with the x86 build tools (the host must be
32-bit to match the engine) and Python 3 for packaging.

```
build.cmd
```

This builds `svwebspeak-host.exe`, copies it into the add-on tree, and produces
`svWebspeak.nvda-addon`, which you can install simply by launching the file and answering the prompts.

SVctl32.dll, sveng32.dll and svspan32.dll must be present in addon/synthDrivers/svWebspeak. You can get these files from a pwWebspeak installation if you have one.

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

## Updates

svWebspeak checks GitHub for new releases once a day and offers to download
and install anything newer. Two items are added to NVDA's Tools menu:

| Item | Notes |
|---|---|
| Check for svWebspeak updates... | Checks immediately and reports the result either way |
| Check for svWebspeak updates automatically | Toggles the daily check; the setting persists |

A failed check is retried in ten minutes rather than waiting another day.
Nothing is installed without asking first, and NVDA prompts to restart once
an update has been applied.

The updater lives in a global plugin rather than the synth driver, so updates
are still found while a different synthesizer is selected.

If you are on 2520 or earlier you will need to update once by hand; those
builds predate the updater.

### Cutting a release

The updater compares the release tag against the `version` in `manifest.ini`,
so the two must agree: version `2521` goes with tag `v2521`. Versions are a
single increasing integer, and that scheme must not change - `2520` would
outrank any `1.x` forever, so anyone already installed would stop being
offered updates.

Attach either the `.nvda-addon` itself or a `.zip` containing one; releases
currently ship a zip so `sv_license.reg` can travel with it, and the updater
unwraps that automatically. Note that it installs only the add-on - a new
registration file still has to be imported by hand.

## Credits

- SoftVoice synthesizer © 1994–1997 SoftVoice, Inc.
- pwWebSpeak by The Productivity Works, Inc.

This project contains no SoftVoice code — only an NVDA driver that calls the
engine's published entry points.
