// src/main.cpp
// GUI 入口：启动 Qt6 Widgets 主窗口（Voice Agent 本地全双工语音助手）
#include "util/log.hpp"
#include "util/paths.hpp"
#include "gui/main_window.hpp"
#include <QApplication>
#include <QPalette>

#include <climits>
#include <crtdbg.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
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
    USHORT n = 0;

    // 优先用异常现场的 ContextRecord 做 StackWalk64 回溯——
    // CaptureStackBackTrace 从过滤器的栈帧往上抓，拿不到崩溃点的真实调用链。
    CONTEXT walkCtx{};
    if (ep && ep->ContextRecord) {
        walkCtx = *ep->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = walkCtx.Rip;   frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = walkCtx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = walkCtx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
        auto pStackWalk = (decltype(StackWalk64)*)GetProcAddress(hDbg, "StackWalk64");
        if (pStackWalk) {
            for (int i = 0; i < 128; ++i) {
                if (!pStackWalk(IMAGE_FILE_MACHINE_AMD64, hProc, hProc, &frame,
                                &walkCtx, nullptr, nullptr, nullptr, nullptr))
                    break;
                if (frame.AddrPC.Offset == 0) break;
                if (n < 128) calls[n++] = reinterpret_cast<void*>(frame.AddrPC.Offset);
            }
        }
    }
    if (n == 0)   // 兜底：无上下文时退回启发式抓取
        n = CaptureStackBackTrace(0, 128, calls, nullptr);
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

    // 初始化日志（核心模块内部使用）。
    // 必须落文件：just run 用 Start-Process 分离启动 GUI，控制台输出根本看不到；
    // 使用中的问题（气泡顺序、打断、转写）全靠这份文件回放定位。
    const std::string log_dir = voice_agent::logs_root();
    std::error_code log_ec;
    std::filesystem::create_directories(log_dir, log_ec);
    const std::string log_file = log_dir + "/voice-agent.log";
    init_logger("voice-agent", "info", log_file);
    LOG_INFO("=== VoiceAgent 启动，日志文件: {} ===", log_file);

    // 模型目录一次性迁移（旧工程 models/ → %LOCALAPPDATA%\VoiceAgent\models），
    // 必须在加载配置/扫描模型之前完成。
    voice_agent::migrate_legacy_models();

    QApplication app(argc, argv);
    QApplication::setOrganizationName("PT123123");
    QApplication::setApplicationName("VoiceAgent");

    // ===== 暗黑主题基础：Fusion 风格 + 深色调色板 =====
    // Windows 原生 style 会忽略大部分 QPalette 角色；切到 Fusion 才能让
    // 设置页/下拉框/菜单等跟随深色。主界面细节色再由 main_window 的 QSS 覆盖。
    app.setStyle("Fusion");
    {
        QPalette p;
        const QColor text(0xec, 0xec, 0xec);
        p.setColor(QPalette::Window, QColor(0x21, 0x21, 0x21));
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
        p.setColor(QPalette::AlternateBase, QColor(0x2a, 0x2a, 0x2a));
        p.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x2a, 0x2a));
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::PlaceholderText, QColor(0x8d, 0x8d, 0x8d));
        p.setColor(QPalette::Button, QColor(0x2f, 0x2f, 0x2f));
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::BrightText, Qt::white);
        p.setColor(QPalette::Highlight, QColor(0x2f, 0x4a, 0x73));
        p.setColor(QPalette::HighlightedText, text);
        p.setColor(QPalette::Link, QColor(0x7a, 0xaa, 0xfc));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6f, 0x6f, 0x6f));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6f, 0x6f, 0x6f));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6f, 0x6f, 0x6f));
        app.setPalette(p);
    }

    voice_agent::gui::MainWindow window;
    window.show();
    return app.exec();
}