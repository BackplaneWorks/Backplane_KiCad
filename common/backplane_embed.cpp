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

#include <backplane_embed.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cctype>
#include <chrono>
#include <cstdio>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/evtloop.h>
#include <wx/frame.h>
#include <wx/log.h>
#include <wx/toplevel.h>
#include <wx/filename.h>
#include <wx/time.h>
#include <wx/weakref.h>

#if defined( __unix__ ) || defined( __APPLE__ )
#include <signal.h>
#include <cerrno>
#elif defined( _WIN32 )
#include <wx/process.h>
#endif

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>

#include <dialog_shim.h>
#include <kiplatform/embed.h>

#include <api/common/envelope.pb.h>
#include <api/backplane/embed.pb.h>
#include <backplane_embed_proto.h>


namespace
{
/// Event payloads for the host: see BACKPLANE_EMBED.md.
const char* const FOCUS_ATOM = "_BACKPLANE_FOCUS";
const char* const KEY_ATOM = "_BACKPLANE_KEY";
const int         FOCUS_WM_APP_OFFSET = 0x101;
const int         KEY_WM_APP_OFFSET = 0x102;

nng_socket  s_eventSocket = NNG_SOCKET_INITIALIZER;
std::string s_eventToken;


struct KEY_NAME
{
    const char* name;
    int         wxCode;
    long        keysym;
};

const KEY_NAME KEY_NAMES[] = {
    { "Tab", WXK_TAB, 0xff09 },           { "Esc", WXK_ESCAPE, 0xff1b },
    { "Escape", WXK_ESCAPE, 0xff1b },     { "Enter", WXK_RETURN, 0xff0d },
    { "Return", WXK_RETURN, 0xff0d },     { "Space", WXK_SPACE, 0x20 },
    { "Backspace", WXK_BACK, 0xff08 },    { "Delete", WXK_DELETE, 0xffff },
    { "Del", WXK_DELETE, 0xffff },        { "Insert", WXK_INSERT, 0xff63 },
    { "Home", WXK_HOME, 0xff50 },         { "End", WXK_END, 0xff57 },
    { "PageUp", WXK_PAGEUP, 0xff55 },     { "PageDown", WXK_PAGEDOWN, 0xff56 },
    { "Left", WXK_LEFT, 0xff51 },         { "Up", WXK_UP, 0xff52 },
    { "Right", WXK_RIGHT, 0xff53 },       { "Down", WXK_DOWN, 0xff54 },
};


long keysymFor( int aWxCode )
{
    for( const KEY_NAME& key : KEY_NAMES )
    {
        if( key.wxCode == aWxCode )
            return key.keysym;
    }

    if( aWxCode >= WXK_F1 && aWxCode <= WXK_F24 )
        return 0xffbe + ( aWxCode - WXK_F1 );

    if( aWxCode > 0x20 && aWxCode < 0x7f )
        return std::tolower( aWxCode );

    return 0;
}


long xModifiersFor( int aWxModifiers )
{
    long mask = 0;

    if( aWxModifiers & wxMOD_SHIFT )
        mask |= 1;          // ShiftMask

    if( aWxModifiers & wxMOD_CONTROL )
        mask |= 4;          // ControlMask

    if( aWxModifiers & wxMOD_ALT )
        mask |= 8;          // Mod1Mask

    if( aWxModifiers & wxMOD_META )
        mask |= 0x40;       // Mod4Mask

    return mask;
}


const int CHORD_MODIFIERS = wxMOD_CONTROL | wxMOD_SHIFT | wxMOD_ALT | wxMOD_META;
} // namespace


BACKPLANE_EMBED& BACKPLANE_EMBED::Get()
{
    // Never destroyed: frames and timers can outlive static destruction order.
    static BACKPLANE_EMBED* s_instance = new BACKPLANE_EMBED();
    return *s_instance;
}


BACKPLANE_EMBED::BACKPLANE_EMBED() :
        wxEvtHandler()
{
    m_pollTimer.SetOwner( this );
    m_parentTimer.SetOwner( this );
    Bind( wxEVT_TIMER, &BACKPLANE_EMBED::onPoll, this, m_pollTimer.GetId() );
    Bind( wxEVT_TIMER,
          [this]( wxTimerEvent& )
          {
              if( m_parentPid <= 0 )
                  return;

              bool gone = false;
#if defined( __unix__ ) || defined( __APPLE__ )
              gone = ::kill( static_cast<pid_t>( m_parentPid ), 0 ) == -1 && errno == ESRCH;
#elif defined( _WIN32 )
              gone = !wxProcess::Exists( static_cast<int>( m_parentPid ) );
#endif
              if( gone )
              {
                  m_parentTimer.Stop();
                  m_parentPid = 0;

                  if( m_onParentGone )
                      m_onParentGone();
              }
          },
          m_parentTimer.GetId() );

    SetReservedKeys( { "Ctrl+K", "Ctrl+J", "Ctrl+Tab", "Ctrl+Shift+Tab", "Ctrl+`" }, nullptr );
}


BACKPLANE_EMBED::~BACKPLANE_EMBED()
{
    StopEventServer();
}


bool BACKPLANE_EMBED::Enable( uint64_t aParent, uint64_t aToplevel )
{
    if( !KIPLATFORM::EMBED::Supported() )
        return false;

    KIPLATFORM::EMBED::HOOKS hooks;
    hooks.hostToplevel = [this]() { return Toplevel(); };
    hooks.isUnmanaged = [this]( uint64_t aWindow ) { return isUnmanaged( aWindow ); };
    hooks.dialog =
            [this]( uint64_t aWindow, bool aMapped, const wxString& aTitle )
            {
                if( aMapped )
                {
                    m_openDialogs[aWindow] = aTitle;
                    PushEvent( EMBED_EVENT_TYPE::DIALOG_OPENED, {}, aTitle.ToStdString( wxConvUTF8 ) );
                }
                else if( m_openDialogs.erase( aWindow ) )
                {
                    PushEvent( EMBED_EVENT_TYPE::DIALOG_CLOSED, {}, aTitle.ToStdString( wxConvUTF8 ) );
                }
            };
    hooks.detached =
            [this]()
            {
                m_parent = 0;
                PushEvent( EMBED_EVENT_TYPE::DETACHED, {} );
            };
    hooks.reparented =
            [this]( uint64_t aParent )
            {
                // The host moved the holder itself: same as an AttachToParent to that window.
                m_parent = aParent;
                PushEvent( EMBED_EVENT_TYPE::ATTACHED, {}, std::to_string( aParent ) );
            };

    KIPLATFORM::EMBED::Install( hooks );

    m_holder = KIPLATFORM::EMBED::CreateHolder( aParent );

    if( !m_holder && aParent )
    {
        // The parent is unusable; start parked rather than fail, so the host can attach later.
        m_holder = KIPLATFORM::EMBED::CreateHolder( 0 );
        aParent = 0;
    }

    if( !m_holder )
        return false;

    m_enabled = true;
    m_parent = aParent;
    m_toplevel = aToplevel;
    m_pollTimer.Start( 250 );
    return true;
}


bool BACKPLANE_EMBED::Attach( uint64_t aParent, uint64_t aToplevel, double aScale )
{
    if( !m_enabled )
        return false;

    if( aToplevel )
        m_toplevel = aToplevel;

    if( aScale > 0 )
        m_scale = aScale;

    if( !KIPLATFORM::EMBED::AttachHolder( m_holder, aParent ) )
        return false;

    m_parent = aParent;
    return true;
}


EMBED_FRAME_KIND BACKPLANE_EMBED::KindOf( FRAME_T aType )
{
    switch( aType )
    {
    case FRAME_PCB_EDITOR:       return EMBED_FRAME_KIND::PCB;
    case FRAME_SCH:              return EMBED_FRAME_KIND::SCH;
    case FRAME_SCH_SYMBOL_EDITOR: return EMBED_FRAME_KIND::SYMBOL_EDITOR;
    case FRAME_FOOTPRINT_EDITOR: return EMBED_FRAME_KIND::FOOTPRINT_EDITOR;
    case FRAME_PCB_DISPLAY3D:    return EMBED_FRAME_KIND::PCB_3D;
    default:                     return EMBED_FRAME_KIND::OTHER;
    }
}


BACKPLANE_EMBED::FRAME_ENTRY* BACKPLANE_EMBED::entry( const wxFrame* aFrame )
{
    for( FRAME_ENTRY& e : m_frames )
    {
        if( e.frame == aFrame )
            return &e;
    }

    return nullptr;
}


const BACKPLANE_EMBED::FRAME_ENTRY* BACKPLANE_EMBED::entry( const wxFrame* aFrame ) const
{
    for( const FRAME_ENTRY& e : m_frames )
    {
        if( e.frame == aFrame )
            return &e;
    }

    return nullptr;
}


bool BACKPLANE_EMBED::IsEmbedded( const wxFrame* aFrame ) const
{
    return m_enabled && entry( aFrame ) != nullptr;
}


void BACKPLANE_EMBED::ParkWindow( wxWindow* aWindow )
{
    if( m_enabled && aWindow )
    {
        if( uint64_t window = KIPLATFORM::EMBED::PrepareFrame( aWindow, m_holder ) )
            m_parked.insert( window );
    }
}


bool BACKPLANE_EMBED::isUnmanaged( uint64_t aWindow ) const
{
    if( !aWindow )
        return false;

    if( m_parked.count( aWindow ) )
        return true;

    for( const FRAME_ENTRY& e : m_frames )
    {
        if( e.window == aWindow )
            return true;
    }

    return m_manager && KIPLATFORM::EMBED::WindowId( m_manager ) == aWindow;
}


EMBED_FRAME_INFO BACKPLANE_EMBED::info( const FRAME_ENTRY& aEntry ) const
{
    EMBED_FRAME_INFO result;
    result.kind = aEntry.kind;
    result.window = aEntry.window;
    result.minWidth = aEntry.minWidth;
    result.minHeight = aEntry.minHeight;
    result.frameName = aEntry.frame->GetName();
    result.title = aEntry.frame->GetTitle();
    result.shown = aEntry.frame->IsShown() && KIPLATFORM::EMBED::IsMapped( aEntry.frame );
    result.focused = result.shown && KIPLATFORM::EMBED::HasFocus( aEntry.frame );
    result.realized = aEntry.frame->IsShown() && !m_prewarming.count( aEntry.frame );

    if( aEntry.ops.document )
        result.document = aEntry.ops.document();

    if( aEntry.ops.modified )
        result.modified = aEntry.ops.modified();

    return result;
}


EMBED_FRAME_INFO BACKPLANE_EMBED::Info( wxFrame* aFrame ) const
{
    if( const FRAME_ENTRY* e = entry( aFrame ) )
        return info( *e );

    return EMBED_FRAME_INFO();
}


std::vector<EMBED_FRAME_INFO> BACKPLANE_EMBED::AllInfo() const
{
    std::vector<EMBED_FRAME_INFO> result;

    for( const FRAME_ENTRY& e : m_frames )
        result.push_back( info( e ) );

    return result;
}


std::vector<wxFrame*> BACKPLANE_EMBED::Frames() const
{
    std::vector<wxFrame*> result;

    for( const FRAME_ENTRY& e : m_frames )
        result.push_back( e.frame );

    return result;
}


wxFrame* BACKPLANE_EMBED::FindFrame( EMBED_FRAME_KIND aKind, uint64_t aWindow ) const
{
    for( const FRAME_ENTRY& e : m_frames )
    {
        if( aWindow ? e.window == aWindow : e.kind == aKind )
            return e.frame;
    }

    return nullptr;
}


const EMBED_FRAME_OPS* BACKPLANE_EMBED::Ops( wxFrame* aFrame ) const
{
    const FRAME_ENTRY* e = entry( aFrame );
    return e ? &e->ops : nullptr;
}


void BACKPLANE_EMBED::SetManager( wxFrame* aManager, const EMBED_FRAME_OPS& aOps )
{
    m_manager = aManager;
    m_managerOps = aOps;
}


bool BACKPLANE_EMBED::registerFrame( wxFrame* aFrame, FRAME_T aType, const EMBED_FRAME_OPS& aOps )
{
    FRAME_ENTRY e;
    e.frame = aFrame;
    e.type = aType;
    e.kind = KindOf( aType );
    e.ops = aOps;

    // KiCad's minimum is reported to the host (which shows "too small" rather than clipping);
    // the window itself must follow whatever rectangle the host gives it.
    wxSize minSize = aFrame->GetMinSize();
    e.minWidth = std::max( 0, minSize.x );
    e.minHeight = std::max( 0, minSize.y );
    aFrame->SetMinSize( wxDefaultSize );
    aFrame->SetSizeHints( wxDefaultSize );

    e.window = KIPLATFORM::EMBED::PrepareFrame( aFrame, m_holder );

    if( !e.window )
        return false;

    aFrame->Bind( wxEVT_ACTIVATE,
                  [this, aFrame]( wxActivateEvent& aEvent )
                  {
                      onFrameActivate( aFrame, aEvent.GetActive() );
                      aEvent.Skip();
                  } );

    e.title = aFrame->GetTitle();
    e.document = aOps.document ? aOps.document() : wxString();
    e.modified = aOps.modified ? aOps.modified() : false;

    m_frames.push_back( e );

    if( e.ops.applyChrome )
        e.ops.applyChrome( m_chrome );

    PushEvent( EMBED_EVENT_TYPE::FRAME_OPENED, info( m_frames.back() ) );
    return true;
}


std::optional<bool> BACKPLANE_EMBED::OnFrameShow( wxFrame* aFrame, FRAME_T aType, bool aIsModal,
                                                  bool aShow, const EMBED_FRAME_OPS& aOps )
{
    if( !m_enabled || !aFrame )
        return std::nullopt;


    if( aType == KICAD_MAIN_FRAME_T )
    {
        // The project manager hosts the KIWAY but is never shown; the host does its job.
        if( aShow )
            return false;

        return std::nullopt;
    }

    // Modal frames (choosers) behave as dialogs: top-levels, transient for the host.
    if( aIsModal )
        return std::nullopt;

    FRAME_ENTRY* e = entry( aFrame );

    if( !e )
    {
        if( !aShow )
            return std::nullopt;

        if( !registerFrame( aFrame, aType, aOps ) )
        {
            // Never let it become a top-level by accident.
            wxLogTrace( wxS( "KICAD_EMBED" ), wxS( "could not embed frame %s" ),
                        aFrame->GetName() );
            return false;
        }

        e = entry( aFrame );
    }


    if( aShow && !aFrame->IsShown() && m_showAllowed == 0 && ( m_reloading > 0 || m_quietShowRequests > 0 ) )
        return false;   // a reload or a prewarm re-shows its frame; the host's choice stands

    if( aShow && !aFrame->IsShown() && m_showAllowed == 0 )
    {
        // KiCad asked for a hidden frame (Edit Symbol, the 3D viewer, the second Show() of the
        // library editors...).  The host decides whether and where it appears.
        PushEvent( EMBED_EVENT_TYPE::SHOW_REQUESTED, info( *e ) );
        return false;
    }

    return std::nullopt;
}


bool BACKPLANE_EMBED::OnFrameRaise( wxFrame* aFrame, FRAME_T aType, bool aIsModal,
                                    const EMBED_FRAME_OPS& aOps )
{
    if( !m_enabled || !aFrame || aIsModal )
        return false;

    if( aType == KICAD_MAIN_FRAME_T )
        return true;

    // A raise maps the window (gtk_window_present), even before the frame's first Show() (KiCad
    // raises frames while loading their documents), which would make it a top-level.  Park it
    // first, and never let a raise undo the host's choice.
    FRAME_ENTRY* e = entry( aFrame );

    if( !e )
    {
        if( !registerFrame( aFrame, aType, aOps ) )
            return true;

        e = entry( aFrame );
    }

    if( !aFrame->IsShown() && m_showAllowed == 0 && m_quietShowRequests == 0 && m_reloading == 0 )
        PushEvent( EMBED_EVENT_TYPE::SHOW_REQUESTED, info( *e ) );

    return true;
}


void BACKPLANE_EMBED::OnFrameDestroyed( wxFrame* aFrame )
{
    if( aFrame == m_manager )
    {
        m_manager = nullptr;
        m_managerOps = EMBED_FRAME_OPS();
    }

    auto it = std::find_if( m_frames.begin(), m_frames.end(),
                            [&]( const FRAME_ENTRY& e ) { return e.frame == aFrame; } );

    if( it == m_frames.end() )
        return;

    EMBED_FRAME_INFO closed;
    closed.kind = it->kind;
    closed.window = it->window;
    closed.title = it->title;
    closed.document = it->document;
    closed.frameName = aFrame->GetName();

    m_frames.erase( it );
    m_conflicts.erase( aFrame );
    m_prewarming.erase( aFrame );
    PushEvent( EMBED_EVENT_TYPE::FRAME_CLOSED, closed );
}


void BACKPLANE_EMBED::WithShowAllowed( const std::function<void()>& aFunc )
{
    ++m_showAllowed;

    try
    {
        aFunc();
    }
    catch( ... )
    {
        --m_showAllowed;
        throw;
    }

    --m_showAllowed;
}


bool BACKPLANE_EMBED::ShowEmbedded( wxFrame* aFrame, bool aShow )
{
    if( !IsEmbedded( aFrame ) )
        return false;

    TRACE_SCOPE trace( std::string( aShow ? "embed:show@" : "embed:hide@" )
                       + aFrame->GetName().ToStdString( wxConvUTF8 ) );

    if( aShow )
        EndPrewarm( aFrame );

    WithShowAllowed( [&]() { aFrame->Show( aShow ); } );

    // The host may have unmapped it itself (a local tab switch); wx still thinks it shown
    if( aShow )
        KIPLATFORM::EMBED::EnsureMapped( aFrame );

    return true;
}


void BACKPLANE_EMBED::SetChrome( const EMBED_CHROME& aChrome )
{
    m_chrome = aChrome;

    for( FRAME_ENTRY& e : m_frames )
    {
        if( e.ops.applyChrome )
            e.ops.applyChrome( m_chrome );
    }
}


std::string BACKPLANE_EMBED::FormatChord( int aModifiers, int aKeyCode )
{
    std::string text;

    if( aModifiers & wxMOD_CONTROL )
        text += "Ctrl+";

    if( aModifiers & wxMOD_ALT )
        text += "Alt+";

    if( aModifiers & wxMOD_SHIFT )
        text += "Shift+";

    if( aModifiers & wxMOD_META )
        text += "Meta+";

    for( const KEY_NAME& key : KEY_NAMES )
    {
        if( key.wxCode == aKeyCode )
            return text + key.name;
    }

    if( aKeyCode >= WXK_F1 && aKeyCode <= WXK_F24 )
        return text + "F" + std::to_string( aKeyCode - WXK_F1 + 1 );

    return text + std::string( 1, static_cast<char>( aKeyCode ) );
}


bool BACKPLANE_EMBED::ParseChord( const std::string& aChord, int& aModifiers, int& aKeyCode )
{
    aModifiers = 0;
    aKeyCode = 0;

    std::vector<std::string> parts;
    size_t                   start = 0;

    // Split on '+', keeping a trailing "+" as the key in "Ctrl++".
    while( start < aChord.size() )
    {
        size_t plus = aChord.find( '+', start );

        if( plus == start && plus + 1 == aChord.size() )
        {
            parts.push_back( "+" );
            break;
        }

        if( plus == std::string::npos )
        {
            parts.push_back( aChord.substr( start ) );
            break;
        }

        parts.push_back( aChord.substr( start, plus - start ) );
        start = plus + 1;
    }

    if( parts.empty() )
        return false;

    auto lower = []( std::string s )
    {
        std::transform( s.begin(), s.end(), s.begin(),
                        []( unsigned char c ) { return std::tolower( c ); } );
        return s;
    };

    for( size_t ii = 0; ii + 1 < parts.size(); ++ii )
    {
        std::string mod = lower( parts[ii] );

        if( mod == "ctrl" || mod == "control" || mod == "cmd" )
            aModifiers |= wxMOD_CONTROL;
        else if( mod == "shift" )
            aModifiers |= wxMOD_SHIFT;
        else if( mod == "alt" || mod == "option" )
            aModifiers |= wxMOD_ALT;
        else if( mod == "meta" || mod == "super" || mod == "win" )
            aModifiers |= wxMOD_META;
        else
            return false;
    }

    const std::string& key = parts.back();

    if( key.size() == 1 )
    {
        aKeyCode = std::toupper( static_cast<unsigned char>( key[0] ) );
        return aKeyCode > 0x20 && aKeyCode < 0x7f;
    }

    for( const KEY_NAME& name : KEY_NAMES )
    {
        if( lower( name.name ) == lower( key ) )
        {
            aKeyCode = name.wxCode;
            return true;
        }
    }

    if( key.size() >= 2 && ( key[0] == 'F' || key[0] == 'f' ) )
    {
        int n = std::atoi( key.c_str() + 1 );

        if( n >= 1 && n <= 24 )
        {
            aKeyCode = WXK_F1 + n - 1;
            return true;
        }
    }

    return false;
}


bool BACKPLANE_EMBED::SetReservedKeys( const std::vector<std::string>& aChords, wxString* aError )
{
    std::vector<CHORD> parsed;

    for( const std::string& text : aChords )
    {
        CHORD chord;

        if( !ParseChord( text, chord.modifiers, chord.keyCode ) )
        {
            if( aError )
                *aError = wxString::Format( wxS( "cannot parse key chord '%s'" ), text );

            return false;
        }

        chord.text = FormatChord( chord.modifiers, chord.keyCode );
        parsed.push_back( chord );
    }

    m_reserved = parsed;
    return true;
}


std::vector<std::string> BACKPLANE_EMBED::ReservedKeys() const
{
    std::vector<std::string> result;

    for( const CHORD& chord : m_reserved )
        result.push_back( chord.text );

    return result;
}


void BACKPLANE_EMBED::sendFocusMessage( wxFrame* aFrame )
{
    const FRAME_ENTRY* e = entry( aFrame );

    if( !e )
        return;

    long data[5] = { static_cast<long>( e->window ), 1, static_cast<long>( e->kind ), 0, 0 };
    KIPLATFORM::EMBED::SendHostMessage( m_parent, FOCUS_ATOM, FOCUS_WM_APP_OFFSET, data );
}


void BACKPLANE_EMBED::onFrameActivate( wxFrame* aFrame, bool aActive )
{
    FRAME_ENTRY* e = entry( aFrame );

    if( !e || e->focused == aActive )
        return;

    e->focused = aActive;
    PushEvent( aActive ? EMBED_EVENT_TYPE::FOCUS_IN : EMBED_EVENT_TYPE::FOCUS_OUT, info( *e ) );
}


int BACKPLANE_EMBED::FilterEvent( wxEvent& aEvent )
{
    wxEventType type = aEvent.GetEventType();

    if( m_traceFile )
    {
        const char* name = nullptr;

        if( type == wxEVT_PAINT ) name = "paint";
        else if( type == wxEVT_SIZE ) name = "size";
        else if( type == wxEVT_TIMER ) name = "timer";
        else if( type == wxEVT_IDLE ) name = "idle";
        else if( type == wxEVT_SHOW ) name = "show";
        else if( type == wxEVT_ACTIVATE ) name = "activate";
        else if( type == wxEVT_CHAR_HOOK || type == wxEVT_KEY_DOWN ) name = "key";
        else if( type == wxEVT_LEFT_DOWN || type == wxEVT_RIGHT_DOWN || type == wxEVT_MIDDLE_DOWN ) name = "click";
        else if( type == wxEVT_MOTION ) name = "motion";
        else if( type == wxEVT_MOUSEWHEEL ) name = "wheel";
        else if( type == wxEVT_MENU || type == wxEVT_TOOL ) name = "command";
        else if( type == wxEVT_UPDATE_UI ) name = "update_ui";
        else if( type == wxEVT_SYS_COLOUR_CHANGED ) name = "colours";

        static std::string other;

        if( !name && type != wxEVT_ENTER_WINDOW && type != wxEVT_LEAVE_WINDOW )
        {
            other = "ev" + std::to_string( static_cast<int>( type ) ) + ":"
                    + wxString( aEvent.GetClassInfo() ? aEvent.GetClassInfo()->GetClassName()
                                                      : wxT( "?" ) ).ToStdString();
            name = other.c_str();
        }

        if( name )
        {
            wxObject* object = aEvent.GetEventObject();
            wxWindow* window = wxDynamicCast( object, wxWindow );
            wxWindow* top = window ? wxGetTopLevelParent( window ) : nullptr;
            std::string where = top ? top->GetName().ToStdString( wxConvUTF8 ) : std::string();
            std::string what( name );

            // Which window resized or appeared: a layout change behind a redraw storm
            if( window && ( type == wxEVT_SIZE || type == wxEVT_SHOW || type == wxEVT_PAINT ) )
                what += ":" + wxString( window->GetClassInfo()->GetClassName() ).ToStdString().substr( 0, 40 );

            TraceNote( where.empty() ? what : what + "@" + where );
        }
    }

    if( !m_enabled )
        return -1;

    bool buttonDown = type == wxEVT_LEFT_DOWN || type == wxEVT_RIGHT_DOWN
                      || type == wxEVT_MIDDLE_DOWN || type == wxEVT_AUX1_DOWN
                      || type == wxEVT_AUX2_DOWN;

    bool key = type == wxEVT_CHAR_HOOK || type == wxEVT_KEY_DOWN || type == wxEVT_CHAR;

    if( !buttonDown && !key )
        return -1;

    wxWindow* window = wxDynamicCast( aEvent.GetEventObject(), wxWindow );

    if( !window )
        return -1;

    // Dialogs keep every key and click; only frames in the holder are handled here.
    wxFrame* frame = wxDynamicCast( wxGetTopLevelParent( window ), wxFrame );

    if( !frame || !IsEmbedded( frame ) )
        return -1;

    if( buttonDown )
    {
        // No window manager hands an embedded frame the keyboard on a click, so take it.  The
        // click itself still goes to KiCad.
        if( !KIPLATFORM::EMBED::HasFocus( frame ) )
        {
            KIPLATFORM::EMBED::TakeFocus( frame );
            sendFocusMessage( frame );
        }

        return -1;
    }

    wxKeyEvent& keyEvent = static_cast<wxKeyEvent&>( aEvent );
    int         modifiers = keyEvent.GetModifiers() & CHORD_MODIFIERS;
    int         code = keyEvent.GetKeyCode();

    // A Ctrl+letter wxEVT_CHAR carries the control character.
    if( type == wxEVT_CHAR && ( modifiers & wxMOD_CONTROL ) && code > 0 && code < 27 )
        code += 'A' - 1;

    if( code > 0 && code < 0x80 )
        code = std::toupper( code );

    for( size_t ii = 0; ii < m_reserved.size(); ++ii )
    {
        const CHORD& chord = m_reserved[ii];

        if( chord.keyCode != code || chord.modifiers != modifiers )
            continue;

        // wxEVT_CHAR_HOOK comes first; once it is consumed no KEY_DOWN/CHAR follows, but
        // swallow those too in case a platform sends them.
        if( type == wxEVT_CHAR_HOOK )
        {
            const FRAME_ENTRY* e = entry( frame );
            long data[5] = { static_cast<long>( e ? e->window : 0 ), keysymFor( code ),
                             xModifiersFor( modifiers ), static_cast<long>( ii ), 1 };
            KIPLATFORM::EMBED::SendHostMessage( m_parent, KEY_ATOM, KEY_WM_APP_OFFSET, data );
        }

        return wxEventFilter::Event_Processed;
    }

    return -1;
}


std::optional<wxString> BACKPLANE_EMBED::ModalBlocker()
{
    if( !wxTheApp )
        return std::nullopt;

    for( wxWindow* window : wxTopLevelWindows )
    {
        if( !window->IsShown() )
            continue;

        if( DIALOG_SHIM* shim = dynamic_cast<DIALOG_SHIM*>( window ) )
        {
            if( shim->IsQuasiModal() || shim->IsModal() )
                return shim->GetTitle();
        }
        else if( wxDialog* dialog = dynamic_cast<wxDialog*>( window ) )
        {
            if( dialog->IsModal() )
                return dialog->GetTitle();
        }
    }

    wxString nativeTitle;

    if( KIPLATFORM::EMBED::NativeModalTitle( nativeTitle ) )
        return nativeTitle;

    // A modal frame (a chooser) runs its own event loop.
    wxEventLoopBase* active = wxEventLoopBase::GetActive();
    wxEventLoopBase* main = wxTheApp->GetMainLoop();

    if( active && main && active != main )
    {
        const BACKPLANE_EMBED& embed = Get();

        for( wxWindow* window : wxTopLevelWindows )
        {
            wxFrame*          frame = wxDynamicCast( window, wxFrame );
            wxTopLevelWindow* top = wxDynamicCast( window, wxTopLevelWindow );

            if( top && window->IsShown() && !( frame && embed.IsEmbedded( frame ) ) )
                return top->GetTitle();
        }

        return wxString( wxS( "a modal window" ) );
    }

    return std::nullopt;
}


void BACKPLANE_EMBED::onPoll( wxTimerEvent& )
{
    TRACE_SCOPE trace( "embed:poll", 1.0 );

    pollFiles();

    for( FRAME_ENTRY& e : m_frames )
    {
        wxString title = e.frame->GetTitle();
        wxString document = e.ops.document ? e.ops.document() : wxString();
        bool     modified = e.ops.modified ? e.ops.modified() : false;
        // A frame being prewarmed off-screen is not shown to anyone
        if( m_prewarming.count( e.frame ) )
            continue;

        bool     shown = e.frame->IsShown() && KIPLATFORM::EMBED::IsMapped( e.frame );

        if( shown != e.shown )
        {
            e.shown = shown;
            PushEvent( shown ? EMBED_EVENT_TYPE::FRAME_SHOWN : EMBED_EVENT_TYPE::FRAME_HIDDEN,
                       info( e ) );
        }

        if( title != e.title )
        {
            e.title = title;
            PushEvent( EMBED_EVENT_TYPE::TITLE_CHANGED, info( e ) );
        }

        if( document != e.document )
        {
            e.document = document;
            PushEvent( EMBED_EVENT_TYPE::DOCUMENT_CHANGED, info( e ) );
        }

        if( modified != e.modified )
        {
            e.modified = modified;
            PushEvent( EMBED_EVENT_TYPE::MODIFIED_CHANGED, info( e ) );
        }
    }
}


void BACKPLANE_EMBED::PushEvent( EMBED_EVENT_TYPE aType, const EMBED_FRAME_INFO& aFrame,
                                 const std::string& aDetail )
{
    {
        std::lock_guard<std::mutex> lock( m_eventMutex );

        EMBED_EVENT event;
        event.seq = m_nextSeq++;
        event.type = aType;
        event.frame = aFrame;
        event.detail = aDetail;
        m_events.push_back( event );

        while( m_events.size() > MAX_EVENTS )
            m_events.pop_front();
    }

    m_eventCv.notify_all();
}


uint64_t BACKPLANE_EMBED::NextEvents( uint64_t aSince, int aWaitMs, size_t aMax,
                                      std::vector<EMBED_EVENT>& aEvents, bool& aLost )
{
    std::unique_lock<std::mutex> lock( m_eventMutex );

    auto ready = [&]()
    {
        return m_eventShutdown.load() || ( !m_events.empty() && m_events.back().seq > aSince );
    };

    if( aWaitMs > 0 && !ready() )
        m_eventCv.wait_for( lock, std::chrono::milliseconds( std::min( aWaitMs, 60000 ) ), ready );

    aLost = !m_events.empty() && m_events.front().seq > aSince + 1;

    uint64_t next = aSince;

    for( const EMBED_EVENT& event : m_events )
    {
        if( event.seq <= aSince )
            continue;

        if( aEvents.size() >= aMax )
            break;

        aEvents.push_back( event );
        next = event.seq;
    }

    if( aEvents.empty() && !m_events.empty() && aSince > m_events.back().seq )
        next = m_events.back().seq;    // a stale `since` from an earlier process

    return next;
}


void BACKPLANE_EMBED::WatchParentPid( long long aPid, std::function<void()> aOnGone )
{
    m_parentPid = aPid;
    m_onParentGone = std::move( aOnGone );

    if( aPid > 0 )
        m_parentTimer.Start( 500 );
    else
        m_parentTimer.Stop();
}


bool BACKPLANE_EMBED::StartEventServer( const std::string& aUrl, const std::string& aToken )
{
    StopEventServer();
    s_eventToken = aToken;

    nng_socket socket;

    if( nng_rep0_open( &socket ) != 0 )
        return false;

    nng_listener listener;

    if( nng_listener_create( &listener, socket, aUrl.c_str() ) != 0 )
    {
        nng_close( socket );
        return false;
    }

    nng_socket_set_ms( socket, NNG_OPT_RECVTIMEO, 500 );

    if( nng_listener_start( listener, 0 ) != 0 )
    {
        nng_close( socket );
        return false;
    }

    s_eventSocket = socket;
    m_eventUrl = aUrl;
    m_eventShutdown.store( false );
    m_eventThread = std::thread( [this]() { eventServerThread(); } );
    return true;
}


void BACKPLANE_EMBED::StopEventServer()
{
    if( !m_eventThread.joinable() )
        return;

    m_eventShutdown.store( true );
    m_eventCv.notify_all();
    m_eventThread.join();
    nng_close( s_eventSocket );
    s_eventSocket = NNG_SOCKET_INITIALIZER;
}


void BACKPLANE_EMBED::eventServerThread()
{
    using namespace kiapi::common;

    while( !m_eventShutdown.load() )
    {
        char*  buf = nullptr;
        size_t size = 0;
        int    ret = nng_recv( s_eventSocket, &buf, &size, NNG_FLAG_ALLOC );

        if( ret == NNG_ETIMEDOUT )
            continue;

        if( ret != 0 )
            break;

        ApiRequest request;
        bool       parsed = request.ParseFromArray( buf, static_cast<int>( size ) );
        nng_free( buf, size );

        ApiResponse response;
        response.mutable_header()->set_kicad_token( s_eventToken );

        kiapi::backplane::embed::NextEvents poll;

        if( !parsed )
        {
            response.mutable_status()->set_status( ApiStatusCode::AS_BAD_REQUEST );
            response.mutable_status()->set_error_message( "request could not be parsed" );
        }
        else if( !request.message().UnpackTo( &poll ) )
        {
            response.mutable_status()->set_status( ApiStatusCode::AS_UNHANDLED );
            response.mutable_status()->set_error_message(
                    "this socket serves only kiapi.backplane.embed.NextEvents" );
        }
        else
        {
            std::vector<EMBED_EVENT> events;
            bool                     lost = false;
            size_t max = poll.max() > 0 ? static_cast<size_t>( poll.max() ) : 256;
            uint64_t next = NextEvents( poll.since(), poll.wait_ms(), max, events, lost );

            kiapi::backplane::embed::NextEventsResponse reply;
            reply.set_next( next );
            reply.set_lost( lost );

            for( const EMBED_EVENT& event : events )
            {
                FillEmbedEvent( reply.add_events(), event );
            }

            response.mutable_status()->set_status( ApiStatusCode::AS_OK );
            response.mutable_message()->PackFrom( reply );
        }

        std::string out = response.SerializeAsString();

        if( m_eventShutdown.load() )
            break;

        nng_send( s_eventSocket, out.data(), out.size(), 0 );
    }
}


// ---- bp.9: theme ---------------------------------------------------------------------------

const wxString BACKPLANE_EMBED::COLOR_THEME_DARK = wxS( "_builtin_backplane_dark" );


wxString BACKPLANE_EMBED::ColorThemeOverride() const
{
    switch( m_theme )
    {
    case EMBED_THEME::LIGHT_MODE: return wxS( "_builtin_default" );
    case EMBED_THEME::DARK_MODE:  return COLOR_THEME_DARK;
    default:                 return wxEmptyString;
    }
}


void BACKPLANE_EMBED::SetTheme( EMBED_THEME aTheme, bool aRefresh )
{
    m_theme = aTheme;
    m_gtkDark = KIPLATFORM::EMBED::SetPreferDark( static_cast<int>( aTheme ) );

    if( !aRefresh )
        return;

    // Widgets drawn from wx's system colours repaint on this; GTK restyles native ones itself.
    for( wxWindow* window : wxTopLevelWindows )
    {
        wxSysColourChangedEvent event;
        event.SetEventObject( window );
        window->GetEventHandler()->ProcessEvent( event );
    }

    // Canvases reload their colour theme, toolbars their icons.
    for( wxFrame* frame : Frames() )
    {
        if( const EMBED_FRAME_OPS* ops = Ops( frame ); ops && ops->settingsChanged )
            ops->settingsChanged();
    }

    if( m_managerOps.settingsChanged )
        m_managerOps.settingsChanged();

    PushEvent( EMBED_EVENT_TYPE::THEME_CHANGED, {},
               aTheme == EMBED_THEME::DARK_MODE ? "dark" : aTheme == EMBED_THEME::LIGHT_MODE ? "light" : "kicad" );
}


// ---- bp.9: selection -----------------------------------------------------------------------

void BACKPLANE_EMBED::SelectionChanged( wxFrame* aFrame, const EMBED_ITEM_NAMES& aSelection )
{
    if( !m_enabled || m_hostSelecting > 0 )
        return;

    const FRAME_ENTRY* e = entry( aFrame );

    if( !e )
        return;

    {
        std::lock_guard<std::mutex> lock( m_eventMutex );

        EMBED_EVENT event;
        event.seq = m_nextSeq++;
        event.type = EMBED_EVENT_TYPE::SELECTION_CHANGED;
        event.frame = info( *e );
        event.selection = aSelection;
        m_events.push_back( event );

        while( m_events.size() > MAX_EVENTS )
            m_events.pop_front();
    }

    m_eventCv.notify_all();
}


// ---- bp.9: reload on disk change -----------------------------------------------------------

namespace
{
wxString normalPath( const wxString& aPath )
{
    wxFileName fn( aPath );
    fn.MakeAbsolute();
    fn.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
    return fn.GetFullPath();
}


/// mtime, size: changes on every write and on an atomic rename-over.  Empty if missing.
std::string fileSignature( const wxString& aPath )
{
    std::error_code       ec;
    std::filesystem::path path( aPath.ToStdString( wxConvUTF8 ) );
    auto                  time = std::filesystem::last_write_time( path, ec );

    if( ec )
        return std::string();

    auto size = std::filesystem::file_size( path, ec );

    if( ec )
        return std::string();

    return std::to_string( time.time_since_epoch().count() ) + ":" + std::to_string( size );
}


bool readFile( const wxString& aPath, std::string& aContent )
{
    std::ifstream in( aPath.ToStdString( wxConvUTF8 ), std::ios::binary );

    if( !in )
        return false;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    aContent = buffer.str();
    return true;
}


/// A KiCad s-expression file that is complete: starts with '(' and its parentheses balance.
/// A writer still in the middle of the file fails this.
bool completeSexpr( const std::string& aText )
{
    size_t first = aText.find_first_not_of( " \t\r\n" );

    if( first == std::string::npos || aText[first] != '(' )
        return false;

    long depth = 0;
    bool inString = false;

    for( size_t ii = first; ii < aText.size(); ++ii )
    {
        char c = aText[ii];

        if( inString )
        {
            if( c == '\\' )
                ++ii;
            else if( c == '"' )
                inString = false;
        }
        else if( c == '"' )
        {
            inString = true;
        }
        else if( c == '(' )
        {
            ++depth;
        }
        else if( c == ')' )
        {
            if( --depth < 0 )
                return false;
        }
    }

    return depth == 0 && !inString;
}


std::string contentHash( const std::string& aContent )
{
    return std::to_string( std::hash<std::string>()( aContent ) ) + ":"
           + std::to_string( aContent.size() );
}
} // namespace


void BACKPLANE_EMBED::baseline( const wxString& aPath )
{
    TRACE_SCOPE trace( "embed:baseline", 0.5 );

    // Only the file's signature (a stat): reading and hashing it here would put the whole
    // file's I/O on the UI thread after every save.  With no hash, the next change of signature
    // counts as a change (a touch without new content reloads a clean document once).
    FILE_STATE& state = m_files[aPath];

    state.signature = fileSignature( aPath );
    state.hash.clear();
    state.pendingSig.clear();
    state.pendingSince = 0;
}


void BACKPLANE_EMBED::NoteWriting( const wxString& aPath )
{
    if( m_enabled )
        m_files[normalPath( aPath )].writing++;
}


void BACKPLANE_EMBED::NoteWritten( const wxString& aPath )
{
    if( !m_enabled )
        return;

    wxString    path = normalPath( aPath );
    FILE_STATE& state = m_files[path];

    if( state.writing > 0 )
        state.writing--;

    baseline( path );
}


wxFrame* BACKPLANE_EMBED::FindFrameByFile( const wxString& aPath ) const
{
    wxString path = normalPath( aPath );

    for( const FRAME_ENTRY& e : m_frames )
    {
        if( !e.ops.watchedFiles )
            continue;

        for( const wxString& file : e.ops.watchedFiles() )
        {
            if( normalPath( file ) == path )
                return e.frame;
        }
    }

    return nullptr;
}


void BACKPLANE_EMBED::pollFiles()
{
    if( m_reloading > 0 )
        return;

    long long now = wxGetUTCTimeMillis().GetValue();

    // Copy: a reload can open or close frames.
    std::vector<wxFrame*> frames = Frames();

    for( wxFrame* frame : frames )
    {
        const FRAME_ENTRY* e = entry( frame );

        if( !e || !e->ops.watchedFiles )
            continue;

        std::vector<wxString> files = e->ops.watchedFiles();

        for( const wxString& file : files )
        {
            if( file.IsEmpty() )
                continue;

            wxString path = normalPath( file );
            auto     it = m_files.find( path );

            if( it == m_files.end() )
            {
                baseline( path );   // first sight: what the frame loaded
                continue;
            }

            FILE_STATE& state = it->second;

            if( state.writing > 0 )
                continue;

            std::string signature = fileSignature( path );

            if( signature.empty() )
                continue;           // missing for a moment (a rename-over in progress)

            if( signature == state.signature )
            {
                state.pendingSig.clear();
                continue;
            }

            // Debounce: act only after the file has been quiet for 300 ms.
            if( signature != state.pendingSig )
            {
                state.pendingSig = signature;
                state.pendingSince = now;
                continue;
            }

            if( now - state.pendingSince < 300 )
                continue;

            std::string content;

            if( !readFile( path, content ) || !completeSexpr( content ) )
            {
                state.pendingSince = now;   // a partial write: wait for it to finish
                continue;
            }

            // A clean frame reloads; not while a dialog or an interactive tool holds its items.
            // Keep the change pending and look again on the next poll.
            if( e->ops.busy && e->ops.busy() && !( e->ops.modified && e->ops.modified() ) )
                continue;

            // Stable over the quiet period and complete.  Only a real content change counts.
            state.signature = signature;
            state.pendingSig.clear();
            std::string hash = contentHash( content );

            if( !state.hash.empty() && hash == state.hash )
                continue;

            state.hash = hash;
            externalChange( frame, path );
            return;   // one change per poll; frames may have changed
        }
    }
}


void BACKPLANE_EMBED::externalChange( wxFrame* aFrame, const wxString& aPath )
{
    const FRAME_ENTRY* e = entry( aFrame );

    if( !e )
        return;

    if( e->ops.modified && e->ops.modified() )
    {
        // Never clobber the person's edits: ask, here and in KiCad's infobar.
        m_conflicts[aFrame] = aPath;

        if( e->ops.showConflict )
            e->ops.showConflict( aPath );

        PushEvent( EMBED_EVENT_TYPE::RELOAD_CONFLICT, info( *e ), aPath.ToStdString( wxConvUTF8 ) );
        return;
    }

    if( doReload( aFrame ) == EMBED_RELOAD_RESULT::RELOADED )
    {
        if( const FRAME_ENTRY* after = entry( aFrame ) )
        {
            PushEvent( EMBED_EVENT_TYPE::DOCUMENT_RELOADED, info( *after ),
                       aPath.ToStdString( wxConvUTF8 ) );
        }
    }
}


EMBED_RELOAD_RESULT BACKPLANE_EMBED::doReload( wxFrame* aFrame )
{
    const FRAME_ENTRY* e = entry( aFrame );

    if( !e || !e->ops.reload )
        return EMBED_RELOAD_RESULT::UNKNOWN;

    EMBED_FRAME_OPS ops = e->ops;

    if( m_conflicts.erase( aFrame ) && ops.showConflict )
        ops.showConflict( wxEmptyString );

    ++m_reloading;
    bool ok = false;
    TRACE_SCOPE trace( "embed:reload" );

    try
    {
        ok = ops.reload();
    }
    catch( ... )
    {
        ok = false;
    }

    --m_reloading;

    // What is on disk now is what the frame has.
    if( entry( aFrame ) && ops.watchedFiles )
    {
        for( const wxString& file : ops.watchedFiles() )
            baseline( normalPath( file ) );
    }

    return ok ? EMBED_RELOAD_RESULT::RELOADED : EMBED_RELOAD_RESULT::UNKNOWN;
}


EMBED_RELOAD_RESULT BACKPLANE_EMBED::Reload( wxFrame* aFrame, bool aDiscard )
{
    const FRAME_ENTRY* e = entry( aFrame );

    if( !e )
        return EMBED_RELOAD_RESULT::UNKNOWN;

    // Only frames that watch files can reload (not yet the library editors).
    if( !e->ops.watchedFiles || e->ops.watchedFiles().empty() )
        return EMBED_RELOAD_RESULT::UNKNOWN;

    bool modified = e->ops.modified && e->ops.modified();

    if( modified && !aDiscard )
        return EMBED_RELOAD_RESULT::CONFLICT;

    if( !modified && e->ops.watchedFiles )
    {
        bool same = true;

        for( const wxString& file : e->ops.watchedFiles() )
        {
            // Untouched since the frame loaded or saved it: same signature (a stat, no read).
            wxString path = normalPath( file );
            auto     it = m_files.find( path );

            if( it == m_files.end() || fileSignature( path ) != it->second.signature )
                same = false;
        }

        if( same )
            return EMBED_RELOAD_RESULT::UNCHANGED;
    }

    EMBED_RELOAD_RESULT result = doReload( aFrame );

    if( result == EMBED_RELOAD_RESULT::RELOADED )
    {
        if( const FRAME_ENTRY* after = entry( aFrame ) )
            PushEvent( EMBED_EVENT_TYPE::DOCUMENT_RELOADED, info( *after ),
                       after->document.ToStdString( wxConvUTF8 ) );
    }

    return result;
}


EMBED_RELOAD_RESULT BACKPLANE_EMBED::ResolveConflict( wxFrame* aFrame, bool aReload )
{
    const FRAME_ENTRY* e = entry( aFrame );

    if( !e )
        return EMBED_RELOAD_RESULT::UNKNOWN;

    if( aReload )
        return Reload( aFrame, true );

    // Keep the person's edits (and their undo stack); the next save overwrites the file.
    if( m_conflicts.erase( aFrame ) && e->ops.showConflict )
        e->ops.showConflict( wxEmptyString );

    return EMBED_RELOAD_RESULT::UNCHANGED;
}


// ---- Frame trace ---------------------------------------------------------------------------

namespace
{
long long nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch() )
            .count();
}


std::string jsonEscape( const std::string& aText )
{
    std::string out;

    for( char c : aText )
    {
        if( c == '"' || c == '\\' )
            out += '\\';

        if( static_cast<unsigned char>( c ) >= 0x20 )
            out += c;
    }

    return out;
}
} // namespace


bool BACKPLANE_EMBED::TraceStart( const wxString& aPath, double aThresholdMs )
{
    m_traceFile = fopen( aPath.ToStdString( wxConvUTF8 ).c_str(), "a" );

    if( !m_traceFile )
        return false;

    setvbuf( m_traceFile, nullptr, _IOLBF, 0 );
    m_traceThreshold = aThresholdMs;
    KIPLATFORM::EMBED::InstallLoopTimer( [this]( double aStart, double aDur ) { traceLoop( aStart, aDur ); } );
    fprintf( m_traceFile, "{\"kind\":\"start\",\"t\":%.3f,\"threshold_ms\":%.1f}\n", nowUs() / 1000.0,
             aThresholdMs );
    return true;
}


void BACKPLANE_EMBED::TraceNote( const std::string& aWhat )
{
    if( !m_traceFile )
        return;

    std::lock_guard<std::mutex> lock( m_traceMutex );

    for( std::pair<std::string, int>& note : m_traceNotes )
    {
        if( note.first == aWhat )
        {
            note.second++;
            return;
        }
    }

    if( m_traceNotes.size() < 64 )
        m_traceNotes.emplace_back( aWhat, 1 );
}


void BACKPLANE_EMBED::TraceSpan( const std::string& aWhat, double aMs )
{
    if( !m_traceFile )
        return;

    TraceNote( aWhat );
    fprintf( m_traceFile, "{\"kind\":\"span\",\"t\":%.3f,\"ms\":%.3f,\"what\":\"%s\"}\n",
             nowUs() / 1000.0 - aMs, aMs, jsonEscape( aWhat ).c_str() );
}


void BACKPLANE_EMBED::traceLoop( double aStartMs, double aDurMs )
{
    std::vector<std::pair<std::string, int>> notes;

    {
        std::lock_guard<std::mutex> lock( m_traceMutex );
        notes.swap( m_traceNotes );
    }

    if( !m_traceFile || aDurMs < m_traceThreshold )
        return;

    std::string ran;

    for( const auto& [note, count] : notes )
    {
        ran += ( ran.empty() ? "\"" : ",\"" ) + jsonEscape( note );
        ran += ( count > 1 ? "*" + std::to_string( count ) : std::string() ) + "\"";
    }

    fprintf( m_traceFile, "{\"kind\":\"loop\",\"t\":%.3f,\"ms\":%.3f,\"ran\":[%s]}\n", aStartMs,
             aDurMs, ran.c_str() );
}


BACKPLANE_EMBED::TRACE_SCOPE::TRACE_SCOPE( std::string aWhat, double aMinMs ) :
        m_what( std::move( aWhat ) ),
        m_minMs( aMinMs ),
        m_startUs( 0 ),
        m_on( BACKPLANE_EMBED::Get().Tracing() )
{
    if( m_on )
        m_startUs = nowUs();
}


BACKPLANE_EMBED::TRACE_SCOPE::~TRACE_SCOPE()
{
    if( !m_on )
        return;

    double ms = ( nowUs() - m_startUs ) / 1000.0;

    if( ms >= m_minMs )
        BACKPLANE_EMBED::Get().TraceSpan( m_what, ms );
    else
        BACKPLANE_EMBED::Get().TraceNote( m_what );
}


// ---- Prewarm --------------------------------------------------------------------------------

void BACKPLANE_EMBED::Prewarm( wxFrame* aFrame, int aWidth, int aHeight )
{
    if( !IsEmbedded( aFrame ) || aFrame->IsShown() || m_prewarming.count( aFrame ) )
        return;

    // Lay it out and paint it once off-screen (clipped by the container, so nothing shows),
    // which builds its layout, its canvas's GL context and its first view; then hide it again.
    m_prewarming.insert( aFrame );
    TRACE_SCOPE trace( "embed:prewarm@" + aFrame->GetName().ToStdString( wxConvUTF8 ) );
    KIPLATFORM::EMBED::SetFrameGeometry( aFrame, -30000, -30000, std::max( aWidth, 400 ), std::max( aHeight, 300 ) );
    WithShowAllowed( [&]() { aFrame->Show( true ); } );

    wxWeakRef<wxFrame> weak( aFrame );

    // Hidden again after it has had time to paint (the paint comes from the next frames)
    aFrame->CallAfter(
            [this, weak, aWidth, aHeight]()
            {
                wxFrame* frame = weak.get();

                if( !frame )
                    return;

                wxTimer* later = new wxTimer();
                later->Bind( wxEVT_TIMER,
                             [this, weak, later, aWidth, aHeight]( wxTimerEvent& )
                             {
                                 later->Stop();

                                 if( wxFrame* f = weak.get(); f && m_prewarming.count( f ) )
                                 {
                                     // Unmapped natively rather than hidden: it stays shown
                                     // to the toolkit, so the host may map it directly
                                     // (FrameInfo.realized), with no first-map cost
                                     KIPLATFORM::EMBED::UnmapNative( f );
                                     KIPLATFORM::EMBED::SetFrameGeometry( f, 0, 0, std::max( aWidth, 400 ),
                                                                          std::max( aHeight, 300 ) );
                                     m_prewarming.erase( f );
                                 }

                                 wxTheApp->CallAfter( [later]() { delete later; } );
                             } );
                later->StartOnce( 400 );
            } );
}


void BACKPLANE_EMBED::EndPrewarm( wxFrame* aFrame )
{
    if( m_prewarming.erase( aFrame ) )
    {
        // Shown for real before the off-screen paint finished: back to the holder's origin
        wxSize size = aFrame->GetSize();
        KIPLATFORM::EMBED::SetFrameGeometry( aFrame, 0, 0, size.x, size.y );
    }
}


bool BACKPLANE_EMBED::Prewarming() const
{
    return !m_prewarming.empty() || m_quietShowRequests > 0;
}
