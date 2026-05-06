#include "ZIOVPOtrayapp.h"

#include <string>

namespace
{
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_EXIT = 1002;
constexpr UINT ID_FILE_EXIT = 2001;
constexpr UINT TRAY_ICON_ID = 1;

const wchar_t kWindowClassName[] = L"ZIOVPOTrayAppWindowClass";
const wchar_t kMutexName[] = L"Local\\ZIOVPOTrayAppSingleInstance";

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HMENU g_mainMenu = nullptr;
HMENU g_fileMenu = nullptr;
UINT g_taskbarCreatedMessage = 0;
bool g_isExiting = false;

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
            ExitApplication();
            return 0;
        default:
            return 0;
        }

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

    return g_mainWindow != nullptr;
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
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
