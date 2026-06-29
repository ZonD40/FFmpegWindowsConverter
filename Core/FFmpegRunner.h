#pragma once
#define NOMINMAX
#include "ProcessRunner.h"
#include "Logger.h"
#include <string>
#include <vector>
#include <functional>
#include <regex>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <atomic>

struct ConversionTask {
    std::wstring inputFile;
    std::wstring outputFile;
    std::string ffmpegArgs;
};

struct ProgressInfo {
    int currentFile = 0;
    int totalFiles = 0;
    double fileProgress = 0.0;
    double totalProgress = 0.0;
    std::string currentFileName;
    bool done = false;
    bool error = false;
    std::string errorMessage;
};

class FFmpegRunner {
public:
    static std::wstring ffmpegPath;

    // Хэндл текущего процесса ffmpeg — чтобы можно было убить его при отмене
    static std::atomic<HANDLE> currentProcess;

    static void cancelCurrent() {
        HANDLE h = currentProcess.exchange(nullptr);
        if (h && h != INVALID_HANDLE_VALUE) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
    }

    static double getDuration(const std::wstring& file) {
        double duration = 0.0;
        ProcessRunner::run(
            L"\"" + ffmpegPath + L"\" -i \"" + file + L"\"",
            [&](const std::string& line) {
                auto pos = line.find("Duration:");
                if (pos != std::string::npos) {
                    int h, m, s;
                    float ms;
                    if (sscanf_s(line.c_str() + pos + 10, "%d:%d:%d.%f", &h, &m, &s, &ms) >= 3)
                        duration = h * 3600.0 + m * 60.0 + s + ms / 100.0;
                }
            }
        );
        return duration;
    }

    static bool convertAll(
        const std::vector<ConversionTask>& tasks,
        std::function<void(const ProgressInfo&)> onProgress,
        std::atomic<bool>& cancelled
    ) {
        ProgressInfo info;
        info.totalFiles = static_cast<int>(tasks.size());

        for (int i = 0; i < static_cast<int>(tasks.size()); i++) {
            if (cancelled) break;

            const auto& task = tasks[i];
            info.currentFile = i + 1;
            info.currentFileName = wstringToString(task.inputFile);
            info.fileProgress = 0.0;
            info.totalProgress = (double)i / info.totalFiles;
            onProgress(info);

            double duration = getDuration(task.inputFile);

            std::wstring cmd = L"\"" + ffmpegPath + L"\" -y -progress pipe:1 -i \""
                + task.inputFile + L"\" "
                + stringToWstring(task.ffmpegArgs)
                + L" \"" + task.outputFile + L"\"";

            double outTimeSeconds = 0.0;

            auto result = ProcessRunner::runCancellable(cmd,
                [&](const std::string& line) {
                    if (line.rfind("out_time_ms=", 0) == 0) {
                        long long ms = 0;
                        try { ms = std::stoll(line.substr(12)); }
                        catch (...) {}
                        outTimeSeconds = ms / 1000000.0;
                        if (duration > 0) {
                            double fp = outTimeSeconds / duration;
                            info.fileProgress = fp < 1.0 ? fp : 1.0;
                            info.totalProgress = ((double)i + info.fileProgress) / info.totalFiles;
                            onProgress(info);
                        }
                    }
                },
                currentProcess,
                cancelled
            );

            // Если отменили — удаляем недоделанный выходной файл
            if (cancelled) {
                break;
            }

            if (result.exitCode != 0) {
                namespace fs = std::filesystem;
                std::error_code ec;
                bool outputExists = fs::exists(task.outputFile, ec)
                    && fs::file_size(task.outputFile, ec) > 0;
                if (outputExists) {
                    // Файл создан успешно, ffmpeg завершился с ненулевым кодом 
                    // (типично для GIF→image: обработал первый кадр и вышел)
                    // Пишем предупреждение в лог, но не считаем ошибкой
                    Logger::log(L"Warning: ffmpeg exit code " + std::to_wstring(result.exitCode)
                        + L" but output exists: " + task.outputFile);
                }
                else {
                    info.error = true;
                    info.errorMessage = "FFmpeg error (code " + std::to_string(result.exitCode)
                        + "): " + wstringToString(task.inputFile);
                    Logger::log(L"Error: " + std::wstring(info.errorMessage.begin(), info.errorMessage.end())
                        + L"\nFFmpeg output:\n" + std::wstring(result.output.begin(), result.output.end()));
                    onProgress(info);
                    return false;
                }
            }
        }

        if (!cancelled) {
            info.done = true;
            info.totalProgress = 1.0;
            info.fileProgress = 1.0;
            onProgress(info);
        }
        return !cancelled;
    }

    static std::wstring makeUniqueOutputPath(const std::wstring& base, const std::wstring& ext) {
        namespace fs = std::filesystem;
        std::wstring candidate = base + ext;
        if (!fs::exists(candidate)) return candidate;
        for (int i = 1; i < 1000; i++) {
            candidate = base + L" (" + std::to_wstring(i) + L")" + ext;
            if (!fs::exists(candidate)) return candidate;
        }
        return candidate;
    }

private:
    static std::string wstringToString(const std::wstring& ws) {
        return std::string(ws.begin(), ws.end());
    }
    static std::wstring stringToWstring(const std::string& s) {
        return std::wstring(s.begin(), s.end());
    }
};

std::wstring FFmpegRunner::ffmpegPath = L"ffmpeg.exe";
std::atomic<HANDLE> FFmpegRunner::currentProcess{ nullptr };