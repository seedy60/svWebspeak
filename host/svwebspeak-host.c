/*
 * svwebspeak-host.exe - 32-bit host for the SoftVoice SVCTL32 engine.
 *
 * NVDA is 64-bit and cannot load this 1997 i386 DLL in-process, so the driver
 * launches this host and talks to it over a loopback socket.
 *
 * Everything here rests on findings recovered by disassembly:
 *   - SVOpenSpeech flags: low 7 bits = language, 0x400000/0x800000 = 8/16-bit,
 *     0x2000000/0x4000000/0x8000000 = 8000/11025/22050 Hz.
 *   - SVRegister() MUST be called or the engine silently clips every utterance
 *     to one 16 KB buffer. It needs HKLM\SOFTWARE\SoftVoice\ProdWorks with
 *     an SV_SSIL value, plus a registration number which is read at runtime
 *     from SV_KEY under the same registry key - it is deliberately not
 *     compiled in, so it never ends up in source control.
 *   - The engine renders to its own waveOut device; we redirect waveOutWrite
 *     to capture PCM and mark the header done so it runs faster than realtime.
 *
 * Wire format (both directions): u32 length, then that many bytes.
 *   command  (driver->host): u8 1, u32 msgId, u16 cmd, args...
 *   response (host->driver): u8 2, u32 msgId, u32 status, data...
 *   event    (host->driver): u8 3, u16 event, data...
 */

#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

#define SV_LANG_ENGLISH 0x00000001
#define SV_LANG_SPANISH 0x00000002
#define SV_BITS_8       0x00400000
#define SV_BITS_16      0x00800000
#define SV_RATE_8000    0x02000000
#define SV_RATE_11025   0x04000000
#define SV_RATE_22050   0x08000000

#define CMD_SPEAK    1
#define CMD_STOP     2
#define CMD_PARAM    3
#define CMD_SHUTDOWN 4
#define CMD_VOICES   5

#define EVT_AUDIO 1
#define EVT_DONE  2

#define P_RATE        1
#define P_PITCH       2
#define P_VOLUME      3
#define P_PERSONALITY 4
#define P_INFLECTION  5
#define P_LANGUAGE    6

typedef DWORD SVHANDLE;
typedef int(WINAPI *PFN_Open)(SVHANDLE *, DWORD, DWORD, DWORD, DWORD);
typedef int(WINAPI *PFN_Close)(SVHANDLE);
typedef int(WINAPI *PFN_TTS)(SVHANDLE, const char *, DWORD, DWORD, DWORD, DWORD,
                             DWORD, DWORD);
typedef int(WINAPI *PFN_Reg)(SVHANDLE, const char *, const char *, DWORD, DWORD);
typedef int(WINAPI *PFN_Set1)(SVHANDLE, DWORD);
typedef int(WINAPI *PFN_Abort)(SVHANDLE);
typedef int(WINAPI *PFN_ErrText)(int, char *, WORD);
typedef int(WINAPI *PFN_GetLangs)(SVHANDLE, DWORD *);

static HMODULE g_dll;
static PFN_Open pOpen;
static PFN_Close pClose;
static PFN_TTS pTTS;
static PFN_Reg pRegister;
static PFN_Abort pAbort;
static PFN_ErrText pErrText;
static PFN_Set1 pSetRate, pSetPitch, pSetVolume, pSetPersonality, pSetF0Range;
static PFN_Set1 pSetLanguage;
static PFN_GetLangs pGetLanguages;
/* Bitmask of languages that actually loaded: 1 English, 2 Spanish, 4 German. */
static DWORD g_langs;

static SOCKET g_sock = INVALID_SOCKET;
static SVHANDLE g_h;
static HWND g_wnd;
static UINT g_syncMsg;
static DWORD g_rate = 22050, g_bits = 16;
static volatile int g_speaking, g_stopReq;
/* Utterance sequence. Audio and DONE carry it so the driver can discard
   anything belonging to an utterance it has already cancelled - a plain
   "cancelled" flag races with the next speak clearing it. */
static volatile DWORD g_seq;   /* cancellation epoch */
static volatile DWORD g_utt;   /* per-utterance id, echoed in DONE */

/* PCM captured from the engine, always normalised to signed 16-bit. */
static short *g_pcm;
static DWORD g_pcmLen, g_pcmCap, g_pcmSent;
static DWORD g_engineBytes;
static CRITICAL_SECTION g_lock;

static MMRESULT(WINAPI *realWaveOutWrite)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT(WINAPI *realWaveOutOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX,
                                         DWORD_PTR, DWORD_PTR, DWORD);
/* The engine refills its double buffer from the MM_WOM_DONE its callback
   window receives when the device drains a header. We never submit to the
   device, so we must post that completion ourselves - otherwise the engine
   stops after the two primed buffers and the utterance truncates. */
static HWND g_engineCbWnd;

static MMRESULT WINAPI myWaveOutOpen(LPHWAVEOUT phwo, UINT dev,
                                     LPCWAVEFORMATEX fmt, DWORD_PTR cb,
                                     DWORD_PTR inst, DWORD flags)
{
    MMRESULT r = realWaveOutOpen(phwo, dev, fmt, cb, inst, flags);
    if (r == MMSYSERR_NOERROR && (flags & 0x70000) == CALLBACK_WINDOW && cb)
        g_engineCbWnd = (HWND)(ULONG_PTR)cb;
    return r;
}

/* Registration number, read at runtime rather than compiled in. A 32-bit
   process is redirected to WOW6432Node, which is where it must live. */
static DWORD readLicense(void)
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
    return buf[0] ? buf : "?";
}

/* ---------------------------------------------------------------- capture */

static void pcmPush(const short *s, DWORD n)
{
    EnterCriticalSection(&g_lock);
    if (g_pcmLen + n > g_pcmCap) {
        DWORD nc = g_pcmCap ? g_pcmCap : 65536;
        while (nc < g_pcmLen + n)
            nc *= 2;
        g_pcm = (short *)realloc(g_pcm, nc * sizeof(short));
        g_pcmCap = nc;
    }
    memcpy(g_pcm + g_pcmLen, s, n * sizeof(short));
    g_pcmLen += n;
    LeaveCriticalSection(&g_lock);
}

static MMRESULT WINAPI myWaveOutWrite(HWAVEOUT hwo, LPWAVEHDR hdr, UINT cb)
{
    (void)hwo;
    (void)cb;
    if (hdr && hdr->lpData && hdr->dwBufferLength) {
        g_engineBytes += hdr->dwBufferLength;
        if (g_bits == 8) {
            unsigned char *p = (unsigned char *)hdr->lpData;
            DWORD i, n = hdr->dwBufferLength;
            short *tmp = (short *)malloc(n * sizeof(short));
            for (i = 0; i < n; i++)
                tmp[i] = (short)(((int)p[i] - 128) << 8);
            pcmPush(tmp, n);
            free(tmp);
        } else {
            pcmPush((short *)hdr->lpData, hdr->dwBufferLength / 2);
        }
    }
    /* Never reach the sound card: NVDA owns playback. Mark the header done and
       synthesise the device completion so the engine refills immediately,
       which also makes synthesis far faster than real time. */
    if (hdr) {
        hdr->dwFlags |= WHDR_DONE;
        if (g_engineCbWnd)
            PostMessageA(g_engineCbWnd, MM_WOM_DONE, (WPARAM)(ULONG_PTR)hwo,
                         (LPARAM)hdr);
    }
    return MMSYSERR_NOERROR;
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

/* ------------------------------------------------------------------ wire  */

static int sendAll(const char *p, int n)
{
    while (n > 0) {
        int k = send(g_sock, p, n, 0);
        if (k <= 0)
            return 0;
        p += k;
        n -= k;
    }
    return 1;
}

/*
 * Outbound frames go through a writer thread. The main thread owns the
 * engine's TOP-LEVEL window, so it must never block: the driver renders far
 * faster than realtime and NVDA's WavePlayer applies backpressure, so a
 * blocking send() here freezes that window and stalls system-wide message
 * broadcasts (measured: 10 s HWND_BROADCAST latency, hung window).
 */
typedef struct Out {
    struct Out *next;
    char *data;
    DWORD len;
} Out;

static Out *g_outHead, *g_outTail;
static CRITICAL_SECTION g_outLock;
static HANDLE g_outEvent;

static int sendFrame(const void *payload, DWORD len)
{
    Out *o = (Out *)malloc(sizeof(Out));
    if (!o)
        return 0;
    o->data = (char *)malloc(len + 4);
    if (!o->data) {
        free(o);
        return 0;
    }
    memcpy(o->data, &len, 4);
    memcpy(o->data + 4, payload, len);
    o->len = len + 4;
    o->next = NULL;
    EnterCriticalSection(&g_outLock);
    if (g_outTail)
        g_outTail->next = o;
    else
        g_outHead = o;
    g_outTail = o;
    LeaveCriticalSection(&g_outLock);
    SetEvent(g_outEvent);
    return 1;
}

static DWORD WINAPI writerThread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Out *o;
        WaitForSingleObject(g_outEvent, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_outLock);
            o = g_outHead;
            if (o) {
                g_outHead = o->next;
                if (!g_outHead)
                    g_outTail = NULL;
            }
            LeaveCriticalSection(&g_outLock);
            if (!o)
                break;
            /* Blocking here is fine: this thread owns no windows. */
            sendAll(o->data, (int)o->len);
            free(o->data);
            free(o);
        }
    }
    return 0;
}

static void sendResponse(DWORD msgId, DWORD status, const void *data, DWORD n)
{
    char *buf = (char *)malloc(9 + n);
    buf[0] = 2;
    memcpy(buf + 1, &msgId, 4);
    memcpy(buf + 5, &status, 4);
    if (n)
        memcpy(buf + 9, data, n);
    sendFrame(buf, 9 + n);
    free(buf);
}

static void sendEvent(WORD evt, const void *data, DWORD n)
{
    char *buf = (char *)malloc(3 + n);
    buf[0] = 3;
    memcpy(buf + 1, &evt, 2);
    if (n)
        memcpy(buf + 3, data, n);
    sendFrame(buf, 3 + n);
    free(buf);
}

/* Ship whatever new PCM the engine has produced. */
static void flushAudio(void)
{
    DWORD have, n;
    short *copy = NULL;
    EnterCriticalSection(&g_lock);
    have = g_pcmLen - g_pcmSent;
    if (have) {
        copy = (short *)malloc(have * sizeof(short));
        memcpy(copy, g_pcm + g_pcmSent, have * sizeof(short));
        g_pcmSent = g_pcmLen;
    }
    LeaveCriticalSection(&g_lock);
    if (!have)
        return;
    n = have * sizeof(short);
    {
        char *buf = (char *)malloc(8 + n);
        DWORD seq = g_seq;
        memcpy(buf, &seq, 4);
        memcpy(buf + 4, &n, 4);
        memcpy(buf + 8, copy, n);
        sendEvent(EVT_AUDIO, buf, 8 + n);
        free(buf);
    }
    free(copy);
}

/* ----------------------------------------------------------------- engine */

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_syncMsg)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static void pumpMessages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static int engineInit(const char *dir, DWORD rate, DWORD bits)
{
    char path[MAX_PATH];
    WNDCLASSA wc;
    DWORD flags;
    int rc;

    SetDllDirectoryA(dir);
    SetCurrentDirectoryA(dir);
    sprintf(path, "%s\\SVctl32.DLL", dir);
    g_dll = LoadLibraryA(path);
    if (!g_dll)
        return -1;

    pOpen = (PFN_Open)GetProcAddress(g_dll, "_SVOpenSpeech@20");
    pClose = (PFN_Close)GetProcAddress(g_dll, "_SVCloseSpeech@4");
    pTTS = (PFN_TTS)GetProcAddress(g_dll, "_SVTTS@32");
    pRegister = (PFN_Reg)GetProcAddress(g_dll, "_SVRegister@20");
    pAbort = (PFN_Abort)GetProcAddress(g_dll, "_SVAbort@4");
    pErrText = (PFN_ErrText)GetProcAddress(g_dll, "_SVGetErrorText@12");
    pSetRate = (PFN_Set1)GetProcAddress(g_dll, "_SVSetRate@8");
    pSetPitch = (PFN_Set1)GetProcAddress(g_dll, "_SVSetPitch@8");
    pSetVolume = (PFN_Set1)GetProcAddress(g_dll, "_SVSetVolume@8");
    pSetPersonality = (PFN_Set1)GetProcAddress(g_dll, "_SVSetPersonality@8");
    pSetF0Range = (PFN_Set1)GetProcAddress(g_dll, "_SVSetF0Range@8");
    pSetLanguage = (PFN_Set1)GetProcAddress(g_dll, "_SVSetLanguage@8");
    pGetLanguages = (PFN_GetLangs)GetProcAddress(
        g_dll, "_SVGetAvailableLanguages@8");
    if (!pOpen || !pTTS || !pRegister)
        return -2;

    patchIat("WINMM.dll", "waveOutWrite", (void *)myWaveOutWrite,
             (void **)&realWaveOutWrite);
    patchIat("WINMM.dll", "waveOutOpen", (void *)myWaveOutOpen,
             (void **)&realWaveOutOpen);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "SvWebspeakHostWnd";
    RegisterClassA(&wc);
    g_wnd = CreateWindowExA(0, "SvWebspeakHostWnd", "svwebspeak-host", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, NULL, wc.hInstance, NULL);
    g_syncMsg = RegisterWindowMessageA("SVSyncMessages");

    flags = SV_LANG_ENGLISH | SV_LANG_SPANISH;
    flags |= (bits == 8) ? SV_BITS_8 : SV_BITS_16;
    flags |= (rate == 8000) ? SV_RATE_8000
                            : (rate == 22050 ? SV_RATE_22050 : SV_RATE_11025);
    rc = pOpen(&g_h, 0, 0, flags, 0);
    if (rc != 0 || !g_h)
        return rc ? rc : -3;

    /* Non-negotiable: without this the engine truncates at 16 KB. */
    {
        DWORD lic = readLicense();
        if (!lic)
            return -4; /* SV_KEY missing; the driver explains how to add it */
        rc = pRegister(g_h, "ProdWorks", "SV_SSIL", lic, 0);
    }
    if (rc != 0)
        return rc;

    /* Which languages actually loaded - Spanish only if Svspan32.dll is
       present. SVSetLanguage refuses anything not loaded here. */
    g_langs = SV_LANG_ENGLISH;
    if (pGetLanguages)
        pGetLanguages(g_h, &g_langs);

    g_rate = rate;
    g_bits = bits;
    return 0;
}

/* Render one utterance, streaming audio out as it appears. */
typedef struct Cmd {
    struct Cmd *next;
    DWORD msgId;
    WORD cmd;
    char *data;
    DWORD len;
} Cmd;

#define WM_SV_COMMAND (WM_APP + 1)

static Cmd *g_qHead, *g_qTail;
static CRITICAL_SECTION g_qLock;

static Cmd *queuePop(void);
static void stopSpeech(void);
static int setParam(WORD p, int v);

/* Commands arriving mid-utterance. speak() pumps messages itself, which
   consumes the WM_SV_COMMAND wake-up before the main loop can see it, so the
   queue must be serviced here or CMD_STOP is never acted on and speech cannot
   be interrupted. Anything that is not a stop is deferred until the current
   utterance ends. */
static Cmd *g_defHead, *g_defTail;

static void deferCommand(Cmd *c)
{
    c->next = NULL;
    if (g_defTail)
        g_defTail->next = c;
    else
        g_defHead = c;
    g_defTail = c;
}

/* Put deferred commands back at the FRONT of the queue: they arrived before
   anything still waiting, so they must keep their order. */
static void requeueDeferred(void)
{
    if (!g_defHead)
        return;
    EnterCriticalSection(&g_qLock);
    g_defTail->next = g_qHead;
    g_qHead = g_defHead;
    if (!g_qTail)
        g_qTail = g_defTail;
    LeaveCriticalSection(&g_qLock);
    g_defHead = g_defTail = NULL;
    PostMessageA(g_wnd, WM_SV_COMMAND, 0, 0);
}

static void serviceQueueWhileSpeaking(void)
{
    Cmd *c;
    while ((c = queuePop()) != NULL) {
        if (c->cmd == CMD_STOP) {
            stopSpeech();
            sendResponse(c->msgId, 0, NULL, 0);
            free(c->data);
            free(c);
        } else if (c->cmd == CMD_PARAM) {
            WORD p;
            int v, prc;
            memcpy(&p, c->data + 7, 2);
            memcpy(&v, c->data + 9, 4);
            prc = setParam(p, v);
            sendResponse(c->msgId, (DWORD)prc, NULL, 0);
            free(c->data);
            free(c);
        } else {
            /* NVDA queues utterances with speak() and only interrupts with
               cancel(). Aborting here would drop queued speech and suppress
               its DONE, leaving NVDA waiting on completions that never come. */
            deferCommand(c);
        }
    }
}

static void speak(const char *text, DWORD seq, DWORD utt)
{
    DWORD idle, lastChange, seen;

    g_seq = seq;
    g_utt = utt;

    EnterCriticalSection(&g_lock);
    g_pcmLen = g_pcmSent = 0;
    LeaveCriticalSection(&g_lock);
    g_engineBytes = 0;
    g_stopReq = 0;
    g_speaking = 1;

    pTTS(g_h, text, 0, 0, 0, 0, 0, 0);

    seen = g_engineBytes;
    lastChange = GetTickCount();
    for (;;) {
        pumpMessages();
        serviceQueueWhileSpeaking();
        /* Check the stop BEFORE flushing, or a cancel still ships the whole
           utterance that was already rendered into the buffer. */
        if (g_stopReq)
            break;
        flushAudio();
        if (g_engineBytes != seen) {
            seen = g_engineBytes;
            lastChange = GetTickCount();
        }
        idle = GetTickCount() - lastChange;
        if (idle > 400) /* engine has stopped producing */
            break;
        Sleep(2);
    }
    if (g_stopReq) {
        /* Discard whatever was rendered but not yet sent, and stay silent:
           NVDA already knows it cancelled, and a DONE here would make say-all
           advance to the next item. */
        EnterCriticalSection(&g_lock);
        g_pcmLen = g_pcmSent = 0;
        LeaveCriticalSection(&g_lock);
        g_speaking = 0;
        requeueDeferred();
        return;
    }
    flushAudio();
    g_speaking = 0;
    {
        DWORD fin[2];
        fin[0] = g_seq;
        fin[1] = g_utt;
        sendEvent(EVT_DONE, fin, 8);
    }
    requeueDeferred();
}

static void stopSpeech(void)
{
    g_stopReq = 1;
    if (pAbort && g_h)
        pAbort(g_h);
    EnterCriticalSection(&g_lock);
    g_pcmLen = g_pcmSent = 0;
    LeaveCriticalSection(&g_lock);
}

static int setParam(WORD p, int v)
{
    int rc = -1;
    switch (p) {
    case P_RATE:
        if (pSetRate)
            rc = pSetRate(g_h, (DWORD)v);
        break;
    case P_PITCH:
        if (pSetPitch)
            rc = pSetPitch(g_h, (DWORD)v);
        break;
    case P_VOLUME:
        if (pSetVolume)
            rc = pSetVolume(g_h, (DWORD)v);
        break;
    case P_PERSONALITY:
        if (pSetPersonality)
            rc = pSetPersonality(g_h, (DWORD)v);
        break;
    case P_INFLECTION:
        if (pSetF0Range)
            rc = pSetF0Range(g_h, (DWORD)v);
        break;
    case P_LANGUAGE:
        if (pSetLanguage)
            rc = pSetLanguage(g_h, (DWORD)v);
        break;
    }
    return rc;
}

/* ------------------------------------------------------------------ main  */
/*
 * SVOpenSpeech creates a TOP-LEVEL window (CreateWindowExA with
 * hWndParent = NULL, verified by disassembly at 0x10e02). Top-level windows
 * receive system broadcasts, so if this process ever stops pumping messages
 * every SendMessage(HWND_BROADCAST, ...) in the whole session blocks until it
 * times out - which shows up as misdirected mouse clicks, modifier keys that
 * appear stuck, and Explorer jumping to the desktop.
 *
 * Therefore the main thread does nothing but pump messages. Socket reads
 * happen on a worker thread, which hands commands over via a queue and wakes
 * the main thread with a posted message. Engine calls stay on the main thread
 * because that is the thread that owns the engine's window.
 */



static void queuePush(Cmd *c)
{
    c->next = NULL;
    EnterCriticalSection(&g_qLock);
    if (g_qTail)
        g_qTail->next = c;
    else
        g_qHead = c;
    g_qTail = c;
    LeaveCriticalSection(&g_qLock);
    PostMessageA(g_wnd, WM_SV_COMMAND, 0, 0);
}

static Cmd *queuePop(void)
{
    Cmd *c;
    EnterCriticalSection(&g_qLock);
    c = g_qHead;
    if (c) {
        g_qHead = c->next;
        if (!g_qHead)
            g_qTail = NULL;
    }
    LeaveCriticalSection(&g_qLock);
    return c;
}

static int recvExact(char *p, int n)
{
    while (n > 0) {
        int k = recv(g_sock, p, n, 0);
        if (k <= 0)
            return 0;
        p += k;
        n -= k;
    }
    return 1;
}

static DWORD WINAPI readerThread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        DWORD len;
        char *buf;
        Cmd *c;
        if (!recvExact((char *)&len, 4) || len < 7 || len > (16u << 20))
            break;
        buf = (char *)malloc(len);
        if (!buf)
            break;
        if (!recvExact(buf, (int)len)) {
            free(buf);
            break;
        }
        c = (Cmd *)malloc(sizeof(Cmd));
        memcpy(&c->msgId, buf + 1, 4);
        memcpy(&c->cmd, buf + 5, 2);
        c->data = buf;
        c->len = len;
        /* Flag the stop for the main loop. Do NOT call into the engine from
           this thread: SVCTL32 is a single-threaded 1997 DLL and calling
           SVAbort here while the main thread is inside SVTTS crashes it with
           an access violation within a few rapid cancel/speak cycles. The
           main thread performs the abort in serviceQueueWhileSpeaking(). */
        if (c->cmd == CMD_STOP && g_speaking)
            g_stopReq = 1;
        queuePush(c);
    }
    /* Connection gone: ask the message loop to exit. */
    PostMessageA(g_wnd, WM_SV_COMMAND, 1, 0);
    return 0;
}

static int handleCommand(Cmd *c)
{
    switch (c->cmd) {
    case CMD_SPEAK: {
        DWORD seq, utt, tl;
        char *text;
        memcpy(&seq, c->data + 7, 4);
        memcpy(&utt, c->data + 11, 4);
        memcpy(&tl, c->data + 15, 4);
        text = (char *)malloc(tl + 1);
        memcpy(text, c->data + 19, tl);
        text[tl] = 0;
        sendResponse(c->msgId, 0, NULL, 0);
        speak(text, seq, utt);
        free(text);
        break;
    }
    case CMD_STOP:
        stopSpeech();
        sendResponse(c->msgId, 0, NULL, 0);
        break;
    case CMD_PARAM: {
        WORD p;
        int v, prc;
        memcpy(&p, c->data + 7, 2);
        memcpy(&v, c->data + 9, 4);
        prc = setParam(p, v);
        sendResponse(c->msgId, (DWORD)prc, NULL, 0);
        break;
    }
    case CMD_SHUTDOWN:
        sendResponse(c->msgId, 0, NULL, 0);
        return 0;
    default:
        sendResponse(c->msgId, 1, NULL, 0);
        break;
    }
    return 1;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    const char *addr = NULL, *dir = NULL;
    DWORD rate = 22050, bits = 16;
    char host[64];
    int port = 0, i, rc;
    struct sockaddr_in sa;
    MSG msg;
    HANDLE th;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--address") && i + 1 < argc)
            addr = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc)
            rate = (DWORD)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bits") && i + 1 < argc)
            bits = (DWORD)atoi(argv[++i]);
    }
    if (!addr || !dir)
        return 1;
    {
        const char *colon = strrchr(addr, ':');
        if (!colon)
            return 1;
        lstrcpynA(host, addr, (int)(colon - addr) + 1);
        port = atoi(colon + 1);
    }

    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_qLock);
    InitializeCriticalSection(&g_outLock);
    g_outEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr(host);
    sa.sin_port = htons((u_short)port);
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        return 2;

    /* Start the writer before anything is sent: sendFrame() only enqueues,
       so a frame written before this thread exists is never delivered. */
    th = CreateThread(NULL, 0, writerThread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);

    rc = engineInit(dir, rate, bits);
    {
        char init[16];
        DWORD r = g_rate, b = g_bits, l = g_langs;
        memcpy(init, &rc, 4);
        memcpy(init + 4, &r, 4);
        memcpy(init + 8, &b, 4);
        memcpy(init + 12, &l, 4);
        sendResponse(0, (DWORD)rc, init, 16);
    }
    if (rc != 0) {
        Sleep(300); /* let the writer flush the init response */
        return 3;
    }

    th = CreateThread(NULL, 0, readerThread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);

    /* Pump forever. Never block this thread on anything else. */
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_SV_COMMAND) {
            Cmd *c;
            if (msg.wParam == 1)
                break; /* peer disconnected */
            while ((c = queuePop()) != NULL) {
                int keepGoing = handleCommand(c);
                free(c->data);
                free(c);
                if (!keepGoing)
                    goto done;
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
done:
    if (pClose && g_h)
        pClose(g_h);
    if (g_dll)
        FreeLibrary(g_dll);
    closesocket(g_sock);
    WSACleanup();
    return 0;
}
