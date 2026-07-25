// macemu - util.h
// Kleine hulpfuncties: logging, string-formatting, hex dumps.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace macemu {

enum class LogLevel : int {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
    Trace = 4,
};

// Globaal log-niveau (instelbaar via --verbose / MACEMU_LOG).
extern LogLevel g_logLevel;

void logMessage(LogLevel level, const char* fmt, ...);

#define MACEMU_LOG_ERROR(...) ::macemu::logMessage(::macemu::LogLevel::Error, __VA_ARGS__)
#define MACEMU_LOG_WARN(...) ::macemu::logMessage(::macemu::LogLevel::Warn, __VA_ARGS__)
#define MACEMU_LOG_INFO(...) ::macemu::logMessage(::macemu::LogLevel::Info, __VA_ARGS__)
#define MACEMU_LOG_DEBUG(...) ::macemu::logMessage(::macemu::LogLevel::Debug, __VA_ARGS__)
#define MACEMU_LOG_TRACE(...) ::macemu::logMessage(::macemu::LogLevel::Trace, __VA_ARGS__)

std::string strFormat(const char* fmt, ...);
std::string hexDump(const uint8_t* data, size_t len, uint64_t baseAddr = 0);

// Leest een heel bestand in. Gooit std::runtime_error als dat niet lukt.
std::vector<uint8_t> readWholeFile(const std::string& path);

// UTF-16LE -> UTF-8 (best effort, genoeg voor logging en ASCII-paden).
std::string utf16ToUtf8(const std::vector<uint16_t>& wide);
std::vector<uint16_t> utf8ToUtf16(const std::string& s);

// Basis-uitzondering voor "de geëmuleerde wereld deed iets fouts".
class EmuError : public std::runtime_error {
public:
    explicit EmuError(const std::string& what) : std::runtime_error(what) {}
};

} // namespace macemu
