#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <windows.h>

struct Conversion {
    std::string label;
    std::string targetExt;
    std::string ffmpegArgs;
};

struct Preset {
    std::string name;
    std::vector<std::string> extensions;
    std::vector<Conversion> conversions;
};

// Минимальный JSON-парсер для наших нужд
class PresetManager {
public:
    static std::wstring getPresetsPath() {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir(path);
        dir = dir.substr(0, dir.rfind(L'\\'));
        return dir + L"\\presets.json";
    }

    bool load(const std::wstring& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        return parse(content);
    }

    const std::vector<Preset>& getPresets() const { return presets_; }

    const Preset* findPresetForExtension(const std::string& ext) const {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (const auto& p : presets_) {
            for (const auto& e : p.extensions) {
                if (e == lower) return &p;
            }
        }
        return nullptr;
    }

private:
    std::vector<Preset> presets_;

    // Простой парсер — ищет нужные строки
    bool parse(const std::string& json) {
        presets_.clear();
        size_t pos = 0;

        while ((pos = json.find("\"name\"", pos)) != std::string::npos) {
            Preset preset;
            pos += 6;
            preset.name = extractString(json, pos);

            size_t extStart = json.find("\"extensions\"", pos);
            size_t convStart = json.find("\"conversions\"", pos);
            if (extStart == std::string::npos || convStart == std::string::npos) break;

            // Парсим extensions
            size_t arrStart = json.find('[', extStart);
            size_t arrEnd = json.find(']', arrStart);
            if (arrStart == std::string::npos || arrEnd == std::string::npos) break;
            size_t p2 = arrStart;
            while (p2 < arrEnd) {
                size_t qs = json.find('"', p2);
                if (qs == std::string::npos || qs > arrEnd) break;
                size_t qe = json.find('"', qs + 1);
                if (qe == std::string::npos || qe > arrEnd) break;
                preset.extensions.push_back(json.substr(qs + 1, qe - qs - 1));
                p2 = qe + 1;
            }

            // Парсим conversions
            size_t cArrStart = json.find('[', convStart);
            size_t cArrEnd = findMatchingBracket(json, cArrStart);
            size_t p3 = cArrStart;
            while (p3 < cArrEnd) {
                size_t objStart = json.find('{', p3);
                if (objStart == std::string::npos || objStart > cArrEnd) break;
                size_t objEnd = findMatchingBrace(json, objStart);

                Conversion conv;
                size_t tmp = objStart;
                size_t labelPos = json.find("\"label\"", objStart);
                size_t targetPos = json.find("\"targetExt\"", objStart);
                size_t argsPos = json.find("\"ffmpegArgs\"", objStart);

                if (labelPos < objEnd) { tmp = labelPos + 7; conv.label = extractString(json, tmp); }
                if (targetPos < objEnd) { tmp = targetPos + 11; conv.targetExt = extractString(json, tmp); }
                if (argsPos < objEnd) { tmp = argsPos + 12; conv.ffmpegArgs = extractString(json, tmp); }

                if (!conv.label.empty()) preset.conversions.push_back(conv);
                p3 = objEnd + 1;
            }

            presets_.push_back(preset);
            pos = convStart + 1;
        }
        return !presets_.empty();
    }

    std::string extractString(const std::string& json, size_t& pos) {
        size_t qs = json.find('"', pos);
        if (qs == std::string::npos) return "";
        size_t qe = json.find('"', qs + 1);
        if (qe == std::string::npos) return "";
        pos = qe + 1;
        return json.substr(qs + 1, qe - qs - 1);
    }

    size_t findMatchingBracket(const std::string& s, size_t start) {
        int depth = 0;
        for (size_t i = start; i < s.size(); i++) {
            if (s[i] == '[') depth++;
            else if (s[i] == ']') { depth--; if (depth == 0) return i; }
        }
        return std::string::npos;
    }

    size_t findMatchingBrace(const std::string& s, size_t start) {
        int depth = 0;
        for (size_t i = start; i < s.size(); i++) {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') { depth--; if (depth == 0) return i; }
        }
        return std::string::npos;
    }
};