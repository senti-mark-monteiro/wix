// Copyright (c) .NET Foundation and contributors. All rights reserved. Licensed under the Microsoft Reciprocal License. See LICENSE.TXT file in the project root for full license information.

#include "precomp.h"

#define BURN_SPLASHSCREEN_CLASS_WINDOW L"WixBurnSplashScreen"

// struct

struct SPLASHSCREEN_INFO
{
    HBITMAP hBitmap;
    SIZE defaultDpiSize;
    SIZE size;
    UINT nDpi;
    HWND hWnd;
    HWND hwndPrevious;
};

struct SPLASHSCREEN_CONTEXT
{
    HANDLE hInitializedEvent;
    HINSTANCE hInstance;
    LPCWSTR wzCaption;
    BURN_SPLASH_SCREEN_CONFIGURATION* pSplashScreenConfiguration;

    HWND* pHwnd;
};

// internal function definitions

static HRESULT LoadConfiguration(
    __in HINSTANCE hInstance,
    __in BURN_SPLASH_SCREEN_CONFIGURATION** ppSplashScreenConfiguration
    );
static DWORD WINAPI ThreadProc(
    __in LPVOID pvContext
    );
static LRESULT CALLBACK WndProc(
    __in HWND hWnd,
    __in UINT uMsg,
    __in WPARAM wParam,
    __in LPARAM lParam
    );
static HRESULT LoadSplashScreen(
    __in SPLASHSCREEN_CONTEXT* pContext,
    __in SPLASHSCREEN_INFO* pSplashScreen
    );
static BOOL OnDpiChanged(
    __in SPLASHSCREEN_INFO* pSplashScreen,
    __in WPARAM wParam,
    __in LPARAM lParam
    );
static void OnEraseBkgnd(
    __in SPLASHSCREEN_INFO* pSplashScreen,
    __in WPARAM wParam
    );
static void OnNcCreate(
    __in HWND hWnd,
    __in LPARAM lParam
    );
static void ScaleSplashScreen(
    __in SPLASHSCREEN_INFO* pSplashScreen,
    __in UINT nDpi,
    __in int x,
    __in int y
    );


// function definitions

extern "C" void SplashScreenCreate(
    __in HINSTANCE hInstance,
    __in_z_opt LPCWSTR wzCaption,
    __out HWND* pHwnd
    )
{
    HRESULT hr = S_OK;
    SPLASHSCREEN_CONTEXT context = { };
    HANDLE rgSplashScreenEvents[2] = { };
    DWORD dwSplashScreenThreadId = 0;

    hr = LoadConfiguration(hInstance, &context.pSplashScreenConfiguration);
    ExitOnFailure(hr, "Failed to load splash screen configuration.");

    switch (context.pSplashScreenConfiguration->type)
    {
    case BURN_SPLASH_SCREEN_TYPE_NONE:
        ExitFunction();
    case BURN_SPLASH_SCREEN_TYPE_BITMAP_RESOURCE:
        break;
    default:
        ExitWithRootFailure(hr, E_INVALIDDATA, "Invalid splash screen type: %i", context.pSplashScreenConfiguration->type);
    }

    rgSplashScreenEvents[0] = ::CreateEventW(NULL, TRUE, FALSE, NULL);
    ExitOnNullWithLastError(rgSplashScreenEvents[0], hr, "Failed to create modal event.");

    // create splash screen thread.
    context.hInitializedEvent = rgSplashScreenEvents[0];
    context.hInstance = hInstance;
    context.wzCaption = wzCaption;
    context.pHwnd = pHwnd;

    rgSplashScreenEvents[1] = ::CreateThread(NULL, 0, ThreadProc, &context, 0, &dwSplashScreenThreadId);
    ExitOnNullWithLastError(rgSplashScreenEvents[1], hr, "Failed to create UI thread.");

    // It doesn't really matter if the thread gets initialized (WAIT_OBJECT_0) or fails and exits
    // prematurely (WAIT_OBJECT_0 + 1), we just want to wait long enough for one of those two
    // events to happen.
    ::WaitForMultipleObjects(countof(rgSplashScreenEvents), rgSplashScreenEvents, FALSE, INFINITE);

LExit:
    ReleaseHandle(rgSplashScreenEvents[1]);
    ReleaseHandle(rgSplashScreenEvents[0]);
}

extern "C" HRESULT SplashScreenDisplayError(
    __in BOOTSTRAPPER_DISPLAY display,
    __in_z LPCWSTR wzBundleName,
    __in HRESULT hrError
    )
{
    HRESULT hr = S_OK;
    LPWSTR sczDisplayString = NULL;

    hr = StrAllocFromError(&sczDisplayString, hrError, NULL);
    ExitOnFailure(hr, "Failed to allocate string to display error message");

    Trace(REPORT_STANDARD, "Error message displayed because: %ls", sczDisplayString);

    if (BOOTSTRAPPER_DISPLAY_NONE == display || BOOTSTRAPPER_DISPLAY_PASSIVE == display || BOOTSTRAPPER_DISPLAY_EMBEDDED == display)
    {
        // Don't display the error dialog in these modes
        ExitFunction1(hr = S_OK);
    }

    ::MessageBoxW(NULL, sczDisplayString, wzBundleName, MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);

LExit:
    ReleaseStr(sczDisplayString);

    return hr;
}


static HRESULT LoadConfiguration(
    __in HINSTANCE hInstance,
    __in BURN_SPLASH_SCREEN_CONFIGURATION** ppSplashScreenConfiguration
    )
{
    HRESULT hr = S_OK;
    DWORD cbData = 0;

    hr = ResReadData(hInstance, MAKEINTRESOURCEA(IDD_BURN_SPLASH_SCREEN_CONFIGURATION), reinterpret_cast<PVOID*>(ppSplashScreenConfiguration), &cbData);
    ExitOnFailure(hr, "Failed to read splash screen configuration resource.");

    AssertSz(sizeof(BURN_SPLASH_SCREEN_CONFIGURATION) == cbData, "Splash screen configuration resource size is wrong");

LExit:
    return hr;
}


static DWORD WINAPI ThreadProc(
    __in LPVOID pvContext
    )
{
    HRESULT hr = S_OK;
    SPLASHSCREEN_CONTEXT* pContext = static_cast<SPLASHSCREEN_CONTEXT*>(pvContext);

    SPLASHSCREEN_INFO splashScreenInfo = { };

    WNDCLASSW wc = { };
    BOOL fRegistered = FALSE;

    BOOL fRet = FALSE;
    MSG msg = { };

    // Register the window class.
    wc.lpfnWndProc = WndProc;
    wc.hInstance = pContext->hInstance;
    wc.hCursor = ::LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = BURN_SPLASHSCREEN_CLASS_WINDOW;
    if (!::RegisterClassW(&wc))
    {
        ExitWithLastError(hr, "Failed to register window.");
    }

    fRegistered = TRUE;

    hr = LoadSplashScreen(pContext, &splashScreenInfo);
    ExitOnFailure(hr, "Failed to load splash screen.");

    // Return the splash screen window and free the main thread waiting for us to be initialized.
    *pContext->pHwnd = splashScreenInfo.hWnd;
    ::SetEvent(pContext->hInitializedEvent);

    // Pump messages until the bootstrapper application destroys the window.
    while (0 != (fRet = ::GetMessageW(&msg, NULL, 0, 0)))
    {
        if (-1 == fRet)
        {
            hr = E_UNEXPECTED;
            ExitOnFailure(hr, "Unexpected return value from message pump.");
        }
        else if (!::IsDialogMessageW(splashScreenInfo.hWnd, &msg))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

LExit:
    if (fRegistered)
    {
        ::UnregisterClassW(BURN_SPLASHSCREEN_CLASS_WINDOW, pContext->hInstance);
    }

    if (splashScreenInfo.hBitmap)
    {
        ::DeleteObject(splashScreenInfo.hBitmap);
    }

    if (splashScreenInfo.hwndPrevious)
    {
        ::PostMessageW(splashScreenInfo.hwndPrevious, WM_CLOSE, 0, 0);
    }

    return hr;
}

static LRESULT CALLBACK WndProc(
    __in HWND hWnd,
    __in UINT uMsg,
    __in WPARAM wParam,
    __in LPARAM lParam
    )
{
    LRESULT lres = 0;
    SPLASHSCREEN_INFO* pSplashScreen = reinterpret_cast<SPLASHSCREEN_INFO*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (uMsg)
    {
    case WM_NCCREATE:
        OnNcCreate(hWnd, lParam);
        break;

    case WM_NCDESTROY:
        lres = ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
        ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        ::PostQuitMessage(0);
        return lres;

    case WM_NCHITTEST:
        return HTCAPTION; // allow window to be moved by grabbing any pixel.

    case WM_DPICHANGED:
        if (OnDpiChanged(pSplashScreen, wParam, lParam))
        {
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        OnEraseBkgnd(pSplashScreen, wParam);
        return 1;

    case WM_ENTERIDLE:
    case WM_MOVING:
        lres = ::DefWindowProcW(hWnd, uMsg, wParam, lParam);

        // We had to create our own splash screen so that Windows would automatically transfer focus from the other process's splash screen.
        // Try to make sure new splash screen has painted before closing old one to avoid flickering.
        if (pSplashScreen->hwndPrevious)
        {
            ::PostMessageW(pSplashScreen->hwndPrevious, WM_CLOSE, 0, 0);
            pSplashScreen->hwndPrevious = NULL;
        }

        return lres;
    }

    return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static HRESULT LoadSplashScreen(
    __in SPLASHSCREEN_CONTEXT* pContext,
    __in SPLASHSCREEN_INFO* pSplashScreen
    )
{
    HRESULT hr = S_OK;
    BITMAP bmp = { };
    POINT pt = { };
    int x = 0;
    int y = 0;
    DPIU_MONITOR_CONTEXT* pMonitorContext = NULL;
    RECT* pMonitorRect = NULL;

    if (::IsWindow(*pContext->pHwnd))
    {
        pSplashScreen->hwndPrevious = *pContext->pHwnd;
    }

    pSplashScreen->nDpi = USER_DEFAULT_SCREEN_DPI;
    pSplashScreen->hBitmap = ::LoadBitmapW(pContext->hInstance, MAKEINTRESOURCEW(pContext->pSplashScreenConfiguration->wResourceId));
    ExitOnNullWithLastError(pSplashScreen->hBitmap, hr, "Failed to load splash screen bitmap.");

    ::GetObject(pSplashScreen->hBitmap, sizeof(bmp), static_cast<void*>(&bmp));
    pSplashScreen->defaultDpiSize.cx = pSplashScreen->size.cx = bmp.bmWidth;
    pSplashScreen->defaultDpiSize.cy = pSplashScreen->size.cy = bmp.bmHeight;

    // Try to default to the monitor with the mouse, otherwise default to the primary monitor.
    if (!::GetCursorPos(&pt))
    {
        pt.x = 0;
        pt.y = 0;
    }

    // Try to center the window on the chosen monitor.
    hr = DpiuGetMonitorContextFromPoint(&pt, &pMonitorContext);
    if (SUCCEEDED(hr))
    {
        pMonitorRect = &pMonitorContext->mi.rcWork;
        if (pMonitorContext->nDpi != pSplashScreen->nDpi)
        {
            ScaleSplashScreen(pSplashScreen, pMonitorContext->nDpi, pMonitorRect->left, pMonitorRect->top);
        }

        x = pMonitorRect->left + (pMonitorRect->right - pMonitorRect->left - pSplashScreen->size.cx) / 2;
        y = pMonitorRect->top + (pMonitorRect->bottom - pMonitorRect->top - pSplashScreen->size.cy) / 2;
    }
    else
    {
        hr = S_OK;
        x = CW_USEDEFAULT;
        y = CW_USEDEFAULT;
    }

    pSplashScreen->hWnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, BURN_SPLASHSCREEN_CLASS_WINDOW, pContext->wzCaption, WS_POPUP | WS_VISIBLE, x, y, pSplashScreen->size.cx, pSplashScreen->size.cy, HWND_DESKTOP, NULL, pContext->hInstance, pSplashScreen);
    ExitOnNullWithLastError(pSplashScreen->hWnd, hr, "Failed to create splash screen window.");

LExit:
    MemFree(pMonitorContext);

    return hr;
}

static BOOL OnDpiChanged(
    __in SPLASHSCREEN_INFO* pSplashScreen,
    __in WPARAM wParam,
    __in LPARAM lParam
    )
{
    UINT nDpi = HIWORD(wParam);
    RECT* pRect = reinterpret_cast<RECT*>(lParam);
    BOOL fDpiChanged = pSplashScreen->nDpi != nDpi;

    if (fDpiChanged)
    {
        ScaleSplashScreen(pSplashScreen, nDpi, pRect->left, pRect->top);
    }

    return fDpiChanged;
}

static void OnEraseBkgnd(
    __in SPLASHSCREEN_INFO* pSplashScreen,
    __in WPARAM wParam
    )
{
    HDC hdc = reinterpret_cast<HDC>(wParam);
    HDC hdcMem = ::CreateCompatibleDC(hdc);
    HBITMAP hDefaultBitmap = static_cast<HBITMAP>(::SelectObject(hdcMem, pSplashScreen->hBitmap));
    ::SetStretchBltMode(hdc, HALFTONE);
    ::StretchBlt(hdc, 0, 0, pSplashScreen->size.cx, pSplashScreen->size.cy, hdcMem, 0, 0, pSplashScreen->defaultDpiSize.cx, pSplashScreen->defaultDpiSize.cy, SRCCOPY);
    ::SelectObject(hdcMem, hDefaultBitmap);
    ::DeleteDC(hdcMem);
}

static void OnNcCreate(
    __in HWND hWnd,
    __in LPARAM lParam
    )
{
    DPIU_WINDOW_CONTEXT windowContext = { };
    CREATESTRUCTW* pCreateStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
    SPLASHSCREEN_INFO* pSplashScreen = reinterpret_cast<SPLASHSCREEN_INFO*>(pCreateStruct->lpCreateParams);

    ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pSplashScreen));
    pSplashScreen->hWnd = hWnd;

    DpiuGetWindowContext(pSplashScreen->hWnd, &windowContext);

    if (windowContext.nDpi != pSplashScreen->nDpi)
    {
        ScaleSplashScreen(pSplashScreen, windowContext.nDpi, pCreateStruct->x, pCreateStruct->y);
    }
}

static void ScaleSplashScreen(
    __in SPLASHSCREEN_INFO* pSplashScreen,
    __in UINT nDpi,
    __in int x,
    __in int y
    )
{
    pSplashScreen->nDpi = nDpi;

    pSplashScreen->size.cx = DpiuScaleValue(pSplashScreen->defaultDpiSize.cx, pSplashScreen->nDpi);
    pSplashScreen->size.cy = DpiuScaleValue(pSplashScreen->defaultDpiSize.cy, pSplashScreen->nDpi);

    if (pSplashScreen->hWnd)
    {
        ::SetWindowPos(pSplashScreen->hWnd, NULL, x, y, pSplashScreen->size.cx, pSplashScreen->size.cy, SWP_NOACTIVATE | SWP_NOZORDER);
    }
}
