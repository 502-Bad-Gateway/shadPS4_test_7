// SPDX-FileCopyrightText: 2026 shadPS4 Windows 7 launcher contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <process.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "third_party/nlohmann/json.hpp"

using json = nlohmann::ordered_json;

namespace {

constexpr wchar_t kWindowClass[] = L"ShadPS4Win7LauncherWindow";
constexpr wchar_t kLauncherTitle[] = L"shadPS4 Windows 7 Launcher v2";
constexpr UINT WM_RUN_FINISHED = WM_APP + 1;

enum ControlId : int {
    ID_GAME_PATH = 100,
    ID_BROWSE,
    ID_SCOPE,
    ID_SECTION,
    ID_FILTER,
    ID_SETTINGS_LIST,
    ID_VALUE,
    ID_APPLY,
    ID_TOGGLE,
    ID_RESET,
    ID_RELOAD,
    ID_OPEN_JSON,
    ID_OPEN_DATA,
    ID_LAUNCH,
    ID_OPEN_RESULTS,
};

struct GameInfo {
    std::wstring title_id;
    std::wstring title;
    std::wstring sfo_path;
};

struct SettingRow {
    std::string section;
    std::string key;
    json value;
    bool overridden = false;
};

struct ValueOption {
    std::wstring label;
    json value;
};

struct GpuModeInfo {
    bool null_gpu = false;
    bool safe_gpu = false;
    std::wstring name = L"vulkan";
};

struct RunContext {
    HANDLE process = nullptr;
    std::wstring result_dir;
    std::wstring data_root;
    std::wstring exe_dir;
    std::wstring global_config;
    std::wstring profile_config;
    std::wstring title_id;
    std::wstring title;
    std::wstring eboot;
    std::wstring mode;
    std::wstring started_at;
    bool null_gpu = false;
    bool safe_gpu = false;
    uint64_t shadps4_log_start = 0;
    uint64_t shad_log_start = 0;
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_game_path = nullptr;
HWND g_browse = nullptr;
HWND g_game_info = nullptr;
HWND g_scope = nullptr;
HWND g_section = nullptr;
HWND g_filter = nullptr;
HWND g_settings = nullptr;
HWND g_value = nullptr;
HWND g_apply = nullptr;
HWND g_toggle = nullptr;
HWND g_reset = nullptr;
HWND g_reload = nullptr;
HWND g_open_json = nullptr;
HWND g_open_data = nullptr;
HWND g_launch = nullptr;
HWND g_open_results = nullptr;
HWND g_status = nullptr;
HFONT g_font = nullptr;

std::wstring g_exe_dir;
std::wstring g_shadps4_exe;
std::wstring g_state_path;
std::wstring g_data_root;
std::wstring g_global_config_path;
std::wstring g_profile_config_path;
std::wstring g_eboot_path;
std::wstring g_last_result;
std::vector<std::wstring> g_recent_games;
GameInfo g_detected_game;
json g_global_config = json::object();
json g_profile_config = json::object();
json g_effective_config = json::object();
json g_launcher_state = json::object();
std::vector<SettingRow> g_rows;
std::vector<SettingRow> g_visible_rows;
bool g_global_config_exists = false;
bool g_profile_config_exists = false;
bool g_running = false;

std::wstring ParentPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    std::wstring value = path;
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    const auto pos = value.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    if (pos == 2 && value.size() >= 3 && value[1] == L':') {
        return value.substr(0, 3);
    }
    return value.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectory(const std::wstring& path) {
    if (DirectoryExists(path)) {
        return true;
    }
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_FILE_EXISTS ||
           result == ERROR_ALREADY_EXISTS;
}

std::wstring GetModuleDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return L".";
    }
    return ParentPath(std::wstring(buffer.data(), length));
}

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(code_page, flags, input.data(),
                                     static_cast<int>(input.size()), nullptr, 0);
    if (length == 0) {
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(code_page, flags, input.data(),
                                     static_cast<int>(input.size()), nullptr, 0);
    }
    if (length <= 0) {
        return L"(invalid text)";
    }
    std::wstring output(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(code_page, flags, input.data(), static_cast<int>(input.size()),
                        output.data(), length);
    return output;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, input.data(),
                                           static_cast<int>(input.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
                        output.data(), length, nullptr, nullptr);
    return output;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring Trim(std::wstring value) {
    const auto is_space = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    while (!value.empty() && is_space(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring GetWindowTextString(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(control, value.data(), length + 1);
    }
    value.resize(static_cast<size_t>(length));
    return value;
}

std::optional<std::string> ReadFileBytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, static_cast<size_t>(1u << 20)));
        DWORD read = 0;
        if (!ReadFile(file, data.data() + offset, chunk, &read, nullptr) || read == 0) {
            CloseHandle(file);
            return std::nullopt;
        }
        offset += read;
    }
    CloseHandle(file);
    return data;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data, bool backup_existing,
                    std::wstring* error) {
    const std::wstring parent = ParentPath(path);
    if (!parent.empty() && !EnsureDirectory(parent)) {
        if (error) {
            *error = L"Unable to create directory:\n" + parent;
        }
        return false;
    }

    if (backup_existing && FileExists(path)) {
        CopyFileW(path.c_str(), (path + L".launcher-backup").c_str(), FALSE);
    }

    const std::wstring temporary = path + L".launcher-tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = L"Unable to create temporary file:\n" + temporary;
        }
        return false;
    }
    size_t offset = 0;
    bool ok = true;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(data.size() - offset, static_cast<size_t>(1u << 20)));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) {
        ok = FlushFileBuffers(file) != FALSE;
    }
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        if (error) {
            *error = L"Unable to replace file:\n" + path;
        }
        return false;
    }
    return true;
}

json DefaultConfig() {
    return json::parse(R"JSON({
  "Audio": {
    "audio_backend": 0,
    "openal_hrtf": 0,
    "openal_main_output_device": "Default Device",
    "openal_mic_device": "Default Device",
    "openal_output_mode": 0,
    "openal_padSpk_output_device": "Default Device",
    "sdl_main_output_device": "Default Device",
    "sdl_mic_device": "Default Device",
    "sdl_padSpk_output_device": "Default Device"
  },
  "Debug": {
    "config_version": "7fb1a530c15415097836521fec6f3483e27c81ae",
    "debug_dump": false,
    "shader_collect": false
  },
  "GPU": {
    "copy_gpu_buffers": false,
    "direct_memory_access_enabled": false,
    "dump_shaders": false,
    "fsr_enabled": false,
    "full_screen": false,
    "full_screen_mode": "Windowed",
    "hdr_allowed": false,
    "internal_screen_height": 720,
    "internal_screen_width": 1280,
    "null_gpu": false,
    "safe_gpu": false,
    "patch_shaders": false,
    "present_mode": "Mailbox",
    "rcas_attenuation": 250,
    "rcas_enabled": true,
    "readback_linear_images_enabled": false,
    "readbacks_mode": 0,
    "vblank_frequency": 60,
    "window_height": 720,
    "window_width": 1280
  },
  "General": {
    "addon_install_dir": "",
    "big_picture_scale": 1000,
    "connected_to_network": false,
    "console_language": 1,
    "dev_kit_mode": false,
    "discord_rpc_enabled": false,
    "enable_upnp": true,
    "extra_dmem_in_mbytes": 0,
    "extra_fmem_in_mbytes": 0,
    "font_dir": "",
    "home_dir": "",
    "install_dirs": [],
    "neo_mode": false,
    "shad_net_enabled": false,
    "shadnet_server": "srv.shadps4.net:31313",
    "shadnet_webapi_server": "http://srv.shadps4.net:31315",
    "show_fps_counter": true,
    "show_splash": true,
    "signaling_info": "",
    "sys_modules_dir": "",
    "trophy_notification_duration": 6.0,
    "trophy_notification_side": "right",
    "trophy_popup_disabled": false,
    "volume_slider": 100
  },
  "Input": {
    "background_controller_input": false,
    "camera_id": -1,
    "cursor_hide_timeout": 5,
    "cursor_state": 1,
    "default_controller_id": "",
    "ime_accessibility_enabled": false,
    "ime_url_mail_short_panel": false,
    "is_circle_enter": false,
    "motion_controls_enabled": true,
    "special_pad_class": 1,
    "usb_device_backend": 0,
    "use_mice_as_mice": false,
    "use_special_pad": false,
    "use_unified_input_config": true
  },
  "Log": {
    "append": false,
    "enable": true,
    "filter": "",
    "flush_level": "",
    "max_skip_duration": 5000,
    "separate": false,
    "size_limit": 104857600,
    "skip_duplicate": true,
    "sync": true,
    "type": "wincolor"
  },
  "Vulkan": {
    "gpu_id": -1,
    "pipeline_cache_archived": false,
    "pipeline_cache_enabled": false,
    "renderdoc_enabled": false,
    "vkcrash_diagnostic_enabled": false,
    "vkguest_markers": false,
    "vkhost_markers": false,
    "vkvalidation_core_enabled": true,
    "vkvalidation_enabled": false,
    "vkvalidation_gpu_enabled": false,
    "vkvalidation_sync_enabled": false
  }
})JSON");
}

bool LoadJson(const std::wstring& path, json* output, std::wstring* error) {
    const auto bytes = ReadFileBytes(path);
    if (!bytes) {
        if (error) {
            *error = L"Unable to read:\n" + path;
        }
        return false;
    }
    try {
        *output = json::parse(*bytes);
        if (!output->is_object()) {
            if (error) {
                *error = L"JSON root is not an object:\n" + path;
            }
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = L"Invalid JSON in:\n" + path + L"\n\n" + Utf8ToWide(exception.what());
        }
        return false;
    }
}

bool SaveJson(const std::wstring& path, const json& value, bool backup,
              std::wstring* error) {
    return WriteFileBytes(path, value.dump(2) + "\n", backup, error);
}

std::wstring FindCusa(const std::wstring& text) {
    for (size_t i = 0; i + 9 <= text.size(); ++i) {
        if (std::towupper(text[i]) != L'C' || std::towupper(text[i + 1]) != L'U' ||
            std::towupper(text[i + 2]) != L'S' || std::towupper(text[i + 3]) != L'A') {
            continue;
        }
        bool digits = true;
        for (size_t j = 4; j < 9; ++j) {
            if (!std::iswdigit(text[i + j])) {
                digits = false;
                break;
            }
        }
        if (digits) {
            std::wstring result = text.substr(i, 9);
            std::transform(result.begin(), result.begin() + 4, result.begin(),
                           [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
            return result;
        }
    }
    return {};
}

uint16_t ReadLe16(const std::string& data, size_t offset) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(static_cast<uint8_t>(data[offset])) |
        static_cast<uint16_t>(static_cast<uint16_t>(static_cast<uint8_t>(data[offset + 1]))
                              << 8));
}

uint32_t ReadLe32(const std::string& data, size_t offset) {
    return static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3])) << 24);
}

bool ParseSfo(const std::wstring& path, GameInfo* info) {
    const auto bytes = ReadFileBytes(path);
    if (!bytes || bytes->size() < 20 || ReadLe32(*bytes, 0) != 0x46535000u) {
        return false;
    }
    const uint32_t key_offset = ReadLe32(*bytes, 8);
    const uint32_t data_offset = ReadLe32(*bytes, 12);
    const uint32_t entry_count = ReadLe32(*bytes, 16);
    if (entry_count > 4096 || key_offset >= bytes->size() || data_offset >= bytes->size() ||
        20ull + static_cast<uint64_t>(entry_count) * 16ull > bytes->size()) {
        return false;
    }

    for (uint32_t index = 0; index < entry_count; ++index) {
        const size_t entry = 20 + static_cast<size_t>(index) * 16;
        const uint16_t key_relative = ReadLe16(*bytes, entry);
        const uint32_t value_length = ReadLe32(*bytes, entry + 4);
        const uint32_t value_relative = ReadLe32(*bytes, entry + 12);
        const size_t key_position = static_cast<size_t>(key_offset) + key_relative;
        const size_t value_position = static_cast<size_t>(data_offset) + value_relative;
        if (key_position >= bytes->size() || value_position >= bytes->size()) {
            continue;
        }
        const size_t key_end = bytes->find('\0', key_position);
        if (key_end == std::string::npos) {
            continue;
        }
        const std::string key = bytes->substr(key_position, key_end - key_position);
        const size_t safe_length = std::min<size_t>(value_length, bytes->size() - value_position);
        std::string value = bytes->substr(value_position, safe_length);
        while (!value.empty() && value.back() == '\0') {
            value.pop_back();
        }
        if (key == "TITLE_ID") {
            info->title_id = FindCusa(Utf8ToWide(value));
        } else if (key == "TITLE") {
            info->title = Trim(Utf8ToWide(value));
        }
    }
    info->sfo_path = path;
    return !info->title_id.empty() || !info->title.empty();
}

GameInfo DetectGame(const std::wstring& eboot) {
    GameInfo result;
    std::wstring directory = ParentPath(eboot);
    for (int depth = 0; depth < 5 && !directory.empty(); ++depth) {
        const std::wstring candidate = JoinPath(JoinPath(directory, L"sce_sys"), L"param.sfo");
        if (FileExists(candidate) && ParseSfo(candidate, &result)) {
            break;
        }
        const std::wstring parent = ParentPath(directory);
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    if (result.title_id.empty()) {
        result.title_id = FindCusa(eboot);
    }
    if (result.title.empty()) {
        result.title = L"Unknown title";
    }
    return result;
}

bool IsOverrideable(const std::string& section, const std::string& key) {
    static const std::vector<std::pair<std::string, std::set<std::string>>> fields = {
        {"General",
         {"volume_slider", "neo_mode", "dev_kit_mode", "extra_dmem_in_mbytes",
          "extra_fmem_in_mbytes", "shad_net_enabled", "trophy_popup_disabled",
          "trophy_notification_duration", "show_splash", "trophy_notification_side",
          "connected_to_network", "console_language", "shadnet_server",
          "shadnet_webapi_server", "signaling_info", "enable_upnp"}},
        {"Log",
         {"append", "enable", "filter", "flush_level", "max_skip_duration", "separate",
          "size_limit", "skip_duplicate", "sync", "type"}},
        {"Debug", {"debug_dump", "shader_collect"}},
        {"Input",
         {"cursor_state", "cursor_hide_timeout", "usb_device_backend",
          "motion_controls_enabled", "background_controller_input",
          "ime_accessibility_enabled", "ime_url_mail_short_panel", "is_circle_enter",
          "camera_id", "use_mice_as_mice"}},
        {"Audio",
         {"audio_backend", "sdl_mic_device", "sdl_main_output_device",
          "sdl_padSpk_output_device", "openal_mic_device", "openal_main_output_device",
          "openal_padSpk_output_device", "openal_hrtf", "openal_output_mode"}},
        {"WindowsGuestRedZoneProtection", {"windows_guest_red_zone_protection_mode"}},
        {"GPU",
         {"null_gpu", "safe_gpu", "copy_gpu_buffers", "full_screen", "full_screen_mode",
          "present_mode", "window_height", "window_width", "hdr_allowed", "fsr_enabled",
          "rcas_enabled", "rcas_attenuation", "dump_shaders", "patch_shaders",
          "readbacks_mode", "readback_linear_images_enabled",
          "direct_memory_access_enabled", "vblank_frequency"}},
        {"Vulkan",
         {"gpu_id", "renderdoc_enabled", "vkvalidation_enabled",
          "vkvalidation_core_enabled", "vkvalidation_sync_enabled",
          "vkvalidation_gpu_enabled", "vkcrash_diagnostic_enabled", "vkhost_markers",
          "vkguest_markers", "pipeline_cache_enabled", "pipeline_cache_archived"}},
    };
    for (const auto& group : fields) {
        if (group.first == section) {
            return group.second.find(key) != group.second.end();
        }
    }
    return false;
}

std::wstring DetermineDataRoot() {
    const std::wstring portable = JoinPath(g_exe_dir, L"user");
    if (DirectoryExists(portable)) {
        return portable;
    }
    wchar_t app_data[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                                   app_data))) {
        return JoinPath(app_data, L"shadPS4");
    }
    return portable;
}

void DeepMerge(json* target, const json& overrides) {
    if (!overrides.is_object()) {
        return;
    }
    for (const auto& section : overrides.items()) {
        if (section.value().is_object()) {
            if (!target->contains(section.key()) || !(*target)[section.key()].is_object()) {
                (*target)[section.key()] = json::object();
            }
            for (const auto& entry : section.value().items()) {
                (*target)[section.key()][entry.key()] = entry.value();
            }
        } else {
            (*target)[section.key()] = section.value();
        }
    }
}

std::wstring JsonDisplay(const json& value) {
    if (value.is_string()) {
        return Utf8ToWide(value.get<std::string>());
    }
    return Utf8ToWide(value.dump());
}

std::optional<json> ParseEditedValue(const std::wstring& text, const json& original,
                                     std::wstring* error);

std::vector<ValueOption> OptionsFor(const SettingRow& row) {
    if (row.value.is_boolean()) {
        return {{L"false", false}, {L"true", true}};
    }
    if (row.section == "Audio" && row.key == "audio_backend") {
        return {{L"0 - SDL", 0}, {L"1 - OpenAL", 1}};
    }
    if (row.section == "Audio" && row.key == "openal_hrtf") {
        return {{L"0 - Auto", 0}, {L"1 - On", 1}, {L"2 - Off", 2}};
    }
    if (row.section == "Audio" && row.key == "openal_output_mode") {
        return {{L"0 - Auto", 0}, {L"1 - Stereo", 1}, {L"2 - Quad", 2},
                {L"3 - Surround 5.1", 3}, {L"4 - Surround 7.1", 4}};
    }
    if (row.section == "GPU" && row.key == "readbacks_mode") {
        return {{L"0 - Disabled", 0}, {L"1 - Relaxed", 1}, {L"2 - Precise", 2}};
    }
    if (row.section == "Input" && row.key == "cursor_state") {
        return {{L"0 - Never", 0}, {L"1 - Idle", 1}, {L"2 - Always", 2}};
    }
    if (row.section == "Input" && row.key == "usb_device_backend") {
        return {{L"0 - Real USB", 0}, {L"1 - Skylanders Portal", 1},
                {L"2 - Infinity Base", 2}, {L"3 - Dimensions Toypad", 3}};
    }
    if (row.section == "GPU" && row.key == "full_screen_mode") {
        return {{L"Windowed", "Windowed"}, {L"Fullscreen", "Fullscreen"},
                {L"Fullscreen (Borderless)", "Fullscreen (Borderless)"}};
    }
    if (row.section == "GPU" && row.key == "present_mode") {
        return {{L"Mailbox", "Mailbox"}, {L"Fifo", "Fifo"},
                {L"Immediate", "Immediate"}};
    }
    if (row.section == "General" && row.key == "trophy_notification_side") {
        return {{L"left", "left"}, {L"right", "right"}, {L"top", "top"},
                {L"bottom", "bottom"}};
    }
    if (row.section == "WindowsGuestRedZoneProtection" &&
        row.key == "windows_guest_red_zone_protection_mode") {
        return {{L"Disabled", "Disabled"}, {L"StaticPatching", "StaticPatching"}};
    }
    if (row.section == "General" && row.key == "console_language") {
        return {{L"0 - Japanese", 0},
                {L"1 - English (United States)", 1},
                {L"2 - French (France)", 2},
                {L"3 - Spanish (Spain)", 3},
                {L"4 - German", 4},
                {L"5 - Italian", 5},
                {L"6 - Dutch", 6},
                {L"7 - Portuguese (Portugal)", 7},
                {L"8 - Russian", 8},
                {L"9 - Korean", 9},
                {L"10 - Traditional Chinese", 10},
                {L"11 - Simplified Chinese", 11},
                {L"12 - Finnish", 12},
                {L"13 - Swedish", 13},
                {L"14 - Danish", 14},
                {L"15 - Norwegian (Bokmaal)", 15},
                {L"16 - Polish", 16},
                {L"17 - Portuguese (Brazil)", 17},
                {L"18 - English (United Kingdom)", 18},
                {L"19 - Turkish", 19},
                {L"20 - Spanish (Latin America)", 20},
                {L"21 - Arabic", 21},
                {L"22 - French (Canada)", 22},
                {L"23 - Czech", 23},
                {L"24 - Hungarian", 24},
                {L"25 - Greek", 25},
                {L"26 - Romanian", 26},
                {L"27 - Thai", 27},
                {L"28 - Vietnamese", 28},
                {L"29 - Indonesian", 29},
                {L"30 - Ukrainian", 30}};
    }
    return {};
}

std::wstring EditorDisplay(const SettingRow& row) {
    for (const auto& option : OptionsFor(row)) {
        if (option.value == row.value) {
            return option.label;
        }
    }
    return JsonDisplay(row.value);
}

std::optional<json> ParseEditorValue(const SettingRow& row, const std::wstring& text,
                                    std::wstring* error) {
    for (const auto& option : OptionsFor(row)) {
        if (text == option.label) {
            return option.value;
        }
    }
    return ParseEditedValue(text, row.value, error);
}

std::optional<json> ParseEditedValue(const std::wstring& text, const json& original,
                                     std::wstring* error) {
    try {
        if (original.is_string()) {
            return WideToUtf8(text);
        }
        std::string utf8 = WideToUtf8(Trim(text));
        if (original.is_boolean()) {
            std::transform(utf8.begin(), utf8.end(), utf8.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (utf8 == "true" || utf8 == "1" || utf8 == "on" || utf8 == "yes") {
                return true;
            }
            if (utf8 == "false" || utf8 == "0" || utf8 == "off" || utf8 == "no") {
                return false;
            }
            if (error) {
                *error = L"Boolean settings accept true or false.";
            }
            return std::nullopt;
        }
        json parsed = json::parse(utf8);
        if (original.is_number_integer() && !parsed.is_number_integer()) {
            if (error) {
                *error = L"This setting requires a whole number.";
            }
            return std::nullopt;
        }
        if (original.is_number_unsigned() && !parsed.is_number_unsigned()) {
            if (error) {
                *error = L"This setting requires a non-negative whole number.";
            }
            return std::nullopt;
        }
        if (original.is_number_float() && !parsed.is_number()) {
            if (error) {
                *error = L"This setting requires a number.";
            }
            return std::nullopt;
        }
        if (original.is_array() && !parsed.is_array()) {
            if (error) {
                *error = L"This setting requires a JSON array.";
            }
            return std::nullopt;
        }
        if (original.is_object() && !parsed.is_object()) {
            if (error) {
                *error = L"This setting requires a JSON object.";
            }
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception& exception) {
        if (error) {
            *error = L"Invalid value:\n" + Utf8ToWide(exception.what());
        }
        return std::nullopt;
    }
}

void SetStatus(const std::wstring& text) {
    SetWindowTextW(g_status, text.c_str());
}

GpuModeInfo GetGpuModeInfo(const json& config) {
    GpuModeInfo result;
    try {
        if (config.contains("GPU") && config["GPU"].is_object()) {
            result.null_gpu = config["GPU"].value("null_gpu", false);
            result.safe_gpu = config["GPU"].value("safe_gpu", false);
        }
    } catch (...) {
    }
    if (result.null_gpu) {
        result.name = L"nullgpu";
    } else if (result.safe_gpu) {
        result.name = L"safegpu";
    }
    return result;
}

bool IsGlobalScope() {
    return SendMessageW(g_scope, CB_GETCURSEL, 0, 0) == 1;
}

bool HasProfileValue(const std::string& section, const std::string& key) {
    return g_profile_config.contains(section) && g_profile_config[section].is_object() &&
           g_profile_config[section].contains(key);
}

void UpdateDetectedLabel() {
    std::wstring id = g_detected_game.title_id.empty() ? L"UNKNOWN" : g_detected_game.title_id;
    const GpuModeInfo gpu_mode = GetGpuModeInfo(g_effective_config);
    std::wstring text = L"Detected: " + id + L"  |  " + g_detected_game.title +
                        L"  |  launch mode from config: " + gpu_mode.name;
    SetWindowTextW(g_game_info, text.c_str());
}

void BuildRows() {
    g_rows.clear();
    const json& source = IsGlobalScope() ? g_global_config : g_effective_config;
    if (!source.is_object()) {
        return;
    }
    for (const auto& section : source.items()) {
        if (!section.value().is_object()) {
            continue;
        }
        for (const auto& entry : section.value().items()) {
            if (!IsGlobalScope() && !IsOverrideable(section.key(), entry.key())) {
                continue;
            }
            SettingRow row;
            row.section = section.key();
            row.key = entry.key();
            row.value = entry.value();
            row.overridden = !IsGlobalScope() && HasProfileValue(row.section, row.key);
            g_rows.push_back(std::move(row));
        }
    }
    std::sort(g_rows.begin(), g_rows.end(), [](const SettingRow& left, const SettingRow& right) {
        if (left.section != right.section) {
            return left.section < right.section;
        }
        return left.key < right.key;
    });
}

void PopulateSectionCombo() {
    const std::wstring previous = GetWindowTextString(g_section);
    SendMessageW(g_section, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_section, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All sections"));
    std::set<std::string> sections;
    for (const auto& row : g_rows) {
        sections.insert(row.section);
    }
    int selected = 0;
    int index = 1;
    for (const auto& section : sections) {
        const std::wstring wide = Utf8ToWide(section);
        SendMessageW(g_section, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        if (wide == previous) {
            selected = index;
        }
        ++index;
    }
    SendMessageW(g_section, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

void UpdateEditorFromSelection() {
    const int selected = ListView_GetNextItem(g_settings, -1, LVNI_SELECTED);
    if (selected < 0) {
        SetWindowTextW(g_value, L"");
        EnableWindow(g_apply, FALSE);
        EnableWindow(g_toggle, FALSE);
        EnableWindow(g_reset, FALSE);
        return;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(g_settings, &item) || item.lParam < 0 ||
        static_cast<size_t>(item.lParam) >= g_visible_rows.size()) {
        return;
    }
    const SettingRow& row = g_visible_rows[static_cast<size_t>(item.lParam)];
    SendMessageW(g_value, CB_RESETCONTENT, 0, 0);
    for (const auto& option : OptionsFor(row)) {
        SendMessageW(g_value, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(option.label.c_str()));
    }
    SetWindowTextW(g_value, EditorDisplay(row).c_str());
    EnableWindow(g_apply, TRUE);
    EnableWindow(g_toggle, row.value.is_boolean() ? TRUE : FALSE);
    EnableWindow(g_reset, (!IsGlobalScope() && row.overridden) ? TRUE : FALSE);
}

void RefreshSettingsList(bool rebuild_sections) {
    BuildRows();
    if (rebuild_sections) {
        PopulateSectionCombo();
    }
    const int section_index = static_cast<int>(SendMessageW(g_section, CB_GETCURSEL, 0, 0));
    const std::wstring selected_section =
        section_index <= 0 ? L"" : GetWindowTextString(g_section);
    const std::wstring filter = ToLower(Trim(GetWindowTextString(g_filter)));

    SendMessageW(g_settings, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_settings);
    g_visible_rows.clear();

    for (const auto& row : g_rows) {
        const std::wstring section = Utf8ToWide(row.section);
        const std::wstring key = Utf8ToWide(row.key);
        const std::wstring setting_name = section + L"." + key;
        const std::wstring value = JsonDisplay(row.value);
        if (!selected_section.empty() && section != selected_section) {
            continue;
        }
        if (!filter.empty() && ToLower(setting_name).find(filter) == std::wstring::npos &&
            ToLower(value).find(filter) == std::wstring::npos) {
            continue;
        }

        const size_t visible_index = g_visible_rows.size();
        g_visible_rows.push_back(row);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(visible_index);
        item.pszText = const_cast<wchar_t*>(setting_name.c_str());
        item.lParam = static_cast<LPARAM>(visible_index);
        const int inserted = ListView_InsertItem(g_settings, &item);
        ListView_SetItemText(g_settings, inserted, 1, const_cast<wchar_t*>(value.c_str()));
        const wchar_t* source = IsGlobalScope() ? L"Global" :
                                (row.overridden ? L"Game override" : L"Global");
        ListView_SetItemText(g_settings, inserted, 2, const_cast<wchar_t*>(source));
    }

    SendMessageW(g_settings, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_settings, nullptr, TRUE);
    UpdateEditorFromSelection();
}

bool LoadConfigurations(bool show_errors) {
    g_data_root = DetermineDataRoot();
    g_global_config_path = JoinPath(g_data_root, L"config.json");
    g_profile_config_path.clear();
    if (!g_detected_game.title_id.empty()) {
        g_profile_config_path = JoinPath(JoinPath(g_data_root, L"custom_configs"),
                                        g_detected_game.title_id + L".json");
    }

    g_global_config_exists = FileExists(g_global_config_path);
    g_global_config = DefaultConfig();
    std::wstring error;
    if (g_global_config_exists) {
        json loaded;
        if (!LoadJson(g_global_config_path, &loaded, &error)) {
            if (show_errors) {
                MessageBoxW(g_window, error.c_str(), L"Configuration error", MB_ICONERROR);
            }
            return false;
        }
        DeepMerge(&g_global_config, loaded);
        for (const auto& item : loaded.items()) {
            if (!g_global_config.contains(item.key())) {
                g_global_config[item.key()] = item.value();
            }
        }
    }

    g_profile_config = json::object();
    g_profile_config_exists = !g_profile_config_path.empty() && FileExists(g_profile_config_path);
    if (g_profile_config_exists &&
        !LoadJson(g_profile_config_path, &g_profile_config, &error)) {
        if (show_errors) {
            MessageBoxW(g_window, error.c_str(), L"Game profile error", MB_ICONERROR);
        }
        return false;
    }

    g_effective_config = g_global_config;
    DeepMerge(&g_effective_config, g_profile_config);
    UpdateDetectedLabel();
    return true;
}

void UpdateScopeLabel() {
    const bool has_id = !g_detected_game.title_id.empty();
    EnableWindow(g_scope, has_id ? TRUE : FALSE);
    if (!has_id) {
        SendMessageW(g_scope, CB_SETCURSEL, 1, 0);
    }
}

void SaveLauncherState() {
    g_launcher_state["last_eboot"] = WideToUtf8(g_eboot_path);
    g_launcher_state["last_result"] = WideToUtf8(g_last_result);
    g_launcher_state["recent_games"] = json::array();
    for (const auto& path : g_recent_games) {
        g_launcher_state["recent_games"].push_back(WideToUtf8(path));
    }
    std::wstring ignored;
    SaveJson(g_state_path, g_launcher_state, false, &ignored);
}

void LoadLauncherState() {
    g_launcher_state = json::object();
    if (!FileExists(g_state_path)) {
        return;
    }
    std::wstring ignored;
    if (!LoadJson(g_state_path, &g_launcher_state, &ignored)) {
        g_launcher_state = json::object();
        return;
    }
    try {
        g_eboot_path = Utf8ToWide(g_launcher_state.value("last_eboot", std::string{}));
        g_last_result = Utf8ToWide(g_launcher_state.value("last_result", std::string{}));
        if (g_launcher_state.contains("recent_games") &&
            g_launcher_state["recent_games"].is_array()) {
            for (const auto& item : g_launcher_state["recent_games"]) {
                if (item.is_string()) {
                    const std::wstring path = Utf8ToWide(item.get<std::string>());
                    if (!path.empty() &&
                        std::find(g_recent_games.begin(), g_recent_games.end(), path) ==
                            g_recent_games.end()) {
                        g_recent_games.push_back(path);
                    }
                }
            }
        }
    } catch (...) {
        g_eboot_path.clear();
        g_last_result.clear();
    }
}

void AddRecentGame(const std::wstring& path) {
    g_recent_games.erase(std::remove(g_recent_games.begin(), g_recent_games.end(), path),
                         g_recent_games.end());
    g_recent_games.insert(g_recent_games.begin(), path);
    if (g_recent_games.size() > 12) {
        g_recent_games.resize(12);
    }
    if (g_game_path) {
        SendMessageW(g_game_path, CB_RESETCONTENT, 0, 0);
        for (const auto& recent : g_recent_games) {
            SendMessageW(g_game_path, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(recent.c_str()));
        }
        SetWindowTextW(g_game_path, path.c_str());
    }
}

void SelectGame(const std::wstring& path) {
    g_eboot_path = path;
    AddRecentGame(path);
    g_detected_game = DetectGame(path);
    SetWindowTextW(g_game_path, path.c_str());
    SendMessageW(g_scope, CB_SETCURSEL,
                 g_detected_game.title_id.empty() ? 1 : 0, 0);
    UpdateScopeLabel();
    LoadConfigurations(true);
    RefreshSettingsList(true);
    SaveLauncherState();
    SetStatus(g_detected_game.title_id.empty()
                  ? L"Game selected, but no CUSA ID was found. Global settings will be used."
                  : L"Game and configuration loaded. Double-click a boolean to toggle it.");
}

bool SaveCurrentConfiguration(std::wstring* error) {
    if (IsGlobalScope()) {
        if (!SaveJson(g_global_config_path, g_global_config, true, error)) {
            return false;
        }
        g_global_config_exists = true;
        return true;
    }
    if (g_profile_config_path.empty()) {
        if (error) {
            *error = L"A CUSA title ID is required for a game profile.";
        }
        return false;
    }
    if (!SaveJson(g_profile_config_path, g_profile_config, true, error)) {
        return false;
    }
    g_profile_config_exists = true;
    return true;
}

std::optional<size_t> SelectedVisibleIndex() {
    const int selected = ListView_GetNextItem(g_settings, -1, LVNI_SELECTED);
    if (selected < 0) {
        return std::nullopt;
    }
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(g_settings, &item) || item.lParam < 0 ||
        static_cast<size_t>(item.lParam) >= g_visible_rows.size()) {
        return std::nullopt;
    }
    return static_cast<size_t>(item.lParam);
}

bool ApplySelectedValue(bool force) {
    const auto selected = SelectedVisibleIndex();
    if (!selected) {
        return true;
    }
    const SettingRow row = g_visible_rows[*selected];
    const std::wstring text = GetWindowTextString(g_value);
    if (!force && text == EditorDisplay(row)) {
        return true;
    }

    std::wstring error;
    const auto parsed = ParseEditorValue(row, text, &error);
    if (!parsed) {
        MessageBoxW(g_window, error.c_str(), L"Invalid setting value", MB_ICONERROR);
        return false;
    }

    if (IsGlobalScope()) {
        g_global_config[row.section][row.key] = *parsed;
    } else {
        g_profile_config[row.section][row.key] = *parsed;
    }
    if (!SaveCurrentConfiguration(&error)) {
        MessageBoxW(g_window, error.c_str(), L"Unable to save configuration", MB_ICONERROR);
        return false;
    }
    LoadConfigurations(false);
    RefreshSettingsList(false);
    SetStatus(L"Setting saved. The emulator executable was not modified.");
    return true;
}

void ToggleSelected() {
    const auto selected = SelectedVisibleIndex();
    if (!selected || !g_visible_rows[*selected].value.is_boolean()) {
        return;
    }
    const SettingRow row = g_visible_rows[*selected];
    if (IsGlobalScope()) {
        g_global_config[row.section][row.key] = !row.value.get<bool>();
    } else {
        g_profile_config[row.section][row.key] = !row.value.get<bool>();
    }
    std::wstring error;
    if (!SaveCurrentConfiguration(&error)) {
        MessageBoxW(g_window, error.c_str(), L"Unable to save configuration", MB_ICONERROR);
        return;
    }
    LoadConfigurations(false);
    RefreshSettingsList(false);
    SetStatus(L"Boolean toggled and saved.");
}

void ResetSelectedOverride() {
    if (IsGlobalScope()) {
        return;
    }
    const auto selected = SelectedVisibleIndex();
    if (!selected) {
        return;
    }
    const SettingRow row = g_visible_rows[*selected];
    if (!HasProfileValue(row.section, row.key)) {
        return;
    }
    g_profile_config[row.section].erase(row.key);
    if (g_profile_config[row.section].empty()) {
        g_profile_config.erase(row.section);
    }
    std::wstring error;
    if (!SaveCurrentConfiguration(&error)) {
        MessageBoxW(g_window, error.c_str(), L"Unable to save game profile", MB_ICONERROR);
        return;
    }
    LoadConfigurations(false);
    RefreshSettingsList(false);
    SetStatus(L"Game override removed; this setting now inherits the global value.");
}

std::wstring QuoteArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring TimestampNow() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf(buffer, 64, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth,
             time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::wstring IsoTimestampNow() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf(buffer, 64, L"%04u-%02u-%02uT%02u:%02u:%02u", time.wYear, time.wMonth,
             time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

void CopyIfPresent(const std::wstring& source, const std::wstring& destination) {
    if (FileExists(source)) {
        CopyFileW(source.c_str(), destination.c_str(), FALSE);
    }
}

uint64_t FileSizeOrZero(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    CloseHandle(file);
    return ok ? static_cast<uint64_t>(size.QuadPart) : 0;
}

void CopyRunLog(const std::wstring& source, const std::wstring& destination, uint64_t start) {
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input, &size) || size.QuadPart < 0) {
        CloseHandle(input);
        return;
    }
    const uint64_t current_size = static_cast<uint64_t>(size.QuadPart);
    if (start > current_size) {
        // The emulator replaced/truncated the log during this run.  In that case the
        // entire current file belongs to this invocation.
        start = 0;
    }

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(start);
    if (!SetFilePointerEx(input, offset, nullptr, FILE_BEGIN)) {
        CloseHandle(input);
        return;
    }

    HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        return;
    }

    std::vector<char> buffer(1u << 20);
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        DWORD written_total = 0;
        while (written_total < read) {
            DWORD written = 0;
            if (!WriteFile(output, buffer.data() + written_total, read - written_total, &written,
                           nullptr) ||
                written == 0) {
                ok = false;
                break;
            }
            written_total += written;
        }
        if (!ok) {
            break;
        }
    }
    if (ok) {
        FlushFileBuffers(output);
    }
    CloseHandle(output);
    CloseHandle(input);
}

unsigned __stdcall WaitForRun(void* parameter) {
    auto* context = static_cast<RunContext*>(parameter);
    WaitForSingleObject(context->process, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(context->process, &exit_code);
    CloseHandle(context->process);
    context->process = nullptr;

    const std::wstring log_dir = JoinPath(context->data_root, L"log");
    CopyRunLog(JoinPath(log_dir, L"shadps4.log"),
               JoinPath(context->result_dir, L"shadps4-" + context->title_id + L".log"),
               context->shadps4_log_start);
    CopyRunLog(JoinPath(log_dir, L"shad_log.txt"),
               JoinPath(context->result_dir,
                        L"shad_log-" + context->title_id + L"-" + context->mode + L".txt"),
               context->shad_log_start);

    const std::wstring dump = JoinPath(context->exe_dir, L"shadps4-crash.dmp");
    if (FileExists(dump)) {
        MoveFileExW(dump.c_str(),
                    JoinPath(context->result_dir,
                             L"shadps4-crash-" + context->title_id + L"-" + context->mode +
                                 L".dmp")
                        .c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    std::wstring ignored;
    WriteFileBytes(JoinPath(context->result_dir, L"exit-code.txt"),
                   std::to_string(exit_code) + "\n", false, &ignored);

    json run_info;
    run_info["launcher_version"] = "2";
    run_info["title_id"] = WideToUtf8(context->title_id);
    run_info["title"] = WideToUtf8(context->title);
    run_info["eboot"] = WideToUtf8(context->eboot);
    run_info["mode"] = WideToUtf8(context->mode);
    run_info["null_gpu"] = context->null_gpu;
    run_info["safe_gpu"] = context->safe_gpu;
    run_info["started_at"] = WideToUtf8(context->started_at);
    run_info["finished_at"] = WideToUtf8(IsoTimestampNow());
    run_info["exit_code"] = exit_code;
    run_info["data_root"] = WideToUtf8(context->data_root);
    run_info["shadps4_exe"] = WideToUtf8(JoinPath(context->exe_dir, L"shadps4.exe"));
    SaveJson(JoinPath(context->result_dir, L"run-info.json"), run_info, false, &ignored);

    PostMessageW(g_window, WM_RUN_FINISHED, static_cast<WPARAM>(exit_code),
                 reinterpret_cast<LPARAM>(context));
    return 0;
}

void SetRunningUi(bool running) {
    g_running = running;
    EnableWindow(g_browse, running ? FALSE : TRUE);
    EnableWindow(g_scope, (!running && !g_detected_game.title_id.empty()) ? TRUE : FALSE);
    EnableWindow(g_section, running ? FALSE : TRUE);
    EnableWindow(g_filter, running ? FALSE : TRUE);
    EnableWindow(g_settings, running ? FALSE : TRUE);
    EnableWindow(g_value, running ? FALSE : TRUE);
    EnableWindow(g_apply, running ? FALSE : TRUE);
    EnableWindow(g_toggle, running ? FALSE : TRUE);
    EnableWindow(g_reset, running ? FALSE : TRUE);
    EnableWindow(g_reload, running ? FALSE : TRUE);
    EnableWindow(g_open_json, running ? FALSE : TRUE);
    EnableWindow(g_launch, running ? FALSE : TRUE);
    SetWindowTextW(g_launch, running ? L"Running..." : L"Launch game");
}

void LaunchGame() {
    if (g_running) {
        return;
    }
    if (!FileExists(g_shadps4_exe)) {
        MessageBoxW(g_window, L"shadps4.exe must be beside this launcher.",
                    L"Missing emulator", MB_ICONERROR);
        return;
    }
    if (!FileExists(g_eboot_path)) {
        MessageBoxW(g_window, L"Select a valid eboot.bin first.", L"Missing game",
                    MB_ICONERROR);
        return;
    }
    if (!ApplySelectedValue(false)) {
        return;
    }
    if (!LoadConfigurations(true)) {
        return;
    }

    const GpuModeInfo gpu_mode = GetGpuModeInfo(g_effective_config);
    const std::wstring mode = gpu_mode.name;
    const std::wstring title_id =
        g_detected_game.title_id.empty() ? L"UNKNOWN" : g_detected_game.title_id;
    const std::wstring result_dir =
        JoinPath(JoinPath(JoinPath(g_exe_dir, L"test-results"), title_id),
                 TimestampNow() + L"-" + mode);
    if (!EnsureDirectory(result_dir)) {
        MessageBoxW(g_window, L"Unable to create the automatic test-results folder.",
                    L"Launch error", MB_ICONERROR);
        return;
    }

    DeleteFileW(JoinPath(g_exe_dir, L"shadps4-crash.dmp").c_str());

    // --log-append intentionally preserves shadPS4's normal accumulated logs.  Capture
    // the pre-launch boundaries so the result directory receives only this invocation.
    const std::wstring run_log_dir = JoinPath(g_data_root, L"log");
    const uint64_t shadps4_log_start = FileSizeOrZero(JoinPath(run_log_dir, L"shadps4.log"));
    const uint64_t shad_log_start = FileSizeOrZero(JoinPath(run_log_dir, L"shad_log.txt"));

    CopyIfPresent(g_global_config_path,
                  JoinPath(result_dir, L"config-global-" + title_id + L".json"));
    if (!g_profile_config_path.empty()) {
        CopyIfPresent(g_profile_config_path,
                      JoinPath(result_dir, L"config-profile-" + title_id + L".json"));
    }

    const std::wstring console_path =
        JoinPath(result_dir, L"console-" + title_id + L"-" + mode + L".txt");
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE console = CreateFileW(console_path.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (console == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_window, L"Unable to create the console capture.", L"Launch error",
                    MB_ICONERROR);
        return;
    }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = console;
    startup.hStdError = console;
    startup.hStdInput = input == INVALID_HANDLE_VALUE ? nullptr : input;

    std::wstring command = QuoteArgument(g_shadps4_exe) + L" -g " +
                           QuoteArgument(g_eboot_path) + L" --log-append";
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        g_shadps4_exe.c_str(), command_buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, g_exe_dir.c_str(), &startup, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(console);
    if (input != INVALID_HANDLE_VALUE) {
        CloseHandle(input);
    }
    if (!created) {
        wchar_t message[256]{};
        swprintf(message, 256, L"CreateProcess failed with Windows error %lu.", create_error);
        MessageBoxW(g_window, message, L"Launch error", MB_ICONERROR);
        return;
    }
    CloseHandle(process.hThread);

    auto* context = new RunContext;
    context->process = process.hProcess;
    context->result_dir = result_dir;
    context->data_root = g_data_root;
    context->exe_dir = g_exe_dir;
    context->global_config = g_global_config_path;
    context->profile_config = g_profile_config_path;
    context->title_id = title_id;
    context->title = g_detected_game.title;
    context->eboot = g_eboot_path;
    context->mode = mode;
    context->started_at = IsoTimestampNow();
    context->null_gpu = gpu_mode.null_gpu;
    context->safe_gpu = gpu_mode.safe_gpu;
    context->shadps4_log_start = shadps4_log_start;
    context->shad_log_start = shad_log_start;

    const uintptr_t waiter_value = _beginthreadex(nullptr, 0, WaitForRun, context, 0, nullptr);
    HANDLE waiter = reinterpret_cast<HANDLE>(waiter_value);
    if (!waiter_value) {
        CloseHandle(process.hProcess);
        delete context;
        MessageBoxW(g_window,
                    L"The emulator started, but the result collector thread could not start.",
                    L"Launcher warning", MB_ICONWARNING);
        return;
    }
    CloseHandle(waiter);

    g_last_result = result_dir;
    SaveLauncherState();
    EnableWindow(g_open_results, TRUE);
    SetRunningUi(true);
    SetStatus(L"Running " + title_id + L" in automatically detected " + mode +
              L" mode. Results are being collected.");
}

void BrowseForGame() {
    std::vector<wchar_t> file(32768, L'\0');
    if (!g_eboot_path.empty() && g_eboot_path.size() + 1 < file.size()) {
        std::copy(g_eboot_path.begin(), g_eboot_path.end(), file.begin());
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_window;
    dialog.lpstrFilter = L"PS4 executable (eboot.bin)\0eboot.bin\0All files\0*.*\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrTitle = L"Select a game's eboot.bin";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        SelectGame(file.data());
    }
}

void OpenActiveJson() {
    std::wstring path = IsGlobalScope() ? g_global_config_path : g_profile_config_path;
    if (path.empty()) {
        return;
    }
    if (!FileExists(path)) {
        std::wstring error;
        const json empty = IsGlobalScope() ? g_global_config : json::object();
        if (!SaveJson(path, empty, false, &error)) {
            MessageBoxW(g_window, error.c_str(), L"Unable to create JSON", MB_ICONERROR);
            return;
        }
    }
    const std::wstring parameters = QuoteArgument(path);
    ShellExecuteW(g_window, L"open", L"notepad.exe", parameters.c_str(), g_exe_dir.c_str(),
                  SW_SHOWNORMAL);
}

void OpenFolder(const std::wstring& path) {
    if (!DirectoryExists(path)) {
        EnsureDirectory(path);
    }
    ShellExecuteW(g_window, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ReloadUi() {
    if (LoadConfigurations(true)) {
        RefreshSettingsList(true);
        SetStatus(L"Configuration reloaded from disk.");
    }
}

void LayoutControls(int width, int height) {
    const int margin = 12;
    const int game_label_width = 46;
    const int browse_width = 88;
    const int top_edit_width = std::max(200, width - margin * 3 - game_label_width - browse_width);
    MoveWindow(g_game_path, margin + game_label_width, 12, top_edit_width, 24, TRUE);
    MoveWindow(g_browse, width - margin - browse_width, 12, browse_width, 24, TRUE);
    MoveWindow(g_game_info, margin, 43, width - margin * 2, 22, TRUE);

    const int filter_width = std::max(170, width - 620);
    MoveWindow(g_scope, margin + 48, 72, 240, 300, TRUE);
    MoveWindow(g_section, margin + 355, 72, 180, 300, TRUE);
    MoveWindow(g_filter, width - margin - filter_width, 72, filter_width, 24, TRUE);

    const int list_top = 106;
    const int list_bottom = height - 132;
    MoveWindow(g_settings, margin, list_top, width - margin * 2,
               std::max(120, list_bottom - list_top), TRUE);

    const int value_y = height - 119;
    const int button_y = height - 83;
    MoveWindow(g_value, margin + 98, value_y, std::max(220, width - 550), 240, TRUE);
    MoveWindow(g_apply, width - 428, value_y, 90, 24, TRUE);
    MoveWindow(g_toggle, width - 330, value_y, 90, 24, TRUE);
    MoveWindow(g_reset, width - 232, value_y, 220, 24, TRUE);

    MoveWindow(g_reload, margin, button_y, 78, 26, TRUE);
    MoveWindow(g_open_json, margin + 86, button_y, 94, 26, TRUE);
    MoveWindow(g_open_data, margin + 188, button_y, 112, 26, TRUE);
    MoveWindow(g_open_results, margin + 308, button_y, 118, 26, TRUE);
    MoveWindow(g_launch, width - margin - 150, button_y, 150, 30, TRUE);
    MoveWindow(g_status, 0, height - 28, width, 28, TRUE);
}

HWND CreateControl(const wchar_t* class_name, const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0,
                                   0, g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   g_instance, nullptr);
    if (control && g_font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    return control;
}

void CreateUi() {
    g_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HWND game_label = CreateControl(L"STATIC", L"Game:", SS_LEFT, 0);
    MoveWindow(game_label, 12, 16, 44, 20, TRUE);
    g_game_path = CreateControl(WC_COMBOBOXW, L"",
                                CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                                ID_GAME_PATH);
    for (const auto& recent : g_recent_games) {
        SendMessageW(g_game_path, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(recent.c_str()));
    }
    g_browse = CreateControl(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_BROWSE);
    g_game_info = CreateControl(L"STATIC", L"Detected: no game selected", SS_LEFT, 0);

    HWND scope_label = CreateControl(L"STATIC", L"Edit:", SS_LEFT, 0);
    MoveWindow(scope_label, 12, 76, 42, 20, TRUE);
    g_scope = CreateControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_SCOPE);
    SendMessageW(g_scope, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Game profile (automatic CUSA)"));
    SendMessageW(g_scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Global config"));
    SendMessageW(g_scope, CB_SETCURSEL, 1, 0);

    HWND section_label = CreateControl(L"STATIC", L"Section:", SS_LEFT, 0);
    MoveWindow(section_label, 308, 76, 58, 20, TRUE);
    g_section = CreateControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_SECTION);

    HWND filter_label = CreateControl(L"STATIC", L"Filter:", SS_LEFT, 0);
    MoveWindow(filter_label, 548, 76, 46, 20, TRUE);
    g_filter = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, ID_FILTER);

    g_settings = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                     LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 0, 0, 0, 0, g_window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SETTINGS_LIST)),
                                 g_instance, nullptr);
    SendMessageW(g_settings, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    ListView_SetExtendedListViewStyle(
        g_settings, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(L"Setting");
    column.cx = 360;
    ListView_InsertColumn(g_settings, 0, &column);
    column.pszText = const_cast<wchar_t*>(L"Value");
    column.cx = 360;
    ListView_InsertColumn(g_settings, 1, &column);
    column.pszText = const_cast<wchar_t*>(L"Source");
    column.cx = 140;
    ListView_InsertColumn(g_settings, 2, &column);

    HWND value_label = CreateControl(L"STATIC", L"Selected value:", SS_LEFT, 0);
    g_value = CreateControl(WC_COMBOBOXW, L"",
                            CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, ID_VALUE);
    g_apply = CreateControl(L"BUTTON", L"Apply", BS_PUSHBUTTON, ID_APPLY);
    g_toggle = CreateControl(L"BUTTON", L"Toggle", BS_PUSHBUTTON, ID_TOGGLE);
    g_reset = CreateControl(L"BUTTON", L"Reset to global", BS_PUSHBUTTON, ID_RESET);
    g_reload = CreateControl(L"BUTTON", L"Reload", BS_PUSHBUTTON, ID_RELOAD);
    g_open_json = CreateControl(L"BUTTON", L"Open JSON", BS_PUSHBUTTON, ID_OPEN_JSON);
    g_open_data = CreateControl(L"BUTTON", L"Data folder", BS_PUSHBUTTON, ID_OPEN_DATA);
    g_open_results =
        CreateControl(L"BUTTON", L"Last results", BS_PUSHBUTTON, ID_OPEN_RESULTS);
    g_launch = CreateControl(L"BUTTON", L"Launch game", BS_DEFPUSHBUTTON, ID_LAUNCH);
    g_status = CreateWindowExW(0, L"STATIC", L"Ready.",
                               WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN, 0, 0, 0, 0,
                               g_window, nullptr, g_instance, nullptr);
    SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

    MoveWindow(value_label, 12, 0, 90, 20, TRUE);
    SetWindowLongPtrW(value_label, GWLP_USERDATA, 1);
    EnableWindow(g_apply, FALSE);
    EnableWindow(g_toggle, FALSE);
    EnableWindow(g_reset, FALSE);
    EnableWindow(g_open_results, DirectoryExists(g_last_result) ? TRUE : FALSE);

    RECT client{};
    GetClientRect(g_window, &client);
    LayoutControls(client.right, client.bottom);
    MoveWindow(value_label, 12, client.bottom - 115, 90, 20, TRUE);
}

void RepositionStaticLabels(int height) {
    for (HWND child = GetWindow(g_window, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (GetWindowLongPtrW(child, GWLP_USERDATA) == 1) {
            MoveWindow(child, 12, height - 115, 90, 20, TRUE);
        }
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_window = window;
        CreateUi();
        DragAcceptFiles(window, TRUE);
        return 0;

    case WM_SIZE: {
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        LayoutControls(width, height);
        RepositionStaticLabels(height);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(static_cast<size_t>(length), L'\0');
        DragQueryFileW(drop, 0, path.data(), length + 1);
        DragFinish(drop);
        if (FileExists(path)) {
            SelectGame(path);
        }
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int notification = HIWORD(wparam);
        if (id == ID_BROWSE && notification == BN_CLICKED) {
            BrowseForGame();
        } else if (id == ID_GAME_PATH && notification == CBN_SELCHANGE) {
            const int selection =
                static_cast<int>(SendMessageW(g_game_path, CB_GETCURSEL, 0, 0));
            if (selection >= 0) {
                const LRESULT length = SendMessageW(g_game_path, CB_GETLBTEXTLEN,
                                                    static_cast<WPARAM>(selection), 0);
                if (length > 0) {
                    std::wstring selected_path(static_cast<size_t>(length) + 1, L'\0');
                    SendMessageW(g_game_path, CB_GETLBTEXT, static_cast<WPARAM>(selection),
                                 reinterpret_cast<LPARAM>(selected_path.data()));
                    selected_path.resize(static_cast<size_t>(length));
                    if (FileExists(selected_path)) {
                        SelectGame(selected_path);
                    }
                }
            }
        } else if (id == ID_SCOPE && notification == CBN_SELCHANGE) {
            RefreshSettingsList(true);
            UpdateDetectedLabel();
        } else if (id == ID_SECTION && notification == CBN_SELCHANGE) {
            RefreshSettingsList(false);
        } else if (id == ID_FILTER && notification == EN_CHANGE) {
            RefreshSettingsList(false);
        } else if (id == ID_APPLY && notification == BN_CLICKED) {
            ApplySelectedValue(true);
        } else if (id == ID_TOGGLE && notification == BN_CLICKED) {
            ToggleSelected();
        } else if (id == ID_RESET && notification == BN_CLICKED) {
            ResetSelectedOverride();
        } else if (id == ID_RELOAD && notification == BN_CLICKED) {
            ReloadUi();
        } else if (id == ID_OPEN_JSON && notification == BN_CLICKED) {
            OpenActiveJson();
        } else if (id == ID_OPEN_DATA && notification == BN_CLICKED) {
            OpenFolder(g_data_root);
        } else if (id == ID_OPEN_RESULTS && notification == BN_CLICKED) {
            if (!g_last_result.empty()) {
                OpenFolder(g_last_result);
            }
        } else if (id == ID_LAUNCH && notification == BN_CLICKED) {
            LaunchGame();
        }
        return 0;
    }

    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (header->idFrom == ID_SETTINGS_LIST) {
            if (header->code == LVN_ITEMCHANGED) {
                UpdateEditorFromSelection();
            } else if (header->code == NM_DBLCLK) {
                const auto selected = SelectedVisibleIndex();
                if (selected && g_visible_rows[*selected].value.is_boolean()) {
                    ToggleSelected();
                } else {
                    SetFocus(g_value);
                    SendMessageW(g_value, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
                }
            }
        }
        return 0;
    }

    case WM_RUN_FINISHED: {
        const DWORD exit_code = static_cast<DWORD>(wparam);
        auto* context = reinterpret_cast<RunContext*>(lparam);
        g_last_result = context->result_dir;
        delete context;
        SetRunningUi(false);
        EnableWindow(g_open_results, TRUE);
        SaveLauncherState();
        SetStatus(L"Emulator finished with exit code " + std::to_wstring(exit_code) +
                  L". Results collected automatically in " + g_last_result);
        return 0;
    }

    case WM_CLOSE:
        if (g_running) {
            MessageBoxW(window,
                        L"Keep the launcher open until the emulator exits so logs and the crash "
                        L"dump can be collected.",
                        L"Emulator is still running", MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        SaveLauncherState();
        if (g_font) {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show_command) {
    g_instance = instance;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    if (!InitCommonControlsEx(&controls)) {
        MessageBoxW(nullptr, L"Windows common controls could not be initialized.",
                    L"shadPS4 launcher startup error", MB_ICONERROR);
        return 1;
    }

    g_exe_dir = GetModuleDirectory();
    SetCurrentDirectoryW(g_exe_dir.c_str());
    g_shadps4_exe = JoinPath(g_exe_dir, L"shadps4.exe");
    g_state_path = JoinPath(g_exe_dir, L"shadps4-win7-launcher-state.json");
    LoadLauncherState();

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    window_class.lpszClassName = kWindowClass;
    window_class.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&window_class)) {
        wchar_t message[192]{};
        swprintf(message, 192, L"Unable to register the launcher window (error %lu).",
                 GetLastError());
        MessageBoxW(nullptr, message, L"shadPS4 launcher startup error", MB_ICONERROR);
        return 1;
    }

    RECT desired{0, 0, 980, 700};
    AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
    g_window = CreateWindowExW(0, kWindowClass, kLauncherTitle, WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left,
                               desired.bottom - desired.top, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        wchar_t message[192]{};
        swprintf(message, 192, L"Unable to create the launcher window (error %lu).",
                 GetLastError());
        MessageBoxW(nullptr, message, L"shadPS4 launcher startup error", MB_ICONERROR);
        return 1;
    }

    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    std::wstring command_game;
    if (arguments && argument_count >= 2) {
        command_game = arguments[1];
    }
    if (arguments) {
        LocalFree(arguments);
    }

    if (!command_game.empty() && FileExists(command_game)) {
        SelectGame(command_game);
    } else if (!g_eboot_path.empty() && FileExists(g_eboot_path)) {
        SelectGame(g_eboot_path);
    } else {
        g_eboot_path.clear();
        g_detected_game = {};
        g_data_root = DetermineDataRoot();
        LoadConfigurations(false);
        UpdateScopeLabel();
        RefreshSettingsList(true);
        SetStatus(L"Select or drag an eboot.bin. Title ID and launch mode will be detected automatically.");
    }

    ShowWindow(g_window, show_command);
    UpdateWindow(g_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}
