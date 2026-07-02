#pragma once
#include <string>

namespace UninstallRoutine {
    // Основная функция удаления
    // Вернёт true если успешно
    bool ExecuteUninstall(
        const std::wstring& installDir,
        bool askForExplorerRestart = true,
        bool showMessages = true
    );

    // Вспомогательные функции
    std::wstring GetInstallDir();
    void UnregisterDLL(const std::wstring& dllPath);
    void RestartExplorer();
    void CleanupRegistry();
    void DeleteInstallationFolder(const std::wstring& dir);
}