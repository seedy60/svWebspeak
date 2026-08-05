/*
 * svvoice.c - dump each personality's voice-parameter block.
 *
 * The driver currently carries a hand-measured NATURAL_PITCH table and infers
 * each preset's pitch from rendered F0 divided by a fudge constant. SVCTL32
 * exports _SVGetVoiceInfo@16, so the engine can be asked instead. This tool
 * works out that function's calling convention, then dumps the block for all
 * 20 personalities so the field holding pitch can be identified by inspection.
 *
 * Build: tools\build-voice.cmd    Run: svvoice.exe [dir] [--license N]
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SV_LANG_ENGLISH 0x00000001
#define SV_BITS_16      0x00800000
#define SV_RATE_22050   0x08000000

#define NPERS 20
#define BUFSZ 512

typedef DWORD SVHANDLE;
typedef int(WINAPI *PFN_Open)(SVHANDLE *, DWORD, DWORD, DWORD, DWORD);
typedef int(WINAPI *PFN_Close)(SVHANDLE);
typedef int(WINAPI *PFN_Reg)(SVHANDLE, const char *, const char *, DWORD, DWORD);
typedef int(WINAPI *PFN_Set1)(SVHANDLE, DWORD);
typedef int(WINAPI *PFN_ErrText)(int, char *, WORD);
/* 16 bytes of arguments; the argument order is what we are here to find. */
typedef int(WINAPI *PFN_GetVoice4)(SVHANDLE, DWORD, void *, DWORD);

static HMODULE g_dll;
static PFN_Open pOpen;
static PFN_Close pClose;
static PFN_Reg pRegister;
static PFN_Set1 pSetPersonality;
static PFN_ErrText pErrText;
static PFN_GetVoice4 pGetVoiceInfo;

static const char *NAMES[NPERS] = {
    "Male", "Female", "Large Male", "Child", "Giant Male", "Mellow Female",
    "Mellow Male", "Crisp Male", "The Fly", "Robotoid", "Martian", "Colossus",
    "Fast Fred", "Old Woman", "Munchkin", "Troll", "Nerd", "Milktoast",
    "Tipsy", "Choir Boy"};

static const char *errText(int rc)
{
    static char b[256];
    if (pErrText && pErrText(rc, b, sizeof(b)) == 0)
        return b;
    return "?";
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

/* The block is dumped as DWORDs: every documented setter takes a DWORD, so
   the stored form is most likely an array of them. */
static void dumpDwords(const unsigned char *b, int cb, int maxw)
{
    int i;
    const DWORD *p = (const DWORD *)b;
    for (i = 0; i < cb / 4 && i < maxw; i++) {
        if (i % 8 == 0)
            printf("\n    [%3d] ", i);
        printf("%10lu", (unsigned long)p[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    const char *dir = "C:\\FTP";
    DWORD lic = 0;
    SVHANDLE h = 0;
    int rc, i, ai, cbOut = 0;
    static unsigned char buf[NPERS][BUFSZ];
    static unsigned char probe[BUFSZ];

    setvbuf(stdout, NULL, _IONBF, 0);
    for (ai = 1; ai < argc; ai++) {
        if (!strcmp(argv[ai], "--license") && ai + 1 < argc)
            lic = (DWORD)strtoul(argv[++ai], NULL, 0);
        else if (argv[ai][0] != '-')
            dir = argv[ai];
    }
    if (!lic)
        lic = readLicense();

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
    pRegister = (PFN_Reg)GetProcAddress(g_dll, "_SVRegister@20");
    pSetPersonality = (PFN_Set1)GetProcAddress(g_dll, "_SVSetPersonality@8");
    pErrText = (PFN_ErrText)GetProcAddress(g_dll, "_SVGetErrorText@12");
    pGetVoiceInfo = (PFN_GetVoice4)GetProcAddress(g_dll, "_SVGetVoiceInfo@16");
    if (!pGetVoiceInfo) {
        printf("no _SVGetVoiceInfo@16 export\n");
        return 1;
    }

    rc = pOpen(&h, 0, 0, SV_LANG_ENGLISH | SV_BITS_16 | SV_RATE_22050, 0);
    printf("SVOpenSpeech rc=%d handle=0x%08lx\n", rc, (unsigned long)h);
    if (rc || !h)
        return 2;
    rc = pRegister(h, "ProdWorks", "SV_SSIL", lic, 0);
    printf("SVRegister(0x%08lx) rc=%d %s\n\n", (unsigned long)lic, rc,
           rc ? errText(rc) : "OK");

    /* Work out the argument order. Try (h, what, buf, cb) with a few values
       of 'what', and (h, buf, cb, 0). A wrong guess normally returns an error
       code rather than faulting, but guard anyway. */
    printf("=== probing _SVGetVoiceInfo@16 signature ===\n");
    for (i = 0; i < 6; i++) {
        int r2;
        memset(probe, 0xCC, sizeof(probe));
        __try {
            r2 = pGetVoiceInfo(h, (DWORD)i, probe, sizeof(probe));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  (h, %d, buf, cb) -> EXCEPTION\n", i);
            continue;
        }
        printf("  (h, %d, buf, cb) -> rc=%d %s", i, r2, r2 ? errText(r2) : "OK");
        if (r2 == 0) {
            int nz = 0, j;
            for (j = 0; j < BUFSZ; j++)
                if (probe[j] != 0xCC)
                    nz++;
            printf("   (%d/%d bytes written)", nz, BUFSZ);
            if (nz && !cbOut)
                cbOut = nz;
        }
        printf("\n");
    }

    if (!cbOut) {
        printf("\nno argument order produced output; dumping raw attempt 0\n");
        cbOut = 64;
    }

    /* Arg 2 looks like a voice index rather than a "what" selector, so ask
       for each voice in turn instead of setting the personality first. */
    {
        int lo = BUFSZ, hi2 = 0, j;
        for (i = 0; i < NPERS; i++) {
            int r2;
            memset(buf[i], 0xCC, BUFSZ);
            __try {
                r2 = pGetVoiceInfo(h, (DWORD)i, buf[i], BUFSZ);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                printf("%2d %-14s EXCEPTION\n", i, NAMES[i]);
                continue;
            }
            if (r2)
                printf("%2d %-14s rc=%d %s\n", i, NAMES[i], r2, errText(r2));
            for (j = 0; j < BUFSZ; j++)
                if (buf[i][j] != 0xCC) {
                    if (j < lo) lo = j;
                    if (j > hi2) hi2 = j;
                }
        }
        printf("\n=== written region: bytes %d..%d ===\n", lo, hi2);

        printf("\n--- as bytes ---\n");
        for (i = 0; i < NPERS; i++) {
            printf("%2d %-14s", i, NAMES[i]);
            for (j = lo; j <= hi2 && j < lo + 40; j++)
                printf(" %02x", buf[i][j]);
            printf("\n");
        }

        printf("\n--- 16-bit fields that vary across voices ---\n");
        for (j = lo & ~1; j + 1 <= hi2; j += 2) {
            int varies = 0, k;
            unsigned short f = *(unsigned short *)(buf[0] + j);
            for (k = 1; k < NPERS; k++)
                if (*(unsigned short *)(buf[k] + j) != f)
                    varies = 1;
            if (!varies)
                continue;
            printf("  off %3d:", j);
            for (k = 0; k < NPERS; k++)
                printf(" %5u", *(unsigned short *)(buf[k] + j));
            printf("\n");
        }

        printf("\n--- 32-bit fields that vary across voices ---\n");
        for (j = lo & ~3; j + 3 <= hi2; j += 4) {
            int varies = 0, k;
            DWORD f = *(DWORD *)(buf[0] + j);
            for (k = 1; k < NPERS; k++)
                if (*(DWORD *)(buf[k] + j) != f)
                    varies = 1;
            if (!varies)
                continue;
            printf("  off %3d:", j);
            for (k = 0; k < NPERS; k++)
                printf(" %6lu", (unsigned long)*(DWORD *)(buf[k] + j));
            printf("\n");
        }
    }

    pClose(h);
    return 0;
}
