#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include "GUIDs.h"
#include "../Core/PresetManager.h"
#include "../Core/FileClassifier.h"
#include <strsafe.h>

#pragma comment(lib, "shlwapi.lib")

#define MAX_MENU_ITEMS 20

struct MenuItem {
    std::wstring label;
    std::wstring targetExt;
    std::string  ffmpegArgs;
};

class FFmpegShellExt : public IShellExtInit, public IContextMenu, public IExplorerCommand {
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

    // IExplorerCommand (Win11+)
    STDMETHOD(GetTitle)(IShellItemArray* psia, LPWSTR* ppszTitle) override;
    STDMETHOD(GetToolTip)(IShellItemArray* psia, LPWSTR* ppszInfotip) override;
    STDMETHOD(GetCanonicalName)(GUID* pguidCommandName) override;
    STDMETHOD(GetState)(IShellItemArray* psia, BOOL fOkToBeSlow, EXPCMDSTATE* pCmdState) override;
    STDMETHOD(Invoke)(IShellItemArray* psia, IBindCtx* pbc) override;
    STDMETHOD(GetFlags)(EXPCMDFLAGS* pFlags) override;
    STDMETHOD(GetIcon)(IShellItemArray* psia, LPWSTR* ppszIcon) override;
    STDMETHOD(EnumSubCommands)(IEnumExplorerCommand** ppEnum) override;

private:
    LONG              m_refCount;
    std::vector<std::wstring> m_files;
    std::vector<MenuItem>     m_menuItems;
    PresetManager     m_presetManager;
    UINT              m_idCmdFirst;

    void buildMenuItems();
    std::wstring getInstallDir();
    void launchConverter(const MenuItem& item);
};

class FFmpegShellExtFactory : public IClassFactory {
public:
    FFmpegShellExtFactory() : m_refCount(1) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(CreateInstance)(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override;
    STDMETHOD(LockServer)(BOOL fLock) override;

private:
    LONG m_refCount;
};