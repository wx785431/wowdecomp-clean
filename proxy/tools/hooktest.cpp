// ===========================================================================
//  hooktest.exe —— 端到端验证 DINPUT8 代理是否真的生效
//
//  做法与游戏完全一致：加载 DINPUT8.dll（触发 IAT 补丁），
//  然后正常 CreateFile + ReadFile 一个"加密"的 MPQ，看读到的头是不是被还原了。
//  不需要启动游戏，可在 GitHub Actions 里跑。
//
//  用法：hooktest.exe <DINPUT8.dll 的完整路径>
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const BYTE kPad[16] = {
    0x61, 0x5E, 0xD7, 0xFB, 0x15, 0x04, 0xB0, 0xC7,
    0x24, 0xA2, 0x96, 0xD7, 0xA6, 0x9E, 0x6C, 0x7B
};

// 明文 -> 加密（模拟 MqpEncrypt 的前 512 字节处理）
static void Scramble(BYTE* p, DWORD len)
{
    for (DWORD i = 0; i < len && i < 512; i++) {
        if (i < 3) p[i] = (BYTE)(i == 0 ? 0x5E : (i == 1 ? 0x24 : 0x26));
        else       p[i] ^= kPad[i & 15];
    }
}

static int g_fail = 0;

static void Check(const char* what, BOOL ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // 无缓冲，崩了也能看到最后一步
    // 第二个参数为 nolog 时，关闭代理 DLL 的日志（用于判断日志是否引发问题）
    if (argc >= 3 && wcscmp(argv[2], L"nolog") == 0) {
        SetEnvironmentVariableW(L"WOWPROXY_NOLOG", L"1");
        printf("[mode] logging disabled\n");
    } else {
        printf("[mode] logging enabled\n");
    }
    printf("[step 0] start\n");
    if (argc < 2) { printf("usage: hooktest.exe <path to DINPUT8.dll>\n"); return 2; }

    // ---- 1. 造一个 1KB 的假 MPQ：前 512 字节按真实规则加密 ----
    wchar_t tmpPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpPath);
    lstrcatW(tmpPath, L"hooktest_sample.mpq");

    BYTE plain[1024];
    memset(plain, 0, sizeof(plain));
    plain[0] = 0x4D; plain[1] = 0x50; plain[2] = 0x51; plain[3] = 0x1A;   // "MPQ\x1A"
    plain[4] = 0x20;                                                       // headerSize = 0x20
    for (int i = 8; i < 512; i++) plain[i] = (BYTE)(i * 7 + 3);            // 随便填点可校验的数据

    BYTE scrambled[1024];
    memcpy(scrambled, plain, sizeof(plain));
    Scramble(scrambled, 512);

    HANDLE f = CreateFileW(tmpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("cannot create sample\n"); return 2; }
    DWORD wr = 0;
    WriteFile(f, scrambled, sizeof(scrambled), &wr, NULL);
    CloseHandle(f);

    printf("[step 1] sample created\n");

    // ---- 2. 加载代理 DLL（游戏里是静态导入，这里用 LoadLibrary 等价触发）----
    printf("[step 2] LoadLibrary %ls\n", argv[1]);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 2; }
    printf("[step 3] proxy loaded: %ls\n", argv[1]);
    Sleep(600);
    printf("[step 3b] slept 600ms\n");    // 等它的工作线程打完 IAT 补丁

    // ---- 3. 正常读文件，头应该已经是明文 ----
    printf("[step 4] opening sample\n");
    f = CreateFileW(tmpPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("cannot open sample\n"); return 2; }

    BYTE buf[512];
    DWORD got = 0;
    memset(buf, 0, sizeof(buf));
    printf("[step 5] reading 512 bytes\n");
    ReadFile(f, buf, 512, &got, NULL);
    printf("[step 5b] got=%lu\n", got);
    Check("整块读取 512 字节，头已还原", got == 512 && memcmp(buf, plain, 512) == 0);

    // ---- 4. 中途定位再读（游戏的随机读取路径）----
    LARGE_INTEGER off; off.QuadPart = 64;
    SetFilePointerEx(f, off, NULL, FILE_BEGIN);
    BYTE part[64];
    memset(part, 0, sizeof(part));
    printf("[step 6] reading at 64\n");
    ReadFile(f, part, 64, &got, NULL);
    Check("SetFilePointerEx 定位到 64 后读取正确", got == 64 && memcmp(part, plain + 64, 64) == 0);

    // ---- 5. 偏移 >= 512 的内容一个字节都不能动 ----
    off.QuadPart = 512;
    SetFilePointerEx(f, off, NULL, FILE_BEGIN);
    BYTE tail[64];
    memset(tail, 0, sizeof(tail));
    printf("[step 7] reading at 512\n");
    ReadFile(f, tail, 64, &got, NULL);
    Check("512 之后不受影响", got == 64 && memcmp(tail, plain + 512, 64) == 0);

    printf("[step 8] closing\n");
    CloseHandle(f);
    DeleteFileW(tmpPath);

    printf("\n结果：%s\n", g_fail ? "有失败项" : "全部通过");
    return g_fail ? 1 : 0;
}
