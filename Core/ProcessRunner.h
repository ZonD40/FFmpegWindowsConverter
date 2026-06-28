#pragma once
#include <string>
#include <functional>
#include <windows.h>

class ProcessRunner {
public:
    struct Result {
        int exitCode = -1;
        std::string output;
        bool timedOut = false;
    };

    // Запускает процесс, читает stdout+stderr построчно
    // onLine вызывается для каждой строки вывода
    static Result run(
        const std::wstring& command,
        std::function<void(const std::string&)> onLine = nullptr,
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
            result.exitCode = -1;
            CloseHandle(hReadOut);
            CloseHandle(hWriteOut);
            return result;
        }

        CloseHandle(hWriteOut); // Закрываем write-конец у родителя

        // Читаем вывод
        std::string buffer;
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hReadOut, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            buffer += buf;
            // Обрабатываем построчно
            if (onLine) {
                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    onLine(line);
                    buffer = buffer.substr(pos + 1);
                }
            }
        }
        if (onLine && !buffer.empty()) onLine(buffer);
        result.output = buffer;

        DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
        result.timedOut = (waitResult == WAIT_TIMEOUT);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        result.exitCode = static_cast<int>(exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadOut);

        return result;
    }
};