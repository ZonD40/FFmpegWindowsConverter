#pragma once
#include <string>
#include <vector>
#include <algorithm>

enum class FileType {
    Video,
    Audio,
    Image,
    Unknown
};

class FileClassifier {
public:
    static FileType classify(const std::wstring& extension) {
        std::wstring ext = toLower(extension);

        static const std::vector<std::wstring> videoExts = {
            L".mp4", L".mov", L".avi", L".mkv", L".wmv", L".flv", L".webm", L".m4v"
        };
        static const std::vector<std::wstring> audioExts = {
            L".mp3", L".wav", L".flac", L".ogg", L".m4a", L".aac", L".wma", L".opus"
        };
        static const std::vector<std::wstring> imageExts = {
            L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".tiff", L".gif"
        };

        if (contains(videoExts, ext)) return FileType::Video;
        if (contains(audioExts, ext)) return FileType::Audio;
        if (contains(imageExts, ext)) return FileType::Image;
        return FileType::Unknown;
    }

    static std::string fileTypeToString(FileType type) {
        switch (type) {
        case FileType::Video: return "Video";
        case FileType::Audio: return "Audio";
        case FileType::Image: return "Image";
        default: return "Unknown";
        }
    }

private:
    static bool contains(const std::vector<std::wstring>& vec, const std::wstring& val) {
        return std::find(vec.begin(), vec.end(), val) != vec.end();
    }

    static std::wstring toLower(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    }
};