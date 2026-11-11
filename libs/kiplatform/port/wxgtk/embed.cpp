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
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

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
    else if( ( ev->type == ReparentNotify && ev->xreparent.window == s_holder && s_parent
               && ev->xreparent.parent != s_parent )
             || ( ev->type == DestroyNotify && s_parent && ev->xdestroywindow.window == s_parent ) )
    {
        // The host's window went away.  If the host had the holder in its save-set the server
        // moved it to the root (mapped); park it unmapped.  Frames stay alive until the next
        // AttachToParent.  Our own reparenting never gets here: s_parent is updated first.
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


void KIPLATFORM::EMBED::Install( const HOOKS& aHooks )
{
    s_hooks = aHooks;

#ifdef GDK_WINDOWING_X11
    if( s_installed || !Supported() )
        return;

    s_installed = true;
    gdk_window_add_filter( nullptr, holderFilter, nullptr );

    guint mapSignal = g_signal_lookup( "map", GTK_TYPE_WIDGET );
    guint unmapSignal = g_signal_lookup( "unmap", GTK_TYPE_WIDGET );

    g_signal_add_emission_hook( mapSignal, 0, mapHook, GINT_TO_POINTER( 1 ), nullptr );
    g_signal_add_emission_hook( unmapSignal, 0, mapHook, GINT_TO_POINTER( 0 ), nullptr );
#endif
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
