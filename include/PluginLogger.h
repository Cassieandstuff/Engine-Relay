#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <filesystem>

// ---------------------------------------------------------------------------
// PluginLogger — shared per-plugin logging utility for the monorepo.
//
// Usage in Plugin.cpp:
//
//   SKSEPluginLoad(...) {
//       PluginLogger::Init("MyPlugin", "My Plugin Folder");
//       ...
//   }
//
// The second argument is the subfolder name under logs/ in the project
// directory. In debug builds (when PLUGIN_LOG_DIR is defined by CMake),
// logs are written to <PLUGIN_LOG_DIR>/logs/<logFolder>/<pluginName>.log
// and the last 5 sessions are kept as numbered copies (_01 = newest,
// _05 = oldest). In release builds, logs go to the standard SKSE log
// directory as before.
//
// Usage everywhere else (after PCH is included):
//
//   LOG_INFO("thing happened: {}", value);
//   LOG_WARN("watch out");
//   LOG_ERROR("oh no");
//   LOG_DEBUG("verbose detail");
//
// Each DLL gets its own logger pointer via the inline variable — no
// cross-DLL state sharing. Init() must be called before any LOG_* macro.
// Calling LOG_* before Init() is a silent no-op.
// ---------------------------------------------------------------------------

namespace PluginLogger {

    inline spdlog::logger* g_logger = nullptr;

    // Maximum number of old log files to keep in the debug log folder.
    inline constexpr int MAX_LOG_HISTORY = 5;

    // Rotate numbered logs: push existing logs up by one slot so that
    // _01 is always the newest previous session and _05 is the oldest.
    //
    //   PluginName.log      → PluginName_01.log   (previous session)
    //   PluginName_01.log   → PluginName_02.log
    //   ...
    //   PluginName_04.log   → PluginName_05.log
    //   PluginName_05.log   → deleted (falls off)
    //
    // After rotation a fresh PluginName.log is created by the sink.
    inline void RotateLogs(const std::filesystem::path& logDir,
                           const std::string& pluginName) {
        namespace fs = std::filesystem;
        std::error_code ec;

        auto numberedPath = [&](int n) -> fs::path {
            return logDir / std::format("{}_{:02d}.log", pluginName, n);
        };

        // Delete the oldest slot if it exists.
        fs::remove(numberedPath(MAX_LOG_HISTORY), ec);

        // Shift each slot up by one: (MAX-1 → MAX), (MAX-2 → MAX-1), ... (1 → 2).
        for (int i = MAX_LOG_HISTORY - 1; i >= 1; --i) {
            const auto src = numberedPath(i);
            if (fs::exists(src)) {
                fs::rename(src, numberedPath(i + 1), ec);
            }
        }

        // Move current log into slot 01.
        const auto currentLog = logDir / std::format("{}.log", pluginName);
        if (fs::exists(currentLog)) {
            fs::rename(currentLog, numberedPath(1), ec);
            // If rename fails (e.g. file locked), the new log will
            // overwrite the current one via truncate.
        }
    }

    // Call once from SKSEPluginLoad.
    //
    // pluginName: used for the log filename and spdlog logger name.
    // logFolder:  subfolder under logs/ in the project directory (debug only).
    //             Must match the folder name you created in logs/.
    inline void Init(const char* pluginName, const char* logFolder) {
        namespace fs = std::filesystem;

        fs::path logPath;

#ifdef PLUGIN_LOG_DIR
        // Debug build: write to <project>/logs/<logFolder>/<pluginName>.log
        auto logDir = fs::path(PLUGIN_LOG_DIR) / "logs" / logFolder;
        fs::create_directories(logDir);
        RotateLogs(logDir, pluginName);
        logPath = logDir / std::format("{}.log", pluginName);
#else
        // Release build: standard SKSE log directory.
        auto skseLogDir = SKSE::log::log_directory();
        if (!skseLogDir) return;
        logPath = *skseLogDir / std::format("{}.log", pluginName);
#endif

        auto sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                          logPath.string(), true);
        auto logger = std::make_shared<spdlog::logger>(pluginName, std::move(sink));

        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::trace);

        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");
        g_logger = logger.get();
    }

    // Backward-compatible overload (no log folder — uses plugin name as folder).
    inline void Init(const char* pluginName) {
        Init(pluginName, pluginName);
    }

}

#define LOG_INFO(...)  do { if (PluginLogger::g_logger) PluginLogger::g_logger->info(__VA_ARGS__);  } while(0)
#define LOG_WARN(...)  do { if (PluginLogger::g_logger) PluginLogger::g_logger->warn(__VA_ARGS__);  } while(0)
#define LOG_ERROR(...) do { if (PluginLogger::g_logger) PluginLogger::g_logger->error(__VA_ARGS__); } while(0)
#define LOG_DEBUG(...) do { if (PluginLogger::g_logger) PluginLogger::g_logger->debug(__VA_ARGS__); } while(0)
