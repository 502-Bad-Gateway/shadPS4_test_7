param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ExpectedInputSha256 = '1e133b7c7cbf054564575b4047f20372a906a4568dba531e49a8d7544396d1a9'
$ExpectedOutputSha256 = '635e1885ed40f2bd8bb7f36ec15896362f509e41730e6bc98abcdb21d24067b4'

if (!(Test-Path $SourcePath)) { throw "Launcher source not found: $SourcePath" }
$InputHash = (Get-FileHash -Algorithm SHA256 $SourcePath).Hash.ToLowerInvariant()
if ($InputHash -ne $ExpectedInputSha256) {
    throw "Unexpected launcher source SHA256: $InputHash"
}

$Text = [IO.File]::ReadAllText($SourcePath)
function Replace-Exact([string]$Old, [string]$New, [string]$Label) {
    if (!$script:Text.Contains($Old)) { throw "Launcher theme anchor missing: $Label" }
    $script:Text = $script:Text.Replace($Old, $New)
}

Replace-Exact @'
HWND g_status = nullptr;
HFONT g_font = nullptr;

std::wstring g_exe_dir;
'@ @'
HWND g_status = nullptr;
HFONT g_font = nullptr;
HBRUSH g_background_brush = nullptr;
HBRUSH g_control_brush = nullptr;
HBRUSH g_status_brush = nullptr;

constexpr COLORREF kDarkBackground = RGB(32, 32, 32);
constexpr COLORREF kDarkControl = RGB(45, 45, 45);
constexpr COLORREF kDarkStatus = RGB(38, 38, 38);
constexpr COLORREF kDarkText = RGB(232, 232, 232);
constexpr COLORREF kDarkDisabledText = RGB(145, 145, 145);

std::wstring g_exe_dir;
'@ 'theme globals'

Replace-Exact @'
HWND CreateControl(const wchar_t* class_name, const wchar_t* text, DWORD style, int id) {
'@ @'
void DisableVisualTheme(HWND control) {
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE module = LoadLibraryW(L"uxtheme.dll");
    static SetWindowThemeFn set_window_theme =
        module ? reinterpret_cast<SetWindowThemeFn>(GetProcAddress(module, "SetWindowTheme")) : nullptr;
    if (set_window_theme) {
        set_window_theme(control, L"", L"");
    }
}

HWND CreateControl(const wchar_t* class_name, const wchar_t* text, DWORD style, int id) {
'@ 'theme helper'

Replace-Exact @'
    if (control && g_font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    return control;
'@ @'
    if (control) {
        DisableVisualTheme(control);
        if (g_font) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
    }
    return control;
'@ 'control theming'

Replace-Exact @'
    SendMessageW(g_settings, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    ListView_SetExtendedListViewStyle(
'@ @'
    SendMessageW(g_settings, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    DisableVisualTheme(g_settings);
    ListView_SetBkColor(g_settings, kDarkControl);
    ListView_SetTextBkColor(g_settings, kDarkControl);
    ListView_SetTextColor(g_settings, kDarkText);
    ListView_SetExtendedListViewStyle(
'@ 'list view colors'

Replace-Exact @'
    SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

    MoveWindow(value_label, 12, 0, 90, 20, TRUE);
'@ @'
    SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    DisableVisualTheme(g_status);

    MoveWindow(value_label, 12, 0, 90, 20, TRUE);
'@ 'status theme'

Replace-Exact @'
    case WM_SIZE: {
'@ @'
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        const bool is_status = control == g_status;
        const bool is_static = message == WM_CTLCOLORSTATIC;
        const bool use_background = is_static && !is_status;
        const COLORREF background =
            is_status ? kDarkStatus : (use_background ? kDarkBackground : kDarkControl);
        SetTextColor(dc, IsWindowEnabled(control) ? kDarkText : kDarkDisabledText);
        SetBkColor(dc, background);
        SetBkMode(dc, OPAQUE);
        return reinterpret_cast<LRESULT>(is_status ? g_status_brush
                                                    : (use_background ? g_background_brush
                                                                      : g_control_brush));
    }

    case WM_SIZE: {
'@ 'control color messages'

Replace-Exact @'
        if (header->idFrom == ID_SETTINGS_LIST) {
            if (header->code == LVN_ITEMCHANGED) {
'@ @'
        if (header->idFrom == ID_SETTINGS_LIST) {
            if (header->code == NM_CUSTOMDRAW) {
                auto* custom = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                if (custom->nmcd.dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    custom->clrText = kDarkText;
                    custom->clrTextBk = kDarkControl;
                    return CDRF_DODEFAULT;
                }
            } else if (header->code == LVN_ITEMCHANGED) {
'@ 'list view custom draw'

Replace-Exact @'
    g_state_path = JoinPath(g_exe_dir, L"shadps4-win7-launcher-state.json");
    LoadLauncherState();

    WNDCLASSEXW window_class{};
'@ @'
    g_state_path = JoinPath(g_exe_dir, L"shadps4-win7-launcher-state.json");
    LoadLauncherState();

    g_background_brush = CreateSolidBrush(kDarkBackground);
    g_control_brush = CreateSolidBrush(kDarkControl);
    g_status_brush = CreateSolidBrush(kDarkStatus);
    if (!g_background_brush || !g_control_brush || !g_status_brush) {
        MessageBoxW(nullptr, L"Unable to create launcher theme resources.",
                    L"shadPS4 launcher startup error", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW window_class{};
'@ 'theme brushes'

Replace-Exact @'
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
'@ @'
    window_class.hbrBackground = g_background_brush;
'@ 'window background'

Replace-Exact @'
    }
    return static_cast<int>(message.wParam);
}
'@ @'
    }
    if (g_status_brush) {
        DeleteObject(g_status_brush);
        g_status_brush = nullptr;
    }
    if (g_control_brush) {
        DeleteObject(g_control_brush);
        g_control_brush = nullptr;
    }
    if (g_background_brush) {
        DeleteObject(g_background_brush);
        g_background_brush = nullptr;
    }
    return static_cast<int>(message.wParam);
}
'@ 'theme cleanup'

[IO.File]::WriteAllText($SourcePath, $Text, [Text.UTF8Encoding]::new($false))
$OutputHash = (Get-FileHash -Algorithm SHA256 $SourcePath).Hash.ToLowerInvariant()
if ($OutputHash -ne $ExpectedOutputSha256) {
    throw "Unexpected themed launcher source SHA256: $OutputHash"
}
Write-Host "Launcher dark theme applied: $OutputHash"
