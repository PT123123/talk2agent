// 读取一倒 minidump，解开崩溃线程调用栈，解析符号 + 源码行号
// 用法: dumpstack <minidump.dmp> [pdb_search_dir]
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "dbghelp.lib")

static FILE* g_out = nullptr;
#define PRINT(...) do { \
    if (g_out) { fprintf(g_out, __VA_ARGS__); fflush(g_out); } \
    else { fprintf(stdout, __VA_ARGS__); fflush(stdout); } } while (0)

static std::string WideToNarrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static void ReadRvaTo(HANDLE hDump, DWORD rva, void* dst, SIZE_T size) {
    DWORD done = 0;
    OVERLAPPED ov{}; ov.Offset = rva;
    ReadFile(hDump, dst, (DWORD)size, &done, &ov);
}

int wmain(int argc, wchar_t* argv[]) {
    g_out = fopen("C:\\ProgramData\\voice-agent-dumps\\stack_diag.txt", "w");
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { PRINT("usage: dumpstack <minidump.dmp> [pdbdir]\n"); return 1; }
    PRINT("STEP argc=%u arg1=%ls\n", argc, argv[1]);
    HANDLE hFile = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    PRINT("STEP CreateFile h=%p %s\n", hFile,
          hFile == INVALID_HANDLE_VALUE ? "FAIL" : "OK");

    MINIDUMP_EXCEPTION_STREAM* exc = nullptr;
    PMINIDUMP_MODULE_LIST mod = nullptr;
    for (ULONG tt = ExceptionStream; tt <= SystemMemoryInfoStream; ++tt) {
        ULONG cnt = 0; PVOID buf = nullptr;
        PRINT("  readstream tt=%u\n", tt);
        if (MiniDumpReadDumpStream(hFile, (MINIDUMP_STREAM_TYPE)tt, nullptr, &buf, &cnt)) {
            PRINT("  -> ok tt=%u cnt=%u buf=%p\n", tt, cnt, buf);
            if (tt == ExceptionStream) exc = (MINIDUMP_EXCEPTION_STREAM*)buf;
            else if (tt == ModuleListStream) mod = (PMINIDUMP_MODULE_LIST)buf;
        }
    }
    if (!exc) { PRINT("no exception stream\n"); return 3; }
    PRINT("STEP streams ok, exc=%p code=0x%08lx thr=%u\n", exc,
           (unsigned long)exc->ExceptionRecord.ExceptionCode, exc->ThreadId);
    PRINT("ExceptionAddress=0x%llx\n", exc->ExceptionRecord.ExceptionAddress);

    CONTEXT ctx{}; memset(&ctx, 0, sizeof(ctx));
    SIZE_T csz = (exc->ThreadContext.DataSize < sizeof(CONTEXT))
                     ? exc->ThreadContext.DataSize : sizeof(CONTEXT);
    ReadRvaTo(hFile, exc->ThreadContext.Rva, &ctx, csz);

    std::string search;
    if (argc >= 3) search = WideToNarrow(argv[2]);

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_IGNORE_CVREC);
    if (!SymInitialize(hFile, nullptr, FALSE)) { PRINT("SymInitialize failed %u\n", GetLastError()); return 4; }
    if (!search.empty()) SymSetSearchPath(hFile, search.c_str());

    if (mod) {
        for (ULONG i = 0; i < mod->NumberOfModules; ++i) {
            auto& m = mod->Modules[i];
            std::wstring name;
            if (m.ModuleNameRva) {
                wchar_t tmp[512] = {}; ReadRvaTo(hFile, m.ModuleNameRva, tmp, sizeof(tmp) - 2);
                name = tmp;
            }
            std::string an = WideToNarrow(name);
            MODLOAD_DATA ld {}; ld.ssize = sizeof(ld);
            SymLoadModuleEx(hFile, nullptr, nullptr, (PSTR)an.c_str(),
                            m.BaseOfImage, m.SizeOfImage, &ld, 0);
            PRINT("module base=0x%llx size=0x%x %s\n", m.BaseOfImage, m.SizeOfImage, an.empty()?"?":an.c_str());
        }
    }

    STACKFRAME64 fr; memset(&fr, 0, sizeof(fr));
    fr.AddrPC.Offset = ctx.Rip;     fr.AddrPC.Mode = AddrModeFlat;
    fr.AddrStack.Offset = ctx.Rsp;  fr.AddrStack.Mode = AddrModeFlat;
    fr.AddrFrame.Offset = ctx.Rbp;  fr.AddrFrame.Mode = AddrModeFlat;
    fr.AddrReturn.Offset = ctx.Rip; fr.AddrReturn.Mode = AddrModeFlat;

    PRINT("\n=== CALL STACK ===\n");
    int frame = 0;
    while (frame < 128) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hFile, GetCurrentThread(),
                         &fr, &ctx, nullptr,
                         (PFUNCTION_TABLE_ACCESS_ROUTINE64)SymFunctionTableAccess64,
                         (PGET_MODULE_BASE_ROUTINE64)SymGetModuleBase64,
                         nullptr))
            break;
        DWORD64 pc = fr.AddrPC.Offset;
        if (pc == 0) break;

        char symName[1200] = "?";
        DWORD64 disp = 0;
        unsigned char buf[sizeof(SYMBOL_INFO) + 1200];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        memset(sym, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 1200;
        if (SymFromAddr(hFile, pc, &disp, sym)) {
            _snprintf_s(symName, sizeof(symName), _TRUNCATE, "%s+0x%llx", sym->Name, disp);
        }

        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD ldisp = 0;
        const char* src = "";
        unsigned ln = 0;
        if (SymGetLineFromAddr64(hFile, pc, &ldisp, &line)) { src = line.FileName; ln = line.LineNumber; }

        PRINT("#%02d 0x%llx  %-55s  %s:%u\n", frame++, pc, symName, src, ln);

        if (fr.AddrReturn.Offset == 0) break;
        fr.AddrPC = fr.AddrReturn;
    }
    SymCleanup(hFile);
    CloseHandle(hFile);
    return 0;
}