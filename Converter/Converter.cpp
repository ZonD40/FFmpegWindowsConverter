
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <sstream>
#include <filesystem>

// Подключаем Core
#include "../Core/FFmpegRunner.h"
#include "../Core/PresetManager.h"
#include "../Core/FileClassifier.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows")

// ID контролов
#define IDC_PROGRESS_TOTAL   101
#define IDC_PROGRESS_FILE    102
#define IDC_LABEL_STATUS     103
#define IDC_LABEL_FILE       104
#define IDC_BTN_CANCEL       105
#define IDC_LABEL_TOTAL_TXT  106

// Кастомные сообщения
#define WM_CONVERSION_PROGRESS  (WM_USER + 1)
#define WM_CONVERSION_DONE      (WM_USER + 2)
#define WM_CONVERSION_ERROR     (WM_USER + 3)

// Глобальные переменные
HWND g_hwnd = nullptr;
HWND g_progressTotal = nullptr;
HWND g_progressFile = nullptr;
HWND g_labelStatus = nullptr;
HWND g_labelFile = nullptr;
HWND g_btnCancel = nullptr;
bool g_cancelled = false;
HANDLE g_hDoneEvent = nullptr;

struct ConversionArgs {
    std::vector<std::wstring> inputFiles;
    std::wstring targetExt;
    std::wstring ffmpegPath;
    std::wstring presetsPath;
};

struct ProgressMessage {
    ProgressInfo info;
};

// Поток конвертации
DWORD WINAPI conversionThread(LPVOID param) {
    ConversionArgs* args = reinterpret_cast<ConversionArgs*>(param);

    FFmpegRunner::ffmpegPath = args->ffmpegPath;

    // Строим задачи
    PresetManager pm;
    pm.load(args->presetsPath);

    std::vector<ConversionTask> tasks;
    for (const auto& file : args->inputFiles) {
        namespace fs = std::filesystem;
        fs::path p(file);
        std::string ext = p.extension().string();

        const Preset* preset = pm.findPresetForExtension(ext);
        std::string ffmpegArgs = "";
        if (preset) {
            for (const auto& conv : preset->conversions) {
                if (conv.targetExt == std::string(args->targetExt.begin(), args->targetExt.end())) {
                    ffmpegArgs = conv.ffmpegArgs;
                    break;
                }
            }
        }

        ConversionTask task;
        task.inputFile = file;
        std::wstring basePath = (p.parent_path() / p.stem()).wstring();
        task.outputFile = FFmpegRunner::makeUniqueOutputPath(basePath, args->targetExt);
        task.ffmpegArgs = ffmpegArgs;
        tasks.push_back(task);
    }

    bool success = FFmpegRunner::convertAll(tasks, [&](const ProgressInfo& info) {
        if (g_cancelled) return;

        // Отправляем в UI поток через PostMessage
        ProgressMessage* msg = new ProgressMessage{ info };

        if (info.done) {
            PostMessage(g_hwnd, WM_CONVERSION_DONE, 0, reinterpret_cast<LPARAM>(msg));
        }
        else if (info.error) {
            PostMessage(g_hwnd, WM_CONVERSION_ERROR, 0, reinterpret_cast<LPARAM>(msg));
        }
        else {
            PostMessage(g_hwnd, WM_CONVERSION_PROGRESS, 0, reinterpret_cast<LPARAM>(msg));
        }
        });

    delete args;
    if (g_hDoneEvent) SetEvent(g_hDoneEvent);
    return 0;
}

// Обновляем UI по прогрессу
void updateProgress(const ProgressInfo& info) {
    // Общий прогресс
    int totalPct = static_cast<int>(info.totalProgress * 100);
    SendMessage(g_progressTotal, PBM_SETPOS, totalPct, 0);

    // Прогресс текущего файла
    int filePct = static_cast<int>(info.fileProgress * 100);
    SendMessage(g_progressFile, PBM_SETPOS, filePct, 0);

    // Статус
    std::wstring status = L"Файл " + std::to_wstring(info.currentFile)
        + L" из " + std::to_wstring(info.totalFiles);
    SetWindowTextW(g_labelStatus, status.c_str());

    // Имя файла (только само имя, без пути)
    std::wstring fileName(info.currentFileName.begin(), info.currentFileName.end());
    namespace fs = std::filesystem;
    try {
        fileName = fs::path(fileName).filename().wstring();
    }
    catch (...) {}
    SetWindowTextW(g_labelFile, fileName.c_str());
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CONVERSION_PROGRESS: {
        ProgressMessage* pm = reinterpret_cast<ProgressMessage*>(lParam);
        updateProgress(pm->info);
        delete pm;
        return 0;
    }
    case WM_CONVERSION_DONE: {
        ProgressMessage* pm = reinterpret_cast<ProgressMessage*>(lParam);

        SendMessage(g_progressTotal, PBM_SETPOS, 100, 0);
        SendMessage(g_progressFile, PBM_SETPOS, 100, 0);
        SetWindowTextW(g_labelStatus, L"Готово!");
        SetWindowTextW(g_labelFile, L"Конвертация завершена успешно.");
        SetWindowTextW(g_btnCancel, L"Закрыть");

        delete pm;

        // Закрываем окно через 100 милисекунд
        SetTimer(g_hwnd, 1, 100, nullptr);
        return 0;
    }
    case WM_CONVERSION_ERROR: {
        ProgressMessage* pm = reinterpret_cast<ProgressMessage*>(lParam);

        std::wstring errMsg(pm->info.errorMessage.begin(), pm->info.errorMessage.end());
        SetWindowTextW(g_labelStatus, L"Ошибка!");
        SetWindowTextW(g_labelFile, errMsg.c_str());
        SetWindowTextW(g_btnCancel, L"Закрыть");

        // Красим прогресс в красный
        SendMessage(g_progressTotal, PBM_SETSTATE, PBST_ERROR, 0);
        SendMessage(g_progressFile, PBM_SETSTATE, PBST_ERROR, 0);

        delete pm;
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_CANCEL) {
            g_cancelled = true;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_CTLCOLORSTATIC: {
        // Белый фон для статик-лейблов
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(245, 245, 245));
        SetTextColor(hdc, RGB(30, 30, 30));
        // Возвращаем кисть точно того же цвета что фон окна
        static HBRUSH hBrush = CreateSolidBrush(RGB(245, 245, 245));
        return reinterpret_cast<LRESULT>(hBrush);
    }
    case WM_TIMER:
        if (wParam == 1) {
            KillTimer(hwnd, 1);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Создаём окно
HWND createWindow(const std::wstring& targetExt, int fileCount) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = CreateSolidBrush(RGB(245, 245, 245));
    wc.lpszClassName = L"FFmpegConverterWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int W = 480, H = 240;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"FFmpegConverterWindow",
        L"FFmpeg Converter",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (screenW - W) / 2, (screenH - H) / 2,
        W, H,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );

    HFONT hFontNormal = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT hFontBold = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // Заголовок
    std::wstring title = L"Конвертация " + std::to_wstring(fileCount)
        + L" файл(ов) → " + targetExt;
    HWND hTitle = CreateWindowW(L"STATIC", title.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 15, 440, 22, hwnd, nullptr, nullptr, nullptr);
    SendMessage(hTitle, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    // Лейбл статуса (Файл X из Y)
    g_labelStatus = CreateWindowW(L"STATIC", L"Подготовка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 45, 440, 18, hwnd, (HMENU)IDC_LABEL_STATUS, nullptr, nullptr);
    SendMessage(g_labelStatus, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

    // Прогресс текущего файла
    g_progressFile = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        20, 68, 440, 20, hwnd, (HMENU)IDC_PROGRESS_FILE, nullptr, nullptr);
    SendMessage(g_progressFile, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_progressFile, PBM_SETPOS, 0, 0);

    // Имя текущего файла
    g_labelFile = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        20, 93, 440, 16, hwnd, (HMENU)IDC_LABEL_FILE, nullptr, nullptr);
    SendMessage(g_labelFile, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

    // Лейбл общего прогресса
    HWND hTotalTxt = CreateWindowW(L"STATIC", L"Общий прогресс:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 116, 200, 18, hwnd, (HMENU)IDC_LABEL_TOTAL_TXT, nullptr, nullptr);
    SendMessage(hTotalTxt, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

    // Общий прогресс-бар
    g_progressTotal = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        20, 138, 440, 20, hwnd, (HMENU)IDC_PROGRESS_TOTAL, nullptr, nullptr);
    SendMessage(g_progressTotal, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_progressTotal, PBM_SETPOS, 0, 0);

    // Кнопка Отмена
    g_btnCancel = CreateWindowW(L"BUTTON", L"Отмена",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        350, 190, 110, 32, hwnd, (HMENU)IDC_BTN_CANCEL, nullptr, nullptr);
    SendMessage(g_btnCancel, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    return hwnd;
}

// Парсим аргументы командной строки
// Формат: Converter.exe --target .mp4 --ffmpeg "C:\path\ffmpeg.exe" --presets "C:\path\presets.json" "file1.mov" "file2.mov"
struct ParsedArgs {
    std::wstring targetExt;
    std::wstring ffmpegPath;
    std::wstring presetsPath;
    std::vector<std::wstring> files;
    bool valid = false;
};

ParsedArgs parseArgs(int argc, wchar_t* argv[]) {
    ParsedArgs result;
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--target" && i + 1 < argc) {
            result.targetExt = argv[++i];
        }
        else if (arg == L"--ffmpeg" && i + 1 < argc) {
            result.ffmpegPath = argv[++i];
        }
        else if (arg == L"--presets" && i + 1 < argc) {
            result.presetsPath = argv[++i];
        }
        else if (arg[0] != L'-') {
            result.files.push_back(arg);
        }
    }
    result.valid = !result.targetExt.empty() && !result.files.empty() && !result.ffmpegPath.empty();
    return result;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    ParsedArgs args = parseArgs(argc, argv);
    LocalFree(argv);

    if (!args.valid) {
        MessageBoxW(nullptr,
            L"Использование:\nConverter.exe --target .mp4 --ffmpeg \"путь\\ffmpeg.exe\" --presets \"путь\\presets.json\" \"file1\" \"file2\"",
            L"FFmpeg Converter", MB_OK | MB_ICONERROR);
        return 1;
    }

    bool showWindow = false;
    if (args.files.size() > 1) {
        showWindow = true;
    }
    else if (args.files.size() == 1) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(args.files[0].c_str(), GetFileExInfoStandard, &fa)) {
            ULONGLONG size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            showWindow = size > 10ULL * 1024 * 1024;
        }
    }

    ConversionArgs* convArgs = new ConversionArgs{
        args.files, args.targetExt, args.ffmpegPath, args.presetsPath
    };

    if (showWindow) {
        g_hwnd = createWindow(args.targetExt, static_cast<int>(args.files.size()));
        CreateThread(nullptr, 0, conversionThread, convArgs, 0, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    else {
        g_hDoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CreateThread(nullptr, 0, conversionThread, convArgs, 0, nullptr);
        WaitForSingleObject(g_hDoneEvent, INFINITE);
        CloseHandle(g_hDoneEvent);
    }
    return 0;
}