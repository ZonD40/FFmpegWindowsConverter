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

#include "../Core/FFmpegRunner.h"
#include "../Core/PresetManager.h"
#include "../Core/FileClassifier.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/subsystem:windows")

#define IDC_PROGRESS_TOTAL   101
#define IDC_PROGRESS_FILE    102
#define IDC_LABEL_STATUS     103
#define IDC_LABEL_FILE       104
#define IDC_BTN_CANCEL       105
#define IDC_LABEL_TOTAL_TXT  106

#define WM_CONVERSION_PROGRESS  (WM_USER + 1)
#define WM_CONVERSION_DONE      (WM_USER + 2)
#define WM_CONVERSION_ERROR     (WM_USER + 3)

// Цвета
static const COLORREF CLR_BG = RGB(250, 250, 250);
static const COLORREF CLR_FOOTER = RGB(240, 240, 240);
static const COLORREF CLR_TEXT = RGB(30, 30, 30);
static const COLORREF CLR_MUTED = RGB(100, 100, 100);

// Геометрия окна
static const int WIN_W = 500;
static const int WIN_H = 270;   // высота с запасом
static const int FOOTER_Y = 220;   // y начала footer-зоны (рисуется вручную)
static const int PAD = 20;

HWND g_hwnd = nullptr;
HWND g_progressTotal = nullptr;
HWND g_progressFile = nullptr;
HWND g_labelStatus = nullptr;
HWND g_labelFile = nullptr;
HWND g_btnCancel = nullptr;
bool g_cancelled = false;
HANDLE g_hDoneEvent = nullptr;

static HBRUSH g_hBrushBg = nullptr;
static HBRUSH g_hBrushFooter = nullptr;

struct ConversionArgs {
    std::vector<std::wstring> inputFiles;
    std::wstring targetExt;
    std::wstring ffmpegPath;
    std::wstring presetsPath;
};

struct ProgressMessage {
    ProgressInfo info;
};

DWORD WINAPI conversionThread(LPVOID param) {
    ConversionArgs* args = reinterpret_cast<ConversionArgs*>(param);
    FFmpegRunner::ffmpegPath = args->ffmpegPath;

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

    FFmpegRunner::convertAll(tasks, [&](const ProgressInfo& info) {
        if (g_cancelled) return;
        ProgressMessage* msg = new ProgressMessage{ info };
        if (info.done)       PostMessage(g_hwnd, WM_CONVERSION_DONE, 0, reinterpret_cast<LPARAM>(msg));
        else if (info.error) PostMessage(g_hwnd, WM_CONVERSION_ERROR, 0, reinterpret_cast<LPARAM>(msg));
        else                 PostMessage(g_hwnd, WM_CONVERSION_PROGRESS, 0, reinterpret_cast<LPARAM>(msg));
        });

    delete args;
    if (g_hDoneEvent) SetEvent(g_hDoneEvent);
    return 0;
}

void updateProgress(const ProgressInfo& info) {
    SendMessage(g_progressTotal, PBM_SETPOS, static_cast<int>(info.totalProgress * 100), 0);
    SendMessage(g_progressFile, PBM_SETPOS, static_cast<int>(info.fileProgress * 100), 0);

    std::wstring status = L"Файл " + std::to_wstring(info.currentFile)
        + L" из " + std::to_wstring(info.totalFiles);
    SetWindowTextW(g_labelStatus, status.c_str());

    std::wstring fileName(info.currentFileName.begin(), info.currentFileName.end());
    namespace fs = std::filesystem;
    try { fileName = fs::path(fileName).filename().wstring(); }
    catch (...) {}
    SetWindowTextW(g_labelFile, fileName.c_str());
}

// Рисуем разделитель и footer вручную в WM_PAINT
void paintFooter(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rcFooter{ 0, FOOTER_Y, WIN_W, WIN_H };
    FillRect(hdc, &rcFooter, g_hBrushFooter);

    // Тонкая линия-разделитель
    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(210, 210, 210));
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, 0, FOOTER_Y, nullptr);
    LineTo(hdc, WIN_W, FOOTER_Y);
    SelectObject(hdc, hOld);
    DeleteObject(hPen);

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_PAINT:
        paintFooter(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        // Верхняя зона — основной фон
        RECT rcTop{ 0, 0, rc.right, FOOTER_Y };
        FillRect(hdc, &rcTop, g_hBrushBg);
        // Нижняя — footer
        RECT rcFoot{ 0, FOOTER_Y, rc.right, rc.bottom };
        FillRect(hdc, &rcFoot, g_hBrushFooter);
        return 1;
    }

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
        SetTimer(g_hwnd, 1, 100, nullptr);
        return 0;
    }
    case WM_CONVERSION_ERROR: {
        ProgressMessage* pm = reinterpret_cast<ProgressMessage*>(lParam);
        std::wstring errMsg(pm->info.errorMessage.begin(), pm->info.errorMessage.end());
        SetWindowTextW(g_labelStatus, L"Ошибка!");
        SetWindowTextW(g_labelFile, errMsg.c_str());
        SetWindowTextW(g_btnCancel, L"Закрыть");
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
        HDC  hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);

        // Определяем, в какой зоне находится контрол
        RECT rc;
        GetWindowRect(hCtrl, &rc);
        POINT pt{ rc.left, rc.top };
        ScreenToClient(hwnd, &pt);

        if (pt.y >= FOOTER_Y) {
            SetBkColor(hdc, CLR_FOOTER);
            SetTextColor(hdc, CLR_MUTED);
            return reinterpret_cast<LRESULT>(g_hBrushFooter);
        }
        SetBkColor(hdc, CLR_BG);
        SetTextColor(hdc, CLR_TEXT);
        return reinterpret_cast<LRESULT>(g_hBrushBg);
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

HWND createWindow(const std::wstring& targetExt, int fileCount) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    g_hBrushBg = CreateSolidBrush(CLR_BG);
    g_hBrushFooter = CreateSolidBrush(CLR_FOOTER);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = g_hBrushBg;   // базовый фон; footer рисуем сами
    wc.lpszClassName = L"FFmpegConverterWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Считаем реальный размер окна с учётом рамки/заголовка
    RECT rc{ 0, 0, WIN_W, WIN_H };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        FALSE, WS_EX_DLGMODALFRAME);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"FFmpegConverterWindow",
        L"FFmpeg Converter",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (screenW - winW) / 2, (screenH - winH) / 2,
        winW, winH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );

    // Шрифты
    HFONT hFontNormal = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBold = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    int cx = WIN_W - PAD * 2;   // ширина контента

    // ── Заголовок ──────────────────────────────────────────────
    std::wstring title = L"Конвертация " + std::to_wstring(fileCount)
        + L" файл(ов)  \u2192  " + targetExt;
    HWND hTitle = CreateWindowW(L"STATIC", title.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, 16, cx, 20, hwnd, nullptr, nullptr, nullptr);
    SendMessage(hTitle, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    // ── "Файл X из Y" ──────────────────────────────────────────
    g_labelStatus = CreateWindowW(L"STATIC", L"Подготовка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, 48, cx, 18, hwnd, (HMENU)IDC_LABEL_STATUS, nullptr, nullptr);
    SendMessage(g_labelStatus, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

    // ── Прогресс файла ─────────────────────────────────────────
    g_progressFile = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        PAD, 70, cx, 16, hwnd, (HMENU)IDC_PROGRESS_FILE, nullptr, nullptr);
    SendMessage(g_progressFile, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_progressFile, PBM_SETPOS, 0, 0);

    // ── Имя файла (мелкий серый) ────────────────────────────────
    g_labelFile = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        PAD, 91, cx, 15, hwnd, (HMENU)IDC_LABEL_FILE, nullptr, nullptr);
    SendMessage(g_labelFile, WM_SETFONT, (WPARAM)hFontSmall, TRUE);

    // ── "Общий прогресс:" ───────────────────────────────────────
    HWND hTotalTxt = CreateWindowW(L"STATIC", L"Общий прогресс:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, 118, 200, 18, hwnd, (HMENU)IDC_LABEL_TOTAL_TXT, nullptr, nullptr);
    SendMessage(hTotalTxt, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

    // ── Общий прогресс-бар ─────────────────────────────────────
    g_progressTotal = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        PAD, 140, cx, 16, hwnd, (HMENU)IDC_PROGRESS_TOTAL, nullptr, nullptr);
    SendMessage(g_progressTotal, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_progressTotal, PBM_SETPOS, 0, 0);

    // ── Кнопка «Отмена» в footer ───────────────────────────────
    // y = FOOTER_Y + (footer_height - btn_height) / 2 = 220 + (50-28)/2 = 231
    g_btnCancel = CreateWindowW(L"BUTTON", L"Отмена",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        WIN_W - PAD - 110, FOOTER_Y + 11, 110, 28,
        hwnd, (HMENU)IDC_BTN_CANCEL, nullptr, nullptr);
    SendMessage(g_btnCancel, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

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
        if (arg == L"--target" && i + 1 < argc) result.targetExt = argv[++i];
        else if (arg == L"--ffmpeg" && i + 1 < argc) result.ffmpegPath = argv[++i];
        else if (arg == L"--presets" && i + 1 < argc) result.presetsPath = argv[++i];
        else if (arg[0] != L'-')                       result.files.push_back(arg);
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
            L"Использование:\nConverter.exe --target .mp4 --ffmpeg \"путь\\ffmpeg.exe\""
            L" --presets \"путь\\presets.json\" \"file1\" \"file2\"",
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