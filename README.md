# LOG — 跨平台异步日志库

一个单类、零第三方依赖的 C++ 异步日志库，Windows 与 Linux 通用。业务线程调用 `writeLog` 只把日志投递到线程安全队列后立即返回，由一个后台线程负责格式化落盘、文件轮转与清理，绝不阻塞业务。

## 特性

- **异步、不阻塞**：前台只做级别过滤 + 格式化 + 入队；单个后台线程批量落盘（swap-drain，锁外写文件）。
- **随首次使用自动启动**，进程退出时优雅关闭（flush + join，`atexit` 兜底）。
- **配置驱动**：只通过 `LogConfig` 结构体注入，库本身**不解析任何配置文件 / 命令行**（配置文件是调用方的事）。
- **文件轮转 / 保留**：按单文件最大大小滚动（`app.log → app.log.1 → .2 …`）、备份数量上限、按天删除过期备份，全部在后台线程完成。
- **背压**：队列满时丢弃最新并计数（`drop-newest`），保证前台永不阻塞。
- **毫秒级本地时间戳**，跨平台线程安全（`localtime_s` / `localtime_r`）。
- **跨平台适配**：程序自身名自省（`GetModuleFileNameA` / `/proc/self/exe`）、`<filesystem>` 且带 `stat`/`rename` 退路、二进制写入统一 `\n`。
- **有意泄漏的单例**：避免静态析构顺序导致的 use-after-free。

## 构建

无构建系统，直接用编译器编译 `clog.cpp` + 你的源码：

```bash
# Linux
g++ -std=c++17 -O2 -pthread clog.cpp your_app.cpp -o your_app

# Windows (MinGW-W64)，-static 产出独立 exe
g++ -std=c++17 -O2 -pthread -static clog.cpp your_app.cpp -o your_app.exe

# 无 C++17 <filesystem> 时的退路（用 stat/rename 实现轮转）
g++ -std=c++11 -O2 -pthread -DLOG_NO_FILESYSTEM clog.cpp your_app.cpp -o your_app
```

> 需要 C++11 及以上（`std::thread` 等）；轮转的时间清理在 C++17 下用 `<filesystem>`，否则走 `-DLOG_NO_FILESYSTEM` 退路。

## 快速上手

```cpp
#include "clog.h"

int main(int argc, char** argv)
{
    // 方式一：直接用，首次打印即自动启动，日志文件默认 = 程序自身名 + ".log"
    LOG_INFO("service started, argc=%d", argc);

    // 方式二：显式配置后再用
    LogConfig cfg;
    cfg.level          = DEBUG;          // 低于该级别的日志被丢弃
    cfg.filePath       = "svc.log";      // 留空则用 LogConfig::defaultLogPath()
    cfg.maxFileSize    = 8 * 1024 * 1024;
    cfg.maxBackupCount = 5;
    cfg.maxBackupDays  = 30;
    Log::getInstance().init(cfg);        // 注入配置 + 启动后台线程（幂等）

    LOG_WARN("value=%d name=%s", 42, "abc");
    LOG_ERROR("open failed: errno=%d", 2);   // >= flushLevel，所在批立即落盘

    Log::getInstance().shutdown();       // 可选：显式优雅关闭（进程退出也会兜底）
    return 0;
}
```

日志行格式：

```
2026-07-11 23:42:51.417 INFO | service started, argc=1
```

## 配置项（`LogConfig`）

| 字段 | 默认值 | 说明 |
|---|---|---|
| `enabled` | `true` | 总开关 |
| `level` | `INFO` | 日志级别阈值，低于此级别丢弃 |
| `filePath` | 空 → 程序自身名 `.log` | 日志文件路径 |
| `maxFileSize` | `10 MiB` | 单文件字节上限；`0` = 不按大小轮转 |
| `maxBackupCount` | `5` | 备份数量上限；`0` = 不保留备份 |
| `maxBackupDays` | `30` | 超过此天数的备份被删除；`0` = 不按时间清理 |
| `maxQueueSize` | `100000` | 队列高水位；`0` = 无上限（不丢弃） |
| `flushLevel` | `ERROR` | ≥ 此级别的日志所在批立即 `fflush` |
| `flushIntervalMs` | `1000` | 后台线程周期唤醒 / flush 间隔（毫秒，最小 1） |

级别：`TRACE < DEBUG < INFO < WARN < ERROR < FATAL`。

## API

| 接口 | 说明 |
|---|---|
| `Log& Log::getInstance()` | 获取全局单例 |
| `void init(const LogConfig&)` | 注入配置并启动后台线程（幂等；运行期可重配） |
| `void writeLog(LogLevel, const char* fmt, ...)` | `printf` 风格写日志（前台非阻塞投递） |
| `LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL(...)` | `writeLog` 的便捷宏 |
| `void flush()` | 阻塞直到队列清空且已落盘 |
| `void shutdown()` | 优雅关闭：drain + flush + join（幂等） |
| `void triggerMaintenance()` | 请求后台线程执行一次轮转/过期清理 |
| `LogStats stats() const` | 读取可靠计数快照：`enqueuedOk / written / dropped / writeErrors` |

## 示例 / 验证原型

`log.cpp` 是一个多线程验证原型（同时充当测试）：4 线程 × 2000 条突发写入，覆盖账目守恒、大小轮转、按时间清理、队列背压、自动启动等场景，每个场景带断言。构建并运行：

```bash
g++ -std=c++17 -O2 -pthread -static clog.cpp log.cpp -o logdemo && ./logdemo
```

## 设计文档

完整技术设计（架构、并发模型、跨平台适配、轮转算法、验收标准）见 [`DESIGN.md`](DESIGN.md)。
