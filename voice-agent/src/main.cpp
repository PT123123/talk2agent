// src/main.cpp
// GUI 入口：启动 Qt6 Widgets 主窗口（Voice Agent 本地全双工语音助手）
#include "util/log.hpp"
#include "gui/main_window.hpp"
#include <QApplication>

#include <crtdbg.h>
#include <fstream>

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

    // 初始化日志（核心模块内部使用）
    init_logger("voice-agent", "info");

    QApplication app(argc, argv);
    QApplication::setOrganizationName("PT123123");
    QApplication::setApplicationName("VoiceAgent");

    voice_agent::gui::MainWindow window;
    window.show();
    return app.exec();
}