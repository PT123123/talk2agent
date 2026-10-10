// src/util/paths.cpp
#include "paths.hpp"
#include "log.hpp"

#include <cstdlib>
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;

namespace voice_agent {
namespace {

// 路径分隔符统一为 '/'，去掉末尾多余斜杠
std::string normalize(std::string p) {
    for (auto& c : p)
        if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string local_app_data() {
    const char* env = std::getenv("LOCALAPPDATA");
    if (env && *env) return env;
    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile) return std::string(profile) + "/AppData/Local";
    return {};
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return normalize(fs::path(buf).parent_path().string());
}

}  // namespace

std::string models_root() {
    const std::string base = local_app_data();
    if (base.empty()) return "models";  // 兜底：退回旧行为（CWD/models）
    return normalize(base + "/VoiceAgent/models");
}

std::string logs_root() {
    const std::string base = local_app_data();
    if (base.empty()) return "logs";    // 兜底：退回 CWD/logs
    return normalize(base + "/VoiceAgent/logs");
}

std::string remap_model_path(const std::string& path) {
    if (path.empty()) return path;
    // 绝对路径（盘符或 POSIX 风格）原样保留
    if (path.size() > 1 && path[1] == ':') return path;
    if (path.front() == '/') return path;
    const std::string prefix = "models/";
    if (path.rfind(prefix, 0) == 0)
        return models_root() + "/" + path.substr(prefix.size());
    if (path == "models") return models_root();
    return path;
}

std::string unmap_model_path(const std::string& path) {
    const std::string root = models_root();
    if (path.rfind(root + "/", 0) != 0) return path;
    return "models/" + path.substr(root.size() + 1);
}

void migrate_legacy_models() {
    const std::string root = models_root();
    std::error_code ec;
    if (fs::exists(root, ec)) return;  // 新目录已存在：无需迁移
    if (ec) {
        LOG_WARN("paths: 探测模型目录 {} 失败：{}", root, ec.message());
        return;
    }

    // 旧版两个可能位置：CWD/models（just run 从工程根启动）、exe 同级 models/（双击启动）
    const std::string exeModels = exe_dir().empty() ? std::string() : exe_dir() + "/models";
    for (const std::string& legacy : {normalize("models"), exeModels}) {
        if (legacy.empty() || !fs::exists(legacy, ec) || !fs::is_directory(legacy, ec))
            continue;
        const fs::path parent = fs::path(root).parent_path();
        fs::create_directories(parent, ec);
        if (ec) {
            LOG_WARN("paths: 创建 {} 失败：{}，模型仍保留在 {}",
                     parent.string(), ec.message(), legacy);
            return;
        }
        fs::rename(legacy, root, ec);
        if (!ec) {
            LOG_INFO("paths: 模型目录已迁移 {} -> {}", legacy, root);
        } else {
            LOG_WARN("paths: 模型目录迁移失败（{} -> {}）：{}。"
                     "请手动移动该目录，否则需重新下载模型。",
                     legacy, root, ec.message());
        }
        return;  // 找到旧目录即止，只迁一次
    }
}

}  // namespace voice_agent
