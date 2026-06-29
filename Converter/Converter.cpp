#include <windows.h>
#include <uxtheme.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")

#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <sstream>
#include <filesystem>

#include "../Core/FFmpegRunner.h"
#include "../Core/PresetManager.h"
#include "../Core/FileClassifier.h"
#include "../Installer/resource.h"

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
static const COLORREF CLR_BG = RGB(255, 255, 255);
static const COLORREF CLR_FOOTER = RGB(243, 243, 243);
static const COLORREF CLR_ACCENT = RGB(44, 122, 210);  // синий прогресс
static const COLORREF CLR_GREEN = RGB(39, 174, 96);  // зелёный общий
static const COLORREF CLR_TEXT = RGB(25, 25, 25);
static const COLORREF CLR_MUTED = RGB(110, 110, 110);
static const COLORREF CLR_BADGE_BG = RGB(255, 90, 40);  // оранжевый бейдж MP4
static const COLORREF CLR_SEP = RGB(218, 218, 218);

static const int WIN_W = 500;
static const int WIN_H = 290;
static const int FOOTER_Y = 245;
static const int PAD = 22;

// Новые глобалы для спиннера и ETA
static int   g_spinAngle = 0;
static HWND  g_spinHwnd = nullptr;   // область спиннера (статик)
static DWORD g_startTick = 0;
static std::wstring g_etaText;
static HWND  g_labelEta = nullptr;
static HWND  g_labelPctFile = nullptr;
static HWND  g_labelPctTotal = nullptr;
static double g_lastProgress = 0.0;

static int g_pctFile = 0;
static int g_pctTotal = 0;
static bool g_errorState = false;

HWND g_hwnd = nullptr;
HWND g_progressTotal = nullptr;
HWND g_progressFile = nullptr;
HWND g_labelStatus = nullptr;
HWND g_labelFile = nullptr;
HWND g_btnCancel = nullptr;
std::atomic<bool> g_cancelled{ false };
HANDLE g_hDoneEvent = nullptr;

static HBRUSH g_hBrushBg = nullptr;
static HBRUSH g_hBrushFooter = nullptr;

static std::wstring g_badgeText = L"";  // "MP4", "MKV" и т.д.
static bool g_convDone = false;

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
        }, g_cancelled);

    delete args;
    if (g_hDoneEvent) SetEvent(g_hDoneEvent);
    return 0;
}

void updateProgress(const ProgressInfo& info) {
    if (g_badgeText.empty() && !info.currentFileName.empty()) {
        namespace fs = std::filesystem;
        std::string ext = fs::path(info.currentFileName).extension().string();
        // убираем точку, переводим в верхний регистр
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
        g_badgeText = std::wstring(ext.begin(), ext.end());
    }
    g_pctFile = (int)(info.fileProgress * 100);
    g_pctTotal = (int)(info.totalProgress * 100);
    // Перерисовываем только полосы
    InvalidateRect(g_hwnd, nullptr, FALSE);

    std::wstring status = L"Файл " + std::to_wstring(info.currentFile)
        + L" из " + std::to_wstring(info.totalFiles);
    SetWindowTextW(g_labelStatus, status.c_str());

    std::wstring fileName(info.currentFileName.begin(), info.currentFileName.end());
    namespace fs = std::filesystem;
    try { fileName = fs::path(fileName).filename().wstring(); }
    catch (...) {}
    SetWindowTextW(g_labelFile, fileName.c_str());

    wchar_t pct[8];
    swprintf_s(pct, L"%d%%", (int)(info.fileProgress * 100));
    SetWindowTextW(g_labelPctFile, pct);
    swprintf_s(pct, L"%d%%", (int)(info.totalProgress * 100));
    SetWindowTextW(g_labelPctTotal, pct);

    // ETA
    double progress = info.totalProgress; // 0.0 - 1.0
    if (progress > 0.01) {  // не считаем пока не начали
        DWORD elapsed = GetTickCount() - g_startTick;
        double totalEstMs = elapsed / progress;
        DWORD remainMs = (DWORD)(totalEstMs - elapsed);

        wchar_t etaBuf[64];
        DWORD remSec = remainMs / 1000;
        if (remSec < 60) {
            swprintf_s(etaBuf, L"Конвертация... осталось около %d сек", remSec);
        }
        else {
            DWORD remMin = remSec / 60;
            DWORD remS = remSec % 60;
            swprintf_s(etaBuf, L"Конвертация... осталось около %d мин %d сек", remMin, remS);
        }
        // hwnd 906 — это лейбл со статусом конвертации
        SetWindowTextW(GetDlgItem(g_hwnd, 906), etaBuf);
    }
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

// r     — прямоугольник всей полосы
// pct   — 0..100
// clrFill — цвет заливки
// clrTrack — цвет фона
// radius — радиус скругления
static void drawRoundBar(HDC hdc, RECT r, int pct, COLORREF clrFill, COLORREF clrTrack, int radius) {
    // Фон (трек) — скруглённый прямоугольник
    HBRUSH hBrTrack = CreateSolidBrush(clrTrack);
    HPEN   hPenNull = (HPEN)GetStockObject(NULL_PEN);
    SelectObject(hdc, hBrTrack);
    SelectObject(hdc, hPenNull);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    DeleteObject(hBrTrack);

    if (pct <= 0) return;

    // Заливка — клипируем по треку, рисуем скруглённый прямоугольник нужной ширины
    int fillW = (int)((r.right - r.left) * pct / 100.0);
    if (fillW < radius * 2) fillW = radius * 2;  // минимум чтобы скругление не ломалось

    // Создаём регион-маску по форме трека
    HRGN hRgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    SelectClipRgn(hdc, hRgn);

    HBRUSH hBrFill = CreateSolidBrush(clrFill);
    SelectObject(hdc, hBrFill);
    RECT rcFill{ r.left, r.top, r.left + fillW, r.bottom };
    // Правый край заливки тоже скруглённый
    RoundRect(hdc, rcFill.left, rcFill.top, rcFill.right, rcFill.bottom, radius * 2, radius * 2);
    DeleteObject(hBrFill);

    SelectClipRgn(hdc, nullptr);
    DeleteObject(hRgn);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Плашка файла: фон + border
        RECT rcBadge{ PAD, 62, WIN_W - PAD, 100 };
        HBRUSH hBrBadge = CreateSolidBrush(RGB(248, 248, 248));
        FillRect(hdc, &rcBadge, hBrBadge);
        DeleteObject(hBrBadge);
        HPEN hOldPen = (HPEN)GetStockObject(NULL_PEN);
        HPEN hPenBorder = CreatePen(PS_SOLID, 1, RGB(224, 224, 224));
        hOldPen = (HPEN)SelectObject(hdc, hPenBorder);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, PAD, 62, WIN_W - PAD, 100);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPenBorder);

        // Бейдж типа файла (оранжевый прямоугольник 32x20)
        RECT rcExt{ PAD + 8, 70, PAD + 40, 90 };
        HBRUSH hBrExt = CreateSolidBrush(CLR_BADGE_BG);
        FillRect(hdc, &rcExt, hBrExt);
        DeleteObject(hBrExt);
        // Текст бейджа
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        HFONT fBadge = CreateFontW(11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdc, fBadge);
        DrawTextW(hdc, g_badgeText.c_str(), -1, &rcExt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, hOldFont);
        DeleteObject(fBadge);

        // Щит (синий кружок с галкой) рядом с заголовком
        RECT rcShield{ PAD, 14, PAD + 22, 36 };
        HBRUSH hBrShield = CreateSolidBrush(RGB(232, 241, 255));
        FillRect(hdc, &rcShield, hBrShield);
        DeleteObject(hBrShield);
        // Галка
        HPEN hPenCheck = CreatePen(PS_SOLID, 2, CLR_ACCENT);
        hOldPen = (HPEN)SelectObject(hdc, hPenCheck);
        MoveToEx(hdc, PAD + 5, 25, nullptr); LineTo(hdc, PAD + 10, 30);
        LineTo(hdc, PAD + 17, 19);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPenCheck);

        // Спиннер (дуга вращается)
        if (!g_convDone) {
            RECT rcSpin;
            GetWindowRect(g_spinHwnd, &rcSpin);
            POINT ptSpin{ rcSpin.left, rcSpin.top };
            ScreenToClient(hwnd, &ptSpin);
            int sx = ptSpin.x, sy = ptSpin.y;

            HPEN hPenTrack = CreatePen(PS_SOLID, 2, RGB(220, 220, 220));
            HPEN hPenArc = CreatePen(PS_SOLID, 2, CLR_ACCENT);
            SelectObject(hdc, hPenTrack);
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Ellipse(hdc, sx, sy, sx + 18, sy + 18);

            // Дуга угол из g_spinAngle
            SelectObject(hdc, hPenArc);
            double a1 = g_spinAngle * 3.14159 / 180.0;
            double a2 = a1 + 3.14159 * 0.75;
            int cx2 = sx + 9, cy2 = sy + 9, r = 7;
            // Рисуем через Arc (в градусах Windows — по часовой стрелке)
            int x1 = cx2 + (int)(r * cos(-a1)), y1 = cy2 + (int)(r * sin(-a1));
            int x2 = cx2 + (int)(r * cos(-a2)), y2 = cy2 + (int)(r * sin(-a2));
            Arc(hdc, sx + 2, sy + 2, sx + 16, sy + 16, x1, y1, x2, y2);

            DeleteObject(hPenTrack);
            DeleteObject(hPenArc);
        }

        // Рисуем прогресс-бары
        auto drawBar = [&](HWND hBar, int pct, COLORREF clr) {
            RECT rc;
            GetWindowRect(hBar, &rc);
            POINT pt{ rc.left, rc.top };
            ScreenToClient(hwnd, &pt);
            RECT r{ pt.x, pt.y, pt.x + (rc.right - rc.left), pt.y + (rc.bottom - rc.top) };
            drawRoundBar(hdc, r, pct, clr, RGB(224, 224, 224), 4);
            };
        drawBar(g_progressFile, g_pctFile, g_errorState ? RGB(196, 43, 28) : CLR_ACCENT);
        drawBar(g_progressTotal, g_pctTotal, g_errorState ? RGB(196, 43, 28) : CLR_GREEN);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT rcTop{ 0, 0, rc.right, FOOTER_Y };
        FillRect(hdc, &rcTop, g_hBrushBg);
        RECT rcFoot{ 0, FOOTER_Y, rc.right, rc.bottom };
        FillRect(hdc, &rcFoot, g_hBrushFooter);
        return 1;  // <-- обязательно 1, не 0, иначе DefWindowProc перерисует поверх
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
        g_convDone = true;
        KillTimer(g_hwnd, 2);
        SetWindowTextW(GetDlgItem(g_hwnd, 906), L"Готово!");
        g_pctFile = g_pctTotal = 100;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CONVERSION_ERROR: {
        ProgressMessage* pm = reinterpret_cast<ProgressMessage*>(lParam);
        std::wstring errMsg(pm->info.errorMessage.begin(), pm->info.errorMessage.end());
        SetWindowTextW(g_labelStatus, L"Ошибка!");
        SetWindowTextW(g_labelFile, errMsg.c_str());
        SetWindowTextW(g_btnCancel, L"Закрыть");
        // Красим полосы в красный при ошибке
        g_pctFile = g_pctTotal = (int)(/* последнее значение */ g_pctFile);
        // Просто меняем цвет — добавь глобал g_errorState
        g_errorState = true;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        delete pm;
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_CANCEL) {
            g_cancelled = true;
            FFmpegRunner::cancelCurrent();
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC  hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);
        int  id = GetDlgCtrlID(hCtrl);

        if (id == IDC_LABEL_FILE || id == 902) {  // имя файла и размер — на серой плашке
            SetBkColor(hdc, RGB(248, 248, 248));
            SetTextColor(hdc, CLR_TEXT);
            static HBRUSH hBrPlashka = CreateSolidBrush(RGB(248, 248, 248));
            return reinterpret_cast<LRESULT>(hBrPlashka);
        }

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
        if (wParam == 1) { KillTimer(hwnd, 1); DestroyWindow(hwnd); }
        if (wParam == 2) {
            g_spinAngle = (g_spinAngle + 15) % 360;
            // Перерисовываем только область спиннера
            RECT rc;
            GetWindowRect(g_spinHwnd, &rc);
            POINT pt{ rc.left, rc.top };
            ScreenToClient(hwnd, &pt);
            RECT rcInv{ pt.x - 2, pt.y - 2, pt.x + 22, pt.y + 22 };
            InvalidateRect(hwnd, &rcInv, TRUE);

            // ETA
            if (g_startTick > 0) {
                // обновляем лейбл ETA
                DWORD elapsed = GetTickCount() - g_startTick;
                wchar_t buf[64];
                swprintf_s(buf, L"Начато в %02d:%02d",
                    (int)(elapsed / 3600000), (int)(elapsed % 3600000 / 60000) /* упрощённо */);
                // Лучше хранить время старта как SYSTEMTIME
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND createWindow(const std::wstring& targetExt, int fileCount, const std::wstring& firstFileExt) {

    std::wstring ext = firstFileExt;
    if (!ext.empty() && ext[0] == L'.') ext = ext.substr(1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towupper);
    g_badgeText = ext;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    g_hBrushBg = CreateSolidBrush(CLR_BG);
    g_hBrushFooter = CreateSolidBrush(CLR_FOOTER);
    g_startTick = GetTickCount();

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t startBuf[32];
    swprintf_s(startBuf, L"Начато в %02d:%02d", st.wHour, st.wMinute);
    // g_labelEta создаётся позже в createWindow, поэтому запомни строку:
    g_etaText = startBuf;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = g_hBrushBg;
    wc.lpszClassName = L"FFmpegConverterWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Иконка из ресурса
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    RegisterClassExW(&wc);

    RECT rc{ 0, 0, WIN_W, WIN_H };
    AdjustWindowRectEx(&rc,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_DLGMODALFRAME);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME,
        L"FFmpegConverterWindow", L"FFmpeg Converter",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (screenW - winW) / 2, (screenH - winH) / 2, winW, winH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    // Иконка в заголовке и панели задач
    HICON hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

    auto makeFont = [](int size, int weight) {
        return CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        };
    HFONT fNormal = makeFont(15, FW_NORMAL);
    HFONT fBold = makeFont(15, FW_SEMIBOLD);
    HFONT fSmall = makeFont(12, FW_NORMAL);
    HFONT fPct = makeFont(13, FW_SEMIBOLD);

    int cx = WIN_W - PAD * 2;

    // ── Заголовок со щитом ─────────────────────────────────────
    // Иконку щита рисуем в WM_PAINT, текст — статик рядом
    std::wstring title = L"    Конвертация " + std::to_wstring(fileCount)
        + L" файла  →  " + targetExt;
    HWND hTitle = CreateWindowW(L"STATIC", title.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, 16, cx, 22, hwnd, (HMENU)900, nullptr, nullptr);
    SendMessage(hTitle, WM_SETFONT, (WPARAM)fBold, TRUE);

    // ── Лейбл секции "ТЕКУЩИЙ ФАЙЛ" ───────────────────────────
    HWND hSec = CreateWindowW(L"STATIC", L"ТЕКУЩИЙ ФАЙЛ",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, 50, 200, 10, hwnd, (HMENU)901, nullptr, nullptr);
    SendMessage(hSec, WM_SETFONT, (WPARAM)fSmall, TRUE);

    // ── Плашка с именем файла ──────────────────────────────────
    // Фон плашки рисуем в WM_PAINT (прямоугольник с border)
    g_labelFile = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        PAD + 45, 73, cx - 42 - 60, 18, hwnd, (HMENU)IDC_LABEL_FILE, nullptr, nullptr);
    SendMessage(g_labelFile, WM_SETFONT, (WPARAM)fNormal, TRUE);

    // Размер файла (справа в плашке) — обновляем при старте
    HWND hFileSize = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        WIN_W - PAD - 62, 73, 58, 18, hwnd, (HMENU)902, nullptr, nullptr);
    SendMessage(hFileSize, WM_SETFONT, (WPARAM)fSmall, TRUE);

    // ── "Файл X из Y" + процент ────────────────────────────────
    g_labelStatus = CreateWindowW(L"STATIC", L"Подготовка...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, 113, cx - 50, 16, hwnd, (HMENU)IDC_LABEL_STATUS, nullptr, nullptr);
    SendMessage(g_labelStatus, WM_SETFONT, (WPARAM)fNormal, TRUE);

    g_labelPctFile = CreateWindowW(L"STATIC", L"0%",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        WIN_W - PAD - 44, 113, 44, 16, hwnd, (HMENU)903, nullptr, nullptr);
    SendMessage(g_labelPctFile, WM_SETFONT, (WPARAM)fPct, TRUE);

    // Прогресс файла (синий)
    g_progressFile = CreateWindowW(L"STATIC", nullptr,
        WS_CHILD,  // без WS_VISIBLE — рисуем сами в WM_PAINT
        PAD, 133, WIN_W - PAD * 2, 8, hwnd, (HMENU)IDC_PROGRESS_FILE, nullptr, nullptr);
    //SetWindowTheme(g_progressFile, L"", L"");
    SendMessage(g_progressFile, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_progressFile, PBM_SETBARCOLOR, 0, (LPARAM)CLR_ACCENT);
    SendMessage(g_progressFile, PBM_SETBKCOLOR, 0, (LPARAM)RGB(230, 230, 230));

    // ── "Общий прогресс" + процент ─────────────────────────────
    HWND hTotalLbl = CreateWindowW(L"STATIC", L"Общий прогресс",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, 153, cx - 50, 16, hwnd, (HMENU)IDC_LABEL_TOTAL_TXT, nullptr, nullptr);
    SendMessage(hTotalLbl, WM_SETFONT, (WPARAM)fNormal, TRUE);

    g_labelPctTotal = CreateWindowW(L"STATIC", L"0%",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        WIN_W - PAD - 44, 153, 44, 16, hwnd, (HMENU)904, nullptr, nullptr);
    SendMessage(g_labelPctTotal, WM_SETFONT, (WPARAM)fPct, TRUE);

    // Прогресс общий (зелёный)
    g_progressTotal = CreateWindowW(L"STATIC", nullptr,
        WS_CHILD,
        PAD, 173, WIN_W - PAD * 2, 8, hwnd, (HMENU)IDC_PROGRESS_TOTAL, nullptr, nullptr);
    //SetWindowTheme(g_progressTotal, L"", L"");
    SendMessage(g_progressTotal, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_progressTotal, PBM_SETBARCOLOR, 0, (LPARAM)CLR_GREEN);
    SendMessage(g_progressTotal, PBM_SETBKCOLOR, 0, (LPARAM)RGB(230, 230, 230));

    // ── Статус со спиннером ────────────────────────────────────
    // Спиннер — область 20x20, рисуем в WM_PAINT по таймеру
    g_spinHwnd = CreateWindowW(L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE,
        PAD, 195, 20, 20, hwnd, (HMENU)905, nullptr, nullptr);

    HWND hStatus2 = CreateWindowW(L"STATIC", L"Конвертация...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD + 26, 198, cx - 26, 15, hwnd, (HMENU)906, nullptr, nullptr);
    SendMessage(hStatus2, WM_SETFONT, (WPARAM)fSmall, TRUE);
    // (текст «осталось около X» обновляем через SetWindowTextW на hwnd 906)

    // ── Footer ─────────────────────────────────────────────────
    // ETA слева
    g_labelEta = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        PAD, FOOTER_Y + 14, 200, 16, hwnd, (HMENU)907, nullptr, nullptr);
    SendMessage(g_labelEta, WM_SETFONT, (WPARAM)fSmall, TRUE);
    SetWindowTextW(g_labelEta, g_etaText.c_str());

    // Кнопка
    g_btnCancel = CreateWindowW(L"BUTTON", L"Отмена",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        WIN_W - PAD - 110, FOOTER_Y + 10, 110, 30,
        hwnd, (HMENU)IDC_BTN_CANCEL, nullptr, nullptr);
    SendMessage(g_btnCancel, WM_SETFONT, (WPARAM)fNormal, TRUE);

    // Таймер спиннера
    SetTimer(hwnd, 2, 80, nullptr);

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

    namespace fs = std::filesystem;
    std::wstring firstExt = fs::path(args.files[0]).extension().wstring();

    bool showWindow = false;
    if (args.files.size() > 1) {
        showWindow = true;
    }
    else if (args.files.size() == 1) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(args.files[0].c_str(), GetFileExInfoStandard, &fa)) {
            ULONGLONG size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            showWindow = size > 2ULL * 1024 * 1024;
        }
    }

    ConversionArgs* convArgs = new ConversionArgs{
        args.files, args.targetExt, args.ffmpegPath, args.presetsPath
    };

    if (showWindow) {
        g_hwnd = createWindow(args.targetExt, static_cast<int>(args.files.size()), firstExt);
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