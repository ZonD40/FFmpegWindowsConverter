#pragma once
#include <string>
#include <vector>
#include <algorithm>

enum class FileType {
    Video,
    Audio,
    Image,
    Raw,
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
            L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".tiff", L".gif", L".heif", L".heic"
        };
        static const std::vector<std::wstring> rawExts = {
            L".cr2", L".cr3", L".nef", L".arw", L".dng", L".raf", L".orf", L".rw2", L".pef", L".srw", L".raw"
        };

        if (contains(videoExts, ext)) return FileType::Video;
        if (contains(audioExts, ext)) return FileType::Audio;
        if (contains(imageExts, ext)) return FileType::Image;
        if (contains(rawExts, ext))   return FileType::Raw;
        return FileType::Unknown;
    }

    static std::string fileTypeToString(FileType type) {
        switch (type) {
        case FileType::Video: return "Video";
        case FileType::Audio: return "Audio";
        case FileType::Image: return "Image";
        case FileType::Raw: return "Raw";
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