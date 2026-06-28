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
        if (pi.hProcess) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
        }
        if (pi.hThread) CloseHandle(pi.hThread);
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

// Проверяем — запущены ли мы из temp (второй экземпляр)
bool isRunningFromTemp() {
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    return _wcsnicmp(selfPath, tempPath, wcslen(tempPath)) == 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {

    // Если мы запущены из temp — выполняем реальное удаление
    if (isRunningFromTemp()) {
        std::wstring dir = lpCmdLine;
        if (!dir.empty() && dir.front() == L'"') dir = dir.substr(1);
        if (!dir.empty() && dir.back() == L'"') dir.pop_back();

        Sleep(1000);

        // 1. Снимаем регистрацию DLL
        std::wstring dllPath = dir + L"\\ShellExtension.dll";
        runAndWait(L"regsvr32 /s /u \"" + dllPath + L"\"");

        // 2. Спрашиваем про перезапуск проводника
        int restartRes = MessageBoxW(nullptr,
            L"Для завершения удаления необходимо перезапустить проводник.\nСделать это сейчас?",
            L"Перезапуск проводника", MB_YESNO | MB_ICONQUESTION);
        if (restartRes == IDYES) {
            // Убиваем проводник жёстко чтобы DLL точно освободилась
            runAndWait(L"taskkill /f /im explorer.exe");
            Sleep(2000);
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", nullptr, nullptr, SW_SHOW);
            Sleep(1500);
        }
        else {
            // Даже если не перезапустил — пробуем удалить через таймаут
            Sleep(500);
        }

        // 3. Удаляем реестр
        RegDeleteTreeW(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter");

        // 4. Удаляем папку через cmd с повторными попытками
        // Используем robocopy трюк: создаём пустую папку и синхронизируем с ней
        wchar_t tempPath[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring emptyDir = std::wstring(tempPath) + L"empty_ffmpeg_tmp";
        CreateDirectoryW(emptyDir.c_str(), nullptr);

        // robocopy копирует пустую папку поверх, потом удаляем обе
        std::wstring delCmd =
            L"cmd /c timeout /t 1 /nobreak >nul"
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

        // 5. Удаляем себя из temp
        wchar_t selfPath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        std::wstring selfDelCmd = L"cmd /c timeout /t 3 /nobreak >nul & del /f /q \"";
        selfDelCmd += selfPath;
        selfDelCmd += L"\"";
        STARTUPINFOW si2{}; si2.cb = sizeof(si2);
        si2.dwFlags = STARTF_USESHOWWINDOW;
        si2.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi2{};
        CreateProcessW(nullptr, selfDelCmd.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2);
        if (pi2.hProcess) CloseHandle(pi2.hProcess);
        if (pi2.hThread)  CloseHandle(pi2.hThread);

        MessageBoxW(nullptr,
            L"FFmpeg Converter успешно удалён.",
            L"Готово", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Первый запуск — спрашиваем подтверждение
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

    // Копируем себя во временную папку
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring tempExe = std::wstring(tempPath) + L"uninst_ffmpeg_tmp.exe";

    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    CopyFileW(selfPath, tempExe.c_str(), FALSE);

    // Запускаем копию из temp с папкой установки как аргументом
    std::wstring cmd = L"\"" + tempExe + L"\" \"" + dir + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);

    // Оригинальный процесс завершается — temp-копия сделает всё остальное
    return 0;
}