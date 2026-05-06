#include "ZIOVPOtrayservice.h"
#include "ZIOVPOTrayRpc.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
const wchar_t kServiceName[] = L"ZIOVPOTrayService";
const wchar_t kRpcEndpoint[] = L"ZIOVPOTrayServiceRpc";
const wchar_t kTrayProcessName[] = L"ZIOVPOtrayapp.exe";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
CRITICAL_SECTION g_processLock{};
std::vector<PROCESS_INFORMATION> g_trayProcesses;

void SetServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32ExitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_SESSIONCHANGE : 0;

    if (g_statusHandle)
    {
        SetServiceStatus(g_statusHandle, &g_status);
    }
}

std::wstring GetServiceDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring fullPath = path;
    const size_t slash = fullPath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : fullPath.substr(0, slash);
}

bool IsProcessRunning(const PROCESS_INFORMATION& processInfo)
{
    return WaitForSingleObject(processInfo.hProcess, 0) == WAIT_TIMEOUT;
}

bool HasTrayInSession(DWORD sessionId)
{
    EnterCriticalSection(&g_processLock);
    const bool exists = std::any_of(g_trayProcesses.begin(), g_trayProcesses.end(), [sessionId](const PROCESS_INFORMATION& processInfo) {
        DWORD processSessionId = 0;
        ProcessIdToSessionId(processInfo.dwProcessId, &processSessionId);
        return processSessionId == sessionId && IsProcessRunning(processInfo);
    });
    LeaveCriticalSection(&g_processLock);
    return exists;
}

void CleanupExitedTrayProcesses()
{
    EnterCriticalSection(&g_processLock);
    auto iterator = g_trayProcesses.begin();
    while (iterator != g_trayProcesses.end())
    {
        if (!IsProcessRunning(*iterator))
        {
            CloseHandle(iterator->hProcess);
            CloseHandle(iterator->hThread);
            iterator = g_trayProcesses.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    LeaveCriticalSection(&g_processLock);
}

void StartTrayForSession(DWORD sessionId)
{
    if (sessionId == 0 || HasTrayInSession(sessionId))
    {
        return;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken))
    {
        return;
    }

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &primaryToken))
    {
        CloseHandle(userToken);
        return;
    }

    void* environment = nullptr;
    CreateEnvironmentBlock(&environment, primaryToken, FALSE);

    const std::wstring serviceDirectory = GetServiceDirectory();
    const std::wstring trayPath = serviceDirectory + L"\\" + kTrayProcessName;
    std::wstring commandLine = L"\"" + trayPath + L"\" --hidden --service-child";

    STARTUPINFO startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessAsUserW(
        primaryToken,
        trayPath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        environment,
        serviceDirectory.c_str(),
        &startupInfo,
        &processInfo);

    if (environment)
    {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(primaryToken);
    CloseHandle(userToken);

    if (created)
    {
        EnterCriticalSection(&g_processLock);
        g_trayProcesses.push_back(processInfo);
        LeaveCriticalSection(&g_processLock);
    }
}

void StartTrayForActiveSessions()
{
    PWTS_SESSION_INFO sessions = nullptr;
    DWORD sessionCount = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount))
    {
        return;
    }

    for (DWORD index = 0; index < sessionCount; ++index)
    {
        if (sessions[index].SessionId != 0 && sessions[index].State == WTSActive)
        {
            StartTrayForSession(sessions[index].SessionId);
        }
    }

    WTSFreeMemory(sessions);
}

void StopAllTrayProcesses()
{
    EnterCriticalSection(&g_processLock);
    for (const PROCESS_INFORMATION& processInfo : g_trayProcesses)
    {
        if (IsProcessRunning(processInfo))
        {
            TerminateProcess(processInfo.hProcess, 0);
        }
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }
    g_trayProcesses.clear();
    LeaveCriticalSection(&g_processLock);
}

DWORD WINAPI SessionMonitorThread(void*)
{
    while (WaitForSingleObject(g_stopEvent, 5000) == WAIT_TIMEOUT)
    {
        CleanupExitedTrayProcesses();
        StartTrayForActiveSessions();
    }
    return 0;
}

DWORD WINAPI RpcServerThread(void*)
{
    RPC_STATUS status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr);

    if (status != RPC_S_OK && status != RPC_S_DUPLICATE_ENDPOINT)
    {
        SetEvent(g_stopEvent);
        return status;
    }

    status = RpcServerRegisterIf(ZIOVPOTrayRpc_v1_0_s_ifspec, nullptr, nullptr);
    if (status != RPC_S_OK)
    {
        SetEvent(g_stopEvent);
        return status;
    }

    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);
    if (status != RPC_S_OK && status != RPC_S_ALREADY_LISTENING && status != RPC_S_SERVER_TOO_BUSY)
    {
        SetEvent(g_stopEvent);
        return status;
    }

    RpcMgmtWaitServerListen();
    RpcServerUnregisterIf(ZIOVPOTrayRpc_v1_0_s_ifspec, nullptr, FALSE);
    SetEvent(g_stopEvent);
    return 0;
}

DWORD WINAPI ServiceControlHandler(DWORD control, DWORD eventType, void* eventData, void*)
{
    if (control == SERVICE_CONTROL_SESSIONCHANGE)
    {
        if (eventType == WTS_SESSION_LOGON || eventType == WTS_SESSION_UNLOCK || eventType == WTS_CONSOLE_CONNECT || eventType == WTS_REMOTE_CONNECT)
        {
            const auto notification = static_cast<WTSSESSION_NOTIFICATION*>(eventData);
            if (notification)
            {
                StartTrayForSession(notification->dwSessionId);
            }
        }
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceControlHandler, nullptr);
    if (!g_statusHandle)
    {
        return;
    }

    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);
    InitializeCriticalSection(&g_processLock);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        DeleteCriticalSection(&g_processLock);
        return;
    }

    HANDLE rpcThread = CreateThread(nullptr, 0, RpcServerThread, nullptr, 0, nullptr);
    HANDLE monitorThread = CreateThread(nullptr, 0, SessionMonitorThread, nullptr, 0, nullptr);

    StartTrayForActiveSessions();
    SetServiceState(SERVICE_RUNNING);

    WaitForSingleObject(g_stopEvent, INFINITE);
    SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);

    RpcMgmtStopServerListening(nullptr);
    if (rpcThread)
    {
        WaitForSingleObject(rpcThread, 5000);
        CloseHandle(rpcThread);
    }

    if (monitorThread)
    {
        WaitForSingleObject(monitorThread, 5000);
        CloseHandle(monitorThread);
    }

    StopAllTrayProcesses();
    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_processLock);
    SetServiceState(SERVICE_STOPPED);
}
}

void RpcStopService(handle_t)
{
    RpcMgmtStopServerListening(nullptr);
}

void* __RPC_USER midl_user_allocate(size_t size)
{
    return malloc(size);
}

void __RPC_USER midl_user_free(void* pointer)
{
    free(pointer);
}

int wmain()
{
    SERVICE_TABLE_ENTRY serviceTable[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr}
    };

    if (!StartServiceCtrlDispatcherW(serviceTable))
    {
        return GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT ? 0 : 1;
    }

    return 0;
}
