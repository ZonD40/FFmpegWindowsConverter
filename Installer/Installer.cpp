
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

#include <urlmon.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <string>
#include <filesystem>
#include <fstream>
#include "resource.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows")

namespace fs = std::filesystem;

// ── Контролы ──
#define IDC_PROGRESS     201
#define IDC_LABEL_STATUS 202
#define IDC_BTN_INSTALL  203
#define IDC_BTN_UNINSTALL 204
#define IDC_PATH_EDIT    205
#define IDC_PATH_BROWSE  206

#define WM_INSTALL_PROGRESS (WM_USER + 10)
#define WM_INSTALL_DONE     (WM_USER + 11)
#define WM_INSTALL_ERROR    (WM_USER + 12)

HWND g_hwnd, g_progress, g_labelStatus, g_btnInstall, g_btnUninstall, g_pathEdit;
bool g_installing = false;

// ── Вспомогалки ──

bool extractResource(int resourceId, const std::wstring& destPath) {
    HMODULE hModule = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) return false;
    void* pData = LockResource(hData);
    DWORD size = SizeofResource(hModule, hRes);
    if (!pData || size == 0) return false;

    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(hFile, pData, size, &written, nullptr);
    CloseHandle(hFile);
    return written == size;
}

std::wstring getDefaultInstallDir() {
    wchar_t buf[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, buf);
    return std::wstring(buf) + L"\\FFmpegConverter";
}

std::wstring getInstallerDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    return s.substr(0, s.rfind(L'\\'));
}

void setStatus(const wchar_t* text, int pct = -1) {
    SetWindowTextW(g_labelStatus, text);
    if (pct >= 0) SendMessage(g_progress, PBM_SETPOS, pct, 0);
}

bool runAndWait(const std::wstring& cmd) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// Распаковка ZIP через PowerShell (без доп. зависимостей)
bool extractZip(const std::wstring& zipPath, const std::wstring& destDir) {
    std::wstring cmd =
        L"powershell -NoProfile -Command \"Expand-Archive -Force -Path '"
        + zipPath + L"' -DestinationPath '" + destDir + L"'\"";
    return runAndWait(cmd);
}

// Найти ffmpeg.exe рекурсивно в папке
std::wstring findFFmpeg(const std::wstring& dir) {
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.path().filename() == L"ffmpeg.exe")
            return entry.path().wstring();
    }
    return L"";
}

// ── Поток установки ──

struct InstallArgs { std::wstring installDir; };

// Callback для прогресса скачивания
class DownloadCallback : public IBindStatusCallback {
public:
    HWND hwnd;
    DownloadCallback(HWND h) : hwnd(h) {}

    STDMETHOD(OnProgress)(ULONG progress, ULONG progressMax, ULONG, LPCWSTR) override {
        if (progressMax > 0) {
            int pct = (int)(20 + (progress * 40.0 / progressMax)); // 20-60%
            PostMessage(hwnd, WM_INSTALL_PROGRESS, pct,
                reinterpret_cast<LPARAM>(L"Скачивание ffmpeg..."));
        }
        return S_OK;
    }

    // Остальные методы — заглушки
    STDMETHOD(QueryInterface)(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHOD_(ULONG, AddRef)()  override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(OnStartBinding)(DWORD, IBinding*) override { return S_OK; }
    STDMETHOD(GetPriority)(LONG*) override { return S_OK; }
    STDMETHOD(OnLowResource)(DWORD) override { return S_OK; }
    STDMETHOD(OnStopBinding)(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHOD(GetBindInfo)(DWORD*, BINDINFO*) override { return S_OK; }
    STDMETHOD(OnDataAvailable)(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHOD(OnObjectAvailable)(REFIID, IUnknown*) override { return S_OK; }
};

DWORD WINAPI installThread(LPVOID param) {
    InstallArgs* args = reinterpret_cast<InstallArgs*>(param);
    std::wstring dir = args->installDir;
    delete args;

    auto fail = [&](const wchar_t* msg) {
        PostMessage(g_hwnd, WM_INSTALL_ERROR, 0,
            reinterpret_cast<LPARAM>(_wcsdup(msg)));
        };

    // 1. Создаём папку установки
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 5,
        reinterpret_cast<LPARAM>(L"Создание папки..."));
    try { fs::create_directories(dir); }
    catch (...) { fail(L"Не удалось создать папку установки.\nЗапусти установщик от имени администратора."); return 1; }

    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 10,
        reinterpret_cast<LPARAM>(L"Извлечение файлов..."));

    if (!extractResource(IDR_CONVERTER_EXE, dir + L"\\Converter.exe")) {
        fail(L"Не удалось извлечь Converter.exe"); return 1;
    }
    if (!extractResource(IDR_SHELLEXT_DLL, dir + L"\\ShellExtension.dll")) {
        fail(L"Не удалось извлечь ShellExtension.dll"); return 1;
    }
    if (!extractResource(IDR_UNINSTALLER_EXE, dir + L"\\uninst.exe")) {
        fail(L"Не удалось извлечь uninst.exe"); return 1;
    }
    if (!extractResource(IDR_PRESETS_JSON, dir + L"\\presets.json")) {
        fail(L"Не удалось извлечь presets.json"); return 1;
    }
    if (!extractResource(IDI_TRAY_ICON, dir + L"\\icon.ico")) {
        fail(L"Не удалось извлечь icon.ico"); return 1;
    }
    if (!extractResource(IDI_APP_ICON, dir + L"\\app.ico")) {
        fail(L"Не удалось извлечь app.ico"); return 1;
    }

    // 3. Скачиваем ffmpeg если его нет
    std::wstring ffmpegDst = dir + L"\\ffmpeg.exe";
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 60,
        reinterpret_cast<LPARAM>(L"Установка ffmpeg..."));
    if (!extractResource(IDR_FFMPEG_EXE, dir + L"\\ffmpeg.exe")) {
        fail(L"Не удалось извлечь ffmpeg.exe"); return 1;
    }

    // 6. Регистрируем DLL через regsvr32
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 85,
        reinterpret_cast<LPARAM>(L"Регистрация расширения..."));
    std::wstring dllPath = dir + L"\\ShellExtension.dll";
    std::wstring regCmd = L"regsvr32 /s \"" + dllPath + L"\"";
    if (!runAndWait(regCmd)) {
        fail(L"Не удалось зарегистрировать ShellExtension.dll.\nЗапусти от имени администратора.");
        return 1;
    }

    

    // 7. Добавляем в реестр для удаления (Add/Remove Programs)
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 92,
        reinterpret_cast<LPARAM>(L"Регистрация в системе..."));

    std::wstring uninstDst = dir + L"\\uninst.exe";

    if (!extractResource(IDR_UNINSTALLER_EXE, uninstDst)) {
        fail(L"Не удалось извлечь uninst.exe"); return 1;
    }

    HKEY hk;
    RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter",
        0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);

    auto regStr = [&](const wchar_t* name, const wchar_t* val) {
        RegSetValueExW(hk, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(val),
            (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        };
    auto regDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(hk, name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        };

    regStr(L"DisplayName", L"FFmpeg Converter");
    regStr(L"Publisher", L"FFmpegShell");
    regStr(L"DisplayVersion", L"1.0.0");
    regStr(L"InstallLocation", dir.c_str());
    regStr(L"UninstallString", uninstDst.c_str());
    regStr(L"QuietUninstallString", (uninstDst + L" /quiet").c_str());
    regStr(L"DisplayIcon", (dir + L"\\Converter.exe").c_str());
    regStr(L"URLInfoAbout", L"https://github.com/");
    regStr(L"DisplayIcon", (dir + L"\\app.ico,0").c_str());
    regDword(L"NoModify", 1);
    regDword(L"NoRepair", 1);
    regDword(L"EstimatedSize", 80000); // ~80 MB в KB
    RegCloseKey(hk);

    PostMessage(g_hwnd, WM_INSTALL_DONE, 0, 0);
    return 0;
}

// ── Поток удаления ──

DWORD WINAPI uninstallThread(LPVOID) {
    auto fail = [](const wchar_t* msg) {
        PostMessage(g_hwnd, WM_INSTALL_ERROR, 0,
            reinterpret_cast<LPARAM>(_wcsdup(msg)));
        };

    wchar_t installDir[MAX_PATH] = {};
    DWORD sz = sizeof(installDir);
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter",
        0, KEY_READ, &hk) != ERROR_SUCCESS) {
        fail(L"FFmpeg Converter не найден в системе.");
        return 1;
    }
    RegQueryValueExW(hk, L"InstallLocation", nullptr, nullptr,
        reinterpret_cast<BYTE*>(installDir), &sz);
    RegCloseKey(hk);

    std::wstring dir = installDir;

    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 20,
        reinterpret_cast<LPARAM>(L"Отмена регистрации DLL..."));
    std::wstring dllPath = dir + L"\\ShellExtension.dll";
    runAndWait(L"regsvr32 /s /u \"" + dllPath + L"\"");

    // Убиваем Explorer чтобы DLL выгрузилась из памяти
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 40,
        reinterpret_cast<LPARAM>(L"Перезапуск проводника..."));
    runAndWait(L"taskkill /f /im explorer.exe");
    Sleep(2000);
    ShellExecuteW(nullptr, nullptr, L"explorer.exe", nullptr, nullptr, SW_SHOW);
    Sleep(1500);

    // Очистка реестра
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 70,
        reinterpret_cast<LPARAM>(L"Очистка реестра..."));
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter");

    // Удаляем папку через cmd отложенно (сам installer.exe там не живёт,
    // но uninst.exe и DLL уже свободны после перезапуска Explorer)
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 85,
        reinterpret_cast<LPARAM>(L"Удаление файлов..."));

    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring emptyDir = std::wstring(tempPath) + L"empty_ffmpeg_tmp";
    CreateDirectoryW(emptyDir.c_str(), nullptr);

    std::wstring delCmd =
        L"cmd /c timeout /t 2 /nobreak >nul"
        L" & robocopy \"" + emptyDir + L"\" \"" + dir + L"\" /MIR /NFL /NDL /NJH /NJS >nul"
        L" & rd /s /q \"" + dir + L"\""
        L" & rd /s /q \"" + emptyDir + L"\"";

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    CreateProcessW(nullptr, delCmd.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);

    PostMessage(g_hwnd, WM_INSTALL_DONE, 1, 0);
    return 0;
}

// ── UI ──

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INSTALL_PROGRESS: {
        int pct = (int)wParam;
        const wchar_t* text = reinterpret_cast<const wchar_t*>(lParam);
        SendMessage(g_progress, PBM_SETPOS, pct, 0);
        if (text) SetWindowTextW(g_labelStatus, text);
        return 0;
    }
    case WM_INSTALL_DONE: {
        SendMessage(g_progress, PBM_SETPOS, 100, 0);
        g_installing = false;
        EnableWindow(g_btnInstall, TRUE);
        EnableWindow(g_btnUninstall, TRUE);

        if (wParam == 0) {
            SetWindowTextW(g_labelStatus, L"✅ Установка завершена!");

            // Спрашиваем про перезапуск проводника
            int restartRes = MessageBoxW(g_hwnd,
                L"Для завершения установки необходимо перезапустить проводник.\nСделать это сейчас?",
                L"Перезапуск проводника", MB_YESNO | MB_ICONQUESTION);
            if (restartRes == IDYES) {
                runAndWait(L"taskkill /f /im explorer.exe");
                Sleep(1500);
                ShellExecuteW(nullptr, nullptr, L"explorer.exe", nullptr, nullptr, SW_SHOW);
            }

            MessageBoxW(g_hwnd,
                L"FFmpeg Converter установлен!\n\n"
                L"Выдели любой видео/аудио/изображение файл,\n"
                L"правой кнопкой → «FFmpeg».",
                L"Установка завершена", MB_OK | MB_ICONINFORMATION);

            DestroyWindow(g_hwnd); // ← закрываем installer
        }
        else {
            SetWindowTextW(g_labelStatus, L"✅ Удаление завершено.");
            MessageBoxW(g_hwnd, L"FFmpeg Converter удалён.",
                L"Удаление завершено", MB_OK | MB_ICONINFORMATION);
            DestroyWindow(g_hwnd);
        }
        return 0;
    }
    case WM_INSTALL_ERROR: {
        wchar_t* msg2 = reinterpret_cast<wchar_t*>(lParam);
        g_installing = false;
        EnableWindow(g_btnInstall, TRUE);
        EnableWindow(g_btnUninstall, TRUE);
        SendMessage(g_progress, PBM_SETSTATE, PBST_ERROR, 0);
        SetWindowTextW(g_labelStatus, L"❌ Ошибка установки.");
        MessageBoxW(hwnd, msg2, L"Ошибка", MB_OK | MB_ICONERROR);
        free(msg2);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_INSTALL: {
            if (g_installing) return 0;
            wchar_t pathBuf[MAX_PATH];
            GetWindowTextW(g_pathEdit, pathBuf, MAX_PATH);
            g_installing = true;
            EnableWindow(g_btnInstall, FALSE);
            EnableWindow(g_btnUninstall, FALSE);
            SendMessage(g_progress, PBM_SETSTATE, PBST_NORMAL, 0);
            SendMessage(g_progress, PBM_SETPOS, 0, 0);
            InstallArgs* args = new InstallArgs{ pathBuf };
            CreateThread(nullptr, 0, installThread, args, 0, nullptr);
            return 0;
        }
        case IDC_BTN_UNINSTALL: {
            if (g_installing) return 0;
            if (MessageBoxW(hwnd, L"Удалить FFmpeg Converter?",
                L"Подтверждение", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g_installing = true;
            EnableWindow(g_btnInstall, FALSE);
            EnableWindow(g_btnUninstall, FALSE);
            SendMessage(g_progress, PBM_SETSTATE, PBST_NORMAL, 0);
            SendMessage(g_progress, PBM_SETPOS, 0, 0);
            CreateThread(nullptr, 0, uninstallThread, nullptr, 0, nullptr);
            return 0;
        }
        case IDC_PATH_BROWSE: {
            // Диалог выбора папки
            wchar_t buf[MAX_PATH] = {};
            BROWSEINFOW bi{};
            bi.hwndOwner = hwnd;
            bi.pszDisplayName = buf;
            bi.lpszTitle = L"Выбери папку установки:";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                SHGetPathFromIDListW(pidl, buf);
                SetWindowTextW(g_pathEdit, buf);
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(245, 245, 245));
        SetTextColor(hdc, RGB(30, 30, 30));
        // Возвращаем кисть точно того же цвета что фон окна
        static HBRUSH hBrush = CreateSolidBrush(RGB(245, 245, 245));
        return reinterpret_cast<LRESULT>(hBrush);
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    CoInitialize(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = CreateSolidBrush(RGB(245, 245, 245));
    wc.lpszClassName = L"FFmpegInstallerWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    // Иконка окна installer
    HICON hAppIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIcon = hAppIcon;
    wc.hIconSm = (HICON)LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, 0);

    RegisterClassExW(&wc);


    int W = 500, H = 260;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME,
        L"FFmpegInstallerWnd", L"FFmpeg Converter — Установщик",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - W) / 2, (sy - H) / 2, W, H,
        nullptr, nullptr, hInst, nullptr);

    HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT hFontBold = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // Заголовок
    HWND hTitle = CreateWindowW(L"STATIC",
        L"🎬 FFmpeg Converter Shell Extension",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 15, 460, 24, g_hwnd, nullptr, nullptr, nullptr);
    SendMessage(hTitle, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    // Путь установки
    HWND hPathLabel = CreateWindowW(L"STATIC", L"Папка установки:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 50, 150, 18, g_hwnd, nullptr, nullptr, nullptr);
    SendMessage(hPathLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

    g_pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        getDefaultInstallDir().c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        20, 72, 370, 24, g_hwnd, (HMENU)IDC_PATH_EDIT, nullptr, nullptr);
    SendMessage(g_pathEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

    HWND hBrowse = CreateWindowW(L"BUTTON", L"...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        398, 72, 62, 24, g_hwnd, (HMENU)IDC_PATH_BROWSE, nullptr, nullptr);
    SendMessage(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Прогресс-бар
    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        20, 112, 460, 20, g_hwnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
    SendMessage(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    // Статус
    g_labelStatus = CreateWindowW(L"STATIC", L"Готов к установке.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 138, 460, 18, g_hwnd, (HMENU)IDC_LABEL_STATUS, nullptr, nullptr);
    SendMessage(g_labelStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Кнопки
    g_btnInstall = CreateWindowW(L"BUTTON", L"Установить",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 175, 150, 36, g_hwnd, (HMENU)IDC_BTN_INSTALL, nullptr, nullptr);
    SendMessage(g_btnInstall, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    g_btnUninstall = CreateWindowW(L"BUTTON", L"Удалить",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        330, 175, 150, 36, g_hwnd, (HMENU)IDC_BTN_UNINSTALL, nullptr, nullptr);
    SendMessage(g_btnUninstall, WM_SETFONT, (WPARAM)hFont, TRUE);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}