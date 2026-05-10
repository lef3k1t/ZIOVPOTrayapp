#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace
{
const wchar_t kServiceName[] = L"ZIOVPOTrayService";
const wchar_t kInstallDir[] = L"C:\\Program Files\\ZIOVPOTrayapp";

std::wstring GetLastErrorText(DWORD error)
{
    wchar_t* buffer = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring result = buffer ? buffer : L"Unknown error";
    if (buffer)
    {
        LocalFree(buffer);
    }
    return result;
}

void ShowMessage(const std::wstring& text, UINT icon = MB_ICONINFORMATION)
{
    MessageBoxW(nullptr, text.c_str(), L"ZIOVPO Tray App Installer", MB_OK | icon);
}

bool IsAdministrator()
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

std::wstring GetModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring fullPath = path;
    const size_t slash = fullPath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : fullPath.substr(0, slash);
}

bool CopyRequiredFile(const std::wstring& sourceDirectory, const wchar_t* fileName, const std::wstring& installDirectory)
{
    const std::wstring source = sourceDirectory + L"\\" + fileName;
    const std::wstring destination = installDirectory + L"\\" + fileName;
    return CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
}

void StopAndDeleteService()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
    {
        return;
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (service)
    {
        SERVICE_STATUS status{};
        ControlService(service, SERVICE_CONTROL_STOP, &status);

        SERVICE_STATUS_PROCESS processStatus{};
        DWORD bytesNeeded = 0;
        for (int index = 0; index < 30; ++index)
        {
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&processStatus), sizeof(processStatus), &bytesNeeded))
            {
                break;
            }
            if (processStatus.dwCurrentState == SERVICE_STOPPED)
            {
                break;
            }
            Sleep(500);
        }

        DeleteService(service);
        CloseServiceHandle(service);
    }

    CloseServiceHandle(manager);
}

bool InstallService(const std::wstring& serviceExe)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!manager)
    {
        return false;
    }

    SC_HANDLE service = CreateServiceW(
        manager,
        kServiceName,
        L"ZIOVPO Tray Service",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        serviceExe.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
    {
        service = OpenServiceW(manager, kServiceName, SERVICE_CHANGE_CONFIG | SERVICE_START);
        if (service)
        {
            ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, serviceExe.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
    }

    if (!service)
    {
        CloseServiceHandle(manager);
        return false;
    }

    StartServiceW(service, 0, nullptr);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return true;
}

bool Install()
{
    const std::wstring sourceDirectory = GetModuleDirectory();
    const std::wstring installDirectory = kInstallDir;
    CreateDirectoryW(installDirectory.c_str(), nullptr);

    if (!CopyRequiredFile(sourceDirectory, L"ZIOVPOtrayapp.exe", installDirectory) ||
        !CopyRequiredFile(sourceDirectory, L"ZIOVPOtrayservice.exe", installDirectory) ||
        !CopyRequiredFile(sourceDirectory, L"avdb-default.bin", installDirectory))
    {
        ShowMessage(L"Failed to copy application files. Make sure installer is next to application artifacts.\n\n" + GetLastErrorText(GetLastError()), MB_ICONERROR);
        return false;
    }

    const std::wstring activeDb = installDirectory + L"\\avdb.bin";
    if (GetFileAttributesW(activeDb.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        CopyFileW((installDirectory + L"\\avdb-default.bin").c_str(), activeDb.c_str(), FALSE);
    }

    StopAndDeleteService();
    if (!InstallService(installDirectory + L"\\ZIOVPOtrayservice.exe"))
    {
        ShowMessage(L"Failed to register Windows service.\n\n" + GetLastErrorText(GetLastError()), MB_ICONERROR);
        return false;
    }

    ShowMessage(L"ZIOVPO Tray App installed successfully.");
    return true;
}

bool Uninstall()
{
    StopAndDeleteService();
    DeleteFileW((std::wstring(kInstallDir) + L"\\ZIOVPOtrayapp.exe").c_str());
    DeleteFileW((std::wstring(kInstallDir) + L"\\ZIOVPOtrayservice.exe").c_str());
    DeleteFileW((std::wstring(kInstallDir) + L"\\avdb-default.bin").c_str());
    DeleteFileW((std::wstring(kInstallDir) + L"\\avdb.bin").c_str());
    DeleteFileW((std::wstring(kInstallDir) + L"\\avdb.bin.bak").c_str());
    DeleteFileW((std::wstring(kInstallDir) + L"\\TrayApp-debug.log").c_str());
    DeleteFileW((std::wstring(kInstallDir) + L"\\TrayAppService-debug.log").c_str());
    RemoveDirectoryW(kInstallDir);
    ShowMessage(L"ZIOVPO Tray App removed.");
    return true;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    if (!IsAdministrator())
    {
        ShowMessage(L"Run installer as administrator.", MB_ICONERROR);
        return 1;
    }

    const std::wstring commandLine = GetCommandLineW();
    if (commandLine.find(L"/uninstall") != std::wstring::npos || commandLine.find(L"--uninstall") != std::wstring::npos)
    {
        return Uninstall() ? 0 : 1;
    }

    return Install() ? 0 : 1;
}
