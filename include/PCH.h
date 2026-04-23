#pragma once

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <format>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

// CommonLib MUST come before Windows.h
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"
#include "SKSE/Impl/PCH.h"

// Windows.h AFTER CommonLib
#include <Windows.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

// PrismaUI — every plugin in this monorepo either uses it now or will.
#include "PrismaUI/PrismaUI_API.h"

// CLibUtilsQTR utilities
#include "CLibUtilsQTR/StringHelpers.hpp"
#include "CLibUtilsQTR/FormReader.hpp"

// Shared per-plugin logger — call PluginLogger::Init("Name") in Plugin.cpp.
// Provides LOG_INFO / LOG_WARN / LOG_ERROR / LOG_DEBUG macros everywhere.
#include "PluginLogger.h"

using namespace std::literals;

