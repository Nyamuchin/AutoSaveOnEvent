// ============================================================================
//  Spore AutoSave On Event
//
//  Config.cpp - INI handling on top of GetPrivateProfileStringW /
//  WritePrivateProfileStringW.
//
//  Spore hands out paths as `const char16_t*`, which std::filesystem::path
//  accepts directly, so we keep a path object and only convert to wstring when
//  calling the Win32 profile APIs.
// ============================================================================
#include "stdafx.h"
#include "Config.hpp"
#include "Log.hpp"

#include <filesystem>
#include <cstdlib>

#define APP_NAME L"AutoSaveOnEvent"
#define CONFIG_FILE_NAME L"AutoSaveOnEvent.ini"

namespace
{
    // Allocated only after a successful Initialize(); Config::Destroy frees it.
    std::filesystem::path* gConfigPath = nullptr;

    /// Every setting the mod knows about, paired with the value written into a
    /// fresh file.
    ///
    /// One table rather than two lists, so "create the defaults" and "add the
    /// keys a newer version introduced" can never disagree about which keys
    /// exist or what they default to.
    struct SettingDefault
    {
        const wchar_t* keyName;
        const wchar_t* defaultValue;
    };

    const SettingDefault kSettingDefaults[] =
    {
        // Master switch.
        { L"EnableAutoSave", L"1" },

        // Optional global throttle: never save more often than this many seconds.
        // Off by default. A trigger is something the player did, and a save that
        // arrives a minute later is not a save of that moment any more - the whole
        // point of saving on an event is that the event's state is on disk before
        // the next thing happens. Bursts are already collapsed by the debounce
        // below and by the pending table, so this only ever delayed real saves.
        { L"MinimumSecondsBetweenSaves", L"0" },

        // Debounce for editor enter/exit. The editor opening is often followed by a
        // second mode change a fraction of a second later; this collapses the noise
        // and lets the world settle before we save.
        { L"EditorExitDelaySeconds", L"2" },

        // How often the milestone trackers are sampled, in seconds.
        { L"PollingIntervalSeconds", L"1" },

        // Per-stage toggles.
        { L"SaveInCellStage", L"1" },
        { L"SaveInCreatureStage", L"1" },
        { L"SaveInTribeStage", L"1" },
        { L"SaveInCivilizationStage", L"1" },
        { L"SaveInSpaceStage", L"1" },

        // Per-event toggles.
        { L"SaveOnEditorEnter", L"1" },
        { L"SaveOnEditorExit", L"1" },
        { L"SaveOnCreatureRankUp", L"1" },
        { L"SaveOnTribeAlliance", L"1" },
        { L"SaveOnTribeDefeat", L"1" },
        { L"SaveOnCityCapture", L"1" },
        { L"SaveOnAdventureEnter", L"1" },
        { L"SaveOnAdventureExit", L"1" },

        // How the automatic save reports itself. The game normally pops up a
        // modal "your game has been saved" box that has to be clicked away;
        // 1 replaces it with a short fading line in the top right corner,
        // 0 keeps the box suppressed and shows nothing at all.
        { L"ShowSaveNotification", L"1" },
    };

    /// True when `keyName` has no value at all in the file.
    ///
    /// GetPrivateProfileStringW copies the supplied default into the buffer when
    /// the key is missing and also when the key exists but is empty, so the
    /// default passed here is a sentinel no user would realistically write:
    /// seeing it come back means "this key is not in the file".
    bool IsKeyAbsent(const wchar_t* keyName)
    {
        if (gConfigPath == nullptr)
        {
            return false;
        }

        static const wchar_t kSentinel[] = L"\x0001<absent>\x0001";

        const std::wstring path = gConfigPath->wstring();

        wchar_t buffer[MAX_PATH] = { 0 };
        GetPrivateProfileStringW(APP_NAME, keyName, kSentinel,
            buffer, MAX_PATH, path.c_str());

        return std::wstring(buffer) == kSentinel;
    }

    /// Writes every setting. Only called when the file does not exist yet.
    void WriteDefaults()
    {
        for (const SettingDefault& setting : kSettingDefaults)
        {
            Config::SetValue(setting.keyName, setting.defaultValue);
        }
    }

    /// Adds the settings a newer version introduced, leaving every value the
    /// user has already set untouched.
    void WriteMissingDefaults()
    {
        for (const SettingDefault& setting : kSettingDefaults)
        {
            if (!IsKeyAbsent(setting.keyName))
            {
                continue;
            }

            Config::SetValue(setting.keyName, setting.defaultValue);
            MOD_LOG_W(L"Settings file was missing ", setting.keyName);
        }
    }
}

namespace Config
{
    bool Initialize()
    {
        if (gConfigPath != nullptr)
        {
            return true;
        }

        auto* preferences = Resource::Paths::GetSaveArea(Resource::SaveAreaID::Preferences);
        if (preferences == nullptr || preferences->GetLocation() == nullptr)
        {
            MOD_LOG_ERR("Preferences directory is unavailable; settings disabled.");
            return false;
        }

        auto* path = new std::filesystem::path(preferences->GetLocation());
        if (path->empty())
        {
            MOD_LOG_ERR("Preferences directory is empty; settings disabled.");
            delete path;
            return false;
        }

        *path /= CONFIG_FILE_NAME;
        gConfigPath = path;

        std::error_code ec;
        if (!std::filesystem::is_regular_file(*gConfigPath, ec))
        {
            WriteDefaults();
            MOD_LOG_W(L"Created default settings at ", gConfigPath->c_str());
        }
        else
        {
            // The file was written by an earlier version. Settings added since
            // then are missing from it, and a missing key falls back to the
            // built-in default - which works, but means the user never sees the
            // option in their file and cannot discover it. Append only the keys
            // that are absent; never touch a value the user has set.
            WriteMissingDefaults();
        }

        return true;
    }

    void Destroy()
    {
        delete gConfigPath;
        gConfigPath = nullptr;
    }

    std::wstring GetValue(const std::wstring& keyName, const std::wstring& defaultValue)
    {
        if (gConfigPath == nullptr)
        {
            return defaultValue;
        }

        const std::wstring path = gConfigPath->wstring();

        wchar_t buffer[MAX_PATH] = { 0 };
        GetPrivateProfileStringW(APP_NAME, keyName.c_str(), defaultValue.c_str(),
            buffer, MAX_PATH, path.c_str());

        return std::wstring(buffer);
    }

    bool SetValue(const std::wstring& keyName, const std::wstring& value)
    {
        if (gConfigPath == nullptr)
        {
            return false;
        }

        const std::wstring path = gConfigPath->wstring();

        return WritePrivateProfileStringW(APP_NAME, keyName.c_str(), value.c_str(),
            path.c_str()) != 0;
    }

    bool GetBool(const std::wstring& keyName, bool defaultValue)
    {
        const std::wstring value = GetValue(keyName, defaultValue ? L"1" : L"0");
        if (value.empty())
        {
            return defaultValue;
        }
        return value != L"0" && value != L"false" && value != L"False";
    }

    int GetInt(const std::wstring& keyName, int defaultValue)
    {
        const std::wstring value = GetValue(keyName, L"");
        if (value.empty())
        {
            return defaultValue;
        }

        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        if (end == value.c_str())
        {
            return defaultValue;
        }
        return static_cast<int>(parsed);
    }

    std::wstring GetConfigPath()
    {
        return gConfigPath != nullptr ? gConfigPath->wstring() : std::wstring();
    }
}
