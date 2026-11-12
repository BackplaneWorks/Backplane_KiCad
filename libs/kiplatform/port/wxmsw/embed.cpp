/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <kiplatform/embed.h>

#include <algorithm>

#include <wx/app.h>
#include <wx/window.h>
#include <windows.h>

// Windows: as on X11, the frames are WS_CHILD windows of a holder window KiCad owns, and the
// holder is a child of the host's container HWND (or, parked, a hidden off-screen popup).
// AttachToParent moves only the holder.  The holder follows its parent's client size and
// notices the host moving it or its parent going away.  Dialogs are owned by the host's
// top-level HWND.

namespace
{
KIPLATFORM::EMBED::HOOKS s_hooks;
HWND                     s_holder = nullptr;
HWND                     s_parent = nullptr;   ///< the container the holder belongs in, or null

constexpr UINT_PTR HOLDER_TIMER = 1;
constexpr int      PARK_X = -32000;
constexpr int      PARK_Y = -32000;


void fitToParent( HWND aHolder, HWND aParent )
{
    RECT client;

    if( !aParent || !GetClientRect( aParent, &client ) )
        return;

    RECT now;
    GetWindowRect( aHolder, &now );
    MapWindowPoints( HWND_DESKTOP, aParent, reinterpret_cast<POINT*>( &now ), 2 );

    if( now.left != 0 || now.top != 0 || now.right - now.left != client.right
        || now.bottom - now.top != client.bottom )
    {
        SetWindowPos( aHolder, nullptr, 0, 0, client.right, client.bottom,
                      SWP_NOZORDER | SWP_NOACTIVATE );
    }
}


void becomeChildOf( HWND aHolder, HWND aParent )
{
    LONG_PTR style = GetWindowLongPtrW( aHolder, GWL_STYLE );
    style = ( style & ~WS_POPUP ) | WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    SetWindowLongPtrW( aHolder, GWL_STYLE, style );
    SetParent( aHolder, aParent );
    SetWindowPos( aHolder, HWND_TOP, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED );
    fitToParent( aHolder, aParent );
    ShowWindow( aHolder, SW_SHOWNA );
}


void park( HWND aHolder )
{
    // Hidden and off-screen, owned by nobody: the frames inside keep running, and a later
    // AttachToParent moves them back in one SetParent
    ShowWindow( aHolder, SW_HIDE );
    SetParent( aHolder, nullptr );
    LONG_PTR style = GetWindowLongPtrW( aHolder, GWL_STYLE );
    style = ( style & ~WS_CHILD ) | WS_POPUP | WS_CLIPCHILDREN;
    SetWindowLongPtrW( aHolder, GWL_STYLE, style );
    SetWindowPos( aHolder, nullptr, PARK_X, PARK_Y, 0, 0,
                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
}


LRESULT CALLBACK holderProc( HWND aHwnd, UINT aMsg, WPARAM aWParam, LPARAM aLParam )
{
    if( aMsg == WM_TIMER && aWParam == HOLDER_TIMER && aHwnd == s_holder )
    {
        HWND actual = GetParent( aHwnd );

        if( s_parent && !IsWindow( s_parent ) )
        {
            // The host's container is gone: park, and tell the host when it comes back
            s_parent = nullptr;
            park( aHwnd );

            if( s_hooks.detached )
                s_hooks.detached();
        }
        else if( s_parent && actual && actual != s_parent )
        {
            // The host moved the holder into another of its windows itself
            s_parent = actual;
            fitToParent( aHwnd, actual );

            if( s_hooks.reparented )
                s_hooks.reparented( reinterpret_cast<uint64_t>( actual ) );
        }
        else if( s_parent && !actual )
        {
            // The host took it out (SetParent(holder, NULL)): treated as a detach
            s_parent = nullptr;
            park( aHwnd );

            if( s_hooks.detached )
                s_hooks.detached();
        }
        else if( s_parent )
        {
            fitToParent( aHwnd, s_parent );
        }

        return 0;
    }

    if( aMsg == WM_ERASEBKGND )
        return 1;   // the frames cover it; no flash of the class brush

    return DefWindowProcW( aHwnd, aMsg, aWParam, aLParam );
}


const wchar_t* holderClass()
{
    static const wchar_t* name = L"BackplaneKiCadEmbedHolder";
    static bool           registered = false;

    if( !registered )
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof( wc );
        wc.lpfnWndProc = holderProc;
        wc.hInstance = GetModuleHandleW( nullptr );
        wc.lpszClassName = name;
        wc.hCursor = LoadCursor( nullptr, IDC_ARROW );
        registered = RegisterClassExW( &wc ) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    return name;
}
}


bool KIPLATFORM::EMBED::Supported()
{
    return true;
}


void KIPLATFORM::EMBED::Install( const HOOKS& aHooks )
{
    s_hooks = aHooks;
}


uint64_t KIPLATFORM::EMBED::CreateHolder( uint64_t aParent )
{
    HWND parent = reinterpret_cast<HWND>( aParent );

    if( parent && !IsWindow( parent ) )
        return 0;

    // Created parked (a hidden popup that never shows in the taskbar), then moved in
    HWND holder = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, holderClass(), L"",
                                   WS_POPUP | WS_CLIPCHILDREN, PARK_X, PARK_Y, 800, 600, nullptr,
                                   nullptr, GetModuleHandleW( nullptr ), nullptr );

    if( !holder )
        return 0;

    s_holder = holder;
    s_parent = parent;

    if( parent )
        becomeChildOf( holder, parent );

    SetTimer( holder, HOLDER_TIMER, 100, nullptr );
    return reinterpret_cast<uint64_t>( holder );
}


bool KIPLATFORM::EMBED::AttachHolder( uint64_t aHolder, uint64_t aParent )
{
    HWND holder = reinterpret_cast<HWND>( aHolder );
    HWND parent = reinterpret_cast<HWND>( aParent );

    if( !holder || !IsWindow( holder ) || ( parent && !IsWindow( parent ) ) )
        return false;

    s_parent = parent;

    if( parent )
        becomeChildOf( holder, parent );
    else
        park( holder );

    return true;
}


uint64_t KIPLATFORM::EMBED::PrepareFrame( wxWindow* aFrame, uint64_t aHolder )
{
    if( !aFrame || !aHolder )
        return 0;

    HWND hwnd = static_cast<HWND>( aFrame->GetHWND() );

    if( !hwnd )
        return 0;

    LONG_PTR style = GetWindowLongPtr( hwnd, GWL_STYLE );
    style &= ~( WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX
                | WS_MAXIMIZEBOX );
    style |= WS_CHILD | WS_CLIPCHILDREN;
    SetWindowLongPtr( hwnd, GWL_STYLE, style );

    LONG_PTR exStyle = GetWindowLongPtr( hwnd, GWL_EXSTYLE );
    exStyle &= ~( WS_EX_APPWINDOW | WS_EX_WINDOWEDGE );
    SetWindowLongPtr( hwnd, GWL_EXSTYLE, exStyle );

    SetParent( hwnd, reinterpret_cast<HWND>( aHolder ) );
    SetWindowPos( hwnd, nullptr, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );

    return reinterpret_cast<uint64_t>( hwnd );
}


uint64_t KIPLATFORM::EMBED::WindowId( wxWindow* aWindow )
{
    return aWindow ? reinterpret_cast<uint64_t>( aWindow->GetHWND() ) : 0;
}


bool KIPLATFORM::EMBED::HasFocus( wxWindow* aFrame )
{
    HWND focus = GetFocus();
    HWND frame = aFrame ? static_cast<HWND>( aFrame->GetHWND() ) : nullptr;

    return frame && focus && ( focus == frame || IsChild( frame, focus ) );
}


void KIPLATFORM::EMBED::TakeFocus( wxWindow* aFrame )
{
    if( aFrame )
        SetFocus( static_cast<HWND>( aFrame->GetHWND() ) );
}


void KIPLATFORM::EMBED::SendHostMessage( uint64_t aTarget, const char*, int aWinMsgOffset,
                                         const long aData[5] )
{
    if( aTarget )
    {
        PostMessage( reinterpret_cast<HWND>( aTarget ), WM_APP + aWinMsgOffset,
                     static_cast<WPARAM>( aData[3] ), static_cast<LPARAM>( aData[0] ) );
    }
}


void KIPLATFORM::EMBED::SetFrameGeometry( wxWindow* aFrame, int aX, int aY, int aWidth,
                                          int aHeight )
{
    if( aFrame )
        aFrame->SetSize( aX, aY, std::max( 1, aWidth ), std::max( 1, aHeight ) );
}


bool KIPLATFORM::EMBED::SetPreferDark( int aMode )
{
    // wxMSW's dark mode (3.3+) applies to windows created after it is enabled, so --theme dark
    // (before any frame exists) gives dark chrome throughout; a live change after startup
    // reaches only windows opened later (dialogs, new frames).  It cannot be turned off again.
#if wxCHECK_VERSION( 3, 3, 0 )
    if( aMode == 2 )
        return wxTheApp && wxTheApp->MSWEnableDarkMode( wxApp::DarkMode_Always );

    if( aMode == 0 )
        return wxTheApp && wxTheApp->MSWEnableDarkMode( wxApp::DarkMode_Auto );
#endif

    return false;
}


void KIPLATFORM::EMBED::InstallLoopTimer( std::function<void( double, double )> )
{
}


bool KIPLATFORM::EMBED::IsMapped( wxWindow* aFrame )
{
    // The frame's own visibility, as X11's map_state: IsWindowVisible() is false whenever an
    // ancestor (the host's container) is hidden, and a host may keep it hidden until a frame
    // reports shown
    HWND hwnd = aFrame ? static_cast<HWND>( aFrame->GetHWND() ) : nullptr;
    return hwnd && ( GetWindowLongPtrW( hwnd, GWL_STYLE ) & WS_VISIBLE ) != 0;
}


void KIPLATFORM::EMBED::EnsureMapped( wxWindow* aFrame )
{
    if( aFrame && aFrame->IsShown() )
        ShowWindow( static_cast<HWND>( aFrame->GetHWND() ), SW_SHOWNA );
}


void KIPLATFORM::EMBED::UnmapNative( wxWindow* aFrame )
{
    if( aFrame )
        ShowWindow( static_cast<HWND>( aFrame->GetHWND() ), SW_HIDE );
}


bool KIPLATFORM::EMBED::NativeModalTitle( wxString& )
{
    return false;
}


void KIPLATFORM::EMBED::MakeTransientFor( wxWindow* aDialog, uint64_t aToplevel )
{
    if( !aDialog || !aToplevel )
        return;

    HWND dialog = static_cast<HWND>( aDialog->GetHWND() );
    HWND owner = reinterpret_cast<HWND>( aToplevel );
    SetWindowLongPtr( dialog, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>( owner ) );

    RECT ownerRect, dialogRect;

    if( GetWindowRect( owner, &ownerRect ) && GetWindowRect( dialog, &dialogRect ) )
    {
        int w = dialogRect.right - dialogRect.left;
        int h = dialogRect.bottom - dialogRect.top;
        int x = ownerRect.left + ( ( ownerRect.right - ownerRect.left ) - w ) / 2;
        int y = ownerRect.top + ( ( ownerRect.bottom - ownerRect.top ) - h ) / 2;
        SetWindowPos( dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    }
}
