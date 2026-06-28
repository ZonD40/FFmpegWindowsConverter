#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/subsystem:windows")

namespace fs = std::filesystem;

std::wstring getInstallDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf);
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter",
        0, KEY_READ, &hk) != ERROR_SUCCESS)
        return L"";
    RegQueryValueExW(hk, L"InstallLocation", nullptr, nullptr,
        reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(hk);
    return buf;
}

void restartExplorer() {
    HWND hShell = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hShell) {
        PostMessage(hShell, WM_USER + 436, 0, 0);
    }
    else {
        std::wstring cmd = L"taskkill /f /im explorer.exe";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) { WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); }
        if (pi.hThread)  CloseHandle(pi.hThread);
    }
    Sleep(1500);
    ShellExecuteW(nullptr, nullptr, L"explorer.exe", nullptr, nullptr, SW_SHOW);
}

bool runAndWait(std::wstring cmd) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int res = MessageBoxW(nullptr,
        L"Вы действительно хотите удалить FFmpeg Converter?\n\n"
        L"Будут удалены все файлы программы и пункты контекстного меню.",
        L"Удаление FFmpeg Converter",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);

    if (res != IDYES) return 0;

    std::wstring dir = getInstallDir();
    if (dir.empty()) {
        MessageBoxW(nullptr,
            L"FFmpeg Converter не найден в системе.",
            L"Ошибка", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Снимаем регистрацию DLL
    std::wstring dllPath = dir + L"\\ShellExtension.dll";
    runAndWait(L"regsvr32 /s /u \"" + dllPath + L"\"");

    // Удаляем из реестра
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter");

    // Перезапускаем проводник чтобы DLL освободилась
    restartExplorer();
    Sleep(1000);

    // uninst.exe не может удалить себя напрямую — запускаем через cmd с задержкой
    std::wstring delCmd = L"cmd /c timeout /t 2 /nobreak >nul & rd /s /q \"" + dir + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    CreateProcessW(nullptr, delCmd.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);

    MessageBoxW(nullptr,
        L"FFmpeg Converter успешно удалён.",
        L"Готово", MB_OK | MB_ICONINFORMATION);

    return 0;
}