#pragma once
#define NOMINMAX
#include "ProcessRunner.h"
#include <string>
#include <vector>
#include <functional>
#include <regex>
#include <sstream>
#include <algorithm>

struct ConversionTask {
    std::wstring inputFile;
    std::wstring outputFile;
    std::string ffmpegArgs;   // например "-c:v libx264 -c:a aac"
};

struct ProgressInfo {
    int currentFile = 0;
    int totalFiles = 0;
    double fileProgress = 0.0;   // 0.0 - 1.0
    double totalProgress = 0.0;  // 0.0 - 1.0
    std::string currentFileName;
    bool done = false;
    bool error = false;
    std::string errorMessage;
};

class FFmpegRunner {
public:
    static std::wstring ffmpegPath;

    // Получить длительность файла в секундах
    static double getDuration(const std::wstring& file) {
        std::wstring cmd = L"\"" + ffmpegPath + L"\" -i \"" + file + L"\" 2>&1";
        // ffprobe точнее, но используем ffmpeg -i
        double duration = 0.0;
        auto result = ProcessRunner::run(
            L"\"" + ffmpegPath + L"\" -i \"" + file + L"\"",
            [&](const std::string& line) {
                // Duration: 00:01:23.45
                auto pos = line.find("Duration:");
                if (pos != std::string::npos) {
                    int h, m, s;
                    float ms;
                    if (sscanf_s(line.c_str() + pos + 10, "%d:%d:%d.%f", &h, &m, &s, &ms) >= 3) {
                        duration = h * 3600.0 + m * 60.0 + s + ms / 100.0;
                    }
                }
            }
        );
        return duration;
    }

    // Конвертирует список файлов, вызывает onProgress при каждом обновлении
    static bool convertAll(
        const std::vector<ConversionTask>& tasks,
        std::function<void(const ProgressInfo&)> onProgress
    ) {
        ProgressInfo info;
        info.totalFiles = static_cast<int>(tasks.size());

        for (int i = 0; i < static_cast<int>(tasks.size()); i++) {
            const auto& task = tasks[i];
            info.currentFile = i + 1;
            info.currentFileName = wstringToString(task.inputFile);
            info.fileProgress = 0.0;
            info.totalProgress = (double)i / info.totalFiles;
            onProgress(info);

            double duration = getDuration(task.inputFile);

            // Строим команду
            // ffmpeg -y -i "input" [args] "output"
            std::wstring cmd = L"\"" + ffmpegPath + L"\" -y -progress pipe:1 -i \""
                + task.inputFile + L"\" "
                + stringToWstring(task.ffmpegArgs)
                + L" \"" + task.outputFile + L"\"";

            double outTimeSeconds = 0.0;

            auto result = ProcessRunner::run(cmd, [&](const std::string& line) {
                // ffmpeg -progress выдаёт "out_time_ms=XXXXX"
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
                });

            if (result.exitCode != 0) {
                info.error = true;
                info.errorMessage = "FFmpeg failed on: " + wstringToString(task.inputFile);
                onProgress(info);
                return false;
            }
        }

        info.done = true;
        info.totalProgress = 1.0;
        info.fileProgress = 1.0;
        onProgress(info);
        return true;
    }

private:
    static std::string wstringToString(const std::wstring& ws) {
        std::string s(ws.begin(), ws.end());
        return s;
    }
    static std::wstring stringToWstring(const std::string& s) {
        return std::wstring(s.begin(), s.end());
    }
};

std::wstring FFmpegRunner::ffmpegPath = L"ffmpeg.exe";