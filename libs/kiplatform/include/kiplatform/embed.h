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

#ifndef KIPLATFORM_EMBED_H_
#define KIPLATFORM_EMBED_H_

#include <cstdint>
#include <functional>
#include <string>

#include <wx/string.h>

class wxWindow;

/**
 * Native helpers for running KiCad frames as child windows of a foreign (host) window.
 *
 * Window ids are native handles: an XID on X11, an HWND on Windows.  The state that ties these
 * together lives in BACKPLANE_EMBED (common/backplane_embed.cpp); everything here is a thin,
 * stateless wrapper except the event hooks, which are installed once by BACKPLANE_EMBED.
 */
namespace KIPLATFORM
{
namespace EMBED
{
    /// Callbacks from native event hooks to the embedding state.
    struct HOOKS
    {
        /// The window that dialogs should be transient for (the host's top-level), or 0.
        std::function<uint64_t()> hostToplevel;

        /// True if aWindow is one of our own frames that the window manager does not manage
        /// (an embedded frame or the hidden project manager).
        std::function<bool( uint64_t aWindow )> isUnmanaged;

        /// A top-level window other than an embedded frame was mapped (true) or unmapped.
        std::function<void( uint64_t aWindow, bool aMapped, const wxString& aTitle )> dialog;

        /// The holder was taken away from its parent (the host's window was destroyed, or the
        /// host parked the holder on the root).
        std::function<void()> detached;

        /// The host moved the holder into another of its windows, which is the parent now.
        std::function<void( uint64_t aParent )> reparented;
    };

    /// A line for the platform's embed trace (Windows: BACKPLANE_KICAD_EMBED_TRACE's log and
    /// OutputDebugString; elsewhere nothing).  Any thread.
    void TraceLine( const std::string& aLine );

    /// Note the calling thread as the UI thread (called from Install).
    void NoteMainThread();

    /// The UI thread's call stack, one "module+0xoffset" per frame, from another thread
    /// (Windows; elsewhere empty).  For a stall report.
    std::string MainThreadStack();

    /// True if the running toolkit backend can embed (X11 or Windows; not Wayland or macOS).
    bool Supported();

    /// Install the native hooks.  Call once, before any frame is shown.
    void Install( const HOOKS& aHooks );

    /**
     * Create the holder: an override-redirect child of aParent with the parent's visual and size
     * that every embedded frame lives in.  It follows the parent's size.  Returns 0 on failure.
     */
    uint64_t CreateHolder( uint64_t aParent );

    /**
     * Move the holder under aParent (mapped, filling it), or park it unmapped if aParent is 0.
     * Returns false if aParent is not a usable window.
     */
    bool AttachHolder( uint64_t aHolder, uint64_t aParent );

    /**
     * Realize aFrame and make it a child of aHolder without mapping it, so the window manager
     * never sees it.  Disables what only makes sense for managed top-levels (frame sync, the
     * GTK backdrop state).  Returns the frame's native window, or 0 on failure.
     */
    uint64_t PrepareFrame( wxWindow* aFrame, uint64_t aHolder );

    /// The native window of a top-level wx window (0 if not realized).
    uint64_t WindowId( wxWindow* aWindow );

    /// True if aFrame's native window is mapped (shown), whoever mapped or unmapped it.
    bool IsMapped( wxWindow* aFrame );

    /// Map aFrame's native window if the toolkit thinks it shown but it was unmapped from outside,
    /// then wait for the display server so the map's exposes are queued together.
    void EnsureMapped( wxWindow* aFrame );

    /// Unmap aFrame's native window as a host's own XUnmapWindow would, leaving the toolkit's
    /// shown state alone, so the host may map it again directly.  Elsewhere it hides the frame.
    void UnmapNative( wxWindow* aFrame );

    /// True if the keyboard focus is in aFrame (the window or a descendant).
    bool HasFocus( wxWindow* aFrame );

    /// Give aFrame the keyboard focus.
    void TakeFocus( wxWindow* aFrame );

    /// A click in aFrame landed in aClicked: give the keyboard to what was clicked, as a click
    /// in a top-level window would (Windows: SetFocus on the frame itself in the middle of the
    /// click left the frame holding the mouse capture, so the next right click never reached
    /// the canvas).  Elsewhere the same as TakeFocus( aFrame ).
    void TakeFocusForClick( wxWindow* aFrame, wxWindow* aClicked );

    /// After a click was handled: a mouse capture the frame itself still holds with no button
    /// down is released (Windows; elsewhere nothing).  False if there was none.
    bool ReleaseStrayCapture( wxWindow* aFrame );

    /**
     * Send a 32-bit ClientMessage of type aAtom with aData to aTarget (X11), or post
     * WM_APP + aWinMsgOffset with wParam = aData[3] and lParam = aData[0] (Windows).
     */
    void SendHostMessage( uint64_t aTarget, const char* aAtom, int aWinMsgOffset,
                          const long aData[5] );

    /// Move aWindow (a frame inside the holder) to the given holder-relative rectangle.
    void SetFrameGeometry( wxWindow* aFrame, int aX, int aY, int aWidth, int aHeight );

    /**
     * True if the toolkit shows a modal window that wx does not track (for example a native
     * message box run by the toolkit's own loop); aTitle gets its title.
     */
    bool NativeModalTitle( wxString& aTitle );

    /**
     * Ask the toolkit for its dark (aMode 2) or light (1) variant for this process, or restore
     * what it had before the first call (0).  Existing windows restyle.  Returns whether the
     * dark variant is now preferred.
     */
    bool SetPreferDark( int aMode );

    /**
     * Time the UI thread's event-loop iterations: aFunc gets each iteration's start (ms since
     * the epoch) and how long it ran (from the poll returning to the next poll).
     */
    void InstallLoopTimer( std::function<void( double aStartMs, double aDurMs )> aFunc );

    /// Make a top-level dialog transient for aToplevel and centre it on aToplevel.
    void MakeTransientFor( wxWindow* aDialog, uint64_t aToplevel );
} // namespace EMBED
} // namespace KIPLATFORM

#endif // KIPLATFORM_EMBED_H_
