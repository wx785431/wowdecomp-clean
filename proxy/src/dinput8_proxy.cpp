// ===========================================================================
//  DINPUT8.dll 代理（x64）—— 让 64 位 WoW 客户端直接读被 MqpEncrypt 加密的 MPQ
//
//  原理：
//    MqpEncrypt 只改动 MPQ 的前 512 字节
//      字节 0~2  写成 5E 24 26
//      字节 3~511 = 原文 ^ PAD[i % 16]
//      512 之后与原包逐字节相同，文件大小不变
//    本 DLL 在进程内把读出来的这 512 字节还原，磁盘上始终是乱码。
//
//  挂法：靠 Wow.exe 静态导入 DINPUT8.dll，放同名 DLL 到客户端根目录即可，
//        不需要注入器、不需要改客户端、不需要 encrypted_key.bin。
//
//  挂钩方式：直接改主模块（以及已加载模块）的导入表（IAT），
//            不做 inline hook，不需要 MinHook，不会破坏指令。
//            钩 ReadFile（普通读取）+ CreateFileMapping/MapViewOfFile（内存映射读取）。
//
//  构建：见 CMakeLists.txt；定义 SELFTEST 可单独编出算法自检程序。
// ===========================================================================

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// 兼容 MSVC / MinGW：不用 _countof
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// --------------------------------------------------------------------------
// 加密参数（已用 patch-zhCN-Y / Z / k 三份补丁实测确认）
// --------------------------------------------------------------------------
static const BYTE kPad[16] = {
    0x61, 0x5E, 0xD7, 0xFB, 0x15, 0x04, 0xB0, 0xC7,
    0x24, 0xA2, 0x96, 0xD7, 0xA6, 0x9E, 0x6C, 0x7B
};
static const BYTE kScrambledMagic[3] = { 0x5E, 0x24, 0x26 };   // 加密后头 3 字节
static const BYTE kMpqMagic[4]        = { 0x4D, 0x50, 0x51, 0x1A };  // "MPQ\x1A"
#define SCRAMBLE_LEN 512

// --------------------------------------------------------------------------
// 还原窗口内的一段字节
//   p   : 读缓冲区
//   pos : 这段缓冲区在文件中的起始偏移
//   len : 字节数
// --------------------------------------------------------------------------
static void RestoreWindow(BYTE* p, uint64_t pos, DWORD len)
{
    for (DWORD i = 0; i < len; i++) {
        uint64_t idx = pos + i;
        if (idx >= SCRAMBLE_LEN) break;
        if (idx < 3) p[i] = kMpqMagic[idx];      // 头 3 字节是覆盖，不是异或
        else         p[i] ^= kPad[idx & 15];
    }
}

// --------------------------------------------------------------------------
// 句柄状态表：0=未知 1=加密MPQ 2=普通MPQ
// --------------------------------------------------------------------------
struct HandleEnt { HANDLE h; int state; };

static HandleEnt        g_handles[512];
static SRWLOCK          g_handleLock = SRWLOCK_INIT;
static volatile LONG    g_handleCount = 0;

static int HandleStateOf(HANDLE h)
{
    int st = 0;
    AcquireSRWLockShared(&g_handleLock);
    for (LONG i = 0; i < g_handleCount; i++) {
        if (g_handles[i].h == h) { st = g_handles[i].state; break; }
    }
    ReleaseSRWLockShared(&g_handleLock);
    return st;
}

static void HandleSetState(HANDLE h, int st)
{
    AcquireSRWLockExclusive(&g_handleLock);
    for (LONG i = 0; i < g_handleCount; i++) {
        if (g_handles[i].h == h) { g_handles[i].state = st; ReleaseSRWLockExclusive(&g_handleLock); return; }
    }
    if (g_handleCount < (LONG)(sizeof(g_handles) / sizeof(g_handles[0]))) {
        g_handles[g_handleCount].h = h;
        g_handles[g_handleCount].state = st;
        g_handleCount++;
    }
    ReleaseSRWLockExclusive(&g_handleLock);
}

// --------------------------------------------------------------------------
// 原始函数
// --------------------------------------------------------------------------
typedef BOOL   (WINAPI *PFN_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef HANDLE (WINAPI *PFN_CreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
typedef HANDLE (WINAPI *PFN_CreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
typedef LPVOID (WINAPI *PFN_MapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);

static PFN_ReadFile          Real_ReadFile;
static PFN_CreateFileMappingW Real_CreateFileMappingW;
static PFN_CreateFileMappingA Real_CreateFileMappingA;
static PFN_MapViewOfFile     Real_MapViewOfFile;

static void ResolveReals()
{
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!k) return;
    if (!Real_ReadFile)           Real_ReadFile           = (PFN_ReadFile)GetProcAddress(k, "ReadFile");
    if (!Real_CreateFileMappingW) Real_CreateFileMappingW = (PFN_CreateFileMappingW)GetProcAddress(k, "CreateFileMappingW");
    if (!Real_CreateFileMappingA) Real_CreateFileMappingA = (PFN_CreateFileMappingA)GetProcAddress(k, "CreateFileMappingA");
    if (!Real_MapViewOfFile)      Real_MapViewOfFile      = (PFN_MapViewOfFile)GetProcAddress(k, "MapViewOfFile");
}

// --------------------------------------------------------------------------
// 日志（只在第一次还原某个文件时记一行，便于排错）
// --------------------------------------------------------------------------
static CRITICAL_SECTION g_logLock;
static BOOL             g_logInit = FALSE;
static wchar_t          g_logPath[MAX_PATH];

static void LogLine(const wchar_t* fmt, ...)
{
    if (!g_logInit) return;
    EnterCriticalSection(&g_logLock);
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        wchar_t buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) {
            DWORD wr = 0;
            WriteFile(f, buf, (DWORD)n * sizeof(wchar_t), &wr, NULL);
        }
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logLock);
}

static void LogHandle(const wchar_t* tag, HANDLE h)
{
    wchar_t path[MAX_PATH] = { 0 };
    DWORD n = GetFinalPathNameByHandleW(h, path, _countof(path) - 1, FILE_NAME_NORMALIZED);
    if (n == 0 || n >= _countof(path)) return;
    LogLine(L"[%s] 已还原头部: %s\r\n", tag, path);
}

// --------------------------------------------------------------------------
// ReadFile 钩子
// --------------------------------------------------------------------------
static BOOL WINAPI HkReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nBytes,
                              LPDWORD lpRead, LPOVERLAPPED lpOverlapped)
{
    if (!Real_ReadFile) ResolveReals();
    if (!Real_ReadFile) return FALSE;

    uint64_t pos = 0;
    BOOL     havePos = FALSE;

    if (lpOverlapped) {
        pos = ((uint64_t)lpOverlapped->OffsetHigh << 32) | lpOverlapped->Offset;
        havePos = TRUE;
    } else {
        LARGE_INTEGER zero = { 0 }, cur = { 0 };
        if (SetFilePointerEx(hFile, zero, &cur, FILE_CURRENT)) {
            pos = (uint64_t)cur.QuadPart;
            havePos = TRUE;
        }
    }

    BOOL  ok = Real_ReadFile(hFile, lpBuffer, nBytes, lpRead, lpOverlapped);
    if (!ok || !havePos || !lpBuffer || !lpRead) return ok;

    DWORD got = *lpRead;
    if (got == 0) return ok;

    // 只有落在前 512 字节的读取才需要处理（游戏绝大多数读取都在 512 之后，
    // 这里直接返回，等于零开销）
    if (pos >= SCRAMBLE_LEN) return ok;

    int st = HandleStateOf(hFile);

    // 每次从 0 开始读都重新判定，避免句柄被复用后状态串味
    if (pos == 0 && got >= 3) {
        const BYTE* b = (const BYTE*)lpBuffer;
        if (b[0] == kScrambledMagic[0] && b[1] == kScrambledMagic[1] && b[2] == kScrambledMagic[2]) {
            if (st != 1) { HandleSetState(hFile, 1); LogHandle(L"read", hFile); }
            st = 1;
        } else if (b[0] == kMpqMagic[0] && b[1] == kMpqMagic[1] && b[2] == kMpqMagic[2]) {
            st = 2;      // 标准 MPQ，不登记，省得表被撑大
        }
    }

    if (st == 1)
        RestoreWindow((BYTE*)lpBuffer, pos, got);

    return ok;
}

// --------------------------------------------------------------------------
// 内存映射钩子：把只读映射改成写时复制（CoW），这样能在视图里改字节
// 而不动磁盘文件 —— 客户端如果走 mmap 读 MPQ，这条路兜住。
// --------------------------------------------------------------------------
static HANDLE  g_sections[64];
static SRWLOCK g_sectionLock = SRWLOCK_INIT;

// 只对 .mpq 做映射改造，别的文件（着色器缓存、日志之类）不动，避免白白产生写时复制页
static BOOL IsMpqHandle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    wchar_t path[MAX_PATH];
    DWORD n = GetFinalPathNameByHandleW(h, path, _countof(path) - 1, FILE_NAME_NORMALIZED);
    if (!n || n >= _countof(path)) return FALSE;
    size_t len = wcslen(path);
    if (len < 4) return FALSE;
    return lstrcmpiW(path + len - 4, L".mpq") == 0;
}

static void SectionMark(HANDLE h)
{
    AcquireSRWLockExclusive(&g_sectionLock);
    for (int i = 0; i < 64; i++) {
        if (g_sections[i] == NULL || g_sections[i] == h) { g_sections[i] = h; break; }
    }
    ReleaseSRWLockExclusive(&g_sectionLock);
}

static BOOL SectionIsOurs(HANDLE h)
{
    BOOL r = FALSE;
    AcquireSRWLockShared(&g_sectionLock);
    for (int i = 0; i < 64; i++) {
        if (g_sections[i] == h) { r = TRUE; break; }
    }
    ReleaseSRWLockShared(&g_sectionLock);
    return r;
}

static HANDLE WINAPI HkCreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES sa, DWORD flProtect,
                                          DWORD dwMaxHigh, DWORD dwMaxLow, LPCWSTR name)
{
    if (!Real_CreateFileMappingW) ResolveReals();
    if (!Real_CreateFileMappingW) return NULL;

    if (flProtect == PAGE_READONLY && IsMpqHandle(hFile)) {
        HANDLE m = Real_CreateFileMappingW(hFile, sa, PAGE_WRITECOPY, dwMaxHigh, dwMaxLow, name);
        if (m) { SectionMark(m); return m; }
    }
    return Real_CreateFileMappingW(hFile, sa, flProtect, dwMaxHigh, dwMaxLow, name);
}

static HANDLE WINAPI HkCreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES sa, DWORD flProtect,
                                          DWORD dwMaxHigh, DWORD dwMaxLow, LPCSTR name)
{
    if (!Real_CreateFileMappingA) ResolveReals();
    if (!Real_CreateFileMappingA) return NULL;

    if (flProtect == PAGE_READONLY && IsMpqHandle(hFile)) {
        HANDLE m = Real_CreateFileMappingA(hFile, sa, PAGE_WRITECOPY, dwMaxHigh, dwMaxLow, name);
        if (m) { SectionMark(m); return m; }
    }
    return Real_CreateFileMappingA(hFile, sa, flProtect, dwMaxHigh, dwMaxLow, name);
}

static LPVOID WINAPI HkMapViewOfFile(HANDLE hMap, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T bytes)
{
    if (!Real_MapViewOfFile) ResolveReals();
    if (!Real_MapViewOfFile) return NULL;

    BOOL ours = SectionIsOurs(hMap);

    LPVOID p = Real_MapViewOfFile(hMap, access | (ours ? FILE_MAP_COPY : 0), offHigh, offLow, bytes);
    if (!p) p = Real_MapViewOfFile(hMap, access, offHigh, offLow, bytes);   // 兜底：非本 DLL 建的段

    if (p && ours && offHigh == 0 && offLow == 0) {
        BYTE* b = (BYTE*)p;
        if (b[0] == kScrambledMagic[0] && b[1] == kScrambledMagic[1] && b[2] == kScrambledMagic[2]) {
            DWORD n = (bytes && bytes < SCRAMBLE_LEN) ? (DWORD)bytes : SCRAMBLE_LEN;
            RestoreWindow(b, 0, n);
            LogLine(L"[mmap] 已还原映射头 (hMap=%p)\r\n", hMap);
        }
    }
    return p;
}

// --------------------------------------------------------------------------
// IAT 打补丁
// --------------------------------------------------------------------------
static BOOL IsKernelDll(const char* dll)
{
    return _stricmp(dll, "KERNEL32.dll") == 0 ||
           _stricmp(dll, "KERNELBASE.dll") == 0 ||
           _strnicmp(dll, "api-ms-win-core-file", 20) == 0;
}

static void PatchOneModule(HMODULE mod)
{
    if (!mod) return;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;   // 只处理 x64 模块

    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRva) return;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + impRva);
    for (; imp->Name; imp++) {
        const char* dllName = (const char*)(base + imp->Name);
        if (!IsKernelDll(dllName)) continue;
        if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;   // 无名字表就跳过，避免误改

        IMAGE_THUNK_DATA64* nameThunk = (IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA64* iatThunk  = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);

        for (; nameThunk->u1.AddressOfData; nameThunk++, iatThunk++) {
            if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            const char* fn = (const char*)(base + nameThunk->u1.AddressOfData + 2);

            void* repl = NULL;
            if      (_stricmp(fn, "ReadFile") == 0)           repl = (void*)HkReadFile;
            else if (_stricmp(fn, "CreateFileMappingW") == 0) repl = (void*)HkCreateFileMappingW;
            else if (_stricmp(fn, "CreateFileMappingA") == 0) repl = (void*)HkCreateFileMappingA;
            else if (_stricmp(fn, "MapViewOfFile") == 0)      repl = (void*)HkMapViewOfFile;
            else continue;

            if ((void*)(ULONG_PTR)iatThunk->u1.Function == repl) continue;

            DWORD old = 0;
            if (VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
                iatThunk->u1.Function = (ULONG_PTR)repl;
                VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), old, &old);
            }
        }
    }
}

static void PatchAllModules()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        PatchOneModule(GetModuleHandleW(NULL));
        return;
    }
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do { PatchOneModule((HMODULE)me.modBaseAddr); } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    PatchOneModule(GetModuleHandleW(NULL));   // 保证主模块一定被处理
}

// --------------------------------------------------------------------------
// 代理导出：全部转发给 System32\dinput8.dll
// --------------------------------------------------------------------------
static HMODULE g_realDinput = NULL;

static HMODULE GetRealDinput()
{
    if (g_realDinput) return g_realDinput;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n) return NULL;
    lstrcatW(path, L"\\dinput8.dll");
    g_realDinput = LoadLibraryW(path);
    return g_realDinput;
}

template <typename T>
static T DinputProc(const char* name)
{
    HMODULE m = GetRealDinput();
    return m ? (T)GetProcAddress(m, name) : (T)NULL;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
                                             LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    typedef HRESULT (WINAPI *F)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    F f = DinputProc<F>("DirectInput8Create");
    return f ? f(hinst, dwVersion, riidltf, ppvOut, punkOuter) : E_FAIL;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    typedef HRESULT (WINAPI *F)(REFCLSID, REFIID, LPVOID*);
    F f = DinputProc<F>("DllGetClassObject");
    return f ? f(rclsid, riid, ppv) : E_FAIL;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
{
    typedef HRESULT (WINAPI *F)(void);
    F f = DinputProc<F>("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

extern "C" HRESULT WINAPI DllRegisterServer(void)
{
    typedef HRESULT (WINAPI *F)(void);
    F f = DinputProc<F>("DllRegisterServer");
    return f ? f() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer(void)
{
    typedef HRESULT (WINAPI *F)(void);
    F f = DinputProc<F>("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

extern "C" void* WINAPI GetdfDIJoystick(void)
{
    typedef void* (WINAPI *F)(void);
    F f = DinputProc<F>("GetdfDIJoystick");
    return f ? f() : NULL;
}

// --------------------------------------------------------------------------
// 初始化
// --------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID)
{
    // 线程在 DllMain 里起，loader lock 释放后才真正运行，
    // 此时主模块的导入表已经填好，可以安全地扫模块 + 打补丁。
    for (int i = 0; i < 5; i++) {
        PatchAllModules();
        Sleep(20);
    }
    LogLine(L"=== DINPUT8 代理已就绪，PID=%lu ===\r\n", GetCurrentProcessId());

    // 再补一次，覆盖启动后才 LoadLibrary 进来的模块（d3d9 等）
    Sleep(1500);
    PatchAllModules();
    return 0;
}

static void InitLog()
{
    if (g_logInit) return;
    InitializeCriticalSection(&g_logLock);
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    lstrcpynW(g_logPath, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(g_logPath, L'\\');
    if (slash) slash[1] = 0; else g_logPath[0] = 0;
    lstrcatW(g_logPath, L"WowProxy64.log");
    g_logInit = TRUE;
}

#ifndef SELFTEST
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        ResolveReals();
        InitLog();
        // 注意：DllMain 里持着 loader lock，不能枚举模块（CreateToolhelp32Snapshot 可能死锁），
        // 只对主模块做一次"尽力而为"的补丁；剩下交给工作线程。
        PatchOneModule(GetModuleHandleW(NULL));
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
#endif

// --------------------------------------------------------------------------
// 算法自检（编译时定义 SELFTEST）
// --------------------------------------------------------------------------
#ifdef SELFTEST
int main()
{
    // patch-zhCN-Z.mpq 的真实前 16 字节：密文 -> 明文
    BYTE buf[16] = { 0x5E,0x24,0x26,0xE1,0x35,0x04,0xB0,0xC7,0x2C,0x55,0xD0,0xD5,0xA6,0x9E,0x6F,0x7B };
    const BYTE want[16] = { 0x4D,0x50,0x51,0x1A,0x20,0x00,0x00,0x00,0x08,0xF7,0x46,0x02,0x00,0x00,0x03,0x00 };

    RestoreWindow(buf, 0, 16);
    if (memcmp(buf, want, 16) != 0) { printf("FAIL: header mismatch\n"); return 1; }

    // 分两次读（模拟半截读）也要对
    BYTE b2[16];
    memcpy(b2, "\x5E\x24\x26\xE1\x35\x04\xB0\xC7\x2C\x55\xD0\xD5\xA6\x9E\x6F\x7B", 16);
    RestoreWindow(b2 + 8, 8, 8);
    if (memcmp(b2 + 8, want + 8, 8) != 0) { printf("FAIL: partial window\n"); return 1; }

    // 偏移 >= 512 的读取不应改动任何字节
    BYTE b3[16]; memset(b3, 0xAB, 16);
    RestoreWindow(b3, 600, 16);
    for (int i = 0; i < 16; i++) if (b3[i] != 0xAB) { printf("FAIL: out-of-window write\n"); return 1; }

    printf("SELFTEST OK\n");
    return 0;
}
#endif
