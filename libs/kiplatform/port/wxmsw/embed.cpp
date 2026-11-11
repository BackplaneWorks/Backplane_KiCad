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

#include <wx/window.h>
#include <windows.h>

// Windows: frames become WS_CHILD windows of the host's container HWND (no separate holder;
// the container is the holder).  Dialogs are owned by the host's top-level HWND.
// Untested on hardware in bp.8: the X11 path is the verified one.

namespace
{
KIPLATFORM::EMBED::HOOKS s_hooks;
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
    return aParent;
}


bool KIPLATFORM::EMBED::AttachHolder( uint64_t, uint64_t aParent )
{
    return aParent == 0 || IsWindow( reinterpret_cast<HWND>( aParent ) );
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
