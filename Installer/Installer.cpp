
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

    // 2. Копируем файлы из папки установщика
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 10,
        reinterpret_cast<LPARAM>(L"Копирование файлов..."));

    // Converter.exe и ShellExtension.dll копируем рядом с installer
    std::wstring srcDir = getInstallerDir();
    for (const auto& f : std::vector<std::wstring>{ L"Converter.exe", L"ShellExtension.dll" }) {
        std::wstring src = srcDir + L"\\" + f;
        std::wstring dst = dir + L"\\" + f;
        if (fs::exists(src)) {
            try { fs::copy_file(src, dst, fs::copy_options::overwrite_existing); }
            catch (...) { fail((L"Не удалось скопировать: " + f).c_str()); return 1; }
        }
    }

    // presets.json извлекаем из ресурсов exe
    if (!extractResource(IDR_PRESETS_JSON, dir + L"\\presets.json")) {
        fail(L"Не удалось извлечь presets.json");
        return 1;
    }

    // Копируем uninst.exe
    std::wstring uninstSrc = srcDir + L"\\Uninstaller.exe";
    std::wstring uninstDst = dir + L"\\uninst.exe";
    if (fs::exists(uninstSrc)) {
        fs::copy_file(uninstSrc, uninstDst, fs::copy_options::overwrite_existing);
    }

    // 3. Скачиваем ffmpeg если его нет
    std::wstring ffmpegDst = dir + L"\\ffmpeg.exe";
    if (!fs::exists(ffmpegDst)) {
        PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 20,
            reinterpret_cast<LPARAM>(L"Скачивание ffmpeg..."));

        std::wstring zipPath = dir + L"\\ffmpeg.zip";

        // Официальная сборка ffmpeg для Windows (essentials)
        const wchar_t* url =
            L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";

        DownloadCallback cb(g_hwnd);
        HRESULT hr = URLDownloadToFileW(nullptr, url, zipPath.c_str(), 0, &cb);
        if (FAILED(hr) || !fs::exists(zipPath)) {
            fail(L"Не удалось скачать ffmpeg.\nПроверь интернет-соединение.");
            return 1;
        }

        // 4. Распаковываем
        PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 62,
            reinterpret_cast<LPARAM>(L"Распаковка ffmpeg..."));
        std::wstring extractDir = dir + L"\\ffmpeg_tmp";
        if (!extractZip(zipPath, extractDir)) {
            fail(L"Не удалось распаковать ffmpeg.zip");
            return 1;
        }

        // 5. Находим ffmpeg.exe и копируем
        PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 75,
            reinterpret_cast<LPARAM>(L"Установка ffmpeg..."));
        std::wstring foundFFmpeg = findFFmpeg(extractDir);
        if (foundFFmpeg.empty()) {
            fail(L"ffmpeg.exe не найден в архиве.");
            return 1;
        }
        fs::copy_file(foundFFmpeg, ffmpegDst, fs::copy_options::overwrite_existing);

        // Чистим временные файлы
        try {
            fs::remove(zipPath);
            fs::remove_all(extractDir);
        }
        catch (...) {}
    }
    else {
        PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 75,
            reinterpret_cast<LPARAM>(L"ffmpeg уже установлен, пропускаем..."));
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

    // Перезапускаем проводник
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 96,
        reinterpret_cast<LPARAM>(L"Перезапуск проводника..."));

    // Мягкий способ — через Shell API
    HWND hShell = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hShell) {
        // Graceful exit проводника
        PostMessage(hShell, WM_USER + 436, 0, 0);
        Sleep(1500);
    }
    else {
        // Жёсткий если не нашли
        runAndWait(L"taskkill /f /im explorer.exe");
        Sleep(1000);
    }
    // Запускаем заново
    ShellExecuteW(nullptr, nullptr, L"explorer.exe",
        nullptr, nullptr, SW_SHOW);

    // 7. Добавляем в реестр для удаления (Add/Remove Programs)
    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 92,
        reinterpret_cast<LPARAM>(L"Регистрация в системе..."));
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

    // Читаем путь установки из реестра
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

    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 20,
        reinterpret_cast<LPARAM>(L"Отмена регистрации DLL..."));
    std::wstring dllPath = std::wstring(installDir) + L"\\ShellExtension.dll";
    runAndWait(L"regsvr32 /s /u \"" + dllPath + L"\"");

    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 50,
        reinterpret_cast<LPARAM>(L"Удаление файлов..."));
    try { fs::remove_all(installDir); }
    catch (...) {}

    PostMessage(g_hwnd, WM_INSTALL_PROGRESS, 80,
        reinterpret_cast<LPARAM>(L"Очистка реестра..."));
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter");

    PostMessage(g_hwnd, WM_INSTALL_DONE, 1, 0); // wParam=1 → удаление
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
            SetWindowTextW(g_labelStatus, L"✅ Установка завершена! Перезапусти Проводник.");
            MessageBoxW(hwnd,
                L"FFmpeg Converter установлен!\n\n"
                L"Выдели любой видео/аудио/изображение файл,\n"
                L"правой кнопкой → «Convert with FFmpeg».\n\n"
                L"Если меню не появилось — перезапусти Проводник\n"
                L"или выйди и войди в Windows.",
                L"Установка завершена", MB_OK | MB_ICONINFORMATION);
        }
        else {
            SetWindowTextW(g_labelStatus, L"✅ Удаление завершено.");
            MessageBoxW(hwnd, L"FFmpeg Converter удалён.",
                L"Удаление завершено", MB_OK | MB_ICONINFORMATION);
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
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
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