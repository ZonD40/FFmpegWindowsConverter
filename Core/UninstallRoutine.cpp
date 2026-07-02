#include "UninstallRoutine.h"
#include <windows.h>
#include <filesystem>
#include <shlobj.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

namespace UninstallRoutine {

    bool RunCommand(const std::wstring& cmd, DWORD timeoutMs = 15000) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};

        std::wstring cmdCopy = cmd;
        if (!CreateProcessW(nullptr, cmdCopy.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return false;

        WaitForSingleObject(pi.hProcess, timeoutMs);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }

    std::wstring GetInstallDir() {
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

    void UnregisterDLL(const std::wstring& dllPath) {
        std::wstring cmd = L"regsvr32 /s /u \"" + dllPath + L"\"";
        RunCommand(cmd);
    }

    void RestartExplorer() {
        HWND hShell = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (hShell) {
            PostMessage(hShell, WM_USER + 436, 0, 0);
            Sleep(1000);
        }
        else {
            RunCommand(L"taskkill /f /im explorer.exe", 5000);
            Sleep(1500);
        }
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", nullptr, nullptr, SW_SHOW);
        Sleep(1500);
    }

    void CleanupRegistry() {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FFmpegConverter");
    }

    void DeleteInstallationFolder(const std::wstring& dir) {
        wchar_t tempPath[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring emptyDir = std::wstring(tempPath) + L"empty_ffmpeg_tmp";
        CreateDirectoryW(emptyDir.c_str(), nullptr);

        std::wstring delCmd =
            L"cmd /c timeout /t 1 /nobreak >nul"
            L" & robocopy \"" + emptyDir + L"\" \"" + dir + L"\" /MIR /NFL /NDL /NJH /NJS >nul"
            L" & rd /s /q \"" + dir + L"\""
            L" & rd /s /q \"" + emptyDir + L"\"";

        STARTUPINFOW si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring delCmdCopy = delCmd;
        CreateProcessW(nullptr, delCmdCopy.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);
    }

    bool ExecuteUninstall(
        const std::wstring& installDir,
        bool askForExplorerRestart,
        bool showMessages
    ) {
        std::wstring dir = installDir;

        if (dir.empty()) {
            dir = GetInstallDir();
            if (dir.empty()) {
                if (showMessages) {
                    MessageBoxW(nullptr,
                        L"FFmpeg Converter not found in system.",
                        L"Error", MB_OK | MB_ICONERROR);
                }
                return false;
            }
        }

        std::wstring dllPath = dir + L"\\ShellExtension.dll";
        UnregisterDLL(dllPath);

        if (askForExplorerRestart) {
            int restartRes = MessageBoxW(nullptr,
                L"To complete the uninstallation, you need to restart Explorer.\n"
                L"Do this now?",
                L"Restart Explorer",
                MB_YESNO | MB_ICONQUESTION);

            if (restartRes == IDYES) {
                RestartExplorer();
            }
            else {
                Sleep(500);
            }
        }
        else {
            RestartExplorer();
        }

        CleanupRegistry();
        DeleteInstallationFolder(dir);

        return true;
    }
}