#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include "GUIDs.h"
#include "../Core/PresetManager.h"
#include "../Core/FileClassifier.h"
#include <strsafe.h>

#pragma comment(lib, "shlwapi.lib")

// Максимум пунктов меню
#define MAX_MENU_ITEMS 20

struct MenuItem {
    std::wstring label;
    std::wstring targetExt;
    std::string  ffmpegArgs;
};

class FFmpegShellExt : public IShellExtInit, public IContextMenu {
public:
    FFmpegShellExt();
    virtual ~FFmpegShellExt();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IShellExtInit
    STDMETHOD(Initialize)(PCIDLIST_ABSOLUTE pidlFolder,
        IDataObject* pdobj,
        HKEY hkeyProgID) override;

    // IContextMenu
    STDMETHOD(QueryContextMenu)(HMENU hmenu, UINT indexMenu,
        UINT idCmdFirst, UINT idCmdLast,
        UINT uFlags) override;
    STDMETHOD(InvokeCommand)(CMINVOKECOMMANDINFO* pici) override;
    STDMETHOD(GetCommandString)(UINT_PTR idCmd, UINT uType,
        UINT* pReserved, CHAR* pszName,
        UINT cchMax) override;

private:
    LONG              m_refCount;
    std::vector<std::wstring> m_files;       // выбранные файлы
    std::vector<MenuItem>     m_menuItems;   // пункты меню
    PresetManager             m_presetManager;
    UINT                      m_idCmdFirst;

    std::wstring getInstallDir();
    void buildMenuItems();
    void launchConverter(const MenuItem& item);
};

// Фабрика классов
class FFmpegShellExtFactory : public IClassFactory {
public:
    FFmpegShellExtFactory() : m_ref(1) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  override { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHOD(CreateInstance)(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        FFmpegShellExt* ext = new FFmpegShellExt();
        HRESULT hr = ext->QueryInterface(riid, ppv);
        ext->Release();
        return hr;
    }
    STDMETHOD(LockServer)(BOOL) override { return S_OK; }

private:
    LONG m_ref;
};

// Глобальный счётчик объектов (для DllCanUnloadNow)
extern LONG g_objectCount;
extern HMODULE g_hModule;