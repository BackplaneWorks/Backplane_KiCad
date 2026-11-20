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
#include <wx/window.h>

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

        /// KiCad's own windows were destroyed under it (X11: the host destroyed its container
        /// with the holder still inside).  Nothing can be shown again: the handler saves what it
        /// can and ends the process.  Called once, on the UI thread.
        std::function<void()> windowsLost;

        /// A key pressed while one of KiCad's popups (a menu, a combo box's list) holds the
        /// keyboard, as a wx key code and wx modifiers, before the popup sees it.  Return true
        /// to take it: the popup does not get it.  aClosePopup closes the popup (call it before
        /// handing the key elsewhere, or the popup keeps the keyboard).  X11 only.
        std::function<bool( int aKeyCode, int aModifiers, const std::function<void()>& aClosePopup )>
                popupKey;

        /// Whether aWindow is an embedded frame that may not take the keyboard or activation now
        /// (a prewarm in progress, or a frame not on screen); aWhy says which.  Windows: a hook
        /// on the UI thread refuses any focus or activation change into such a frame, whoever
        /// asks (wx restoring its last focus, a control taking focus as it is built).
        std::function<bool( uint64_t aWindow, std::string& aWhy )> focusBlocked;

        /// Whether aWindow is a top-level frame being built for a prewarm (created while one is
        /// opening, not registered yet): still a captioned top-level, so a SetFocus in it (a
        /// draw panel's constructor) activated it.  Windows: refused like focusBlocked.
        std::function<bool( uint64_t aWindow )> buildingForPrewarm;
    };

    /// A line for the platform's embed trace (Windows: BACKPLANE_KICAD_EMBED_TRACE's log and
    /// OutputDebugString; elsewhere nothing).  Any thread.
    void TraceLine( const std::string& aLine );

    /// Note the calling thread as the UI thread (called from Install).
    void NoteMainThread();

    /// The UI thread's call stack, one "module+0xoffset" per frame, from another thread
    /// (Windows; elsewhere empty).  For a stall report.
    std::string MainThreadStack();

    /// Focus, capture, foreground and active window, for a trace line (Windows; elsewhere
    /// empty).  Any thread.
    std::string InputState();

    /// Write a minidump of the whole process (every thread's stack) beside the embed trace, for
    /// a hang the trace alone cannot explain.  Returns its path, or why none was written
    /// (Windows with BACKPLANE_KICAD_EMBED_TRACE set; elsewhere empty).  Any thread.
    std::string WriteMiniDump( const std::string& aWhy );

    /// True if the running toolkit backend can embed (X11 or Windows; not Wayland or macOS).
    bool Supported();

    /// Whether the keyboard is in aFrame at the platform level, whatever wx's own focus says (a
    /// disabled frame has none): X11 and Windows, the input focus is in its window; macOS, KiCad
    /// is the active app and aFrame's window is key.  False when the host has the keyboard.
    bool FrameIsKey( wxWindow* aFrame );

    /// True once KiCad's own windows may have been destroyed under it (X11: an X error on one of
    /// them, until checked; then for good if the holder is gone).  Nothing may draw then: GL on a
    /// destroyed drawable crashes the driver or raises an error GDK exits on.  Elsewhere false.
    bool WindowsGone();

    /// WindowsGone(), after asking the server whether the holder still exists (X11: one round
    /// trip; a destroyed holder is handled as lost at once).  For a graphics failure, which can
    /// come before any X error.  Elsewhere false.
    bool CheckWindowsGone();

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

    /// Show aFrame for a prewarm without making it the active window.  Windows: wx's Show()
    /// shows a top-level with SW_SHOW, which activates it, and the activation stayed with the
    /// hidden frame: the next activation of KiCad's input queue (a click on an editor tab)
    /// gave the person's keys to it.  Elsewhere aFrame->Show( true ).
    void ShowInactive( wxWindow* aFrame );

    /// The window holding the keyboard focus now (any process), for ReturnFocusFrom.
    uint64_t FocusedWindow();

    /// If the keyboard focus is inside aFrame (a prewarmed frame, about to be hidden), give it
    /// back to aBefore when that window still exists, is outside aFrame and belongs to this
    /// thread; else leave it where it is (a SetFocus from KiCad's thread cannot reach another
    /// thread's window).  Traced either way.  Windows only; elsewhere nothing.
    void ReturnFocusFrom( wxWindow* aFrame, uint64_t aBefore );

    /// True if the keyboard focus is in aFrame (the window or a descendant).
    bool HasFocus( wxWindow* aFrame );

    /// Give aFrame the keyboard focus.
    void TakeFocus( wxWindow* aFrame );

    /// A click in aFrame landed in aClicked: give the keyboard to what was clicked, as a click
    /// in a top-level window would (Windows: SetFocus on the frame itself in the middle of the
    /// click left the frame holding the mouse capture, so the next right click never reached
    /// the canvas).  Elsewhere the same as TakeFocus( aFrame ).
    void TakeFocusForClick( wxWindow* aFrame, wxWindow* aClicked );

    /// #1198 (bp.13.3 b2): whether a key request made by a click must wait before it moves the key
    /// window and wx's focus: while a mouse button is held (aButtonsDown, NSEvent.pressedMouseButtons)
    /// or a menu is tracking.  Moving them between the click's down and its up made the draw panel lose
    /// focus (EDA_DRAW_PANEL_GAL::onLostFocus resets the tool dispatcher's buttons), so the up had no
    /// down: a right click opened no context menu and a left click did nothing but activate KiCad.
    inline bool KeyWaitsForGesture( unsigned long aButtonsDown, bool aMenuTracking )
    {
        return aButtonsDown != 0 || aMenuTracking;
    }

    /// #1198: whether wx's focus stays where it is rather than going to aClicked after a click's
    /// activation: already on aClicked, inside it (the GL canvas inside the draw panel), or aClicked
    /// inside the focused window.  The same rule as the click's own focus on Windows
    /// (BACKPLANE_EMBED::FilterEvent): moving it between the draw panel and its GL canvas cost the
    /// canvas its focus mid-click.
    inline bool FocusStays( wxWindow* aFocus, wxWindow* aClicked )
    {
        if( !aFocus || !aClicked )
            return aFocus != nullptr;

        return aFocus == aClicked || aClicked->IsDescendant( aFocus ) || aFocus->IsDescendant( aClicked );
    }

    /// Trace only (#1198 diagnosis): a button went down in embedded aFrame; aHasFocus is what
    /// HasFocus() said.  macOS writes the app's and the frame's key state to the embed trace;
    /// elsewhere nothing.
    void TraceClick( wxWindow* aFrame, bool aHasFocus );

    /// A reserved chord is being handed to the host (#1399): macOS 14+ activation is cooperative,
    /// so KiCad, the active app, consents now for the host's app to activate itself (the host does,
    /// on the chord).  The consent given when KiCad became active no longer counts once the person
    /// has clicked and typed in KiCad, and the host's activate was ignored; on macOS 26 the consent
    /// alone is not enough either, so KiCad also activates the host itself.  Only on a reserved
    /// chord the person pressed.  Elsewhere nothing.
    void YieldActivationToHost();

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

    /// Place aFrame where a prewarm lays it out and paints it unseen, at aWidth x aHeight.  X11
    /// and Windows: off the container at (-30000, -30000).  macOS: ordered in transparent, behind
    /// everything, ignoring the mouse, never key.
    void ParkForPrewarm( wxWindow* aFrame, int aWidth, int aHeight );

    /// macOS: the host's container rect in screen points (top-left origin of the primary
    /// display); visible false orders every frame out (they stay shown to KiCad), raise orders
    /// the shown frames just above the host window.  Elsewhere nothing (frames are children of
    /// the container).
    void SetHostRect( int aX, int aY, int aWidth, int aHeight, bool aVisible, bool aRaise );

    /**
     * macOS: run aFunc on the main thread the next time the main run loop runs a modal session
     * (NSModalPanelRunLoopMode: [NSAlert runModal], the open and save panels), where wx's
     * CallAfter waits for the session to end; it waits for one until then.  Callable from any
     * thread.  Returns false elsewhere (nothing posted).
     */
    bool PostToMainModal( std::function<void()> aFunc );

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
