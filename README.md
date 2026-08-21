# CClaw SDK

CClaw SDK 是独立的纯 C Agent Runtime。它提供可移植的 core、ports、adapters
和平台适配，不包含任何具体产品的 `main/`、板级驱动或 ESP32S31 业务逻辑。

## 仓库边界

- `cclaw/core/`：Agent runtime、上下文、会话、事件、结果和基础设施。
- `cclaw/ports/`：文件系统、HTTP、LLM、存储、工具等抽象接口。
- `cclaw/adapters/`：通用 provider、存储和通用工具实现。
- `cclaw/platforms/`：POSIX、Windows、FreeRTOS、ESP32 平台适配。
- `cclaw/tests/`：SDK 级测试。
- `cmake/`：SDK 安装和下游 `find_package(CClaw)` 支持。

ESP32S31 的摄像头、LCD、USB、Lua、OTA、Web 控制台和产品配置属于独立的
`C_Claw_ESP32S31` 仓库，不应回写到本仓库。

## 构建

```bash
cmake -S . -B build/core-minimal -DCC_PROFILE=core-minimal -DCC_BUILD_TESTS=ON
cmake --build build/core-minimal
ctest --test-dir build/core-minimal --output-on-failure
```

ESP32 平台需要在 ESP-IDF 工程中配置；产品仓库通过固定版本的 SDK 快照接入。

## 版本

产品仓库必须依赖发布 tag 或固定 commit，不依赖 SDK 的浮动 `main` 分支。

当前目录是从 CClaw 工程整理出的独立 SDK 初始快照；构建和安装验证尚未在本目录执行。

## License

MIT License，详见根目录 `LICENSE`。`cclaw/` 内的第三方组件仍以其各自的许可证为准。
