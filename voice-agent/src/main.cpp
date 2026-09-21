// src/main.cpp
// GUI 入口：启动 Qt6 Widgets 主窗口（Voice Agent 本地全双工语音助手）
#include "util/log.hpp"
#include "gui/main_window.hpp"
#include <QApplication>

#include <climits>
#include <crtdbg.h>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <windows.h>
#include <dbghelp.h>

// ===== 顶部崩溃处理器：把未处理异常（访问违规等导致"启动即退出"）
// 的调用栈写入 crash_stack.txt，便于定位崩溃点。动态加载 dbghelp，不改链接。 =====
static void dump_stack_to_file(EXCEPTION_POINTERS* ep) {
    HMODULE hDbg = LoadLibraryA("dbghelp.dll");
    if (!hDbg) return;
    auto pSymInit   = (decltype(SymInitialize)*)GetProcAddress(hDbg, "SymInitialize");
    auto pSymOpt    = (decltype(SymSetOptions)*)GetProcAddress(hDbg, "SymSetOptions");
    auto pSymPath   = (decltype(SymSetSearchPath)*)GetProcAddress(hDbg, "SymSetSearchPath");
    auto pSymAddr   = (decltype(SymFromAddr)*)GetProcAddress(hDbg, "SymFromAddr");
    auto pSymLine   = (decltype(SymGetLineFromAddr64)*)GetProcAddress(hDbg, "SymGetLineFromAddr64");
    auto pSymClean  = (decltype(SymCleanup)*)GetProcAddress(hDbg, "SymCleanup");
    if (!pSymInit || !pSymOpt || !pSymPath || !pSymAddr || !pSymLine) {
        FreeLibrary(hDbg);
        return;
    }

    HANDLE hProc = GetCurrentProcess();
    pSymOpt(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_IGNORE_CVREC);
    // TRUE = 侵袭式加载当前进程所有模块及其 PDB，这样才能解析我们的代码符号/行号
    if (!pSymInit(hProc, nullptr, TRUE)) { FreeLibrary(hDbg); return; }

    char exeDir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) *slash = '\0';
    char searchPath[MAX_PATH + 64] = {};
    snprintf(searchPath, sizeof(searchPath), "%s;build-msvc;.", exeDir);
    pSymPath(hProc, searchPath);

    FILE* f = fopen("crash_stack.txt", "w");
    if (!f) { FreeLibrary(hDbg); pSymClean(hProc); return; }
    if (ep && ep->ExceptionRecord) {
        fprintf(f, "Unhandled exception code=0x%08lx addr=0x%p thread=%lu\n",
                ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    }

    void* calls[128] = {};
    USHORT n = CaptureStackBackTrace(0, 128, calls, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        std::uintptr_t pc = reinterpret_cast<std::uintptr_t>(calls[i]);
        SYMBOL_INFO sym{};
        sym.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.MaxNameLen = 1023;
        DWORD64 disp = 0;
        char name[1024] = "?";
        if (pSymAddr(hProc, (DWORD64)pc, &disp, &sym))
            snprintf(name, sizeof(name), "%s+0x%llx", sym.Name, disp);

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD ldisp = 0;
        const char* src = "";
        unsigned lno = 0;
        if (pSymLine(hProc, (DWORD64)pc, &ldisp, &line)) { src = line.FileName; lno = line.LineNumber; }

        fprintf(f, "#%02u 0x%016llx  %-55s  %s:%u\n",
                i, (unsigned long long)pc, name, src, lno);
    }
    fclose(f);
    pSymClean(hProc);
    FreeLibrary(hDbg);
}

static LONG WINAPI top_crash_handler(EXCEPTION_POINTERS* ep) {
    dump_stack_to_file(ep);
    // 崩溃点状态未定义，无法安全继续；记录后交回系统终止进程。
    return EXCEPTION_CONTINUE_SEARCH;
}

// CRT 报告钩子：把断言信息写入日志文件，而不弹出 "Debug Assertion Failed" MessageBox 打断操作。
// 返回 TRUE 表示消息已处理（不继续默认的弹窗逻辑）。
// 该钩子在 Release 和 Debug CRT 下均可链接使用，故不依赖 _DEBUG 宏。
static int __cdecl crt_report_hook(int nReportType, char* szMsg, int* pnRet) {
    std::ofstream ofs("debug_assert.log", std::ios::app);
    if (ofs) {
        ofs << "[crt rpt=" << nReportType << "] "
            << (szMsg ? szMsg : "(no message)") << "\n";
    }
    if ((nReportType == _CRT_ASSERT || nReportType == _CRT_ERROR) && pnRet) {
        *pnRet = 0;          // 断言/错误失败也继续执行，避免反复弹窗
        return 1;            // 已处理，不显示对话框（TRUE）
    }
    return 0;                // 其余继续默认处理（FALSE）
}

int main(int argc, char* argv[]) {
    // 先禁用默认的弹窗报告，再挂上自定义钩子
    _CrtSetReportMode(_CRT_ASSERT | _CRT_ERROR | _CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT | _CRT_ERROR | _CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, crt_report_hook);

    // 捕获未处理异常调用栈到 crash_stack.txt
    SetUnhandledExceptionFilter(top_crash_handler);

    // 初始化日志（核心模块内部使用）
    init_logger("voice-agent", "info");

    QApplication app(argc, argv);
    QApplication::setOrganizationName("PT123123");
    QApplication::setApplicationName("VoiceAgent");

    voice_agent::gui::MainWindow window;
    window.show();
    return app.exec();
}