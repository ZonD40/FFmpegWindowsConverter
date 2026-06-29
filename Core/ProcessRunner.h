#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <windows.h>

class ProcessRunner {
public:
    struct Result {
        int exitCode = -1;
        std::string output;
        bool timedOut = false;
    };

    // Обычный запуск (для getDuration и т.п.)
    static Result run(
        const std::wstring& command,
        std::function<void(const std::string&)> onLine = nullptr,
        DWORD timeoutMs = INFINITE
    ) {
        std::atomic<HANDLE> dummy{ nullptr };
        std::atomic<bool>   neverCancel{ false };
        return runCancellable(command, onLine, dummy, neverCancel, timeoutMs);
    }

    // Запуск с поддержкой отмены: сохраняет HANDLE процесса в outHandle
    static Result runCancellable(
        const std::wstring& command,
        std::function<void(const std::string&)> onLine,
        std::atomic<HANDLE>& outHandle,
        std::atomic<bool>& cancelled,
        DWORD timeoutMs = INFINITE
    ) {
        Result result;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadOut, hWriteOut;
        CreatePipe(&hReadOut, &hWriteOut, &sa, 0);
        SetHandleInformation(hReadOut, HANDLE_FLAG_INHERIT, 0);

        std::wstring cmdCopy = command;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.hStdOutput = hWriteOut;
        si.hStdError = hWriteOut;
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(
            nullptr, cmdCopy.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi
        );

        if (!ok) {
            CloseHandle(hReadOut);
            CloseHandle(hWriteOut);
            return result;
        }

        CloseHandle(hWriteOut);

        // Сохраняем хэндл процесса, чтобы снаружи можно было его убить
        // Дублируем, чтобы и мы и cancelCurrent могли закрыть независимо
        HANDLE hForKill = nullptr;
        DuplicateHandle(GetCurrentProcess(), pi.hProcess,
            GetCurrentProcess(), &hForKill,
            0, FALSE, DUPLICATE_SAME_ACCESS);
        outHandle.store(hForKill);

        HANDLE hWatchdog = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            auto* ctx = reinterpret_cast<std::pair<std::atomic<bool>*, HANDLE>*>(p);
            while (!ctx->first->load()) Sleep(100);
            TerminateProcess(ctx->second, 1);
            delete ctx;
            return 0;
            }, new std::pair<std::atomic<bool>*, HANDLE>(&cancelled, pi.hProcess), 0, nullptr);

        std::string buffer;
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hReadOut, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            buffer += buf;
            if (onLine) {
                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    onLine(line);
                    buffer = buffer.substr(pos + 1);
                }
            }
            if (cancelled) break;
        }
        if (onLine && !buffer.empty()) onLine(buffer);

        if (cancelled) {
            TerminateProcess(pi.hProcess, 1);
        }
        // Ждём реального завершения процесса перед возвратом
        WaitForSingleObject(pi.hProcess, 8000);
        WaitForSingleObject(hWatchdog, 2000);
        CloseHandle(hWatchdog);

        // Обнуляем outHandle — процесс уже завершён
        HANDLE stored = outHandle.exchange(nullptr);
        if (stored && stored != INVALID_HANDLE_VALUE)
            CloseHandle(stored);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        result.exitCode = static_cast<int>(exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadOut);

        return result;
    }
};