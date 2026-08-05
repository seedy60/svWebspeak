/*
 * svindex.c - diagnostic for SoftVoice's word-index scheduler.
 *
 * SVOpenSpeech stores its arg2 at ctx[0]. The periodic timer callback at
 * 0x12850 walks a table at ctx+0xa4 and does:
 *
 *     PostMessageA(ctx[0], ctx[0x20], entry.word@+0xc, &entry+4)
 *
 * so passing a real HWND as arg2 should make the engine report word positions.
 * This tool passes one, intercepts USER32!PostMessageA out of SVCTL32's import
 * table, and prints every post alongside how much audio had been rendered at
 * that moment - which is what maps an index onto a position in the PCM.
 *
 * Build: tools\build-index.cmd     Run: svindex.exe [dir] ["text"]
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SV_LANG_ENGLISH 0x00000001
#define SV_BITS_16      0x00800000
#define SV_RATE_22050   0x08000000

typedef DWORD SVHANDLE;
typedef int(WINAPI *PFN_Open)(SVHANDLE *, DWORD, DWORD, DWORD, DWORD);
typedef int(WINAPI *PFN_Close)(SVHANDLE);
typedef int(WINAPI *PFN_TTS)(SVHANDLE, const char *, DWORD, DWORD, DWORD, DWORD,
                             DWORD, DWORD);
typedef int(WINAPI *PFN_Reg)(SVHANDLE, const char *, const char *, DWORD, DWORD);
typedef int(WINAPI *PFN_ErrText)(int, char *, WORD);

static HMODULE g_dll;
static PFN_Open pOpen;
static PFN_Close pClose;
static PFN_TTS pTTS;
static PFN_Reg pRegister;
static PFN_ErrText pErrText;

static HWND g_wnd;
static DWORD g_audioBytes;
static UINT g_syncMsg;
static SVHANDLE g_h;

static BOOL(WINAPI *realPostMessageA)(HWND, UINT, WPARAM, LPARAM);
static MMRESULT(WINAPI *realWaveOutWrite)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT(WINAPI *realWaveOutOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX,
                                         DWORD_PTR, DWORD_PTR, DWORD);
static HWND g_engineCbWnd;
static int g_posts;

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

static BOOL WINAPI myPostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    /* MM_WOM_DONE is our own synthetic completion; ignore it here. */
    if (msg != MM_WOM_DONE) {
        printf("  POST hwnd=%p msg=0x%04x wParam=%-6u lParam=0x%08lx"
               "  audio=%lu B (%.3f s)\n",
               (void *)h, msg, (unsigned)wp, (unsigned long)lp, g_audioBytes,
               g_audioBytes / 2.0 / 22050.0);
        if (lp) {
            DWORD *p = (DWORD *)(lp - 4); /* lParam = &entry + 4 */
            printf("        entry: +0=%08lx +4=%08lx +8=%08lx(t) "
                   "+c=%08lx(w) +10=%08lx\n",
                   p[0], p[1], p[2], p[3], p[4]);
        }
        g_posts++;
    }
    return realPostMessageA(h, msg, wp, lp);
}

static MMRESULT WINAPI myWaveOutOpen(LPHWAVEOUT phwo, UINT dev,
                                     LPCWAVEFORMATEX fmt, DWORD_PTR cb,
                                     DWORD_PTR inst, DWORD flags)
{
    MMRESULT r = realWaveOutOpen(phwo, dev, fmt, cb, inst, flags);
    if (r == MMSYSERR_NOERROR && (flags & 0x70000) == CALLBACK_WINDOW && cb)
        g_engineCbWnd = (HWND)(ULONG_PTR)cb;
    return r;
}

static MMRESULT WINAPI myWaveOutWrite(HWAVEOUT hwo, LPWAVEHDR hdr, UINT cb)
{
    (void)cb;
    if (hdr && hdr->dwBufferLength)
        g_audioBytes += hdr->dwBufferLength;
    if (hdr) {
        hdr->dwFlags |= WHDR_DONE;
        if (g_engineCbWnd)
            realPostMessageA(g_engineCbWnd, MM_WOM_DONE,
                             (WPARAM)(ULONG_PTR)hwo, (LPARAM)hdr);
    }
    return MMSYSERR_NOERROR;
}

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

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m >= WM_APP || m == g_syncMsg)
        printf("  WNDPROC msg=0x%04x wParam=%u lParam=0x%08lx\n",
               m, (unsigned)w, (unsigned long)l);
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "C:\\FTP";
    const char *text = argc > 2 ? argv[2]
                                : "One two three four five six seven eight.";
    char path[MAX_PATH];
    WNDCLASSA wc;
    DWORD lic, flags, start;
    DWORD licOverride = 0;
    int rc, ai;
    MSG msg;

    /* --license <n> overrides the registry value so the unregistered
       behaviour can be demonstrated without editing HKLM. */
    for (ai = 1; ai < argc - 1; ai++)
        if (strcmp(argv[ai], "--license") == 0)
            licOverride = (DWORD)strtoul(argv[ai + 1], NULL, 0);

    setvbuf(stdout, NULL, _IONBF, 0);
    SetDllDirectoryA(dir);
    SetCurrentDirectoryA(dir);
    sprintf(path, "%s\\SVctl32.DLL", dir);
    g_dll = LoadLibraryA(path);
    if (!g_dll) {
        printf("cannot load %s\n", path);
        return 1;
    }
    pOpen = (PFN_Open)GetProcAddress(g_dll, "_SVOpenSpeech@20");
    pClose = (PFN_Close)GetProcAddress(g_dll, "_SVCloseSpeech@4");
    pTTS = (PFN_TTS)GetProcAddress(g_dll, "_SVTTS@32");
    pRegister = (PFN_Reg)GetProcAddress(g_dll, "_SVRegister@20");
    pErrText = (PFN_ErrText)GetProcAddress(g_dll, "_SVGetErrorText@12");

    patchIat("USER32.dll", "PostMessageA", (void *)myPostMessageA,
             (void **)&realPostMessageA);
    patchIat("WINMM.dll", "waveOutWrite", (void *)myWaveOutWrite,
             (void **)&realWaveOutWrite);
    patchIat("WINMM.dll", "waveOutOpen", (void *)myWaveOutOpen,
             (void **)&realWaveOutOpen);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "SvIndexWnd";
    RegisterClassA(&wc);
    g_wnd = CreateWindowExA(0, "SvIndexWnd", "svindex", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, NULL, wc.hInstance, NULL);
    g_syncMsg = RegisterWindowMessageA("SVSyncMessages");
    printf("notify window %p, SVSyncMessages=0x%04x\n", (void *)g_wnd,
           g_syncMsg);

    flags = SV_LANG_ENGLISH | SV_BITS_16 | SV_RATE_22050;
    /* arg2 is the notification window the index scheduler posts to. */
    rc = pOpen(&g_h, (DWORD)(ULONG_PTR)g_wnd, 0, flags, 0);
    printf("SVOpenSpeech(arg2=hwnd) rc=%d handle=0x%08lx\n", rc, g_h);
    if (rc || !g_h)
        return 2;

    lic = licOverride ? licOverride : readLicense();
    rc = pRegister(g_h, "ProdWorks", "SV_SSIL", lic, 0);
    printf("SVRegister(key=0x%08lx) rc=%d%s\n", lic, rc,
           rc ? "  *** UNREGISTERED - expect truncation ***" : "  OK");
    /* Deliberately carry on when rc != 0: an unregistered engine still
       speaks, it just caps each utterance at one 16 KB buffer, and that
       cap is the thing this run is meant to show. */

    printf("\nspeaking: \"%s\"\n", text);
    printf("ctx[0]=%08lx (hwnd) ctx[0x20]=%08lx (msg) ctx[0xa4]=%08lx (table)\n",
           ((DWORD *)(ULONG_PTR)g_h)[0], ((DWORD *)(ULONG_PTR)g_h)[0x20 / 4],
           ((DWORD *)(ULONG_PTR)g_h)[0xa4 / 4]);
    g_audioBytes = 0;
    pTTS(g_h, text, 0, 0, 0, 0, 0, 0);

    start = GetTickCount();
    while (GetTickCount() - start < 6000) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(5);
    }
    printf("\ntotal audio %lu B (%.2f s), %d posts intercepted\n", g_audioBytes,
           g_audioBytes / 2.0 / 22050.0, g_posts);
    /* Walk the scheduler table directly: 32-byte entries at ctx[0xa4], an
       active-flag byte per entry at table+0xc84+i, entry+8 = time offset,
       entry+0xc = the value posted as wParam. */
    {
        BYTE *tbl = (BYTE *)(ULONG_PTR)((DWORD *)(ULONG_PTR)g_h)[0xa4 / 4];
        int i, live = 0;
        printf("\nscheduler table at %p\n", (void *)tbl);
        if (tbl && !IsBadReadPtr(tbl, 0xc84 + 100)) {
            printf("  cursor = %u\n", *(WORD *)(tbl + 2));
            for (i = 0; i < 100; i++) {
                BYTE active = tbl[0xc84 + i];
                DWORD t = *(DWORD *)(tbl + i * 32 + 8);
                WORD w = *(WORD *)(tbl + i * 32 + 0xc);
                if (active || t || w) {
                    printf("  [%2d] active=%u time=%lu word=%u\n", i, active,
                           (unsigned long)t, w);
                    live++;
                }
            }
            printf("  %d non-empty entries\n", live);
        } else {
            printf("  table not readable\n");
        }
    }

    if (pClose)
        pClose(g_h);
    return 0;
}
