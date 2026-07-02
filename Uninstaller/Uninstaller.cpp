#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include "../Core/UninstallRoutine.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/subsystem:windows")

namespace fs = std::filesystem;

bool isRunningFromTemp() {
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    return _wcsnicmp(selfPath, tempPath, wcslen(tempPath)) == 0;
}

bool runCommand(const std::wstring& cmd) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {

    // Если запущены из temp — выполняем удаление из другого процесса
    if (isRunningFromTemp()) {
        std::wstring dir = lpCmdLine;
        if (!dir.empty() && dir.front() == L'"') dir = dir.substr(1);
        if (!dir.empty() && dir.back() == L'"') dir.pop_back();

        Sleep(1000);

        // Используем унифицированную логику удаления
        UninstallRoutine::ExecuteUninstall(dir, true, true);

        // 5. Удаляем себя из temp
        wchar_t selfPath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        std::wstring selfDelCmd = L"cmd /c timeout /t 3 /nobreak >nul & del /f /q \"";
        selfDelCmd += selfPath;
        selfDelCmd += L"\"";

        STARTUPINFOW si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        CreateProcessW(nullptr, (LPWSTR)selfDelCmd.c_str(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);

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

    std::wstring dir = UninstallRoutine::GetInstallDir();
    if (dir.empty()) {
        MessageBoxW(nullptr,
            L"FFmpeg Converter не найден в системе.",
            L"Ошибка", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Копируем себя во временную папку и запускаем оттуда
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring tempExe = std::wstring(tempPath) + L"uninst_ffmpeg_tmp.exe";

    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    CopyFileW(selfPath, tempExe.c_str(), FALSE);

    std::wstring cmd = L"\"" + tempExe + L"\" \"" + dir + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);

    return 0;
}