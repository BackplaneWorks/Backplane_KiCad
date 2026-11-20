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

// Embedding on macOS (BACKPLANE_EMBED.md, "macOS").  A window of another process cannot be
// parented into ours, so nothing is reparented: every embedded frame stays its own NSWindow,
// borderless, placed in screen coordinates over the host's container and ordered just above the
// host's window.  The process is an accessory application (no Dock icon, no Cmd-Tab entry, no
// menu bar of its own).  The host tells us where its container is (SetHostRect, the
// SetHostGeometry command) whenever its window moves or resizes, and whether its window can be
// seen at all; that is an overlay on top of each frame's own mapped state, which IsMapped keeps
// reporting.  Window ids are CGWindowIDs (NSWindow.windowNumber).

#include <kiplatform/embed.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <objc/runtime.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>

#include <wx/app.h>
#include <wx/event.h>
#include <wx/nonownedwnd.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>
#include <wx/window.h>

namespace
{
KIPLATFORM::EMBED::HOOKS s_hooks;
bool                     s_installed = false;

/// The host's window (a CGWindowID); 0 while parked.
uint64_t s_parent = 0;

/// The host's container in screen points (top-left origin of the primary display, as wx and
/// CGWindowList count), and whether the host's window can be seen at all.
bool   s_hostKnown = false;
double s_hostX = 0, s_hostY = 0, s_hostW = 0, s_hostH = 0;
bool   s_hostVisible = true;

/// Per embedded frame: where it goes in the container, and its own mapped state (what IsMapped
/// reports; the host's visibility is an overlay on it).
struct FRAME
{
    int  x = 0, y = 0, w = 0, h = 0;
    bool mapped = false;   ///< ordered in as far as the frame itself is concerned
    bool parked = false;   ///< laid out for a prewarm: alpha 0, behind everything, never key
};

std::map<wxWindow*, FRAME>     s_frames;
std::map<NSWindow*, wxWindow*> s_byWindow;  ///< the same frames by their NSWindow (built once, at PrepareFrame)
std::set<NSInteger>            s_numbers;   ///< windowNumbers of embedded frames

dispatch_source_t s_ownerWatch = nullptr;

/// #1198: a click's activation of KiCad still on its way.  [NSApp activate] is asynchronous, and
/// makeKeyWindow on a window of an app that is not yet active does nothing: KiCad became active with
/// no key window and the person's keys went nowhere until a second click.  The frame to make key and
/// what was clicked, done when the app becomes active (only within KEY_PENDING_S of the click; a later
/// click, or another app becoming active first, drops it), so no later activation takes the key.
struct PENDING_KEY
{
    NSInteger           window = 0;
    wxWeakRef<wxWindow> clicked;
    double              at = 0;
};

PENDING_KEY            s_pendingKey;
constexpr double       KEY_PENDING_S = 1.0;
/// Which key request is current: a click and the app resigning active bump it, so an older request's
/// retries (applyKey) stop
unsigned               s_keyGen = 0;
/// AppKit can ignore makeKeyWindow on a frame ordered in a moment before (Edit's first focused show:
/// still not key 250 ms after, while the same call 4 s later took): asked again this often, this many times
constexpr int          KEY_RETRY_MS = 50;
constexpr int          KEY_RETRIES = 10;
/// Menus of ours tracking (NSMenuDidBeginTracking +1, ..EndTracking -1, never below 0; back to 0 when
/// KiCad resigns active, which no menu outlives): a context menu the click opened.  A count, so a nested
/// submenu's end does not end the outer menu's wait, and a missed end cannot hold key requests for good
int                    s_menuDepth = 0;

double nowS()
{
    return [NSDate date].timeIntervalSince1970;
}


/// On show as far as the frame and wx are concerned: mapped by us, not parked for a prewarm, and shown
/// to wx (a HideFrame hides it through wx's Show(false), which does not come through UnmapNative: a
/// frame hidden that way must never be ordered in again by a host rect, a raise or a re-attach)
bool onShow( wxWindow* aFrame, const FRAME& aState )
{
    return aState.mapped && !aState.parked && aFrame && aFrame->IsShown();
}
pid_t             s_owner = 0;


NSWindow* nsWindowOf( wxWindow* aWindow )
{
    if( !aWindow )
        return nil;

    if( wxNonOwnedWindow* top = dynamic_cast<wxNonOwnedWindow*>( aWindow ) )
    {
        if( NSWindow* w = top->GetWXWindow() )
            return w;
    }

    NSView* view = (NSView*) aWindow->GetHandle();
    return view ? [view window] : nil;
}


FRAME* frameOf( NSWindow* aWindow )
{
    // Never through the wxWindow pointers: a frame may be gone (wxEVT_DESTROY drops it here)
    auto it = s_byWindow.find( aWindow );

    if( it == s_byWindow.end() )
        return nullptr;

    auto f = s_frames.find( it->second );
    return f == s_frames.end() ? nullptr : &f->second;
}


void trace( const std::string& aLine )
{
    KIPLATFORM::EMBED::TraceLine( aLine );
}


/// #1198 diagnosis (trace only): the app's activation and window number aNumber's key state, and
/// which window is key and who holds wx's focus
std::string keyState( NSInteger aNumber )
{
    NSWindow* w = aNumber ? [NSApp windowWithWindowNumber:aNumber] : nil;
    NSWindow* key = NSApp.keyWindow;
    wxWindow* focus = wxWindow::FindFocus();
    NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;

    return std::string( "NSApp.active=" ) + ( NSApp.active ? "1" : "0" )
           + " frame.isKeyWindow=" + ( w && w.keyWindow ? "1" : "0" )
           + " NSApp.keyWindow=" + std::to_string( key ? (long) key.windowNumber : 0 )
           + " frontmost=" + ( front ? std::to_string( front.processIdentifier ) : std::string( "?" ) )
           + " (us " + std::to_string( getpid() ) + ")"
           + " wxFocus=" + ( focus ? wxString( focus->GetClassInfo()->GetClassName() ) : wxString( "none" ) ).ToStdString();
}


/// Order a frame in just above the host's window (or out, when the host cannot be seen).
void orderIn( wxWindow* aFrame, FRAME& aState )
{
    NSWindow* w = nsWindowOf( aFrame );

    if( !w )
        return;

    if( aState.parked )
    {
        // A prewarm: on screen to the window server (so it lays out and paints) but invisible,
        // behind everything and out of the mouse's way
        w.alphaValue = 0.0;
        w.ignoresMouseEvents = YES;
        [w orderBack:nil];
        return;
    }

    w.alphaValue = 1.0;
    w.ignoresMouseEvents = NO;

    // Never on screen before the host has said where its container is: without it the frame would be
    // shown at wx's own geometry, a window over the host's whole width (first seen with the real client,
    // whose first SetHostGeometry came while KiCad was still starting)
    if( !s_hostVisible || !s_parent || !s_hostKnown )
    {
        [w orderOut:nil];
        return;
    }

    [w orderWindow:NSWindowAbove relativeTo:(NSInteger) s_parent];
}


/// Move a frame to its place: it fills the host's container, as an X11 or Win32 frame fills the
/// container window the host sizes to its pane. ShowFrame's own rect is the host's notion of its
/// container (the client's whole window on macOS), so the host's rect is the one that counts here.
void place( wxWindow* aFrame, FRAME& aState )
{
    if( aState.parked || !s_hostKnown || s_hostW <= 0 || s_hostH <= 0 )
        return;

    // wx counts screen coordinates from the primary display's top-left, as CGWindowList does
    aFrame->SetSize( (int) s_hostX, (int) s_hostY, (int) s_hostW, (int) s_hostH );
}


// ---- Interposed NSWindow / NSView methods ------------------------------------------------------
// Class-level, on wx's window classes, so a frame still being built (not yet prepared) is
// covered: a draw panel's constructor or wx itself may try to make it key.

std::map<Class, IMP> s_canBecomeKey;
std::map<Class, IMP> s_canBecomeMain;
std::map<Class, IMP> s_orderFront;
std::map<Class, IMP> s_makeKeyAndOrderFront;
std::map<Class, IMP> s_acceptsFirstMouse;


IMP original( std::map<Class, IMP>& aMap, id aSelf )
{
    for( Class c = object_getClass( aSelf ); c; c = class_getSuperclass( c ) )
    {
        auto it = aMap.find( c );

        if( it != aMap.end() )
            return it->second;
    }

    return nullptr;
}


/// An embedded frame, registered or still being built for a prewarm.  Every interposed method
/// passes any other window (dialogs, file panels, popups, tooltips, menus) through untouched.
bool isOurs( NSWindow* aWindow )
{
    uint64_t number = (uint64_t) aWindow.windowNumber;
    return s_numbers.count( aWindow.windowNumber )
           || ( s_hooks.buildingForPrewarm && s_hooks.buildingForPrewarm( number ) );
}


/// Whether this window may take the keyboard or activation now (traced when it may not).
/// Always true for a window that is not ours.
bool keyAllowed( NSWindow* aWindow, const char* aWhat )
{
    std::string why;
    uint64_t    number = (uint64_t) aWindow.windowNumber;

    if( !isOurs( aWindow ) )
        return true;

    if( s_hooks.buildingForPrewarm && s_hooks.buildingForPrewarm( number ) )
        why = "built for a prewarm";
    else if( s_hooks.focusBlocked && s_hooks.focusBlocked( number, why ) )
        ;
    else if( FRAME* f = frameOf( aWindow ); f && f->parked )
        why = "prewarm";
    else
        return true;

    trace( std::string( "macos: refused " ) + aWhat + " for window " + std::to_string( number ) + ": " + why );
    return false;
}


/// #1198: make the frame of a key request key once KiCad is active, and give what was clicked wx's
/// focus back; asked again every KEY_RETRY_MS while AppKit has not made it key, up to KEY_RETRIES
/// times, for as long as the request is current, the frame on show and no other window of ours key
void applyKey( NSInteger aNumber, wxWeakRef<wxWindow> aClicked, unsigned aGen, int aAttempt, bool aWaited = false )
{
    std::string number = std::to_string( (long) aNumber );

    if( aGen != s_keyGen )
        return;   // a newer click, or KiCad resigned active

    // The click that asked for it still going on (its button held: a click, a drag of any length) or a
    // menu it opened still tracking: nothing moves until it ends, so the click acts as the click it was.
    // A key change between its down and its up cost the draw panel its focus, which reset the tool
    // dispatcher's buttons: the up had no down (bp.13.3 b2: no context menu).  No limit while it lasts;
    // the retries below count only once it has ended.  A newer click or KiCad resigning active still
    // cancels it (aGen); a dialog the click opened is key when it ends, and keeps the key (below).
    if( KIPLATFORM::EMBED::KeyWaitsForGesture( [NSEvent pressedMouseButtons], s_menuDepth > 0 ) )
    {
        if( !aWaited )
            trace( "macos: key window for frame " + number + " waits for the click to end"
                   + ( s_menuDepth > 0 ? std::string( " (a menu is open)" ) : std::string() ) );

        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, (int64_t) KEY_RETRY_MS * NSEC_PER_MSEC ), dispatch_get_main_queue(), ^{
            applyKey( aNumber, aClicked, aGen, aAttempt, true );
        } );
        return;
    }

    NSWindow* w = [NSApp windowWithWindowNumber:aNumber];
    auto      byWindow = w ? s_byWindow.find( w ) : s_byWindow.end();
    wxWindow* frame = byWindow != s_byWindow.end() ? byWindow->second : nullptr;
    auto      state = frame ? s_frames.find( frame ) : s_frames.end();

    // gone, hidden, parked or refused the keyboard since the click: nothing made key
    if( !NSApp.active || state == s_frames.end() || !onShow( frame, state->second ) || !keyAllowed( w, "key on activation" ) )
    {
        trace( "macos: key window for frame " + number + " dropped: the frame is not on show (or KiCad not active)" );
        return;
    }

    // another window of ours took the key meanwhile (a dialog): it keeps it
    if( NSApp.keyWindow && NSApp.keyWindow != w )
    {
        trace( "macos: key window for frame " + number + " left: window " + std::to_string( (long) NSApp.keyWindow.windowNumber ) + " is key" );
        return;
    }

    if( !w.keyWindow )
        [w makeKeyWindow];

    if( !w.keyWindow )
    {
        if( aAttempt >= KEY_RETRIES )
        {
            trace( "macos: key window for frame " + number + " not taken after " + std::to_string( aAttempt + 1 ) + " tries: " + keyState( aNumber ) );
            return;
        }

        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, (int64_t) KEY_RETRY_MS * NSEC_PER_MSEC ), dispatch_get_main_queue(), ^{
            applyKey( aNumber, aClicked, aGen, aAttempt + 1, aWaited );
        } );
        return;
    }

    // and the keyboard back to what was clicked (makeKeyWindow restores the frame's last focus), unless
    // it is already there or inside it (KIPLATFORM::EMBED::FocusStays)
    if( wxWindow* clicked = aClicked.get(); clicked && clicked->AcceptsFocus()
                                            && !KIPLATFORM::EMBED::FocusStays( wxWindow::FindFocus(), clicked ) )
    {
        for( wxWindow* p = clicked; p; p = p->GetParent() )
        {
            if( p == frame )
            {
                clicked->SetFocus();
                break;
            }
        }
    }

    trace( "macos: key window set on activation (try " + std::to_string( aAttempt + 1 )
           + ( aWaited ? ", after the click ended" : "" ) + "): " + keyState( aNumber ) );
}


BOOL bpCanBecomeKey( id self, SEL _cmd )
{
    NSWindow* w = (NSWindow*) self;

    if( !keyAllowed( w, "key" ) )
        return NO;

    // A borderless window cannot become key unless its class says so; an embedded frame must
    if( s_numbers.count( w.windowNumber ) )
        return YES;

    IMP imp = original( _cmd == @selector( canBecomeMainWindow ) ? s_canBecomeMain : s_canBecomeKey, self );
    return imp ? ( (BOOL( * )( id, SEL )) imp )( self, _cmd ) : YES;
}


void bpOrderFront( id self, SEL _cmd, id aSender )
{
    NSWindow* w = (NSWindow*) self;

    if( auto it = s_byWindow.find( w ); it != s_byWindow.end() )
    {
        // wx's Show(): an embedded frame goes just above the host, never to the front of
        // everything (KiCad never orders its own frames while embedded)
        FRAME& state = s_frames[it->second];
        state.mapped = true;
        orderIn( it->second, state );
        return;
    }

    IMP imp = original( s_orderFront, self );

    if( imp )
        ( (void( * )( id, SEL, id )) imp )( self, _cmd, aSender );
}


void bpMakeKeyAndOrderFront( id self, SEL _cmd, id aSender )
{
    NSWindow* w = (NSWindow*) self;

    if( auto it = s_byWindow.find( w ); it != s_byWindow.end() )
    {
        FRAME& state = s_frames[it->second];
        state.mapped = true;
        orderIn( it->second, state );

        if( keyAllowed( w, "makeKeyAndOrderFront" ) && !state.parked && s_hostVisible )
            [w makeKeyWindow];

        return;
    }

    // Not registered yet but being built for a prewarm: ordered, never made key.  Anything that
    // is not ours falls through to its own behaviour (keyAllowed is true for it).
    if( !keyAllowed( w, "makeKeyAndOrderFront" ) )
    {
        IMP imp = original( s_orderFront, self );

        if( imp )
            ( (void( * )( id, SEL, id )) imp )( self, @selector( orderFront: ), aSender );

        return;
    }

    IMP imp = original( s_makeKeyAndOrderFront, self );

    if( imp )
        ( (void( * )( id, SEL, id )) imp )( self, _cmd, aSender );
}


BOOL bpAcceptsFirstMouse( id self, SEL _cmd, NSEvent* aEvent )
{
    // A click on a frame of an inactive accessory app would otherwise only activate it: the
    // person's first click on the board must act
    if( NSWindow* w = [(NSView*) self window]; w && s_numbers.count( w.windowNumber ) )
        return YES;

    IMP imp = original( s_acceptsFirstMouse, self );
    return imp ? ( (BOOL( * )( id, SEL, NSEvent* )) imp )( self, _cmd, aEvent ) : NO;
}


void interpose( Class aClass, SEL aSel, IMP aImp, std::map<Class, IMP>& aMap )
{
    if( !aClass || aMap.count( aClass ) )
        return;

    Method m = class_getInstanceMethod( aClass, aSel );

    // Already ours through a superclass (a runtime subclass made later, such as KVO's): nothing
    // to add, and recording our own function as the original would recurse
    if( !m || method_getImplementation( m ) == aImp )
        return;

    // Only the class's own implementation: an inherited one is interposed on its owner
    bool own = false;
    unsigned int count = 0;

    if( Method* list = class_copyMethodList( aClass, &count ) )
    {
        for( unsigned int ii = 0; ii < count; ++ii )
            own = own || method_getName( list[ii] ) == aSel;

        free( list );
    }

    if( own )
    {
        aMap[aClass] = method_setImplementation( m, aImp );
    }
    else
    {
        // Add an override to this class that falls back to the inherited implementation
        aMap[aClass] = method_getImplementation( m );
        class_addMethod( aClass, aSel, aImp, method_getTypeEncoding( m ) );
    }
}


void interposeWindowClass( Class aClass )
{
    interpose( aClass, @selector( canBecomeKeyWindow ), (IMP) bpCanBecomeKey, s_canBecomeKey );
    interpose( aClass, @selector( canBecomeMainWindow ), (IMP) bpCanBecomeKey, s_canBecomeMain );
    interpose( aClass, @selector( orderFront: ), (IMP) bpOrderFront, s_orderFront );
    interpose( aClass, @selector( makeKeyAndOrderFront: ), (IMP) bpMakeKeyAndOrderFront,
               s_makeKeyAndOrderFront );
}


void interposeViews( NSView* aView )
{
    if( !aView )
        return;

    interpose( object_getClass( aView ), @selector( acceptsFirstMouse: ), (IMP) bpAcceptsFirstMouse,
               s_acceptsFirstMouse );

    for( NSView* sub in aView.subviews )
        interposeViews( sub );
}


void orderOutAll();


std::set<NSInteger> s_dialogs;   ///< KiCad's dialogs reported open (hooks.dialog)


/// A dialog of KiCad's (any titled wx top-level that is not an embedded frame) centred on the
/// host's container (the pane, SetHostRect) or, before the host has sent it, on the host's window
/// (its bounds from the window list, no permission); ordered above the frontmost of KiCad's
/// windows over the host.
void centreOnHost( NSWindow* aWindow )
{
    if( !s_parent )
        return;

    // Top-left origin of the primary display, as the window list and SetHostRect count
    CGRect host = CGRectNull;

    if( s_hostKnown )
    {
        host = CGRectMake( s_hostX, s_hostY, s_hostW, s_hostH );
    }
    else if( CFArrayRef list = CGWindowListCopyWindowInfo( kCGWindowListOptionIncludingWindow, (CGWindowID) s_parent ) )
    {
        if( CFArrayGetCount( list ) )
        {
            NSDictionary* info = (NSDictionary*) CFArrayGetValueAtIndex( list, 0 );

            if( !CGRectMakeWithDictionaryRepresentation( (CFDictionaryRef)[info objectForKey:(NSString*) kCGWindowBounds], &host ) )
                host = CGRectNull;
        }

        CFRelease( list );
    }

    if( !CGRectIsNull( host ) )
    {
        // Cocoa counts from the primary display's bottom-left
        double H = NSScreen.screens[0].frame.size.height;
        NSSize size = aWindow.frame.size;
        [aWindow setFrameOrigin:NSMakePoint( host.origin.x + ( host.size.width - size.width ) / 2,
                                             H - ( host.origin.y + ( host.size.height + size.height ) / 2 ) )];
    }

    // Above the frontmost of KiCad's own windows over the host (the frame on show, a dialog already open), never just above the
    // host: there it went between the host and the frame, under the frame, and the host's re-stacking (its window kept just below
    // the frame) then put it under the host too: Find open, unseen, its Escape lost (lane M's client run 18)
    NSInteger above = (NSInteger) s_parent;
    std::set<NSInteger> mine( s_dialogs );

    for( auto& [frame, state] : s_frames )
    {
        if( NSWindow* f = onShow( frame, state ) ? nsWindowOf( frame ) : nil )
            mine.insert( f.windowNumber );
    }

    mine.erase( aWindow.windowNumber );

    if( CFArrayRef order = CGWindowListCopyWindowInfo( kCGWindowListOptionOnScreenAboveWindow, (CGWindowID) s_parent ) )
    {
        // front to back: the first of ours is the frontmost
        for( CFIndex i = 0; i < CFArrayGetCount( order ); i++ )
        {
            NSDictionary* info = (NSDictionary*) CFArrayGetValueAtIndex( order, i );
            NSInteger     n = [[info objectForKey:(NSString*) kCGWindowNumber] integerValue];

            if( mine.count( n ) )
            {
                above = n;
                break;
            }
        }

        CFRelease( order );
    }

    [aWindow orderWindow:NSWindowAbove relativeTo:above];
}




/// What X11's map and unmap of a top-level are on GTK: a dialog shown or gone, reported to the
/// embedding state (ET_DIALOG_OPENED / CLOSED, AS_BUSY) and centred on the host when it appears.
void dialogChanged( NSWindow* aWindow, bool aClosing )
{
    if( !aWindow || s_numbers.count( aWindow.windowNumber ) )
        return;

    uint64_t number = (uint64_t) aWindow.windowNumber;

    if( s_hooks.isUnmanaged && s_hooks.isUnmanaged( number ) )
        return;

    // wx's own top-levels with a title bar: not menus, tooltips, popups or AppKit's panels; and
    // the window of a modal session AppKit runs for KiCad (NSAlert: the native message boxes, such
    // as "KiCad found unsaved changes..." at a project's open, before the API answers)
    Class wxWin = NSClassFromString( @"wxNSWindow" );
    Class wxPanel = NSClassFromString( @"wxNSPanel" );
    bool  modal = aWindow == NSApp.modalWindow;
    bool  known = s_dialogs.count( aWindow.windowNumber ) > 0;   // reported shown: its closing too

    if( !modal && !known && !( ( wxWin && [aWindow isKindOfClass:wxWin] ) || ( wxPanel && [aWindow isKindOfClass:wxPanel] ) ) )
        return;

    if( !modal && !known && !( aWindow.styleMask & NSWindowStyleMaskTitled ) )
        return;

    bool shown = aWindow.visible && !aClosing;

    if( shown == ( s_dialogs.count( aWindow.windowNumber ) > 0 ) )
        return;

    if( shown )
    {
        s_dialogs.insert( aWindow.windowNumber );
        centreOnHost( aWindow );
    }
    else
    {
        s_dialogs.erase( aWindow.windowNumber );
    }

    trace( std::string( "macos: dialog " ) + ( shown ? "shown " : "gone " ) + std::to_string( number ) );

    if( s_hooks.dialog )
        s_hooks.dialog( number, shown, wxString::FromUTF8( aWindow.title.UTF8String ? aWindow.title.UTF8String : "" ) );
}

/// Hide every frame the moment the process that owns the host's window ends (the client, which
/// is not --parent-pid: the hub starts KiCad).  Its pid comes from the window list (no
/// permission needed); the shared parent-pid poll still ends KiCad with the hub.
void watchOwnerOf( uint64_t aParent )
{
    pid_t owner = 0;

    if( aParent )
    {
        CFArrayRef list = CGWindowListCopyWindowInfo( kCGWindowListOptionIncludingWindow,
                                                      (CGWindowID) aParent );

        if( list && CFArrayGetCount( list ) )
        {
            NSDictionary* info = (NSDictionary*) CFArrayGetValueAtIndex( list, 0 );
            owner = (pid_t)[[info objectForKey:(NSString*) kCGWindowOwnerPID] intValue];
        }

        if( list )
            CFRelease( list );
    }

    if( owner == s_owner )
        return;

    if( s_ownerWatch )
    {
        dispatch_source_cancel( s_ownerWatch );
        dispatch_release( s_ownerWatch );
        s_ownerWatch = nullptr;
    }

    s_owner = owner;

    if( owner <= 0 )
        return;

    s_ownerWatch = dispatch_source_create( DISPATCH_SOURCE_TYPE_PROC, owner, DISPATCH_PROC_EXIT,
                                           dispatch_get_main_queue() );
    dispatch_source_set_event_handler( s_ownerWatch,
                                       ^{
                                           trace( "macos: the host's process ended; frames hidden" );
                                           s_parent = 0;
                                           orderOutAll();
                                       } );
    dispatch_resume( s_ownerWatch );
}


void orderOutAll()
{
    for( auto& [frame, state] : s_frames )
    {
        if( NSWindow* w = nsWindowOf( frame ) )
            [w orderOut:nil];
    }
}
} // namespace


void KIPLATFORM::EMBED::TraceLine( const std::string& aLine )
{
    static std::mutex mutex;
    const char*       path = getenv( "BACKPLANE_KICAD_EMBED_TRACE" );

    if( !path || !*path )
        return;

    std::lock_guard<std::mutex> lock( mutex );

    if( FILE* f = fopen( path, "a" ) )
    {
        struct timespec ts;
        clock_gettime( CLOCK_REALTIME, &ts );
        fprintf( f, "%lld.%03ld %s\n", (long long) ts.tv_sec, ts.tv_nsec / 1000000, aLine.c_str() );
        fclose( f );
    }
}


void KIPLATFORM::EMBED::NoteMainThread() {}
std::string KIPLATFORM::EMBED::MainThreadStack() { return {}; }
std::string KIPLATFORM::EMBED::InputState() { return {}; }
std::string KIPLATFORM::EMBED::WriteMiniDump( const std::string& ) { return {}; }


bool KIPLATFORM::EMBED::Supported()
{
    return true;
}


bool KIPLATFORM::EMBED::FrameIsKey( wxWindow* aFrame )
{
    NSWindow* w = nsWindowOf( aFrame );
    return w && NSApp.active && w.keyWindow;
}


bool KIPLATFORM::EMBED::WindowsGone()
{
    return false;
}


bool KIPLATFORM::EMBED::CheckWindowsGone()
{
    return false;
}


void KIPLATFORM::EMBED::Install( const HOOKS& aHooks )
{
    s_hooks = aHooks;

    if( s_installed )
        return;

    s_installed = true;

    // No Dock icon, no Cmd-Tab entry, no menu bar: the host's window is the application.  The
    // person who wants KiCad's full menus pops it out (a separate, ordinary KiCad).
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    for( NSString* name in @[ @"wxNSWindow", @"wxNSPanel" ] )
    {
        if( Class c = NSClassFromString( name ) )
            interposeWindowClass( c );
    }

    interposeWindowClass( [NSWindow class] );

    // Dialogs: a window ordered in or out changes its occlusion state; one closed says so first
    NSNotificationCenter* centre = [NSNotificationCenter defaultCenter];

    // AppKit keeps the physical Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+` for its own keyboard navigation:
    // wx never sees them, so the host's reserved chords on those keys (RawCtrl+Tab...) never matched.
    // Seen first here, in an embedded frame they become the key event wx would have made, which the
    // shared FilterEvent matches as on every other platform; anything it does not take goes on as it was.
    //
    // Backspace (the key marked delete, 51) is handed to wx here too, before the main menu: AppKit
    // matches it against menu key equivalents first (Edit > Delete's Del is that key), and an
    // embedded frame's menus are not the ones the person sees.  The shared FilterEvent makes it
    // Delete as stock KiCad on a Mac does; a text field keeps it (AppKit's field editor).
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                          handler:^NSEvent*( NSEvent* aEvent ) {
                                              NSEventModifierFlags m = aEvent.modifierFlags;
                                              unsigned short       code = aEvent.keyCode;   // 48 Tab, 50 `, 51 Backspace
                                              const NSEventModifierFlags chordMods =
                                                      NSEventModifierFlagControl | NSEventModifierFlagOption
                                                      | NSEventModifierFlagCommand | NSEventModifierFlagShift;

                                              if( code == 51 && !( m & chordMods ) )
                                              {
                                                  NSWindow* key = NSApp.keyWindow;

                                                  if( !key || !s_numbers.count( key.windowNumber )
                                                      || [key.firstResponder isKindOfClass:[NSText class]] )
                                                  {
                                                      return aEvent;
                                                  }

                                                  wxWindow* focus = wxWindow::FindFocus();

                                                  if( !focus )
                                                      return aEvent;

                                                  wxKeyEvent ke( wxEVT_CHAR_HOOK );
                                                  ke.m_keyCode = WXK_BACK;
                                                  ke.m_uniChar = WXK_BACK;
                                                  ke.SetEventObject( focus );
                                                  ke.SetId( focus->GetId() );

                                                  bool taken = focus->GetEventHandler()->ProcessEvent( ke );
                                                  trace( std::string( "macos: Backspace handed to wx: " )
                                                         + ( taken ? "taken" : "not taken" ) );
                                                  return taken ? nil : aEvent;
                                              }

                                              // Cmd chords (Undo, Redo, Copy, Find...): AppKit offers them to the menu
                                              // bar's key equivalents before the window, and the menu bar installed in an
                                              // accessory app is not the focused editor's.  Handed to wx first, as the
                                              // window would get them: the canvas's hotkeys, then the frame's own menu bar
                                              // (EDA_BASE_FRAME::OnCharHook), then the embed layer's reserved chords
                                              // (FilterEvent, first of all).  A text field keeps them (AppKit's field
                                              // editor); a chord nobody takes goes on to AppKit as before.
                                              if( ( m & NSEventModifierFlagCommand ) && code != 48 && code != 50 )
                                              {
                                                  NSWindow* key = NSApp.keyWindow;
                                                  NSString* chars = aEvent.charactersIgnoringModifiers;

                                                  if( !key || !s_numbers.count( key.windowNumber )
                                                      || [key.firstResponder isKindOfClass:[NSText class]]
                                                      || chars.length != 1 )
                                                  {
                                                      return aEvent;
                                                  }

                                                  unichar ch = [chars characterAtIndex:0];

                                                  // Printable ASCII only: letters, digits, punctuation (zoom's = and -)
                                                  if( ch <= ' ' || ch >= 0x7F )
                                                      return aEvent;

                                                  wxWindow* focus = wxWindow::FindFocus();

                                                  if( !focus )
                                                      return aEvent;

                                                  wxKeyEvent ke( wxEVT_CHAR_HOOK );
                                                  // Cmd+= zooms in as on every Mac app: KiCad binds Ctrl++
                                                  // (Shift+= on a US layout), so plain Cmd+= is handed on as +
                                                  if( ch == '=' && !( m & NSEventModifierFlagShift ) )
                                                      ch = '+';

                                                  ke.m_keyCode = std::toupper( (int) ch );
                                                  ke.m_uniChar = ch;
                                                  ke.m_controlDown = true;   // Cmd: wx's Ctrl on macOS
                                                  ke.m_rawControlDown = ( m & NSEventModifierFlagControl ) != 0;
                                                  ke.m_shiftDown = ( m & NSEventModifierFlagShift ) != 0;
                                                  ke.m_altDown = ( m & NSEventModifierFlagOption ) != 0;
                                                  ke.SetEventObject( focus );
                                                  ke.SetId( focus->GetId() );

                                                  bool taken = focus->GetEventHandler()->ProcessEvent( ke );
                                                  trace( std::string( "macos: Cmd+" ) + (char) ke.m_keyCode
                                                         + ( ke.m_shiftDown ? " (Shift)" : "" ) + " handed to wx ("
                                                         + wxString( focus->GetClassInfo()->GetClassName() ).ToStdString()
                                                         + "): " + ( taken ? "taken" : "not taken" ) );
                                                  return taken ? nil : aEvent;
                                              }

                                              if( !( m & NSEventModifierFlagControl ) || ( code != 48 && code != 50 ) )
                                                  return aEvent;

                                              NSWindow* key = NSApp.keyWindow;

                                              if( !key || !s_numbers.count( key.windowNumber ) )
                                                  return aEvent;

                                              wxWindow* focus = wxWindow::FindFocus();

                                              if( !focus )
                                                  return aEvent;

                                              wxKeyEvent ke( wxEVT_CHAR_HOOK );
                                              ke.m_keyCode = code == 48 ? WXK_TAB : '`';
                                              ke.m_rawControlDown = true;
                                              ke.m_shiftDown = ( m & NSEventModifierFlagShift ) != 0;
                                              ke.m_altDown = ( m & NSEventModifierFlagOption ) != 0;
                                              ke.m_controlDown = ( m & NSEventModifierFlagCommand ) != 0;
                                              ke.SetEventObject( focus );
                                              ke.SetId( focus->GetId() );

                                              trace( std::string( "macos: raw Ctrl+" ) + ( code == 48 ? "Tab" : "`" ) + " handed to wx" );
                                              return focus->GetEventHandler()->ProcessEvent( ke ) ? nil : aEvent;
                                          }];

    // macOS 14+ activation is cooperative: the active app must consent before another may activate
    // itself. Each time KiCad becomes active, it consents for the host's app, so the host can take the
    // keyboard back (a reserved chord, Escape to Backplane) without a click. Nothing is activated here.
    [centre addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                    usingBlock:^( NSNotification* ) {
                        if( s_owner <= 0 )
                            return;

                        if( @available( macOS 14.0, * ) )
                        {
                            if( NSRunningApplication* host = [NSRunningApplication runningApplicationWithProcessIdentifier:s_owner] )
                                [NSApp yieldActivationToApplication:host];
                        }
                    }];
    // #1198: the key window a click asked for while KiCad was not yet active (TakeFocusForClick)
    [centre addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                    usingBlock:^( NSNotification* ) {
                        PENDING_KEY pending = s_pendingKey;
                        s_pendingKey = PENDING_KEY();

                        if( !pending.window )
                            return;

                        std::string number = std::to_string( (long) pending.window );

                        if( nowS() - pending.at > KEY_PENDING_S )
                        {
                            trace( "macos: key window for frame " + number + " dropped: the activation came too late" );
                            return;
                        }

                        applyKey( pending.window, pending.clicked, s_keyGen, 0 );
                    }];
    // a menu of ours tracking (a context menu the click opened): a key request waits for it to close
    [centre addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
                    usingBlock:^( NSNotification* ) { s_menuDepth++; trace( "macos: menu tracking begins (depth " + std::to_string( s_menuDepth ) + ")" ); }];
    [centre addObserverForName:NSMenuDidEndTrackingNotification object:nil queue:nil
                    usingBlock:^( NSNotification* ) { s_menuDepth = std::max( 0, s_menuDepth - 1 ); trace( "macos: menu tracking ends (depth " + std::to_string( s_menuDepth ) + ")" ); }];
    // another app became active before KiCad did: the click's request is no longer the person's
    [centre addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                    usingBlock:^( NSNotification* ) { s_pendingKey = PENDING_KEY(); s_keyGen++; s_menuDepth = 0; }];
    // #1198 diagnosis (trace only): when KiCad becomes and stops being the active app
    [centre addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                    usingBlock:^( NSNotification* ) { trace( "macos: app did become active: " + keyState( 0 ) ); }];
    [centre addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                    usingBlock:^( NSNotification* ) { trace( "macos: app did resign active: " + keyState( 0 ) ); }];
    [centre addObserverForName:NSWindowDidChangeOcclusionStateNotification object:nil queue:nil
                    usingBlock:^( NSNotification* aNote ) { dialogChanged( (NSWindow*) aNote.object, false ); }];
    [centre addObserverForName:NSWindowWillCloseNotification object:nil queue:nil
                    usingBlock:^( NSNotification* aNote ) { dialogChanged( (NSWindow*) aNote.object, true ); }];
}


uint64_t KIPLATFORM::EMBED::CreateHolder( uint64_t aParent )
{
    // No holder window: frames are placed on screen over the host's container
    s_parent = aParent;
    watchOwnerOf( aParent );
    return 1;
}


bool KIPLATFORM::EMBED::AttachHolder( uint64_t, uint64_t aParent )
{
    s_parent = aParent;
    watchOwnerOf( aParent );

    if( !aParent )
        orderOutAll();
    else
    {
        for( auto& [frame, state] : s_frames )
        {
            if( onShow( frame, state ) )
                orderIn( frame, state );
        }
    }

    return true;
}


uint64_t KIPLATFORM::EMBED::PrepareFrame( wxWindow* aFrame, uint64_t )
{
    NSWindow* w = nsWindowOf( aFrame );

    if( !w )
        return 0;

    interposeWindowClass( object_getClass( w ) );

    w.styleMask = NSWindowStyleMaskBorderless;
    w.hasShadow = NO;
    w.collectionBehavior = NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle
                           | NSWindowCollectionBehaviorFullScreenNone;
    w.releasedWhenClosed = NO;

    if( !s_frames.count( aFrame ) )
    {
        NSInteger number = w.windowNumber;

        // A destroyed frame leaves every table here, so nothing reaches a dead wxWindow
        aFrame->Bind( wxEVT_DESTROY,
                      [aFrame, w, number]( wxWindowDestroyEvent& aEvent )
                      {
                          if( aEvent.GetEventObject() == aFrame )
                          {
                              s_frames.erase( aFrame );
                              s_byWindow.erase( w );
                              s_numbers.erase( number );
                          }

                          aEvent.Skip();
                      } );
    }

    s_frames[aFrame];
    s_byWindow[w] = aFrame;
    s_numbers.insert( w.windowNumber );
    interposeViews( w.contentView );

    if( !aFrame->IsShown() )
        [w orderOut:nil];

    return (uint64_t) w.windowNumber;
}


uint64_t KIPLATFORM::EMBED::WindowId( wxWindow* aWindow )
{
    NSWindow* w = nsWindowOf( aWindow );
    return w ? (uint64_t) w.windowNumber : 0;
}


bool KIPLATFORM::EMBED::IsMapped( wxWindow* aFrame )
{
    // The frame's own state, whatever the host's window is doing (minimised, full screen, another
    // Space): the poll derives FrameInfo.shown and ET_FRAME_SHOWN/HIDDEN from it
    auto it = s_frames.find( aFrame );

    if( it != s_frames.end() )
        return it->second.mapped && !it->second.parked;

    return aFrame && aFrame->IsShown();
}


void KIPLATFORM::EMBED::EnsureMapped( wxWindow* aFrame )
{
    auto it = s_frames.find( aFrame );

    if( it == s_frames.end() || !aFrame->IsShown() )
        return;

    it->second.parked = false;
    it->second.mapped = true;
    place( aFrame, it->second );
    orderIn( aFrame, it->second );
    [CATransaction flush];
}


void KIPLATFORM::EMBED::UnmapNative( wxWindow* aFrame )
{
    auto it = s_frames.find( aFrame );

    if( it == s_frames.end() )
    {
        if( aFrame )
            aFrame->Show( false );

        return;
    }

    it->second.mapped = false;
    it->second.parked = false;

    if( NSWindow* w = nsWindowOf( aFrame ) )
    {
        [w orderOut:nil];
        w.alphaValue = 1.0;
        w.ignoresMouseEvents = NO;
    }
}


void KIPLATFORM::EMBED::ShowInactive( wxWindow* aFrame )
{
    if( !aFrame )
        return;

    auto it = s_frames.find( aFrame );

    if( it == s_frames.end() )
    {
        aFrame->Show( true );
        return;
    }

    // wx shown state for the toolkit; the ordering is ours (never key, never activated)
    if( !aFrame->IsShown() )
        aFrame->Show( true );   // goes through bpOrderFront

    it->second.mapped = !it->second.parked;
    orderIn( aFrame, it->second );
}


void KIPLATFORM::EMBED::ParkForPrewarm( wxWindow* aFrame, int aWidth, int aHeight )
{
    auto it = s_frames.find( aFrame );

    if( it == s_frames.end() )
        return;

    it->second.parked = true;

    // Any place on the primary display will do: it is invisible and behind everything.  Off
    // every display, the window server may count it occluded and never let it paint.
    aFrame->SetSize( 0, 0, aWidth, aHeight );

    if( NSWindow* w = nsWindowOf( aFrame ) )
    {
        w.alphaValue = 0.0;
        w.ignoresMouseEvents = YES;
    }
}


uint64_t KIPLATFORM::EMBED::FocusedWindow()
{
    return 0;
}


void KIPLATFORM::EMBED::ReturnFocusFrom( wxWindow*, uint64_t ) {}


bool KIPLATFORM::EMBED::HasFocus( wxWindow* aFrame )
{
    NSWindow* w = nsWindowOf( aFrame );

    if( !w || !NSApp.active || !w.keyWindow )
        return false;

    for( wxWindow* f = wxWindow::FindFocus(); f; f = f->GetParent() )
    {
        if( f == aFrame )
            return true;
    }

    return false;
}


void KIPLATFORM::EMBED::TakeFocus( wxWindow* aFrame )
{
    TakeFocusForClick( aFrame, nullptr );
}




void KIPLATFORM::EMBED::TraceClick( wxWindow* aFrame, bool aHasFocus )
{
    NSWindow* w = nsWindowOf( aFrame );
    NSInteger number = w ? w.windowNumber : 0;
    trace( "macos: click on frame " + std::to_string( (long) number ) + ": HasFocus=" + ( aHasFocus ? "1" : "0" ) + " "
           + keyState( number ) + ( aHasFocus ? " (TakeFocusForClick not called)" : "" ) );
}


void KIPLATFORM::EMBED::TakeFocusForClick( wxWindow* aFrame, wxWindow* aClicked )
{
    NSWindow* w = nsWindowOf( aFrame );
    s_pendingKey = PENDING_KEY();   // a new click replaces any request still waiting
    s_keyGen++;
    trace( "macos: TakeFocusForClick entered for frame " + std::to_string( w ? (long) w.windowNumber : 0 ) + ": "
           + keyState( w ? w.windowNumber : 0 ) );

    if( !w || !keyAllowed( w, "TakeFocus" ) )
        return;

    // Only a frame the person can see: a ShowFrame{focus} while the host's window is minimised, in
    // full screen or parked would otherwise activate KiCad over whatever they are doing, with no
    // KiCad window on screen
    auto it = s_frames.find( aFrame );

    if( it == s_frames.end() || !onShow( aFrame, it->second ) || !s_hostVisible || !s_parent )
    {
        trace( "macos: TakeFocus skipped: the frame is not on show" );
        return;
    }

    // An accessory app may become active; the host's window then shows as inactive, as it does
    // when any other window takes the keyboard
    if( @available( macOS 14.0, * ) )
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];

    [w makeKeyWindow];

    // Not active yet (the activation is asynchronous): makeKeyWindow did nothing; done again on activation
    if( !NSApp.active )
    {
        s_pendingKey.window = w.windowNumber;
        s_pendingKey.clicked = aClicked;
        s_pendingKey.at = nowS();
        trace( "macos: key window waits for the activation: frame " + std::to_string( (long) w.windowNumber ) );
    }

    // #1198 diagnosis (trace only): activation is asynchronous; what it is now, one run-loop turn
    // later, and 250 ms later
    {
        NSInteger number = w.windowNumber;
        trace( "macos: TakeFocusForClick after activate+makeKeyWindow: " + keyState( number ) );
        dispatch_async( dispatch_get_main_queue(), ^{
            trace( "macos: TakeFocusForClick one run-loop turn later: " + keyState( number ) );
        } );
        dispatch_after( dispatch_time( DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC ), dispatch_get_main_queue(), ^{
            trace( "macos: TakeFocusForClick 250 ms later: " + keyState( number ) );
        } );
    }

    // The keyboard to what was clicked (ShowFrame{focus} passes the canvas, where KiCad's hotkeys
    // are served): makeKeyWindow alone gives back the frame's last focus, which can be a toolbar
    // field, and then F2 does nothing
    if( aClicked && aClicked->AcceptsFocus() )
    {
        for( wxWindow* p = aClicked; p; p = p->GetParent() )
        {
            if( p == aFrame )
            {
                aClicked->SetFocus();
                break;
            }
        }
    }
}


bool KIPLATFORM::EMBED::ReleaseStrayCapture( wxWindow* )
{
    return false;
}


void KIPLATFORM::EMBED::SendHostMessage( uint64_t, const char*, int, const long[5] )
{
    // macOS has no host window to post to: focus and reserved keys go out as events
    // (ET_FOCUS_IN/OUT, ET_RESERVED_KEY)
}


void KIPLATFORM::EMBED::SetFrameGeometry( wxWindow* aFrame, int aX, int aY, int aWidth, int aHeight )
{
    auto it = s_frames.find( aFrame );

    if( it == s_frames.end() )
    {
        if( aFrame )
            aFrame->SetSize( aX, aY, aWidth, aHeight );

        return;
    }

    FRAME& f = it->second;
    f.x = aX;
    f.y = aY;
    f.w = aWidth;
    f.h = aHeight;

    // A frame not on show keeps its size (layout) and stays ordered out; alpha 1 is applied when
    // EnsureMapped or ShowInactive orders it in
    if( !onShow( aFrame, f ) )
    {
        aFrame->SetSize( aWidth, aHeight );
        return;
    }

    place( aFrame, f );
    [CATransaction flush];
}


void KIPLATFORM::EMBED::SetHostRect( int aX, int aY, int aWidth, int aHeight, bool aVisible,
                                     bool aRaise )
{
    bool wasKnown = s_hostKnown;
    s_hostKnown = aWidth > 0 && aHeight > 0;
    s_hostX = aX;
    s_hostY = aY;
    s_hostW = aWidth;
    s_hostH = aHeight;

    bool visibilityChanged = aVisible != s_hostVisible;
    s_hostVisible = aVisible;

    for( auto& [frame, state] : s_frames )
    {
        if( !onShow( frame, state ) )
            continue;

        NSWindow* w = nsWindowOf( frame );

        if( !w )
            continue;

        if( !s_hostVisible )
        {
            [w orderOut:nil];
            continue;
        }

        place( frame, state );

        if( aRaise || visibilityChanged || !wasKnown || !w.visible )
            orderIn( frame, state );
    }

    // A dialog shown before the host's first rect (the recover prompt at a project's open, an
    // NSAlert run before the API answers), or while the host was hidden, was centred on the host's
    // whole window or not at all: now on the pane, and above the frames just ordered in
    if( ( !wasKnown || visibilityChanged ) && s_hostKnown && s_hostVisible )
    {
        for( NSInteger number : std::set<NSInteger>( s_dialogs ) )
        {
            if( NSWindow* dialog = [NSApp windowWithWindowNumber:number] )
            {
                if( dialog.visible )
                    centreOnHost( dialog );
            }
        }
    }

    // Commit now: this runs from a dispatched block, not a run-loop pass, and the frame change
    // otherwise waited (a resize lagged ~126 ms in the prototype)
    [CATransaction flush];
}


bool KIPLATFORM::EMBED::PostToMainModal( std::function<void()> aFunc )
{
    if( !aFunc )
        return false;

    // Only the modal panel mode ([NSApp runModalForWindow:], which [NSAlert runModal] runs): not
    // the default mode wx's own loop runs, nor event tracking (menus, live resize)
    CFRunLoopRef main = CFRunLoopGetMain();
    CFRunLoopPerformBlock( main, (CFStringRef) NSModalPanelRunLoopMode, ^{ aFunc(); } );
    CFRunLoopWakeUp( main );
    return true;
}


bool KIPLATFORM::EMBED::SetPreferDark( int aMode )
{
    static bool       saved = false;
    static NSAppearance* before = nil;

    if( !saved )
    {
        before = [NSApp.appearance retain];
        saved = true;
    }

    if( aMode == 2 )
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    else if( aMode == 1 )
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    else
        NSApp.appearance = before;

    NSAppearanceName name = [NSApp.effectiveAppearance
            bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [name isEqualToString:NSAppearanceNameDarkAqua];
}


void KIPLATFORM::EMBED::InstallLoopTimer( std::function<void( double, double )> aFunc )
{
    if( !aFunc )
        return;

    static std::function<void( double, double )> func;
    static double                                 start = 0;
    func = aFunc;

    CFRunLoopObserverRef observer = CFRunLoopObserverCreateWithHandler(
            kCFAllocatorDefault, kCFRunLoopAfterWaiting | kCFRunLoopBeforeWaiting, true, 0,
            ^( CFRunLoopObserverRef, CFRunLoopActivity aActivity ) {
                double now = [[NSDate date] timeIntervalSince1970] * 1000.0;

                if( aActivity == kCFRunLoopAfterWaiting )
                    start = now;
                else if( start > 0 )
                    func( start, now - start );
            } );

    CFRunLoopAddObserver( CFRunLoopGetMain(), observer, kCFRunLoopCommonModes );
}


bool KIPLATFORM::EMBED::NativeModalTitle( wxString& aTitle )
{
    NSWindow* modal = NSApp.modalWindow;

    if( !modal )
        return false;

    aTitle = wxString::FromUTF8( modal.title.UTF8String ? modal.title.UTF8String : "" );
    return true;
}


void KIPLATFORM::EMBED::MakeTransientFor( wxWindow* aDialog, uint64_t aToplevel )
{
    NSWindow* w = nsWindowOf( aDialog );

    if( !w || !aToplevel )
        return;

    // Centre on the host's window (its bounds need no permission) and keep it above the host
    CFArrayRef list = CGWindowListCopyWindowInfo( kCGWindowListOptionIncludingWindow,
                                                  (CGWindowID) aToplevel );

    if( list && CFArrayGetCount( list ) )
    {
        CGRect host;
        NSDictionary* info = (NSDictionary*) CFArrayGetValueAtIndex( list, 0 );

        if( CGRectMakeWithDictionaryRepresentation(
                    (CFDictionaryRef)[info objectForKey:(NSString*) kCGWindowBounds], &host ) )
        {
            wxSize size = aDialog->GetSize();
            aDialog->SetPosition( wxPoint( (int) ( host.origin.x + ( host.size.width - size.x ) / 2 ),
                                           (int) ( host.origin.y + ( host.size.height - size.y ) / 2 ) ) );
        }
    }

    if( list )
        CFRelease( list );

    [w orderWindow:NSWindowAbove relativeTo:(NSInteger) aToplevel];
}
