/*
 * svprobe.c - SoftVoice SVCTL32 probe / synthesis harness (32-bit)
 *
 * Findings this encodes (all recovered by disassembly + measurement):
 *   - SVctl32 exports 35 stdcall functions; the handle from SVOpenSpeech is
 *     the calloc'd context pointer itself.
 *   - Output is mono PCM. Flags select depth (0x400000 = 8-bit,
 *     0x800000 = 16-bit) and rate (0x2000000 = 8k, 0x4000000 = 11025,
 *     0x8000000 = 22050); low 7 bits select language.
 *   - SVNarrate speaks PHONEMES. SVTTS speaks text, and with flag 0x80 it
 *     instead returns the phoneme transcription (arg3 = char** which the
 *     engine repoints at its own buffer, arg4 = DWORD* in/out size).
 *   - Every speak call is capped by a single GlobalAlloc(0x4000) wave buffer:
 *     16384 BYTES, regardless of text, rate or mode. Since the cap is in
 *     bytes, a smaller sample format buys proportionally more speech per call.
 *   - Completion arrives as RegisterWindowMessage("SVSyncMessages"), wParam
 *     1001, posted to the HWND passed to SVNarrate.
 *
 * Usage: svprobe [--dir D] [--fmt 0..3] [--group N] [--text "..."]
 *                [--personality N] [--trace]
 *   --fmt 0 = 11025/16 (0.74s per call)   1 = 8000/16 (1.02s)
 *         2 = 11025/8  (1.49s)            3 = 8000/8  (2.05s)
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SV_LANG_ENGLISH 0x00000001
#define SV_BITS_8       0x00400000
#define SV_BITS_16      0x00800000
#define SV_RATE_8000    0x02000000
#define SV_RATE_11025   0x04000000
#define SV_RATE_22050   0x08000000
#define SV_NARRATE_PURGE 0x00000020
#define WAVE_BUF_BYTES  0x4000  /* the engine's hard per-call ceiling */
#define FADE_MS         4

typedef DWORD SVHANDLE;
typedef int(WINAPI *PFN_Open)(SVHANDLE *, DWORD, DWORD, DWORD, DWORD);
typedef int(WINAPI *PFN_Close)(SVHANDLE);
typedef int(WINAPI *PFN_Narrate)(SVHANDLE, const char *, HWND, DWORD, DWORD);
typedef int(WINAPI *PFN_TTS)(SVHANDLE, const char *, DWORD, DWORD, DWORD, DWORD,
                             DWORD, DWORD);
typedef int(WINAPI *PFN_ErrText)(int, char *, WORD);
typedef int(WINAPI *PFN_Set1)(SVHANDLE, DWORD);
typedef int(WINAPI *PFN_Abort)(SVHANDLE); /* _SVAbort@4 takes only the handle */
/* _SVTextToPhon@24, from the call site inside SVTTS at 0x12bc4:
   (handle, text, phonBuf, bufSize, arg7, handle). Converts silently. */
typedef int(WINAPI *PFN_TextToPhon)(SVHANDLE, const char *, char *, DWORD,
                                    DWORD, SVHANDLE);
/* _SVRegister@20: builds "SOFTWARE\SoftVoice\\" + arg2, opens it under
   HKLM, reads the DWORD value named arg3, and hashes both into the
   registration state at ctx+0x516/0x51a/0x51e. Unregistered = every
   utterance clipped to one 16 KB buffer. */
typedef int(WINAPI *PFN_Register)(SVHANDLE, const char *, const char *, DWORD,
                                  DWORD);

static HMODULE g_dll;
static PFN_Open pOpen;
static PFN_Close pClose;
static PFN_Narrate pNarrate;
static PFN_TTS pTTS;
static PFN_ErrText pErrText;
static PFN_Abort pAbort;
static PFN_Set1 pSetPersonality, pSetRate, pSetPitch;
static PFN_TextToPhon pTextToPhon;
static PFN_Register pRegister;

static UINT g_syncMsg;
static volatile int g_done;
static int g_trace;
static int g_noAbort; /* SVAbort appears to latch the engine stop flag */

/* Engine output format for the current session. */
static DWORD g_rate = 11025, g_bits = 16;

/* Captured audio is always normalised to signed 16-bit, whatever the engine
   renders, so the rest of the pipeline (and NVDA) sees one format. */
static short *g_chunk;
static DWORD g_chunkSamples, g_chunkCap;
static short *g_asm;
static DWORD g_asmSamples, g_asmCap;
static DWORD g_engineBytes; /* raw bytes the engine emitted this call */
static int g_captureOnly;   /* 1 = swallow buffers, 0 = forward to device */
static HWAVEOUT g_hwo;      /* engine device handle, for muting */
static HWND g_engineCbWnd;  /* engine's waveOut callback window */

static MMRESULT(WINAPI *realWaveOutWrite)(HWAVEOUT, LPWAVEHDR, UINT);

static DWORD probeReadLicense(void)
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\SoftVoice\\ProdWorks", 0,
                      KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(k, "SV_KEY", NULL, &type, (LPBYTE)&v, &sz)
            != ERROR_SUCCESS || type != REG_DWORD)
        v = 0;
    RegCloseKey(k);
    return v;
}

static const char *errText(int code)
{
    static char buf[256];
    buf[0] = 0;
    if (pErrText)
        pErrText(code, buf, (WORD)sizeof(buf));
    return buf[0] ? buf : "(none)";
}

static void pushSamples(short **buf, DWORD *n, DWORD *cap, const short *src,
                        DWORD count)
{
    if (*n + count > *cap) {
        DWORD nc = *cap ? *cap : 65536;
        while (nc < *n + count)
            nc *= 2;
        *buf = (short *)realloc(*buf, nc * sizeof(short));
        *cap = nc;
    }
    memcpy(*buf + *n, src, count * sizeof(short));
    *n += count;
}

/* The engine renders into its own waveOut buffers and offers no callback, so
   we redirect its WINMM import. We deliberately do NOT forward to the device:
   marking the header done lets the engine's 50ms poll advance immediately, so
   synthesis runs far faster than real time. This is what the add-on host does. */
static MMRESULT WINAPI myWaveOutWrite(HWAVEOUT hwo, LPWAVEHDR hdr, UINT cb)
{
    if (hdr && hdr->lpData && hdr->dwBufferLength) {
        DWORD i, n;
        g_engineBytes += hdr->dwBufferLength;
        if (g_bits == 8) {
            unsigned char *p = (unsigned char *)hdr->lpData;
            n = hdr->dwBufferLength;
            for (i = 0; i < n; i++) {
                short v = (short)(((int)p[i] - 128) << 8);
                pushSamples(&g_chunk, &g_chunkSamples, &g_chunkCap, &v, 1);
            }
        } else {
            n = hdr->dwBufferLength / 2;
            pushSamples(&g_chunk, &g_chunkSamples, &g_chunkCap,
                        (short *)hdr->lpData, n);
        }
    }
    if (g_captureOnly) {
        if (hdr) {
            hdr->dwFlags |= WHDR_DONE;
            /* Marking the header done is not enough: the engine opens the
               device with CALLBACK_WINDOW and only queues the next buffer
               once it sees MM_WOM_DONE. Without this it emits exactly one
               16 KB buffer per call, which at 22050 Hz is 0.37 s and clips
               mid-word. The add-on host synthesises the same message. */
            if (g_engineCbWnd)
                PostMessageA(g_engineCbWnd, MM_WOM_DONE,
                             (WPARAM)(ULONG_PTR)hwo, (LPARAM)hdr);
        }
        return MMSYSERR_NOERROR;
    }
    return realWaveOutWrite ? realWaveOutWrite(hwo, hdr, cb) : MMSYSERR_NOERROR;
}

/* Grab the device handle so phoneme conversion can run silently. */
static MMRESULT(WINAPI *realWaveOutOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX,
                                         DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT WINAPI myWaveOutOpen(LPHWAVEOUT phwo, UINT dev,
                                     LPCWAVEFORMATEX fmt, DWORD_PTR cb,
                                     DWORD_PTR inst, DWORD flags)
{
    MMRESULT r = realWaveOutOpen(phwo, dev, fmt, cb, inst, flags);
    if (r == MMSYSERR_NOERROR && phwo && *phwo)
        g_hwo = *phwo;
    /* Remember where to send the synthetic MM_WOM_DONE. */
    if (r == MMSYSERR_NOERROR && (flags & 0x70000) == CALLBACK_WINDOW && cb)
        g_engineCbWnd = (HWND)(ULONG_PTR)cb;
    return r;
}

static void **findIatSlot(HMODULE mod, const char *dll, const char *fn)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader
                    .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                    .VirtualAddress;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!rva)
        return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (_stricmp((const char *)(base + imp->Name), dll) != 0)
            continue;
        oft = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk
                                               ? imp->OriginalFirstThunk
                                               : imp->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG32)
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, fn) == 0)
                return (void **)&ft->u1.Function;
        }
    }
    return NULL;
}

static int patchIat(const char *dll, const char *fn, void *repl, void **orig)
{
    DWORD old;
    void **slot = findIatSlot(g_dll, dll, fn);
    if (!slot || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return 0;
    *orig = *slot;
    *slot = repl;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_syncMsg) {
        if (g_trace)
            printf("      [notify] wParam=%u\n", (unsigned)w);
        g_done = 1;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Pump until the engine stops producing audio for idleMs, rather than until it
   claims completion - a streaming render keeps emitting after the first
   notification. */
static DWORD pumpQuiescent(DWORD maxMs, DWORD idleMs)
{
    DWORD start = GetTickCount(), lastChange = start, seen = g_engineBytes;
    MSG msg;
    for (;;) {
        DWORD now;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        now = GetTickCount();
        if (g_engineBytes != seen) {
            seen = g_engineBytes;
            lastChange = now;
        }
        if (now - lastChange > idleMs || now - start > maxMs)
            break;
        Sleep(2);
    }
    return g_engineBytes;
}

static int pump(DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    MSG msg;
    while (!g_done && GetTickCount() - start < timeoutMs) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(2);
    }
    return g_done;
}

/* Trim silence from both ends and fade both edges. Fading only the tail leaves
   a step at the next chunk's start, which is audible as a click. */
static DWORD trimChunk(short *s, DWORD n, DWORD *outStart)
{
    DWORD i, first = 0, last = 0;
    DWORD fade = (g_rate * FADE_MS) / 1000;
    *outStart = 0;
    for (i = 0; i < n; i++)
        if (abs(s[i]) > 150) {
            first = i;
            break;
        }
    for (i = n; i > 0; i--)
        if (abs(s[i - 1]) > 150) {
            last = i;
            break;
        }
    if (last <= first)
        return 0;
    first = (first > fade) ? first - fade : 0;
    if (last + fade <= n)
        last += fade;
    for (i = 0; i < fade && first + i < last; i++)
        s[first + i] = (short)(s[first + i] * (long)i / (long)fade);
    for (i = 0; i < fade && last - i > first; i++)
        s[last - 1 - i] = (short)(s[last - 1 - i] * (long)i / (long)fade);
    *outStart = first;
    return last - first;
}

/* text -> phoneme transcription, via SVTTS flag 0x80. */
static int textToPhonemes(SVHANDLE h, const char *text, char *out, DWORD outSz)
{
    static BYTE scratch[1 << 20];
    void *pbuf = scratch;
    DWORD size = sizeof(scratch);
    int rc;
    out[0] = 0;
    /* The previous render still owns the device; release it or this returns
       7014 "Speech device busy". But SVAbort also latches ctx+0x31e, which
       kills the refill loop, so it must be skippable. */
    if (pAbort && !g_noAbort)
        pAbort(h);
    g_captureOnly = 0;
    if (g_hwo)
        waveOutSetVolume(g_hwo, 0);
    rc = pTTS(h, text, (DWORD)(ULONG_PTR)&pbuf, (DWORD)(ULONG_PTR)&size, 0,
              0x80, 0, 0);
    if (g_hwo)
        waveOutSetVolume(g_hwo, 0xFFFFFFFF);
    if (rc != 0 || !pbuf || !size || size >= outSz)
        return rc ? rc : -1;
    memcpy(out, pbuf, size);
    out[size] = 0;
    return 0;
}

static DWORD renderPhonemes(SVHANDLE h, HWND hwnd, const char *phon,
                            int *truncated)
{
    DWORD start, keep;
    g_chunkSamples = 0;
    g_engineBytes = 0;
    g_done = 0;
    g_captureOnly = 1;
    if (pAbort && !g_noAbort)
        pAbort(h);
    if (pNarrate(h, phon, hwnd, SV_NARRATE_PURGE, 0) != 0)
        return 0;
    pump(5000);
    /* No trailing padding left means the call ran out of buffer mid-phrase. */
    keep = trimChunk(g_chunk, g_chunkSamples, &start);
    *truncated = (g_engineBytes >= WAVE_BUF_BYTES &&
                  keep * (g_bits / 8) + 256 >= g_engineBytes);
    if (keep)
        pushSamples(&g_asm, &g_asmSamples, &g_asmCap, g_chunk + start, keep);
    return keep;
}

static void writeWav(const char *path, const short *pcm, DWORD samples,
                     DWORD rate)
{
    BYTE hdr[44];
    DWORD bytes = samples * 2, n;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    memcpy(hdr, "RIFF\0\0\0\0WAVEfmt ", 16);
    *(DWORD *)(hdr + 16) = 16;
    *(WORD *)(hdr + 20) = 1;
    *(WORD *)(hdr + 22) = 1;
    *(DWORD *)(hdr + 24) = rate;
    *(DWORD *)(hdr + 28) = rate * 2;
    *(WORD *)(hdr + 32) = 2;
    *(WORD *)(hdr + 34) = 16;
    memcpy(hdr + 36, "data\0\0\0\0", 8);
    n = bytes + 36;
    memcpy(hdr + 4, &n, 4);
    memcpy(hdr + 40, &bytes, 4);
    fwrite(hdr, 1, 44, f);
    fwrite(pcm, 1, bytes, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    static const DWORD fRate[] = {SV_RATE_11025, SV_RATE_8000, SV_RATE_11025,
                                  SV_RATE_8000,  SV_RATE_22050};
    static const DWORD fBits[] = {SV_BITS_16, SV_BITS_16, SV_BITS_8, SV_BITS_8,
                                  SV_BITS_16};
    static const DWORD fHz[] = {11025, 8000, 11025, 8000, 22050};
    static const DWORD fDepth[] = {16, 16, 8, 8, 16};
    static const int fGroup[] = {1, 2, 3, 4, 1};

    const char *dir = "C:\\FTP";
    /* Matches HKLM\SOFTWARE\SoftVoice\ProdWorks : SV_SSIL from the VM. */
    const char *regKey = "ProdWorks";
    const char *regVal = "SV_SSIL";
    /* The registration argument pwspeech.dll passes at its own SVRegister
       call site (0x11019007). Without it the engine stays unregistered and
       silently clips every utterance to one 16 KB buffer. */
    /* Read from HKLM\SOFTWARE\SoftVoice\ProdWorks : SV_KEY, or pass
       --license <n>. Deliberately not compiled in. */
    DWORD license = 0;
    const char *text = "Hello. This is the SoftVoice speech synthesizer "
                       "running under Windows 11.";
    int fmt = 0, group = -1, personality = -1, flagSweep = 0, streamTest = 0;
    int pitch = -1;
    char dllPath[MAX_PATH], outPath[MAX_PATH];
    SVHANDLE h = 0;
    HWND hwnd;
    WNDCLASSA wc;
    char *words[512];
    char *textCopy;
    int nw = 0, i, rc, nCalls = 0, nTrunc = 0;
    DWORD flags;

    setvbuf(stdout, NULL, _IONBF, 0); /* so a hang still shows its last line */

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--fmt") && i + 1 < argc)
            fmt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--group") && i + 1 < argc)
            group = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--text") && i + 1 < argc)
            text = argv[++i];
        else if (!strcmp(argv[i], "--personality") && i + 1 < argc)
            personality = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc)
            pitch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--license") && i + 1 < argc)
            license = (DWORD)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace"))
            g_trace = 1;
        else if (!strcmp(argv[i], "--flagsweep"))
            flagSweep = 1;
        else if (!strcmp(argv[i], "--stream"))
            streamTest = 1;
        else if (!strcmp(argv[i], "--regkey") && i + 1 < argc)
            regKey = argv[++i];
        else if (!strcmp(argv[i], "--regval") && i + 1 < argc)
            regVal = argv[++i];
    }
    if (!license)
        license = probeReadLicense();
    if (fmt < 0 || fmt > 4)
        fmt = 0;
    if (group <= 0)
        group = fGroup[fmt];
    g_rate = fHz[fmt];
    g_bits = fDepth[fmt];

    printf("=== SoftVoice probe: fmt %d = %lu Hz / %lu-bit, %d word(s)/call ===\n",
           fmt, g_rate, g_bits, group);
    printf("per-call ceiling: %d bytes = %.2f s of buffer\n", WAVE_BUF_BYTES,
           WAVE_BUF_BYTES / (g_bits / 8.0) / g_rate);

    SetDllDirectoryA(dir);
    SetCurrentDirectoryA(dir);
    sprintf(dllPath, "%s\\SVctl32.DLL", dir);
    g_dll = LoadLibraryA(dllPath);
    if (!g_dll) {
        printf("FATAL: LoadLibrary(%s) failed (%lu)\n", dllPath,
               GetLastError());
        return 1;
    }
    pOpen = (PFN_Open)GetProcAddress(g_dll, "_SVOpenSpeech@20");
    pClose = (PFN_Close)GetProcAddress(g_dll, "_SVCloseSpeech@4");
    pNarrate = (PFN_Narrate)GetProcAddress(g_dll, "_SVNarrate@20");
    pTTS = (PFN_TTS)GetProcAddress(g_dll, "_SVTTS@32");
    pErrText = (PFN_ErrText)GetProcAddress(g_dll, "_SVGetErrorText@12");
    pAbort = (PFN_Abort)GetProcAddress(g_dll, "_SVAbort@4");
    pSetPersonality = (PFN_Set1)GetProcAddress(g_dll, "_SVSetPersonality@8");
    pSetRate = (PFN_Set1)GetProcAddress(g_dll, "_SVSetRate@8");
    pSetPitch = (PFN_Set1)GetProcAddress(g_dll, "_SVSetPitch@8");
    pTextToPhon = (PFN_TextToPhon)GetProcAddress(g_dll, "_SVTextToPhon@24");
    pRegister = (PFN_Register)GetProcAddress(g_dll, "_SVRegister@20");
    if (!pOpen || !pNarrate || !pTTS) {
        printf("FATAL: core exports missing\n");
        return 1;
    }
    patchIat("WINMM.dll", "waveOutWrite", (void *)myWaveOutWrite,
             (void **)&realWaveOutWrite);
    patchIat("WINMM.dll", "waveOutOpen", (void *)myWaveOutOpen,
             (void **)&realWaveOutOpen);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "SvProbeClass";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "SvProbeClass", "svprobe", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, wc.hInstance, NULL);
    g_syncMsg = RegisterWindowMessageA("SVSyncMessages");

    flags = SV_LANG_ENGLISH | fBits[fmt] | fRate[fmt];
    rc = pOpen(&h, 0, 0, flags, 0);
    if (rc != 0 || !h) {
        printf("FATAL: SVOpenSpeech(flags=0x%lx) rc=%d %s\n", flags, rc,
               errText(rc));
        return 2;
    }
    printf("opened: handle=0x%08lx\n", h);
    /* Without this the engine stays unregistered, and an unregistered engine
       silently clips every utterance to one 16 KB buffer. */
    if (pRegister) {
        int rr = pRegister(h, regKey, regVal, license, 0);
        printf("SVRegister(\"%s\", \"%s\", 0x%08lx) rc=%d %s\n", regKey, regVal,
               license, rr, rr ? errText(rr) : "OK");
    }
    if (personality >= 0 && pSetPersonality)
        printf("personality %d -> rc=%d\n", personality,
               pSetPersonality(h, (DWORD)personality));
    /* Must follow SVSetPersonality: the preset resets pitch to its own. */
    if (pitch >= 0 && pSetPitch)
        printf("pitch %d -> rc=%d\n", pitch, pSetPitch(h, (DWORD)pitch));

    /* pwWebSpeak speaks continuously with this same DLL, so the 16 KB ceiling
       must be a mode, not a limit. The engine double-buffers one 0x4000 block
       as two 8192-byte halves and refills them from its own message loop, so
       sweep SVNarrate's flags word for the bit that enables streaming.
       Forwarding to the real device here, so MM_WOM_DONE actually fires. */
    if (streamTest) {
        char phon[4096];
        DWORD got;
        g_noAbort = 1;
        /* SVTextToPhon converts without speaking, so the device stays free
           and we never need SVAbort (which latches the stop flag). */
        phon[0] = 0;
        rc = pTextToPhon ? pTextToPhon(h, text, phon, sizeof(phon), 0, h) : -1;
        printf("SVTextToPhon rc=%d, %d chars\n", rc, (int)strlen(phon));
        if (!phon[0]) {
            printf("SVTextToPhon produced nothing; falling back to SVTTS\n");
            if (textToPhonemes(h, text, phon, sizeof(phon)) != 0) {
                printf("phoneme conversion failed\n");
                return 3;
            }
        }
        printf("phonemes (%d chars): %s\n\n", (int)strlen(phon), phon);
        g_captureOnly = 0; /* real device so the refill timer sees drainage */
        if (g_hwo)
            waveOutSetVolume(g_hwo, 0xFFFFFFFF);
        g_engineBytes = 0;
        g_chunkSamples = 0;
        g_done = 0;
        /* SVTTS is what pwWebSpeak's pwspeech.dll actually imports, and the
           only call that ever produced more than one buffer here. */
        rc = pTTS(h, text, 0, 0, 0, 0, 0, 0);
        got = pumpQuiescent(40000, 2500);
        printf("SVTTS(text), no SVAbort: rc=%d  %lu bytes = %.2f s\n", rc, got,
               got / (g_bits / 8.0) / g_rate);
        printf("  (SVNarrate on the same phonemes gave 16384)\n");
        printf("%s\n",
               got > WAVE_BUF_BYTES
                   ? "*** CONTINUOUS - registration lifted the 16 KB cap ***"
                   : "still capped at one buffer (registration failed?)");
        if (g_chunkSamples) {
            char wp[64];
            sprintf(wp, "svprobe_stream_fmt%d.wav", fmt);
            writeWav(wp, g_chunk, g_chunkSamples, g_rate);
            printf("wrote %s (%.2f s) - playing:\n", wp,
                   g_chunkSamples / (double)g_rate);
            PlaySoundA(wp, NULL, SND_FILENAME | SND_SYNC);
            printf("playback done.\n");
        }
        if (pClose)
            pClose(h);
        FreeLibrary(g_dll);
        return 0;
    }

    if (flagSweep) {
        static const DWORD cand[] = {0x0,   0x1,   0x2,   0x4,  0x8,   0x10,
                                     0x20,  0x40,  0x80,  0x100, 0x200, 0x400,
                                     0x21,  0x22,  0x24,  0x30};
        char phon[4096];
        int c;
        if (textToPhonemes(h, text, phon, sizeof(phon)) != 0) {
            printf("phoneme conversion failed\n");
            return 3;
        }
        printf("phonemes (%d chars): %s\n\n", (int)strlen(phon), phon);
        printf("baseline ceiling is %d bytes; anything larger means streaming\n",
               WAVE_BUF_BYTES);
        for (c = 0; c < (int)(sizeof(cand) / sizeof(cand[0])); c++) {
            DWORD got;
            if (pAbort)
                pAbort(h);
            g_captureOnly = 0; /* real device, so the engine gets MM_WOM_DONE */
            if (g_hwo)
                waveOutSetVolume(g_hwo, 0); /* keep the sweep silent */
            g_engineBytes = 0;
            g_chunkSamples = 0;
            g_done = 0;
            rc = pNarrate(h, phon, hwnd, cand[c], 0);
            got = pumpQuiescent(15000, 1200);
            printf("  flags 0x%-5lx rc=%-5d %7lu bytes (%.2f s)%s\n", cand[c],
                   rc, got, got / (g_bits / 8.0) / g_rate,
                   got > WAVE_BUF_BYTES ? "   *** STREAMS ***" : "");
        }
        if (pClose)
            pClose(h);
        FreeLibrary(g_dll);
        return 0;
    }

    /* Split the input TEXT into words. Converting a small group of words at a
       time keeps every SVTTS(0x80) call short, which sidesteps the engine
       returning only its first phoneme chunk for long inputs. */
    textCopy = _strdup(text);
    {
        char *p = textCopy;
        while (*p && nw < 512) {
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                break;
            words[nw++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
                *p++ = 0;
        }
    }
    printf("input: %d words\n\n", nw);

    for (i = 0; i < nw; i += group) {
        char src[512], phon[2048];
        int k, take = (i + group <= nw) ? group : (nw - i);
        int trunc = 0;
        DWORD got;
        src[0] = 0;
        for (k = 0; k < take; k++) {
            if (k)
                strcat(src, " ");
            strcat(src, words[i + k]);
        }
        rc = textToPhonemes(h, src, phon, sizeof(phon));
        if (rc != 0) {
            printf("  [%2d] \"%s\" -> phoneme conversion failed rc=%d %s\n",
                   nCalls + 1, src, rc, errText(rc));
            nCalls++;
            continue;
        }
        got = renderPhonemes(h, hwnd, phon, &trunc);
        printf("  [%2d] \"%s\"\n       phonemes: %s\n"
               "       %lu samples (%.2f s)%s\n",
               nCalls + 1, src, phon, got, got / (double)g_rate,
               trunc ? "   *** TRUNCATED - reduce --group ***" : "");
        if (trunc)
            nTrunc++;
        nCalls++;
    }

    printf("\n%d calls, %lu samples total = %.2f s at %lu Hz\n", nCalls,
           g_asmSamples, g_asmSamples / (double)g_rate, g_rate);
    if (nTrunc)
        printf("WARNING: %d call(s) truncated; try --group %d\n", nTrunc,
               group > 1 ? group - 1 : 1);

    if (g_asmSamples) {
        /* Include group in the name: a --group experiment must never clobber
           the reference render for that format. */
        sprintf(outPath, "svprobe_fmt%d_g%d.wav", fmt, group);
        writeWav(outPath, g_asm, g_asmSamples, g_rate);
        printf("wrote %s\\%s (16-bit, %lu Hz) - playing:\n", dir, outPath,
               g_rate);
        PlaySoundA(outPath, NULL, SND_FILENAME | SND_SYNC);
        printf("playback done.\n");
    }

    if (pClose)
        pClose(h);
    FreeLibrary(g_dll);
    free(textCopy);
    return 0;
}
