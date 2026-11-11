// ============================================================================
//  Spore AutoSave On Event
//
//  Log.hpp - thin wrappers over ModAPI::Log.
//
//  ModAPI::Log writes to BOTH spore_log.txt (next to SporeApp.exe, e.g.
//  Spore2\SporebinEP1\spore_log.txt) and the in-game cheat console. The file is
//  the important one: it survives the session, so a bug report can be diagnosed
//  afterwards. Never use App::ConsolePrintF here - it only reaches the console.
//
//  Every line is prefixed so the mod's output is easy to grep:
//      findstr /C:"[AutoSaveOnEvent]" spore_log.txt
// ============================================================================
#pragma once

#include <cstdio>

namespace Log
{
    /// printf-style logging into spore_log.txt and the cheat console.
    inline void Print(const char* fmt, ...)
    {
        char buffer[1024];

        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        // ModAPI::Log adds its own "[hh:mm:ss]: " elapsed-time prefix and flushes
        // the file after every line, so the log is complete even if the game dies.
        ModAPI::Log("[AutoSaveOnEvent] %s", buffer);
    }

    /// Same as Print(), but takes a wide string (used for filesystem paths).
    inline void PrintW(const wchar_t* prefix, const wchar_t* detail)
    {
        // The log is byte-oriented; paths are ASCII in practice, so a plain
        // narrowing conversion keeps this simple and dependency free.
        char prefixNarrow[256] = { 0 };
        char detailNarrow[MAX_PATH] = { 0 };

        if (prefix != nullptr)
        {
            WideCharToMultiByte(CP_ACP, 0, prefix, -1, prefixNarrow, (int)sizeof(prefixNarrow) - 1, nullptr, nullptr);
        }
        if (detail != nullptr)
        {
            WideCharToMultiByte(CP_ACP, 0, detail, -1, detailNarrow, (int)sizeof(detailNarrow) - 1, nullptr, nullptr);
        }

        ModAPI::Log("[AutoSaveOnEvent] %s%s", prefixNarrow, detailNarrow);
    }
}

// Wrappers so call sites stay terse. The format string must be a literal.
#define MOD_LOG(...)     Log::Print(__VA_ARGS__)
#define MOD_LOG_W(a, b)  Log::PrintW((a), (b))
#define MOD_LOG_ERR(...) Log::Print("ERROR: " __VA_ARGS__)
