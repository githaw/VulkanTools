# VulkanTools Project Development Guidelines for AI Agents

本文档提供了在 VulkanTools 代码库中进行开发的基本说明和最佳实践。请遵循这些准则，以确保代码的一致性和质量。

## Overview
- 项目目标：提供 Vulkan 开发生态工具，包括 Vulkan Configurator 与一组 Vulkan 层。
- 主要产物：
  - Vulkan Configurator（GUI/CLI/核心库，位于 `vkconfig_gui/`, `vkconfig_cmd/`, `vkconfig_core/`）
  - Vulkan Layers（位于 `layersvt/`）
- 支持平台：Windows、Linux、macOS、Android（不同平台可用层不同）。
- 构建系统：CMake 为主，存在 GN 构建描述用于集成（`BUILD.gn`）。
- 版本与生成代码：部分层代码由脚本生成，且需要随 PR 一起提交生成结果。

## 核心软件工程原则

编写和修改代码时，请遵循以下原则：
- 避免代码重复：在编写新函数之前，先在代码库中搜索是否存在提供类似功能的现有函数。
- 复用与重构：如果存在合适的函数，请直接复用；如果功能相近但不完全匹配，可以考虑重构现有函数以适应新的使用场景，而不是创建副本。
- 不确定时请咨询：如果你正在考虑复制函数或重要代码块，请先咨询用户意见。

## 目录结构
- `layersvt/`: Vulkan Layers 源码与说明，含生成代码目录 `layersvt/generated/`。
- `vkconfig_core/`: Vulkan Configurator 核心库。
- `vkconfig_cmd/`: Vulkan Configurator 命令行工具。
- `vkconfig_gui/`: Vulkan Configurator GUI（Qt6）。
- `scripts/`: 代码生成与依赖管理脚本。
- `tests/`: 测试相关（CMake 控制是否构建）。
- `external/`: 第三方依赖库，根据构建脚本和平台自动下载相应的目录。

## 构建与依赖约束
- 必需工具版本：
  - CMake >= 3.22.1
  - Python >= 3.10
  - C++17 编译器
  - Qt >= 6.9.1（仅 Vulkan Configurator GUI 所需）
- 依赖拉取：
  - 使用 `-D UPDATE_DEPS=ON` 触发脚本下载依赖（基于 `scripts/known_good.json`）。
  - 默认 `UPDATE_DEPS=OFF`，以兼容系统包管理器。
- 编译选项：
  - `BUILD_WERROR` 默认 OFF，需显式开启以将警告视为错误。
  - `BUILD_TESTS` 控制是否构建测试。
  - `VT_CODEGEN` 控制是否启用生成代码目标。

## 代码生成约束
- 生成目录：`layersvt/generated/`。
- 生成脚本：`scripts/generate_source.py`。
- 重要约束：
  - 修改 `scripts/` 时必须运行生成脚本，并将 `layersvt/generated/` 的变更一并提交。
  - 生成不会在默认构建流程中自动触发（需显式执行脚本或使用 `vt_codegen` 目标）。

## 平台差异与构建开关
- Windows/Linux/BSD：
  - 默认构建 layers：api_dump、monitor、screenshot、fps_stdout。
  - 可构建 Vulkan Configurator（需要 Qt6）。
- Android：
  - 支持 layers：api_dump、screenshot、fps_stdout。
  - 不构建 monitor 与 Vulkan Configurator。
- macOS：
  - 支持 layers：api_dump、screenshot、fps_stdout。
  - Vulkan Configurator 仅在 Darwin 下启用且需要 Qt6。

## 测试约束
- `BUILD_TESTS` 开启后会启用 GTest 并添加 `tests/`。
- 贡献要求：提交前后运行测试，保持测试结果稳定（除非有意更改）。

## 代码风格与贡献流程
- 代码风格：Google C++ Style Guide 的变体。
- clang-format：
  - 列宽 132。
  - 缩进 4 空格。
  - 仓库内已有 `.clang-format` 文件。
- 提交信息规范：
  - 主题行 50 字符以内。
  - 以组件名作为前缀，如 `layer:` / `tests:` 等。
  - 主题行首字母大写、使用祈使语气、不以句号结尾。
  - 正文 72 字符换行，说明 “what/why”。
- 贡献流程：基于 GitHub PR，需至少一个审批，遵循 `CONTRIBUTING.md`。
- 许可：Apache 2.0。

## AI Agent 开发注意事项
- 修改 `scripts/` 或生成逻辑时，务必更新 `layersvt/generated/` 并说明生成步骤。
- Vulkan Configurator GUI 依赖 Qt6，未找到 Qt6 时会自动跳过构建。
- `BUILD_WERROR` 默认 OFF，若需更严格 CI，可显式开启但注意跨平台警告差异。
- Android 与 macOS 构建层功能有限，新增层功能需确认平台开关。
