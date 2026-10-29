/* hogs_thumb.dll — Explorer thumbnail provider for Hogs media player (2026-10-27).
   Windows' own video thumbnailer can't decode HEVC/x265, RMVB, VOB and friends, so with Hogs as the default
   app most of the library showed a blank icon instead of a picture.
   WHY IT IS SHAPED LIKE THIS:
   * Per-user (HKCU) registration is the only kind the player can do for every user without admin — but
     Windows' isolated thumbnail process ignores per-user COM classes (measured: REGDB_E_CLASSNOTREG), so the
     class is registered with DisableProcessIsolation=1 and this DLL is loaded INSIDE Explorer.
   * Therefore this DLL does no decoding at all: it runs `hogofwar.exe --thumb <file> <size>` (hidden, no
     window) and reads the picture from a pipe. A broken or hostile video can only crash that throwaway
     process, never Explorer. Hard 10 s timeout.
   COM: IThumbnailProvider + IInitializeWithFile. Registered by the player itself on startup (per-user), or by
   register_thumbnails.ps1. Build: see build.bat. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <thumbcache.h>
#include <stdint.h>

/* {26687DFC-63C4-4F37-8139-5E958C287DB4} */
static const CLSID CLSID_HogsThumb = {0x26687dfc,0x63c4,0x4f37,{0x81,0x39,0x5e,0x95,0x8c,0x28,0x7d,0xb4}};
static const IID IID_IThumbnailProvider_ = {0xe357fccd,0xa995,0x4576,{0xb0,0x1f,0x23,0x46,0x30,0x15,0x4e,0x96}};
static const IID IID_IInitializeWithFile_ = {0xb7d14566,0x0509,0x4cce,{0xa7,0x1f,0x0a,0x55,0x42,0x33,0xbd,0x9b}};
static HINSTANCE g_inst; static volatile LONG g_locks = 0;

typedef struct IInitializeWithFile_ IInitializeWithFile_;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IInitializeWithFile_*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IInitializeWithFile_*);
    ULONG   (STDMETHODCALLTYPE *Release)(IInitializeWithFile_*);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IInitializeWithFile_*, LPCWSTR, DWORD);
} IInitializeWithFileVtbl_;
struct IInitializeWithFile_ { IInitializeWithFileVtbl_ *lpVtbl; };

/* run the player in thumbnail mode and collect "HTB1" + w + h + BGRA from its stdout */
static HRESULT grab(const wchar_t *file, UINT cx, HBITMAP *out){
    wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(g_inst, exe, MAX_PATH);
    while(n > 0 && exe[n-1] != L'\\') n--; exe[n] = 0;
    if(n + 16 >= MAX_PATH) return E_FAIL;
    lstrcatW(exe, L"hogofwar.exe");
    size_t cl = (size_t)lstrlenW(exe) + (size_t)lstrlenW(file) + 64;
    wchar_t *cmd = HeapAlloc(GetProcessHeap(), 0, cl * sizeof(wchar_t)); if(!cmd) return E_OUTOFMEMORY;
    wsprintfW(cmd, L"\"%s\" --thumb \"%s\" %u", exe, file, cx);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if(!CreatePipe(&rd, &wr, &sa, 1 << 20)){ HeapFree(GetProcessHeap(), 0, cmd); return E_FAIL; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);          /* only the write end goes to the child */
    STARTUPINFOW si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdInput = NULL; si.hStdError = NULL;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
    CloseHandle(wr); HeapFree(GetProcessHeap(), 0, cmd);
    if(!ok){ CloseHandle(rd); return E_FAIL; }
    /* read as it arrives (the child must never block on a full pipe), 10 s hard limit */
    uint8_t *buf = NULL; size_t have = 0, cap = 0; DWORD t0 = GetTickCount(); HRESULT hr = E_FAIL;
    for(;;){
        DWORD avail = 0;
        if(!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;        /* broken pipe = child finished */
        if(avail){
            if(have + avail > cap){ size_t nc = (have + avail) * 2 + 65536;
                uint8_t *t = buf ? HeapReAlloc(GetProcessHeap(), 0, buf, nc) : HeapAlloc(GetProcessHeap(), 0, nc);
                if(!t) break; buf = t; cap = nc; }
            DWORD got = 0; if(!ReadFile(rd, buf + have, avail, &got, NULL)) break; have += got; continue;
        }
        if(WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0){
            DWORD a2 = 0; if(PeekNamedPipe(rd, NULL, 0, NULL, &a2, NULL) && a2) continue; break; }
        if(GetTickCount() - t0 > 10000){ TerminateProcess(pi.hProcess, 1); break; }
    }
    CloseHandle(rd); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if(buf && have >= 12 && !memcmp(buf, "HTB1", 4)){
        int32_t w, h; memcpy(&w, buf + 4, 4); memcpy(&h, buf + 8, 4);
        if(w > 0 && h > 0 && w <= 4096 && h <= 4096 && have >= 12 + (size_t)w * h * 4){
            BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            void *bits = NULL; HBITMAP hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if(hb && bits){ memcpy(bits, buf + 12, (size_t)w * h * 4); *out = hb; hr = S_OK; }
        }
    }
    if(buf) HeapFree(GetProcessHeap(), 0, buf);
    return hr;
}

/* ---------------- COM object: IThumbnailProvider + IInitializeWithFile ---------------- */
typedef struct {
    IThumbnailProvider tp; IInitializeWithFile_ iwf;
    volatile LONG refs; wchar_t *path;
} HogsThumb;
#define FROM_TP(p)  ((HogsThumb*)(p))
#define FROM_IWF(p) ((HogsThumb*)((char*)(p) - offsetof(HogsThumb, iwf)))

static HRESULT ht_qi(HogsThumb *o, REFIID riid, void **pv){
    if(IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IThumbnailProvider_)) *pv = &o->tp;
    else if(IsEqualIID(riid, &IID_IInitializeWithFile_)) *pv = &o->iwf;
    else { *pv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&o->refs); return S_OK;
}
static ULONG ht_release(HogsThumb *o){
    LONG r = InterlockedDecrement(&o->refs);
    if(!r){ if(o->path) HeapFree(GetProcessHeap(), 0, o->path); HeapFree(GetProcessHeap(), 0, o); InterlockedDecrement(&g_locks); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE tp_qi(IThumbnailProvider *t, REFIID r, void **pv){ return ht_qi(FROM_TP(t), r, pv); }
static ULONG STDMETHODCALLTYPE tp_addref(IThumbnailProvider *t){ return (ULONG)InterlockedIncrement(&FROM_TP(t)->refs); }
static ULONG STDMETHODCALLTYPE tp_release(IThumbnailProvider *t){ return ht_release(FROM_TP(t)); }
static HRESULT STDMETHODCALLTYPE tp_get(IThumbnailProvider *t, UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *alpha){
    HogsThumb *o = FROM_TP(t);
    if(!phbmp) return E_POINTER; *phbmp = NULL;
    if(alpha) *alpha = WTSAT_RGB;
    if(!o->path) return E_UNEXPECTED;
    return grab(o->path, cx ? cx : 256, phbmp);
}
static IThumbnailProviderVtbl tp_vtbl = { tp_qi, tp_addref, tp_release, tp_get };
static HRESULT STDMETHODCALLTYPE iwf_qi(IInitializeWithFile_ *t, REFIID r, void **pv){ return ht_qi(FROM_IWF(t), r, pv); }
static ULONG STDMETHODCALLTYPE iwf_addref(IInitializeWithFile_ *t){ return (ULONG)InterlockedIncrement(&FROM_IWF(t)->refs); }
static ULONG STDMETHODCALLTYPE iwf_release(IInitializeWithFile_ *t){ return ht_release(FROM_IWF(t)); }
static HRESULT STDMETHODCALLTYPE iwf_init(IInitializeWithFile_ *t, LPCWSTR file, DWORD mode){
    (void)mode; HogsThumb *o = FROM_IWF(t);
    if(o->path) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if(!file) return E_INVALIDARG;
    int n = lstrlenW(file) + 1;
    o->path = HeapAlloc(GetProcessHeap(), 0, (size_t)n * sizeof(wchar_t)); if(!o->path) return E_OUTOFMEMORY;
    lstrcpyW(o->path, file); return S_OK;
}
static IInitializeWithFileVtbl_ iwf_vtbl = { iwf_qi, iwf_addref, iwf_release, iwf_init };

/* ---------------- class factory ---------------- */
static HRESULT STDMETHODCALLTYPE cf_qi(IClassFactory *t, REFIID r, void **pv){
    if(IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IClassFactory)){ *pv = t; return S_OK; }
    *pv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IClassFactory *t){ (void)t; InterlockedIncrement(&g_locks); return 2; }
static ULONG STDMETHODCALLTYPE cf_release(IClassFactory *t){ (void)t; InterlockedDecrement(&g_locks); return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(IClassFactory *t, IUnknown *outer, REFIID r, void **pv){
    (void)t; *pv = NULL;
    if(outer) return CLASS_E_NOAGGREGATION;
    HogsThumb *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
    if(!o) return E_OUTOFMEMORY;
    o->tp.lpVtbl = &tp_vtbl; o->iwf.lpVtbl = &iwf_vtbl; o->refs = 1; InterlockedIncrement(&g_locks);
    HRESULT hr = ht_qi(o, r, pv); ht_release(o); return hr;
}
static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *t, BOOL lock){ (void)t; if(lock) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks); return S_OK; }
static IClassFactoryVtbl cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static IClassFactory g_cf = { &cf_vtbl };

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **pv){
    if(!IsEqualCLSID(clsid, &CLSID_HogsThumb)){ *pv = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
    return cf_qi(&g_cf, riid, pv);
}
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void){ return g_locks == 0 ? S_OK : S_FALSE; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r){ (void)r; if(why == DLL_PROCESS_ATTACH){ g_inst = h; DisableThreadLibraryCalls(h); } return TRUE; }
