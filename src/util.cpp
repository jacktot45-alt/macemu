#include "macemu/util.h"

#include <cstring>
#include <fstream>

namespace macemu {

LogLevel g_logLevel = LogLevel::Info;

static const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Trace: return "TRACE";
    }
    return "?????";
}

void logMessage(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) > static_cast<int>(g_logLevel)) return;
    std::fprintf(stderr, "[%s] ", levelName(level));
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

std::string strFormat(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    std::vsnprintf(&out[0], static_cast<size_t>(n) + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

std::string hexDump(const uint8_t* data, size_t len, uint64_t baseAddr) {
    std::string out;
    for (size_t i = 0; i < len; i += 16) {
        out += strFormat("%016llx  ", static_cast<unsigned long long>(baseAddr + i));
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len)
                out += strFormat("%02x ", data[i + j]);
            else
                out += "   ";
            if (j == 7) out += ' ';
        }
        out += " |";
        for (size_t j = 0; j < 16 && i + j < len; ++j) {
            uint8_t c = data[i + j];
            out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
        }
        out += "|\n";
    }
    return out;
}

std::vector<uint8_t> readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw EmuError("kan bestand niet openen: " + path);
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 0) throw EmuError("kan bestandsgrootte niet bepalen: " + path);
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(buf.data()), size);
    if (!f) throw EmuError("lezen mislukt: " + path);
    return buf;
}

std::string utf16ToUtf8(const std::vector<uint16_t>& wide) {
    std::string out;
    for (size_t i = 0; i < wide.size(); ++i) {
        uint32_t cp = wide[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 &&
            wide[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (wide[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::vector<uint16_t> utf8ToUtf16(const std::string& s) {
    std::vector<uint16_t> out;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp;
        size_t extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = '?'; extra = 0; }
        if (i + extra >= s.size()) extra = 0;
        for (size_t k = 1; k <= extra; ++k)
            cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        i += extra + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(cp));
        }
    }
    return out;
}

} // namespace macemu
