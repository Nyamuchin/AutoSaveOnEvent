// ============================================================================
//  Spore AutoSave On Event
//
//  Config.hpp - persistent settings, stored in the Spore preferences folder as
//  "AutoSaveOnEvent.ini" (same approach as the reference SporeAutoSave mod).
// ============================================================================
#pragma once

#include <string>

namespace Config
{
    /// Creates the .ini with defaults if it does not exist yet, and caches its path.
    /// Safe to call more than once.
    bool Initialize();

    /// Releases cached state. Called when the game shuts down.
    void Destroy();

    /// Returns the value for `keyName`, or `defaultValue` when the key is missing.
    std::wstring GetValue(const std::wstring& keyName, const std::wstring& defaultValue);

    /// Writes `value` under `keyName`. Returns false on failure.
    bool SetValue(const std::wstring& keyName, const std::wstring& value);

    /// Convenience: reads a boolean written as 0/1. Missing keys use `defaultValue`.
    bool GetBool(const std::wstring& keyName, bool defaultValue);

    /// Convenience: reads an integer, falling back to `defaultValue` on garbage input.
    int GetInt(const std::wstring& keyName, int defaultValue);

    /// Absolute path of the ini file, valid after a successful Initialize().
    std::wstring GetConfigPath();
}
