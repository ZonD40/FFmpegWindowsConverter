#pragma once
#include <windows.h>
#include <string>
#include <fstream>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

class Logger {
public:
    static std::wstring getLogPath() {
        // Логи в %LOCALAPPDATA%\FFmpegConverter\converter.log
        // — туда не нужны права админа
        wchar_t buf[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf);
        std::wstring dir = std::wstring(buf) + L"\\FFmpegConverter";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\log.log";
    }

    static void log(const std::wstring& message) {
        std::wstring path = getLogPath();
        std::wofstream f(path, std::ios::app);
        if (!f.is_open()) return;

        // Временная метка
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t ts[32];
        swprintf_s(ts, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);

        f << ts << message << L"\n";
    }

    // Логируем весь вывод ffmpeg при ошибке
    static void logFFmpegError(const std::wstring& inputFile,
        int exitCode,
        const std::string& ffmpegOutput) {
        std::wstring msg = L"FFmpeg failed (exit " + std::to_wstring(exitCode) + L")\n"
            + L"  Input: " + inputFile + L"\n"
            + L"  Output from ffmpeg:\n";
        log(msg);

        // Пишем вывод ffmpeg построчно
        std::wstring path = getLogPath();
        std::ofstream f(path, std::ios::app | std::ios::binary);
        if (f.is_open()) f << ffmpegOutput << "\n---\n";
    }
};