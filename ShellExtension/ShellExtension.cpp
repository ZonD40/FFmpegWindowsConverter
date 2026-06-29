
#include "ShellExtension.h"
#include <shellapi.h>
#include <filesystem>
#include <sstream>

LONG   g_objectCount = 0;
HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (rclsid != CLSID_FFmpegShellExt) return CLASS_E_CLASSNOTAVAILABLE;
    FFmpegShellExtFactory* factory = new FFmpegShellExtFactory();
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_objectCount == 0) ? S_OK : S_FALSE;
}

static void regWrite(HKEY root, const wchar_t* path,
    const wchar_t* name, const wchar_t* value) {
    HKEY hk;
    RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    RegSetValueExW(hk, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
}

STDAPI DllRegisterServer() {
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);


    wchar_t clsidStr[64];
    StringFromGUID2(CLSID_FFmpegShellExt, clsidStr, 64);

    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + clsidStr;
    regWrite(HKEY_LOCAL_MACHINE, clsidKey.c_str(),
        nullptr, L"FFmpegShellExt");

    std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    regWrite(HKEY_LOCAL_MACHINE, inprocKey.c_str(), nullptr, dllPath);
    regWrite(HKEY_LOCAL_MACHINE, inprocKey.c_str(),
        L"ThreadingModel", L"Apartment");

    std::wstring extKey = L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\FFmpegConverter";
    regWrite(HKEY_LOCAL_MACHINE, extKey.c_str(), nullptr, clsidStr);

    regWrite(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
        clsidStr, L"FFmpegConverter");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    wchar_t clsidStr[64];
    StringFromGUID2(CLSID_FFmpegShellExt, clsidStr, 64);

    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + clsidStr;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKey.c_str());

    std::wstring extKey = L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\FFmpegConverter";
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, extKey.c_str());

    HKEY hk;
    RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
        0, KEY_WRITE, &hk);
    RegDeleteValueW(hk, clsidStr);
    RegCloseKey(hk);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}


FFmpegShellExt::FFmpegShellExt() : m_refCount(1), m_idCmdFirst(0) {
    InterlockedIncrement(&g_objectCount);
    std::wstring presetsPath = getInstallDir() + L"\\presets.json";
    m_presetManager.load(presetsPath);
}

FFmpegShellExt::~FFmpegShellExt() {
    InterlockedDecrement(&g_objectCount);
}

std::wstring FFmpegShellExt::getInstallDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.rfind(L'\\'));
}

STDMETHODIMP FFmpegShellExt::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IShellExtInit) {
        *ppv = static_cast<IShellExtInit*>(this);
    }
    else if (riid == IID_IContextMenu) {
        *ppv = static_cast<IContextMenu*>(this);
    }
    else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) FFmpegShellExt::AddRef() { return InterlockedIncrement(&m_refCount); }
STDMETHODIMP_(ULONG) FFmpegShellExt::Release() {
    LONG r = InterlockedDecrement(&m_refCount);
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP FFmpegShellExt::Initialize(
    PCIDLIST_ABSOLUTE, IDataObject* pdobj, HKEY)
{
    if (!pdobj) return E_INVALIDARG;
    m_files.clear();

    FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};
    if (FAILED(pdobj->GetData(&fmt, &stg))) return E_FAIL;

    HDROP hDrop = reinterpret_cast<HDROP>(GlobalLock(stg.hGlobal));
    if (!hDrop) { ReleaseStgMedium(&stg); return E_FAIL; }

    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; i++) {
        wchar_t buf[MAX_PATH];
        DragQueryFileW(hDrop, i, buf, MAX_PATH);
        m_files.push_back(buf);
    }

    GlobalUnlock(stg.hGlobal);
    ReleaseStgMedium(&stg);

    buildMenuItems();
    return m_files.empty() ? E_FAIL : S_OK;
}

void FFmpegShellExt::buildMenuItems() {
    m_menuItems.clear();
    if (m_files.empty()) return;

    namespace fs = std::filesystem;
    std::string ext = fs::path(m_files[0]).extension().string();

    FileType type = FileClassifier::classify(
        std::wstring(ext.begin(), ext.end()));
    if (type == FileType::Unknown) return;

    for (size_t i = 1; i < m_files.size(); i++) {
        std::string e = fs::path(m_files[i]).extension().string();
        if (FileClassifier::classify(std::wstring(e.begin(), e.end())) != type)
            return; 
    }

    const Preset* preset = m_presetManager.findPresetForExtension(ext);
    if (!preset) return;

    for (const auto& conv : preset->conversions) {

        if (conv.targetExt == ext) continue;

        MenuItem item;
        item.label = std::wstring(conv.label.begin(), conv.label.end());
        item.targetExt = std::wstring(conv.targetExt.begin(), conv.targetExt.end());
        item.ffmpegArgs = conv.ffmpegArgs;
        m_menuItems.push_back(item);
    }
}


STDMETHODIMP FFmpegShellExt::QueryContextMenu(
    HMENU hmenu, UINT indexMenu,
    UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
{
    if ((uFlags & CMF_DEFAULTONLY) || m_menuItems.empty())
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

    m_idCmdFirst = idCmdFirst;


    HMENU hSub = CreatePopupMenu();
    for (UINT i = 0; i < m_menuItems.size() && i < MAX_MENU_ITEMS; i++) {
        InsertMenuW(hSub, i, MF_BYPOSITION | MF_STRING,
            idCmdFirst + i,
            m_menuItems[i].label.c_str());
    }


    std::wstring iconPath = getInstallDir() + L"\\icon.ico";
    HICON hIcon = (HICON)LoadImageW(nullptr, iconPath.c_str(),
        IMAGE_ICON, 16, 16, LR_LOADFROMFILE);

    HBITMAP hBitmap = nullptr;
    if (hIcon) {

        HDC hdcScreen = GetDC(nullptr);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = 16;
        bmi.bmiHeader.biHeight = -16;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* pvBits = nullptr;
        hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
        HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBitmap);
        DrawIconEx(hdcMem, 0, 0, hIcon, 16, 16, 0, nullptr, DI_NORMAL);
        SelectObject(hdcMem, hOld);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        DestroyIcon(hIcon);
    }

    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_ID;
    if (hBitmap) mii.fMask |= MIIM_BITMAP;
    mii.wID = idCmdFirst + MAX_MENU_ITEMS;
    mii.hSubMenu = hSub;
    mii.hbmpItem = hBitmap;
    mii.dwTypeData = const_cast<wchar_t*>(L"FFmpeg");
    InsertMenuItemW(hmenu, indexMenu, TRUE, &mii);
    InsertMenuW(hmenu, indexMenu + 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, m_menuItems.size() + 1);
}

STDMETHODIMP FFmpegShellExt::InvokeCommand(CMINVOKECOMMANDINFO* pici) {
    if (IS_INTRESOURCE(pici->lpVerb)) {
        UINT id = LOWORD(pici->lpVerb);
        if (id < m_menuItems.size()) {
            launchConverter(m_menuItems[id]);
            return S_OK;
        }
    }
    return E_FAIL;
}

STDMETHODIMP FFmpegShellExt::GetCommandString(
    UINT_PTR idCmd, UINT uType, UINT*, CHAR* pszName, UINT cchMax)
{
    if (uType == GCS_HELPTEXTW && idCmd < m_menuItems.size()) {
        StringCchCopyW(reinterpret_cast<wchar_t*>(pszName), cchMax,
            m_menuItems[idCmd].label.c_str());
        return S_OK;
    }
    return E_NOTIMPL;
}


void FFmpegShellExt::launchConverter(const MenuItem& item) {
    std::wstring dir = getInstallDir();
    std::wstring converterPath = dir + L"\\Converter.exe";
    std::wstring ffmpegPath = dir + L"\\ffmpeg.exe";
    std::wstring presetsPath = dir + L"\\presets.json";


    std::wstring cmd = L"\"" + converterPath + L"\""
        + L" --target \"" + item.targetExt + L"\""
        + L" --ffmpeg \"" + ffmpegPath + L"\""
        + L" --presets \"" + presetsPath + L"\"";

    for (const auto& f : m_files) {
        cmd += L" \"" + f + L"\"";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    CreateProcessW(nullptr, cmd.data(),
        nullptr, nullptr, FALSE,
        0, nullptr, nullptr, &si, &pi);

    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread)  CloseHandle(pi.hThread);
}