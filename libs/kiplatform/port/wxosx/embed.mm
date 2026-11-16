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

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <wx/app.h>
#include <wx/nonownedwnd.h>
#include <wx/toplevel.h>
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

    if( !s_hostVisible || !s_parent )
    {
        [w orderOut:nil];
        return;
    }

    [w orderWindow:NSWindowAbove relativeTo:(NSInteger) s_parent];
}


/// Move a frame to its place: the host's container origin plus its container-relative rect.
void place( wxWindow* aFrame, FRAME& aState )
{
    if( aState.parked || !s_hostKnown || aState.w <= 0 || aState.h <= 0 )
        return;

    // wx counts screen coordinates from the primary display's top-left, as CGWindowList does
    aFrame->SetSize( (int) ( s_hostX + aState.x ), (int) ( s_hostY + aState.y ), aState.w, aState.h );
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


/// A dialog of KiCad's (any titled wx top-level that is not an embedded frame) centred on the
/// host's window and ordered just above it; its bounds come from the window list (no permission).
void centreOnHost( NSWindow* aWindow )
{
    if( !s_parent )
        return;

    CFArrayRef list = CGWindowListCopyWindowInfo( kCGWindowListOptionIncludingWindow, (CGWindowID) s_parent );

    if( list && CFArrayGetCount( list ) )
    {
        CGRect        host;
        NSDictionary* info = (NSDictionary*) CFArrayGetValueAtIndex( list, 0 );

        if( CGRectMakeWithDictionaryRepresentation( (CFDictionaryRef)[info objectForKey:(NSString*) kCGWindowBounds], &host ) )
        {
            // window-list coordinates count from the primary display's top-left; Cocoa's from its bottom-left
            double H = NSScreen.screens[0].frame.size.height;
            NSSize size = aWindow.frame.size;
            [aWindow setFrameOrigin:NSMakePoint( host.origin.x + ( host.size.width - size.width ) / 2,
                                                 H - ( host.origin.y + ( host.size.height + size.height ) / 2 ) )];
        }
    }

    if( list )
        CFRelease( list );

    [aWindow orderWindow:NSWindowAbove relativeTo:(NSInteger) s_parent];
}


std::set<NSInteger> s_dialogs;   ///< KiCad's dialogs reported open (hooks.dialog)


/// What X11's map and unmap of a top-level are on GTK: a dialog shown or gone, reported to the
/// embedding state (ET_DIALOG_OPENED / CLOSED, AS_BUSY) and centred on the host when it appears.
void dialogChanged( NSWindow* aWindow, bool aClosing )
{
    if( !aWindow || s_numbers.count( aWindow.windowNumber ) )
        return;

    uint64_t number = (uint64_t) aWindow.windowNumber;

    if( s_hooks.isUnmanaged && s_hooks.isUnmanaged( number ) )
        return;

    // wx's own top-levels with a title bar: not menus, tooltips, popups or AppKit's panels
    Class wxWin = NSClassFromString( @"wxNSWindow" );
    Class wxPanel = NSClassFromString( @"wxNSPanel" );

    if( !( ( wxWin && [aWindow isKindOfClass:wxWin] ) || ( wxPanel && [aWindow isKindOfClass:wxPanel] ) ) )
        return;

    if( !( aWindow.styleMask & NSWindowStyleMaskTitled ) )
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
    NSWindow* w = nsWindowOf( aFrame );

    if( !w || !keyAllowed( w, "TakeFocus" ) )
        return;

    // An accessory app may become active; the host's window then shows as inactive, as it does
    // when any other window takes the keyboard
    if( @available( macOS 14.0, * ) )
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];

    [w makeKeyWindow];
}


void KIPLATFORM::EMBED::TakeFocusForClick( wxWindow* aFrame, wxWindow* )
{
    TakeFocus( aFrame );
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

        if( aRaise || visibilityChanged || !w.visible )
            orderIn( frame, state );
    }

    // Commit now: this runs from a dispatched block, not a run-loop pass, and the frame change
    // otherwise waited (a resize lagged ~126 ms in the prototype)
    [CATransaction flush];
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
