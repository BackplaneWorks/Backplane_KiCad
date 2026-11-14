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
#include <cctype>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include <wx/defs.h>
#include <wx/window.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace
{
KIPLATFORM::EMBED::HOOKS s_hooks;

#ifdef GDK_WINDOWING_X11
Window s_holder = 0;   ///< the holder window (ours)
Window s_parent = 0;   ///< the host's container the holder is in, or 0 when parked
bool   s_installed = false;


Display* xDisplay()
{
    GdkDisplay* display = gdk_display_get_default();

    if( !display || !GDK_IS_X11_DISPLAY( display ) )
        return nullptr;

    return GDK_DISPLAY_XDISPLAY( display );
}


/// Run aFunc with X errors trapped (foreign windows can vanish at any time).  True if no error.
template <typename FUNC>
bool trapped( FUNC aFunc )
{
    GdkDisplay* display = gdk_display_get_default();
    gdk_x11_display_error_trap_push( display );
    aFunc();
    XSync( GDK_DISPLAY_XDISPLAY( display ), False );
    return gdk_x11_display_error_trap_pop( display ) == 0;
}


Window xidOf( GtkWidget* aWidget )
{
    if( !aWidget || !gtk_widget_get_realized( aWidget ) )
        return 0;

    GdkWindow* window = gtk_widget_get_window( aWidget );

    if( !window || !GDK_IS_X11_WINDOW( window ) )
        return 0;

    return GDK_WINDOW_XID( window );
}


/// Root-relative geometry of aWindow.
bool rootGeometry( Display* aDisplay, Window aWindow, int& aX, int& aY, int& aW, int& aH )
{
    bool ok = false;

    trapped( [&]()
             {
                 XWindowAttributes attrs;

                 if( !XGetWindowAttributes( aDisplay, aWindow, &attrs ) )
                     return;

                 Window child;
                 ok = XTranslateCoordinates( aDisplay, aWindow, attrs.root, 0, 0, &aX, &aY,
                                             &child );
                 aW = attrs.width;
                 aH = attrs.height;
             } );

    return ok;
}


/// The holder follows the host container's size, and notices when the host goes away.
GdkFilterReturn holderFilter( GdkXEvent* aXEvent, GdkEvent*, gpointer )
{
    XEvent* ev = static_cast<XEvent*>( aXEvent );

    if( !s_holder )
        return GDK_FILTER_CONTINUE;

    if( ev->type == ConfigureNotify && s_parent && ev->xconfigure.window == s_parent
        && ev->xconfigure.event == s_parent )
    {
        Display* display = ev->xconfigure.display;
        int      w = std::max( 1, ev->xconfigure.width );
        int      h = std::max( 1, ev->xconfigure.height );

        trapped( [&]() { XResizeWindow( display, s_holder, w, h ); } );
    }
    else if( ev->type == ReparentNotify && ev->xreparent.window == s_holder
             && ev->xreparent.parent != s_parent
             && ev->xreparent.parent != DefaultRootWindow( ev->xany.display ) )
    {
        // The host moved the holder into another of its windows itself (a pane moved to another
        // window, or the frames parked on View taken back).  Follow it: that window is the parent
        // now (its size, and where focus/key messages go).  Our own reparenting never gets here:
        // s_parent is updated first.
        Display* display = ev->xany.display;
        Window   oldParent = s_parent;
        Window   parent = ev->xreparent.parent;
        int      w = 1, h = 1;

        s_parent = parent;

        trapped( [&]()
                 {
                     if( oldParent )
                         XSelectInput( display, oldParent, NoEventMask );

                     XSelectInput( display, parent, StructureNotifyMask );

                     XWindowAttributes attrs;

                     if( XGetWindowAttributes( display, parent, &attrs ) )
                     {
                         w = std::max( 1, attrs.width );
                         h = std::max( 1, attrs.height );
                     }

                     XMoveResizeWindow( display, s_holder, 0, 0, w, h );
                     XMapWindow( display, s_holder );
                 } );

        if( s_hooks.reparented )
            s_hooks.reparented( parent );
    }
    else if( ( ev->type == ReparentNotify && ev->xreparent.window == s_holder && s_parent
               && ev->xreparent.parent != s_parent )
             || ( ev->type == DestroyNotify && s_parent && ev->xdestroywindow.window == s_parent ) )
    {
        // The holder went to the root (the host parked it there, or its connection closed and
        // the server moved it, being in the host's save-set), or the host's window went away.
        // Park it unmapped; frames stay alive until the next AttachToParent or until the host
        // moves the holder into a window again.
        Display* display = ev->xany.display;
        s_parent = 0;

        // With a core save-set the server puts the holder in the nearest surviving ancestor,
        // usually the window manager's frame, which the WM is about to destroy.  Move it to the
        // root at once (a host using XFixesChangeSaveSet( SaveSetRoot, SaveSetUnmap ) has
        // already done both).
        trapped( [&]()
                 {
                     XUnmapWindow( display, s_holder );
                     XReparentWindow( display, s_holder, DefaultRootWindow( display ), -10000,
                                      -10000 );
                 } );

        if( s_hooks.detached )
            s_hooks.detached();
    }

    return GDK_FILTER_CONTINUE;
}


void suppressBackdrop( GtkWidget* aWidget, GtkStateFlags, gpointer )
{
    // An embedded frame never gets the window manager's focused state, so GTK would draw it
    // with the :backdrop (inactive) style forever.
    if( gtk_widget_get_state_flags( aWidget ) & GTK_STATE_FLAG_BACKDROP )
        gtk_widget_unset_state_flags( aWidget, GTK_STATE_FLAG_BACKDROP );
}


void centreOn( GtkWindow* aWindow, Window aOn )
{
    Display* display = xDisplay();
    int      x = 0, y = 0, w = 0, h = 0;

    if( !display || !rootGeometry( display, aOn, x, y, w, h ) )
        return;

    int dw = 0, dh = 0;
    gtk_window_get_size( aWindow, &dw, &dh );

    gtk_window_move( aWindow, x + std::max( 0, ( w - dw ) / 2 ),
                     y + std::max( 0, ( h - dh ) / 2 ) );
}


/// Dialogs and modal frames: transient for the host's top-level, centred on it.
gboolean mapHook( GSignalInvocationHint*, guint aCount, const GValue* aParams, gpointer aMapped )
{
    if( aCount < 1 || !G_VALUE_HOLDS_OBJECT( &aParams[0] ) )
        return TRUE;

    GObject* object = G_OBJECT( g_value_get_object( &aParams[0] ) );

    if( !GTK_IS_WINDOW( object ) )
        return TRUE;

    GtkWindow* window = GTK_WINDOW( object );

    if( gtk_window_get_window_type( window ) != GTK_WINDOW_TOPLEVEL )
        return TRUE;

    Window xid = xidOf( GTK_WIDGET( window ) );

    if( !xid || ( s_hooks.isUnmanaged && s_hooks.isUnmanaged( xid ) ) )
        return TRUE;

    bool        mapped = GPOINTER_TO_INT( aMapped ) != 0;
    const char* title = gtk_window_get_title( window );

    if( mapped && s_hooks.hostToplevel )
    {
        if( uint64_t top = s_hooks.hostToplevel() )
        {
            GtkWindow* parent = gtk_window_get_transient_for( window );
            Window     parentXid = parent ? xidOf( GTK_WIDGET( parent ) ) : 0;

            // Retarget only what would otherwise point at a window the WM does not manage; a
            // dialog over another dialog keeps its real parent.
            if( !parentXid || ( s_hooks.isUnmanaged && s_hooks.isUnmanaged( parentXid ) ) )
            {
                Display* display = xDisplay();

                trapped( [&]() { XSetTransientForHint( display, xid, (Window) top ); } );
                centreOn( window, (Window) top );
            }
        }
    }

    if( s_hooks.dialog )
        s_hooks.dialog( xid, mapped, wxString::FromUTF8( title ? title : "" ) );

    return TRUE;
}
#endif
} // namespace


bool KIPLATFORM::EMBED::Supported()
{
#ifdef GDK_WINDOWING_X11
    return xDisplay() != nullptr;
#else
    return false;
#endif
}


#ifdef GDK_WINDOWING_X11
namespace
{
/// The key as wx reports it in a key event: its unshifted key (the keymap's level 0, so
/// Ctrl+Shift+Tab is Tab with Shift) as a wx key code, and the modifiers.  False for a key no
/// chord can name.
bool wxKeyFor( const GdkEventKey& aKey, int& aCode, int& aModifiers )
{
    aModifiers = 0;

    if( aKey.state & GDK_CONTROL_MASK )
        aModifiers |= wxMOD_CONTROL;

    if( aKey.state & GDK_SHIFT_MASK )
        aModifiers |= wxMOD_SHIFT;

    if( aKey.state & GDK_MOD1_MASK )
        aModifiers |= wxMOD_ALT;

    if( aKey.state & ( GDK_SUPER_MASK | GDK_META_MASK ) )
        aModifiers |= wxMOD_META;

    guint keyval = aKey.keyval;
    GdkKeymap* keymap = gdk_keymap_get_for_display( gdk_window_get_display( aKey.window ) );

    if( keymap )
    {
        gdk_keymap_translate_keyboard_state( keymap, aKey.hardware_keycode, (GdkModifierType) 0,
                                             aKey.group, &keyval, nullptr, nullptr, nullptr );
    }

    switch( keyval )
    {
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: aCode = WXK_TAB;    return true;
    case GDK_KEY_Escape:       aCode = WXK_ESCAPE; return true;
    case GDK_KEY_Return:       aCode = WXK_RETURN; return true;
    default: break;
    }

    if( keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12 )
    {
        aCode = WXK_F1 + static_cast<int>( keyval - GDK_KEY_F1 );
        return true;
    }

    gunichar character = gdk_keyval_to_unicode( keyval );

    if( character > 0x20 && character < 0x80 )
    {
        aCode = std::toupper( static_cast<int>( character ) );
        return true;
    }

    return false;
}


/// Close the popup that holds the keyboard: a menu (with its parents, so no submenu is left
/// open) is cancelled; any other popup gets an Escape.
void closePopup( GdkEvent* aKeyEvent )
{
    GtkWidget* grab = gtk_grab_get_current();

    if( grab && GTK_IS_MENU_SHELL( grab ) )
    {
        GtkWidget* outer = grab;

        while( GtkWidget* parent = gtk_menu_shell_get_parent_shell( GTK_MENU_SHELL( outer ) ) )
            outer = parent;

        gtk_menu_shell_cancel( GTK_MENU_SHELL( outer ) );
        return;
    }

    GdkEvent* escape = gdk_event_copy( aKeyEvent );
    escape->key.keyval = GDK_KEY_Escape;
    escape->key.state = 0;

    GdkKeymapKey* keys = nullptr;
    gint          count = 0;
    GdkKeymap*    keymap = gdk_keymap_get_for_display( gdk_window_get_display( escape->key.window ) );

    if( keymap && gdk_keymap_get_entries_for_keyval( keymap, GDK_KEY_Escape, &keys, &count ) && count > 0 )
        escape->key.hardware_keycode = keys[0].keycode;

    g_free( keys );
    gtk_main_do_event( escape );
    gdk_event_free( escape );
}


/// True while one of our popups holds the keyboard: GTK's current grab is a menu or sits in a
/// popup window (a combo box's list), or the key is addressed to a popup window itself.  A
/// menu's grab reports keys on the focused frame's window, so the grab is the test that counts.
bool popupHasKeyboard( const GdkEventKey& aKey )
{
    if( GtkWidget* grab = gtk_grab_get_current() )
    {
        if( GTK_IS_MENU_SHELL( grab ) )
            return true;

        GtkWidget* top = gtk_widget_get_toplevel( grab );

        if( GTK_IS_WINDOW( top ) && gtk_window_get_window_type( GTK_WINDOW( top ) ) == GTK_WINDOW_POPUP )
            return true;
    }

    return aKey.window
           && gdk_window_get_window_type( gdk_window_get_toplevel( aKey.window ) ) == GDK_WINDOW_TEMP;
}


/// A key snooper: GTK calls it for every key press before the grab widget (a popup menu) gets
/// it.  A key bound for one of our popups is offered to the popupKey hook first, since the
/// popup's grab keeps it from any frame and filter.  (Not gdk_event_handler_set: wxGTK's yield
/// resets that handler to gtk_main_do_event every time it runs.)
gint keySnooper( GtkWidget*, GdkEventKey* aKey, gpointer )
{
    if( aKey->type != GDK_KEY_PRESS || !s_hooks.popupKey || !popupHasKeyboard( *aKey ) )
        return FALSE;

    int code = 0;
    int modifiers = 0;

    if( !wxKeyFor( *aKey, code, modifiers ) )
        return FALSE;

    GdkEvent* event = reinterpret_cast<GdkEvent*>( aKey );

    return s_hooks.popupKey( code, modifiers, [event]() { closePopup( event ); } ) ? TRUE : FALSE;
}
} // namespace
#endif


void KIPLATFORM::EMBED::Install( const HOOKS& aHooks )
{
    s_hooks = aHooks;

#ifdef GDK_WINDOWING_X11
    if( s_installed || !Supported() )
        return;

    s_installed = true;
    gdk_window_add_filter( nullptr, holderFilter, nullptr );
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_key_snooper_install( keySnooper, nullptr );
    G_GNUC_END_IGNORE_DEPRECATIONS

    guint mapSignal = g_signal_lookup( "map", GTK_TYPE_WIDGET );
    guint unmapSignal = g_signal_lookup( "unmap", GTK_TYPE_WIDGET );

    g_signal_add_emission_hook( mapSignal, 0, mapHook, GINT_TO_POINTER( 1 ), nullptr );
    g_signal_add_emission_hook( unmapSignal, 0, mapHook, GINT_TO_POINTER( 0 ), nullptr );
#endif
}


void KIPLATFORM::EMBED::TraceLine( const std::string& )
{
}


uint64_t KIPLATFORM::EMBED::CreateHolder( uint64_t aParent )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();

    if( !display )
        return 0;

    if( s_holder )
        return AttachHolder( s_holder, aParent ) ? s_holder : 0;

    Window root = DefaultRootWindow( display );
    Window parent = aParent ? (Window) aParent : root;
    int    w = 1, h = 1;

    bool ok = trapped( [&]()
                       {
                           XWindowAttributes attrs;

                           if( XGetWindowAttributes( display, parent, &attrs ) )
                           {
                               w = std::max( 1, attrs.width );
                               h = std::max( 1, attrs.height );
                           }
                       } );

    if( !ok )
        return 0;

    XSetWindowAttributes attrs = {};
    attrs.override_redirect = True;          // never managed, even when parked at the root
    attrs.background_pixmap = ParentRelative;
    attrs.event_mask = StructureNotifyMask;  // ReparentNotify when the host goes away

    Window holder = 0;

    ok = trapped( [&]()
                  {
                      holder = XCreateWindow( display, parent, aParent ? 0 : -10000,
                                              aParent ? 0 : -10000, w, h, 0, CopyFromParent,
                                              InputOutput, CopyFromParent,
                                              CWOverrideRedirect | CWBackPixmap | CWEventMask,
                                              &attrs );
                  } );

    if( !ok || !holder )
        return 0;

    s_holder = holder;
    s_parent = 0;

    if( !AttachHolder( holder, aParent ) )
        return 0;

    return holder;
#else
    return 0;
#endif
}


bool KIPLATFORM::EMBED::AttachHolder( uint64_t aHolder, uint64_t aParent )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();

    if( !display || !aHolder )
        return false;

    Window holder = (Window) aHolder;
    Window root = DefaultRootWindow( display );
    Window oldParent = s_parent;

    if( !aParent )
    {
        s_parent = 0;

        trapped( [&]()
                 {
                     if( oldParent )
                         XSelectInput( display, oldParent, NoEventMask );

                     XUnmapWindow( display, holder );
                     XReparentWindow( display, holder, root, -10000, -10000 );
                 } );

        return true;
    }

    Window parent = (Window) aParent;
    int    w = 1, h = 1;

    bool ok = trapped( [&]()
                       {
                           XWindowAttributes attrs;

                           if( XGetWindowAttributes( display, parent, &attrs ) )
                           {
                               w = std::max( 1, attrs.width );
                               h = std::max( 1, attrs.height );
                           }
                       } );

    if( !ok )
        return false;

    // Set before reparenting so the filter does not take our own ReparentNotify for a detach.
    s_parent = parent;

    ok = trapped( [&]()
                  {
                      if( oldParent && oldParent != parent )
                          XSelectInput( display, oldParent, NoEventMask );

                      XSelectInput( display, parent, StructureNotifyMask );
                      XReparentWindow( display, holder, parent, 0, 0 );
                      XMoveResizeWindow( display, holder, 0, 0, w, h );
                      XMapWindow( display, holder );
                  } );

    if( !ok )
        s_parent = 0;

    return ok;
#else
    return false;
#endif
}


uint64_t KIPLATFORM::EMBED::PrepareFrame( wxWindow* aFrame, uint64_t aHolder )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();

    if( !display || !aFrame || !aHolder )
        return 0;

    GtkWidget* widget = static_cast<GtkWidget*>( aFrame->GetHandle() );

    if( !widget )
        return 0;

    if( !gtk_widget_get_realized( widget ) )
    {
        // As wxTopLevelWindowGTK::Show() does for a deferred show: realizing normally size-
        // allocates the tree, which sends size events in the wrong order unless the allocation
        // is not the default (1,1).  Realizing before Show() also stops wx deferring the show
        // until the window manager reports frame extents, which an embedded frame never gets.
        GtkAllocation alloc;
        gtk_widget_get_allocation( widget, &alloc );
        const int allocWidth = alloc.width;

        if( allocWidth == 1 )
        {
            alloc.width = 2;
            gtk_widget_set_allocation( widget, &alloc );
        }

        gtk_widget_realize( widget );

        if( allocWidth == 1 )
        {
            alloc.width = 1;
            gtk_widget_set_allocation( widget, &alloc );
        }
    }

    GdkWindow* gdkWindow = gtk_widget_get_window( widget );

    if( !gdkWindow || !GDK_IS_X11_WINDOW( gdkWindow ) )
        return 0;

    // No compositor will ever send _NET_WM_FRAME_DRAWN for a window it does not manage; with
    // frame sync on, GDK would stop painting after the first frame.  (GDK documents this for
    // XEMBED children; it is the same situation.)
    gdk_x11_window_set_frame_sync_enabled( gdkWindow, FALSE );

    g_signal_handlers_disconnect_by_func( widget, reinterpret_cast<void*>( suppressBackdrop ),
                                          nullptr );
    g_signal_connect( widget, "state-flags-changed", G_CALLBACK( suppressBackdrop ), nullptr );
    suppressBackdrop( widget, GTK_STATE_FLAG_NORMAL, nullptr );

    Window xid = GDK_WINDOW_XID( gdkWindow );

    bool ok = trapped( [&]()
                       {
                           // Normally the frame was never mapped.  If something mapped it already
                           // (a top-level the WM may manage), withdraw it first so the WM lets go.
                           XWindowAttributes attrs;

                           if( XGetWindowAttributes( display, xid, &attrs )
                               && attrs.map_state != IsUnmapped )
                           {
                               XWithdrawWindow( display, xid, DefaultScreen( display ) );
                           }

                           XReparentWindow( display, xid, (Window) aHolder, 0, 0 );
                       } );


    return ok ? xid : 0;
#else
    return 0;
#endif
}


uint64_t KIPLATFORM::EMBED::WindowId( wxWindow* aWindow )
{
#ifdef GDK_WINDOWING_X11
    if( !aWindow || !xDisplay() )
        return 0;

    return xidOf( static_cast<GtkWidget*>( aWindow->GetHandle() ) );
#else
    return 0;
#endif
}


bool KIPLATFORM::EMBED::IsMapped( wxWindow* aFrame )
{
    GtkWidget* widget = aFrame ? static_cast<GtkWidget*>( aFrame->GetHandle() ) : nullptr;
    GdkWindow* window = widget ? gtk_widget_get_window( widget ) : nullptr;

    if( !window )
        return false;

#ifdef GDK_WINDOWING_X11
    // Ask the server: GDK does not always follow an outside XUnmapWindow of a toplevel's
    // window, and the host maps and unmaps frame windows itself
    if( Display* display = xDisplay(); display && GDK_IS_X11_WINDOW( window ) )
    {
        XWindowAttributes attributes;

        if( XGetWindowAttributes( display, GDK_WINDOW_XID( window ), &attributes ) )
            return attributes.map_state != IsUnmapped;
    }
#endif

    return gdk_window_is_visible( window );
}


void KIPLATFORM::EMBED::EnsureMapped( wxWindow* aFrame )
{
    GtkWidget* widget = aFrame ? static_cast<GtkWidget*>( aFrame->GetHandle() ) : nullptr;
    GdkWindow* window = widget ? gtk_widget_get_window( widget ) : nullptr;

    if( window && gtk_widget_get_visible( widget ) && !IsMapped( aFrame ) )
    {
#ifdef GDK_WINDOWING_X11
        // Mapped as the host would: GDK may still think the window it never saw unmapped is shown
        if( Display* display = xDisplay(); display && GDK_IS_X11_WINDOW( window ) )
            XMapWindow( display, GDK_WINDOW_XID( window ) );
        else
#endif
            gdk_window_show_unraised( window );
    }

    // Wait for the server to map it, so its exposes are queued before the first paint: the
    // canvas then paints once for all of them instead of once early and again on the expose.
    if( window )
        gdk_display_sync( gdk_window_get_display( window ) );
}


void KIPLATFORM::EMBED::UnmapNative( wxWindow* aFrame )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();
    Window   window = (Window) WindowId( aFrame );

    if( display && window )
    {
        XUnmapWindow( display, window );
        XFlush( display );
        return;
    }
#endif

    if( aFrame )
        aFrame->Show( false );
}


void KIPLATFORM::EMBED::ShowInactive( wxWindow* aFrame )
{
    // A prewarmed frame lives in the holder (an X11 child), which the window manager never
    // activates: Show() as before
    if( aFrame )
        aFrame->Show( true );
}


uint64_t KIPLATFORM::EMBED::FocusedWindow()
{
    return 0;
}


void KIPLATFORM::EMBED::ReturnFocusFrom( wxWindow*, uint64_t )
{
}


bool KIPLATFORM::EMBED::HasFocus( wxWindow* aFrame )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();
    Window   target = (Window) WindowId( aFrame );

    if( !display || !target )
        return false;

    Window focus = 0;
    int    revert = 0;
    bool   found = false;

    trapped( [&]()
             {
                 XGetInputFocus( display, &focus, &revert );
                 Window root = DefaultRootWindow( display );

                 // Walk up from the focus window; GDK may focus an input-only child.
                 for( int depth = 0; depth < 16 && focus && focus != PointerRoot && focus != root;
                      ++depth )
                 {
                     if( focus == target )
                     {
                         found = true;
                         break;
                     }

                     Window       rootReturn = 0, parent = 0;
                     Window*      children = nullptr;
                     unsigned int count = 0;

                     if( !XQueryTree( display, focus, &rootReturn, &parent, &children, &count ) )
                         break;

                     if( children )
                         XFree( children );

                     focus = parent;
                 }
             } );

    return found;
#else
    return false;
#endif
}


void KIPLATFORM::EMBED::TakeFocus( wxWindow* aFrame )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();
    Window   target = (Window) WindowId( aFrame );

    if( !display || !target )
        return;

    trapped( [&]() { XSetInputFocus( display, target, RevertToParent, CurrentTime ); } );
#endif
}


void KIPLATFORM::EMBED::SendHostMessage( uint64_t aTarget, const char* aAtom, int,
                                         const long aData[5] )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();

    if( !display || !aTarget )
        return;

    XEvent ev = {};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = (Window) aTarget;
    ev.xclient.message_type = XInternAtom( display, aAtom, False );
    ev.xclient.format = 32;

    for( int ii = 0; ii < 5; ++ii )
        ev.xclient.data.l[ii] = aData[ii];

    // An empty event mask delivers the event to the client that created the target window.
    trapped( [&]() { XSendEvent( display, (Window) aTarget, False, NoEventMask, &ev ); } );
#endif
}


void KIPLATFORM::EMBED::SetFrameGeometry( wxWindow* aFrame, int aX, int aY, int aWidth,
                                          int aHeight )
{
    if( !aFrame )
        return;

    aFrame->SetSize( aX, aY, std::max( 1, aWidth ), std::max( 1, aHeight ) );

    // wx skips a move to where it believes the frame already is, and its idea of a frame inside
    // the holder can be stale; GTK keeps its own requested position and re-applies it when it
    // configures the window.  Only when the X window is not where it was asked to be (a
    // prewarmed frame left at its off-screen -30000) set GTK's position and move the X window
    // too: an unconditional gtk_window_move shifted frames that were already in place.
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();
    Window   window = (Window) WindowId( aFrame );

    if( !display || !window )
        return;

    XSync( display, False );

    Window       root = 0;
    int          x = 0, y = 0;
    unsigned int w = 0, h = 0, border = 0, depth = 0;

    if( !XGetGeometry( display, window, &root, &x, &y, &w, &h, &border, &depth ) )
        return;

    if( x == aX && y == aY )
        return;

    if( GtkWidget* widget = static_cast<GtkWidget*>( aFrame->GetHandle() ); widget && GTK_IS_WINDOW( widget ) )
        gtk_window_move( GTK_WINDOW( widget ), aX, aY );

    XMoveResizeWindow( display, window, aX, aY, std::max( 1, aWidth ), std::max( 1, aHeight ) );
    XFlush( display );
#endif
}


/// True if GTK can load aTheme's dark variant (built in, or an installed gtk-dark.css).
static bool themeHasDarkVariant( const gchar* aTheme )
{
    if( g_strcmp0( aTheme, "Adwaita" ) == 0 )
        return true;

    std::vector<std::string> roots;
    roots.push_back( std::string( g_get_user_data_dir() ) + "/themes" );
    roots.push_back( std::string( g_get_home_dir() ) + "/.themes" );

    for( const gchar* const* dir = g_get_system_data_dirs(); dir && *dir; ++dir )
        roots.push_back( std::string( *dir ) + "/themes" );

    for( const std::string& root : roots )
    {
        std::string file = root + "/" + aTheme + "/gtk-3.0/gtk-dark.css";

        if( g_file_test( file.c_str(), G_FILE_TEST_EXISTS ) )
            return true;
    }

    return false;
}


static std::function<void( double, double )> s_loopTimer;
static GPollFunc                             s_originalPoll = nullptr;
static gint64                                s_dispatchStart = 0;

/// The poll that ends one iteration and starts the next: the time since the last poll returned
/// is how long that iteration's dispatching took.
static gint loopTimedPoll( GPollFD* aFds, guint aCount, gint aTimeout )
{
    gint64 now = g_get_real_time();

    if( s_dispatchStart && s_loopTimer )
        s_loopTimer( s_dispatchStart / 1000.0, ( now - s_dispatchStart ) / 1000.0 );

    gint result = s_originalPoll( aFds, aCount, aTimeout );
    s_dispatchStart = g_get_real_time();
    return result;
}


void KIPLATFORM::EMBED::InstallLoopTimer( std::function<void( double, double )> aFunc )
{
    s_loopTimer = std::move( aFunc );

    if( !s_originalPoll )
    {
        s_originalPoll = g_main_context_get_poll_func( nullptr );
        g_main_context_set_poll_func( nullptr, loopTimedPoll );
    }
}


bool KIPLATFORM::EMBED::SetPreferDark( int aMode )
{
    GtkSettings* settings = gtk_settings_get_default();

    if( !settings )
        return false;

    // The toolkit's own values before the first override, restored by aMode 0.
    static int    s_originalDark = -1;
    static gchar* s_originalTheme = nullptr;

    gboolean current = FALSE;
    gchar*   theme = nullptr;
    g_object_get( settings, "gtk-application-prefer-dark-theme", &current, "gtk-theme-name", &theme,
                  nullptr );

    if( s_originalDark < 0 )
    {
        s_originalDark = current ? 1 : 0;
        s_originalTheme = g_strdup( theme ? theme : "Adwaita" );
    }

    g_free( theme );

    gboolean wanted = aMode == 2 ? TRUE : aMode == 1 ? FALSE : ( s_originalDark == 1 );

    // A theme without a dark variant (or one configured but not installed, which GTK replaces
    // with plain Adwaita) ignores prefer-dark; use Adwaita's dark variant then.
    const gchar* themeName = s_originalTheme;

    if( wanted && !themeHasDarkVariant( s_originalTheme ) )
        themeName = "Adwaita";

    g_object_set( settings, "gtk-theme-name", themeName, "gtk-application-prefer-dark-theme", wanted,
                  nullptr );

    return wanted == TRUE;
}


bool KIPLATFORM::EMBED::NativeModalTitle( wxString& aTitle )
{
    GList* toplevels = gtk_window_list_toplevels();
    bool   found = false;

    for( GList* item = toplevels; item; item = item->next )
    {
        GtkWindow* window = GTK_WINDOW( item->data );

        if( gtk_window_get_modal( window ) && gtk_widget_get_visible( GTK_WIDGET( window ) ) )
        {
            const char* title = gtk_window_get_title( window );
            aTitle = wxString::FromUTF8( title ? title : "" );
            found = true;
            break;
        }
    }

    g_list_free( toplevels );
    return found;
}


void KIPLATFORM::EMBED::MakeTransientFor( wxWindow* aDialog, uint64_t aToplevel )
{
#ifdef GDK_WINDOWING_X11
    Display* display = xDisplay();

    if( !display || !aDialog || !aToplevel )
        return;

    GtkWidget* widget = static_cast<GtkWidget*>( aDialog->GetHandle() );
    Window     xid = xidOf( widget );

    if( !xid )
        return;

    trapped( [&]() { XSetTransientForHint( display, xid, (Window) aToplevel ); } );

    if( GTK_IS_WINDOW( widget ) )
        centreOn( GTK_WINDOW( widget ), (Window) aToplevel );
#endif
}


void KIPLATFORM::EMBED::TakeFocusForClick( wxWindow* aFrame, wxWindow* )
{
    TakeFocus( aFrame );
}


bool KIPLATFORM::EMBED::ReleaseStrayCapture( wxWindow* )
{
    return false;
}


void KIPLATFORM::EMBED::NoteMainThread()
{
}


std::string KIPLATFORM::EMBED::MainThreadStack()
{
    return {};
}


std::string KIPLATFORM::EMBED::InputState()
{
    return {};
}


std::string KIPLATFORM::EMBED::WriteMiniDump( const std::string& )
{
    return {};
}
