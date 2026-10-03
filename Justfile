# talk2agent 根目录任务配方 —— 委托 voice-agent 子项目
set shell := ["powershell.exe", "-NoLogo", "-NoProfile", "-Command"]

va := "voice-agent"

# 默认：列出可用命令
default:
    @just --list

# 构建主程序 + 测试（MSVC + Ninja + Qt6 + Vulkan，未配置会自动 configure）
build:
    just -f {{va}}/Justfile -d {{va}} build

# 构建并运行 GUI（自动编译 + 配置 Qt DLL 路径）
run:
    just -f {{va}}/Justfile -d {{va}} run

# 清空构建产物（cmake target clean，保留构建目录与配置）
clean:
    just -f {{va}}/Justfile -d {{va}} clean

# 彻底删除构建目录（下次 build 会重新全量配置）
purge:
    just -f {{va}}/Justfile -d {{va}} purge

# 运行测试集（ctest）
test:
    just -f {{va}}/Justfile -d {{va}} test

# 下载音频依赖库
deps:
    just -f {{va}}/Justfile -d {{va}} deps
