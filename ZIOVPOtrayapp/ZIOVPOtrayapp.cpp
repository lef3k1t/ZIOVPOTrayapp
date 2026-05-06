#include "ZIOVPOtrayapp.h"
#include "ZIOVPOTrayRpc.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <string>

namespace
{
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_EXIT = 1002;
constexpr UINT ID_FILE_EXIT = 2001;
constexpr UINT ID_LOGIN_BUTTON = 3001;
constexpr UINT ID_ACTIVATE_BUTTON = 3002;
constexpr UINT ID_LOGOUT_BUTTON = 3003;
constexpr UINT ID_LICENSE_TIMER = 4001;
constexpr UINT TRAY_ICON_ID = 1;
constexpr DWORD kNotAuthenticated = 12001;
constexpr DWORD kNoLicense = 12002;

const wchar_t kWindowClassName[] = L"ZIOVPOTrayAppWindowClass";
const wchar_t kMutexName[] = L"Local\\ZIOVPOTrayAppSingleInstance";
const wchar_t kServiceName[] = L"ZIOVPOTrayService";
const wchar_t kServiceProcessName[] = L"ziovpotrayservice.exe";
const wchar_t kRpcEndpoint[] = L"ZIOVPOTrayServiceRpc";

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HMENU g_mainMenu = nullptr;
HMENU g_fileMenu = nullptr;
UINT g_taskbarCreatedMessage = 0;
bool g_isExiting = false;
HWND g_statusLabel = nullptr;
HWND g_loginEdit = nullptr;
HWND g_passwordEdit = nullptr;
HWND g_loginButton = nullptr;
HWND g_activationEdit = nullptr;
HWND g_activateButton = nullptr;
HWND g_logoutButton = nullptr;
std::wstring g_currentUser;

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

std::wstring FileNameFromPath(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

DWORD GetParentProcessId()
{
    const DWORD currentProcessId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD parentProcessId = 0;

    if (Process32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == currentProcessId)
            {
                parentProcessId = entry.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return parentProcessId;
}

bool IsStartedByService()
{
    const DWORD parentProcessId = GetParentProcessId();
    if (parentProcessId == 0)
    {
        return false;
    }

    HANDLE parentProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentProcessId);
    if (!parentProcess)
    {
        return false;
    }

    wchar_t parentPath[MAX_PATH]{};
    DWORD parentPathSize = ARRAYSIZE(parentPath);
    const bool queried = QueryFullProcessImageNameW(parentProcess, 0, parentPath, &parentPathSize) != FALSE;
    CloseHandle(parentProcess);

    if (!queried)
    {
        return false;
    }

    return ToLower(FileNameFromPath(parentPath)) == kServiceProcessName;
}

bool WaitForServiceState(SC_HANDLE service, DWORD expectedState, DWORD timeoutMs)
{
    const DWORD startedAt = GetTickCount();
    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;

    do
    {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded))
        {
            return false;
        }

        if (status.dwCurrentState == expectedState)
        {
            return true;
        }

        Sleep(250);
    } while (GetTickCount() - startedAt < timeoutMs);

    return false;
}

bool EnsureServiceRunningAndExitIfStarted()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
    {
        return false;
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS | SERVICE_START);
    if (!service)
    {
        CloseServiceHandle(manager);
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    bool shouldExit = false;

    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded))
    {
        if (status.dwCurrentState == SERVICE_STOPPED)
        {
            StartServiceW(service, 0, nullptr);
            WaitForServiceState(service, SERVICE_RUNNING, 30000);
            shouldExit = true;
        }
        else if (status.dwCurrentState == SERVICE_START_PENDING)
        {
            WaitForServiceState(service, SERVICE_RUNNING, 30000);
            shouldExit = true;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return shouldExit;
}

bool IsHiddenStartupMode()
{
    const std::wstring commandLine = GetCommandLineW();
    return commandLine.find(L"--hidden") != std::wstring::npos ||
        commandLine.find(L"--background") != std::wstring::npos ||
        commandLine.find(L"--minimized") != std::wstring::npos;
}

void ShowMainWindow()
{
    if (!g_mainWindow)
    {
        return;
    }

    ShowWindow(g_mainWindow, SW_SHOWNORMAL);
    SetForegroundWindow(g_mainWindow);
}

void RemoveTrayIcon()
{
    NOTIFYICONDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_mainWindow;
    data.uID = TRAY_ICON_ID;

    Shell_NotifyIcon(NIM_DELETE, &data);
}

void AddTrayIcon()
{
    NOTIFYICONDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_mainWindow;
    data.uID = TRAY_ICON_ID;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_TRAYICON;
    data.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"ZIOVPO Tray App");

    Shell_NotifyIcon(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &data);
}

bool RequestServiceStop()
{
    RPC_WSTR bindingString = nullptr;
    handle_t binding = nullptr;
    RPC_STATUS status = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr,
        &bindingString);

    if (status == RPC_S_OK)
    {
        status = RpcBindingFromStringBindingW(bindingString, &binding);
    }

    if (bindingString)
    {
        RpcStringFreeW(&bindingString);
    }

    if (status != RPC_S_OK)
    {
        return false;
    }

    RpcTryExcept
    {
        RpcStopService(binding);
        status = RPC_S_OK;
    }
    RpcExcept(1)
    {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    RpcBindingFree(&binding);
    return status == RPC_S_OK;
}

bool CreateRpcBinding(handle_t* binding)
{
    RPC_WSTR bindingString = nullptr;
    RPC_STATUS status = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr,
        &bindingString);

    if (status == RPC_S_OK)
    {
        status = RpcBindingFromStringBindingW(bindingString, binding);
    }

    if (bindingString)
    {
        RpcStringFreeW(&bindingString);
    }

    return status == RPC_S_OK;
}

void FreeRpcBinding(handle_t binding)
{
    if (binding)
    {
        RpcBindingFree(&binding);
    }
}

void SetWindowTextSafe(HWND window, const std::wstring& text)
{
    if (window)
    {
        SetWindowTextW(window, text.c_str());
    }
}

std::wstring GetControlText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    std::wstring text(length + 1, L'\0');
    GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(length);
    return text;
}

void SetAuthControlsVisible(bool visible)
{
    const int command = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(g_loginEdit, command);
    ShowWindow(g_passwordEdit, command);
    ShowWindow(g_loginButton, command);
}

void SetActivationControlsVisible(bool visible)
{
    const int command = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(g_activationEdit, command);
    ShowWindow(g_activateButton, command);
}

bool RpcGetUser(bool& authenticated, std::wstring& userName)
{
    handle_t binding = nullptr;
    if (!CreateRpcBinding(&binding))
    {
        return false;
    }

    int rpcAuthenticated = 0;
    wchar_t rpcUserName[256]{};
    RPC_STATUS status = RPC_S_OK;
    RpcTryExcept
    {
        status = RpcGetCurrentUser(binding, &rpcAuthenticated, rpcUserName, ARRAYSIZE(rpcUserName));
    }
    RpcExcept(1)
    {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    FreeRpcBinding(binding);
    authenticated = rpcAuthenticated != 0;
    userName = rpcUserName;
    return status == RPC_S_OK;
}

DWORD RpcLoginUser(const std::wstring& userName, const std::wstring& password, bool& authenticated)
{
    handle_t binding = nullptr;
    if (!CreateRpcBinding(&binding))
    {
        return RPC_S_SERVER_UNAVAILABLE;
    }

    int rpcAuthenticated = 0;
    RPC_STATUS status = RPC_S_OK;
    RpcTryExcept
    {
        status = RpcLogin(binding, userName.c_str(), password.c_str(), &rpcAuthenticated);
    }
    RpcExcept(1)
    {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    FreeRpcBinding(binding);
    authenticated = rpcAuthenticated != 0;
    return status;
}

void RpcLogoutUser()
{
    handle_t binding = nullptr;
    if (!CreateRpcBinding(&binding))
    {
        return;
    }

    RpcTryExcept
    {
        RpcLogout(binding);
    }
    RpcExcept(1)
    {
    }
    RpcEndExcept

    FreeRpcBinding(binding);
}

DWORD RpcGetLicense(bool& active, std::wstring& expiresAt)
{
    handle_t binding = nullptr;
    if (!CreateRpcBinding(&binding))
    {
        return RPC_S_SERVER_UNAVAILABLE;
    }

    int rpcActive = 0;
    wchar_t rpcExpiresAt[128]{};
    RPC_STATUS status = RPC_S_OK;
    RpcTryExcept
    {
        status = RpcGetLicenseInfo(binding, &rpcActive, rpcExpiresAt, ARRAYSIZE(rpcExpiresAt));
    }
    RpcExcept(1)
    {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    FreeRpcBinding(binding);
    active = rpcActive != 0;
    expiresAt = rpcExpiresAt;
    return status;
}

DWORD RpcActivateLicense(const std::wstring& code, bool& active, std::wstring& expiresAt)
{
    handle_t binding = nullptr;
    if (!CreateRpcBinding(&binding))
    {
        return RPC_S_SERVER_UNAVAILABLE;
    }

    int rpcActive = 0;
    wchar_t rpcExpiresAt[128]{};
    RPC_STATUS status = RPC_S_OK;
    RpcTryExcept
    {
        status = RpcActivateProduct(binding, code.c_str(), &rpcActive, rpcExpiresAt, ARRAYSIZE(rpcExpiresAt));
    }
    RpcExcept(1)
    {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    FreeRpcBinding(binding);
    active = rpcActive != 0;
    expiresAt = rpcExpiresAt;
    return status;
}

void RefreshAccountUi()
{
    bool authenticated = false;
    std::wstring userName;
    if (!RpcGetUser(authenticated, userName) || !authenticated)
    {
        g_currentUser.clear();
        SetWindowTextSafe(g_statusLabel, L"Пользователь не вошел. Функции антивируса заблокированы.");
        SetAuthControlsVisible(true);
        SetActivationControlsVisible(false);
        return;
    }

    g_currentUser = userName;
    SetAuthControlsVisible(false);

    bool licenseActive = false;
    std::wstring expiresAt;
    const DWORD licenseStatus = RpcGetLicense(licenseActive, expiresAt);
    if (licenseStatus == RPC_S_OK && licenseActive)
    {
        SetActivationControlsVisible(false);
        SetWindowTextSafe(g_statusLabel, L"Пользователь: " + g_currentUser + L"\r\nЛицензия активна до: " + expiresAt + L"\r\nФункции антивируса разблокированы.");
        return;
    }

    SetActivationControlsVisible(true);
    SetWindowTextSafe(g_statusLabel, L"Пользователь: " + g_currentUser + L"\r\nЛицензия отсутствует. Функции антивируса заблокированы.");
}

void CreateAccountControls()
{
    g_statusLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 36, 560, 80, g_mainWindow, nullptr, g_instance, nullptr);
    CreateWindowW(L"STATIC", L"Логин:", WS_CHILD | WS_VISIBLE, 24, 136, 80, 22, g_mainWindow, nullptr, g_instance, nullptr);
    g_loginEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 112, 132, 220, 26, g_mainWindow, nullptr, g_instance, nullptr);
    CreateWindowW(L"STATIC", L"Пароль:", WS_CHILD | WS_VISIBLE, 24, 172, 80, 22, g_mainWindow, nullptr, g_instance, nullptr);
    g_passwordEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 112, 168, 220, 26, g_mainWindow, nullptr, g_instance, nullptr);
    g_loginButton = CreateWindowW(L"BUTTON", L"Войти", WS_CHILD | WS_VISIBLE, 352, 150, 120, 30, g_mainWindow, reinterpret_cast<HMENU>(ID_LOGIN_BUTTON), g_instance, nullptr);

    CreateWindowW(L"STATIC", L"Код активации:", WS_CHILD | WS_VISIBLE, 24, 226, 120, 22, g_mainWindow, nullptr, g_instance, nullptr);
    g_activationEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 152, 222, 220, 26, g_mainWindow, nullptr, g_instance, nullptr);
    g_activateButton = CreateWindowW(L"BUTTON", L"Активировать", WS_CHILD | WS_VISIBLE, 392, 220, 140, 30, g_mainWindow, reinterpret_cast<HMENU>(ID_ACTIVATE_BUTTON), g_instance, nullptr);
    g_logoutButton = CreateWindowW(L"BUTTON", L"Выйти из аккаунта", WS_CHILD | WS_VISIBLE, 24, 300, 180, 30, g_mainWindow, reinterpret_cast<HMENU>(ID_LOGOUT_BUTTON), g_instance, nullptr);
}

void ExitApplication()
{
    g_isExiting = true;
    RemoveTrayIcon();
    DestroyWindow(g_mainWindow);
}

void ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }

    AppendMenu(menu, MF_STRING, ID_TRAY_OPEN, L"\u041e\u0442\u043a\u0440\u044b\u0442\u044c");
    AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(menu, MF_STRING, ID_TRAY_EXIT, L"\u0412\u044b\u0445\u043e\u0434");

    POINT cursorPosition{};
    GetCursorPos(&cursorPosition);
    SetForegroundWindow(g_mainWindow);
    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        cursorPosition.x,
        cursorPosition.y,
        0,
        g_mainWindow,
        nullptr);

    DestroyMenu(menu);
}

void CreateMainMenu()
{
    g_mainMenu = CreateMenu();
    g_fileMenu = CreatePopupMenu();

    AppendMenu(g_fileMenu, MF_STRING, ID_FILE_EXIT, L"\u0412\u044b\u0445\u043e\u0434");
    AppendMenu(g_mainMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_fileMenu), L"\u0424\u0430\u0439\u043b");
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == g_taskbarCreatedMessage)
    {
        AddTrayIcon();
        return 0;
    }

    switch (message)
    {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_LBUTTONUP)
        {
            ShowMainWindow();
        }
        else if (LOWORD(lParam) == WM_RBUTTONUP)
        {
            ShowTrayMenu();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_TRAY_OPEN:
            ShowMainWindow();
            return 0;
        case ID_TRAY_EXIT:
        case ID_FILE_EXIT:
            if (!RequestServiceStop())
            {
                ExitApplication();
            }
            return 0;
        case ID_LOGIN_BUTTON:
        {
            bool authenticated = false;
            const DWORD result = RpcLoginUser(GetControlText(g_loginEdit), GetControlText(g_passwordEdit), authenticated);
            if (result != RPC_S_OK || !authenticated)
            {
                SetWindowTextSafe(g_statusLabel, L"Ошибка входа. Проверьте логин и пароль.");
                SetAuthControlsVisible(true);
                SetActivationControlsVisible(false);
            }
            else
            {
                RefreshAccountUi();
            }
            return 0;
        }
        case ID_ACTIVATE_BUTTON:
        {
            bool active = false;
            std::wstring expiresAt;
            const DWORD result = RpcActivateLicense(GetControlText(g_activationEdit), active, expiresAt);
            if (result != RPC_S_OK || !active)
            {
                SetWindowTextSafe(g_statusLabel, L"Ошибка активации. Введите корректный код активации.");
                SetActivationControlsVisible(true);
            }
            else
            {
                RefreshAccountUi();
            }
            return 0;
        }
        case ID_LOGOUT_BUTTON:
            RpcLogoutUser();
            RefreshAccountUi();
            return 0;
        default:
            return 0;
        }

    case WM_TIMER:
        if (wParam == ID_LICENSE_TIMER)
        {
            RefreshAccountUi();
            return 0;
        }
        break;

    case WM_CLOSE:
        if (g_isExiting)
        {
            DestroyWindow(window);
        }
        else
        {
            ShowWindow(window, SW_HIDE);
        }
        return 0;

    case WM_DESTROY:
        if (g_isExiting)
        {
            PostQuitMessage(0);
        }
        return 0;

    default:
        return DefWindowProc(window, message, wParam, lParam);
    }
}

bool RegisterMainWindowClass()
{
    WNDCLASSEX windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = g_instance;
    windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    return RegisterClassEx(&windowClass) != 0;
}

bool CreateMainWindow()
{
    CreateMainMenu();

    g_mainWindow = CreateWindowEx(
        0,
        kWindowClassName,
        L"ZIOVPO Tray App",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        420,
        nullptr,
        g_mainMenu,
        g_instance,
        nullptr);

    if (!g_mainWindow)
    {
        return false;
    }

    CreateAccountControls();
    SetTimer(g_mainWindow, ID_LICENSE_TIMER, 30000, nullptr);
    RefreshAccountUi();
    return true;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    if (EnsureServiceRunningAndExitIfStarted())
    {
        return 0;
    }

    if (!IsStartedByService())
    {
        return 0;
    }

    HANDLE singleInstanceMutex = CreateMutex(nullptr, TRUE, kMutexName);
    if (!singleInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (singleInstanceMutex)
        {
            CloseHandle(singleInstanceMutex);
        }
        return 0;
    }

    g_instance = instance;
    g_taskbarCreatedMessage = RegisterWindowMessage(L"TaskbarCreated");

    if (!RegisterMainWindowClass() || !CreateMainWindow())
    {
        CloseHandle(singleInstanceMutex);
        return 1;
    }

    AddTrayIcon();

    if (!IsHiddenStartupMode())
    {
        ShowMainWindow();
    }

    MSG message{};
    while (GetMessage(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    CloseHandle(singleInstanceMutex);
    return static_cast<int>(message.wParam);
}

void* __RPC_USER midl_user_allocate(size_t size)
{
    return malloc(size);
}

void __RPC_USER midl_user_free(void* pointer)
{
    free(pointer);
}
