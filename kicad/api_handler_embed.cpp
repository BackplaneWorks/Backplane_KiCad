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

#include "api_handler_embed.h"

#include <fmt/format.h>

#include <wx/frame.h>

#include <backplane_embed.h>
#include <backplane_embed_proto.h>
#include <kiplatform/embed.h>
#include <kiway.h>
#include <mail_type.h>

#include <api/common/commands/cross_probe_commands.pb.h>
#include <api/common/envelope.pb.h>

using namespace kiapi::backplane::embed;
using kiapi::common::ApiStatusCode;


namespace
{
void fillFrame( FrameInfo* aOut, const EMBED_FRAME_INFO& aIn )
{
    aOut->set_kind( static_cast<FrameKind>( aIn.kind ) );
    aOut->set_document( aIn.document.ToStdString( wxConvUTF8 ) );
    aOut->set_title( aIn.title.ToStdString( wxConvUTF8 ) );
    aOut->set_modified( aIn.modified );
    aOut->set_window( aIn.window );
    aOut->set_min_width( aIn.minWidth );
    aOut->set_min_height( aIn.minHeight );
    aOut->set_shown( aIn.shown );
    aOut->set_focused( aIn.focused );
    aOut->set_frame_name( aIn.frameName.ToStdString( wxConvUTF8 ) );
}


ApiResponseStatus failure( ApiStatusCode aCode, const std::string& aMessage )
{
    ApiResponseStatus status;
    status.set_status( aCode );
    status.set_error_message( aMessage );
    return status;
}


/// The manager action that opens a frame kind, if there is one.
const char* managerActionFor( EMBED_FRAME_KIND aKind )
{
    switch( aKind )
    {
    case EMBED_FRAME_KIND::PCB:              return "kicad.Control.editPCB";
    case EMBED_FRAME_KIND::SCH:              return "kicad.Control.editSchematic";
    case EMBED_FRAME_KIND::SYMBOL_EDITOR:    return "kicad.Control.editSymbols";
    case EMBED_FRAME_KIND::FOOTPRINT_EDITOR: return "kicad.Control.editFootprints";
    default:                                 return nullptr;
    }
}


/// Select (and centre on) a reference in the PCB or schematic editor, as cross-probing does.
void crossProbe( KIWAY& aKiway, FRAME_T aFrame, const std::string& aReference )
{
    kiapi::common::commands::SyncSelection sync;
    sync.set_context( kiapi::common::commands::SSC_EXPLICIT );
    sync.set_mode( kiapi::common::commands::SSM_ITEMS_ONLY );
    sync.mutable_focus_item()->mutable_footprint()->set_reference( aReference );
    sync.add_items()->mutable_footprint()->set_reference( aReference );

    kiapi::common::ApiRequest request;
    request.mutable_message()->PackFrom( sync );

    std::string payload = request.SerializeAsString();
    aKiway.ExpressMail( aFrame, MAIL_CROSS_PROBE, payload );
}
} // namespace


API_HANDLER_EMBED::API_HANDLER_EMBED( KIWAY& aKiway ) :
        API_HANDLER(),
        m_kiway( aKiway )
{
    registerHandler<AttachToParent, AttachToParentResponse>(
            &API_HANDLER_EMBED::handleAttachToParent, true );
    registerHandler<ShowFrame, ShowFrameResponse>( &API_HANDLER_EMBED::handleShowFrame );
    registerHandler<HideFrame, HideFrameResponse>( &API_HANDLER_EMBED::handleHideFrame, true );
    registerHandler<GetFrames, GetFramesResponse>( &API_HANDLER_EMBED::handleGetFrames, true );
    registerHandler<NextEvents, NextEventsResponse>( &API_HANDLER_EMBED::handleNextEvents, true );
    registerHandler<SetChrome, SetChromeResponse>( &API_HANDLER_EMBED::handleSetChrome, true );
    registerHandler<SetReservedKeys, SetReservedKeysResponse>(
            &API_HANDLER_EMBED::handleSetReservedKeys, true );
    registerHandler<Close, CloseResponse>( &API_HANDLER_EMBED::handleClose );
    registerHandler<SetTheme, SetThemeResponse>( &API_HANDLER_EMBED::handleSetTheme, true );
    registerHandler<ReloadDocument, ReloadDocumentResponse>( &API_HANDLER_EMBED::handleReloadDocument );
    registerHandler<ResolveConflict, ResolveConflictResponse>( &API_HANDLER_EMBED::handleResolveConflict );
}


HANDLER_RESULT<AttachToParentResponse> API_HANDLER_EMBED::handleAttachToParent(
        const HANDLER_CONTEXT<AttachToParent>& aCtx )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    if( !embed.Attach( aCtx.Request.window(), aCtx.Request.toplevel(), aCtx.Request.scale() ) )
    {
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST,
                                        fmt::format( "window {} cannot be used as a parent",
                                                     aCtx.Request.window() ) ) );
    }

    AttachToParentResponse response;
    response.set_holder( embed.Holder() );

    for( const EMBED_FRAME_INFO& frame : embed.AllInfo() )
        fillFrame( response.add_frames(), frame );

    return response;
}


namespace
{
/// Open a frame of aKind through the manager (as KiCad's own buttons do).  With aShow false
/// it stays hidden and KiCad's own show requests for it are dropped silently.
bool openFrame( EMBED_FRAME_KIND aKind, bool aShow )
{
    BACKPLANE_EMBED&       embed = BACKPLANE_EMBED::Get();
    const EMBED_FRAME_OPS& managerOps = embed.ManagerOps();

    if( !managerOps.runAction )
        return false;

    if( !aShow )
        embed.m_quietShowRequests++;

    auto run = [&]( const std::function<void()>& aFunc )
               {
                   if( aShow )
                       embed.WithShowAllowed( aFunc );
                   else
                       aFunc();
               };

    if( aKind == EMBED_FRAME_KIND::PCB_3D )
    {
        wxFrame* pcb = embed.FindFrame( EMBED_FRAME_KIND::PCB );

        // Load the board without showing its editor; the 3D viewer belongs to it.
        if( !pcb )
        {
            embed.m_quietShowRequests++;
            managerOps.runAction( "kicad.Control.editPCB" );
            embed.m_quietShowRequests--;
            pcb = embed.FindFrame( EMBED_FRAME_KIND::PCB );
        }

        if( const EMBED_FRAME_OPS* pcbOps = pcb ? embed.Ops( pcb ) : nullptr; pcbOps && pcbOps->runAction )
            run( [&]() { pcbOps->runAction( "common.Control.show3DViewer" ); } );
    }
    else if( const char* action = managerActionFor( aKind ) )
    {
        run( [&]() { managerOps.runAction( action ); } );
    }

    if( !aShow )
        embed.m_quietShowRequests--;

    return embed.FindFrame( aKind ) != nullptr;
}


void fillAllFrames( google::protobuf::RepeatedPtrField<FrameInfo>* aOut )
{
    for( const EMBED_FRAME_INFO& frame : BACKPLANE_EMBED::Get().AllInfo() )
        fillFrame( aOut->Add(), frame );
}
} // namespace


HANDLER_RESULT<ShowFrameResponse> API_HANDLER_EMBED::handleShowFrame(
        const HANDLER_CONTEXT<ShowFrame>& aCtx )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();
    EMBED_FRAME_KIND kind = static_cast<EMBED_FRAME_KIND>( aCtx.Request.kind() );

    if( kind == EMBED_FRAME_KIND::UNKNOWN || kind == EMBED_FRAME_KIND::OTHER )
    {
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST,
                                        "ShowFrame needs one of FK_PCB, FK_SCH, FK_SYMBOL_EDITOR, "
                                        "FK_FOOTPRINT_EDITOR or FK_PCB_3D" ) );
    }

    wxFrame* frame = embed.FindFrame( kind );

    if( aCtx.Request.prewarm() )
    {
        // Open (if needed) and paint off-screen at the UI thread's next idle time; reply now.
        int w = aCtx.Request.width() > 0 ? aCtx.Request.width() : 1280;
        int h = aCtx.Request.height() > 0 ? aCtx.Request.height() : 800;

        if( wxFrame* manager = embed.Manager() )
        {
            manager->CallAfter(
                    [kind, w, h]()
                    {
                        BACKPLANE_EMBED& e = BACKPLANE_EMBED::Get();

                        if( BACKPLANE_EMBED::ModalBlocker() )
                            return;   // never open frames under a modal dialog

                        wxFrame* f = e.FindFrame( kind );

                        if( !f && openFrame( kind, false ) )
                            f = e.FindFrame( kind );

                        if( f && !f->IsShown() )
                            e.Prewarm( f, w, h );
                    } );
        }

        ShowFrameResponse response;

        if( frame )
            fillFrame( response.mutable_frame(), embed.Info( frame ) );
        else
            response.mutable_frame()->set_kind( aCtx.Request.kind() );

        fillAllFrames( response.mutable_frames() );
        return response;
    }

    // A board or schematic frame without its document (opened hidden by some other path)
    // goes through the manager, which loads the project's file.
    bool needsDocument = frame && ( kind == EMBED_FRAME_KIND::PCB || kind == EMBED_FRAME_KIND::SCH )
                         && embed.Info( frame ).document.IsEmpty();

    if( !frame || needsDocument )
    {
        if( std::optional<wxString> dialog = BACKPLANE_EMBED::ModalBlocker() )
        {
            return tl::unexpected( failure(
                    ApiStatusCode::AS_BUSY,
                    fmt::format( "KiCad is showing the dialog '{}'; a new frame cannot be opened "
                                 "until it is closed",
                                 dialog->ToStdString( wxConvUTF8 ) ) ) );
        }

        if( !embed.ManagerOps().runAction )
        {
            return tl::unexpected( failure( ApiStatusCode::AS_NOT_READY,
                                            "the project manager is not ready" ) );
        }

        openFrame( kind, true );
        frame = embed.FindFrame( kind );

        if( !frame )
        {
            return tl::unexpected( failure( ApiStatusCode::AS_INTERNAL_ERROR,
                                            "KiCad did not open the frame (see its log)" ) );
        }
    }

    if( aCtx.Request.width() > 0 && aCtx.Request.height() > 0 )
    {
        KIPLATFORM::EMBED::SetFrameGeometry( frame, aCtx.Request.x(), aCtx.Request.y(),
                                             aCtx.Request.width(), aCtx.Request.height() );
    }

    embed.ShowEmbedded( frame, true );

    const std::string& document = aCtx.Request.document();
    const std::string& item = aCtx.Request.item();

    if( kind == EMBED_FRAME_KIND::SCH && !document.empty() )
    {
        std::string payload = document;
        m_kiway.ExpressMail( FRAME_SCH, MAIL_SCH_NAVIGATE_TO_SHEET, payload );
    }

    if( !item.empty() )
    {
        std::string payload = item;

        switch( kind )
        {
        case EMBED_FRAME_KIND::PCB: crossProbe( m_kiway, FRAME_PCB_EDITOR, item ); break;
        case EMBED_FRAME_KIND::SCH: crossProbe( m_kiway, FRAME_SCH, item ); break;

        case EMBED_FRAME_KIND::SYMBOL_EDITOR:
            m_kiway.ExpressMail( FRAME_SCH_SYMBOL_EDITOR, MAIL_SCH_EDIT_LIBID, payload );
            break;

        case EMBED_FRAME_KIND::FOOTPRINT_EDITOR:
            m_kiway.ExpressMail( FRAME_FOOTPRINT_EDITOR, MAIL_FP_EDIT_LIBID, payload );
            break;

        default: break;
        }
    }

    if( aCtx.Request.focus() )
        KIPLATFORM::EMBED::TakeFocus( frame );

    ShowFrameResponse response;
    fillFrame( response.mutable_frame(), embed.Info( frame ) );
    fillAllFrames( response.mutable_frames() );
    return response;
}


HANDLER_RESULT<HideFrameResponse> API_HANDLER_EMBED::handleHideFrame(
        const HANDLER_CONTEXT<HideFrame>& aCtx )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();
    wxFrame* frame = embed.FindFrame( static_cast<EMBED_FRAME_KIND>( aCtx.Request.kind() ),
                                      aCtx.Request.window() );

    if( !frame )
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST, "no such frame" ) );

    embed.ShowEmbedded( frame, false );

    HideFrameResponse response;
    fillAllFrames( response.mutable_frames() );
    return response;
}


HANDLER_RESULT<GetFramesResponse> API_HANDLER_EMBED::handleGetFrames(
        const HANDLER_CONTEXT<GetFrames>& )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();
    GetFramesResponse response;

    for( const EMBED_FRAME_INFO& frame : embed.AllInfo() )
        fillFrame( response.add_frames(), frame );

    response.set_parent( embed.Parent() );
    response.set_toplevel( embed.Toplevel() );
    response.set_holder( embed.Holder() );
    response.set_scale( embed.Scale() );
    response.set_events_socket( embed.EventServerUrl() );
    response.set_theme( static_cast<ThemeMode>( embed.Theme() ) );

    for( const std::string& chord : embed.ReservedKeys() )
        response.add_reserved_keys( chord );

    return response;
}


HANDLER_RESULT<NextEventsResponse> API_HANDLER_EMBED::handleNextEvents(
        const HANDLER_CONTEXT<NextEvents>& aCtx )
{
    // The main socket serves one request at a time: never wait here (see events_socket).
    std::vector<EMBED_EVENT> events;
    bool                     lost = false;
    size_t max = aCtx.Request.max() > 0 ? static_cast<size_t>( aCtx.Request.max() ) : 256;
    uint64_t next = BACKPLANE_EMBED::Get().NextEvents( aCtx.Request.since(), 0, max, events, lost );

    NextEventsResponse response;
    response.set_next( next );
    response.set_lost( lost );

    for( const EMBED_EVENT& event : events )
    {
        FillEmbedEvent( response.add_events(), event );
    }

    return response;
}


HANDLER_RESULT<SetChromeResponse> API_HANDLER_EMBED::handleSetChrome(
        const HANDLER_CONTEXT<SetChrome>& aCtx )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();
    EMBED_CHROME     chrome = embed.Chrome();

    if( aCtx.Request.has_menubar() )
        chrome.menubar = aCtx.Request.menubar();

    if( aCtx.Request.has_top_toolbar() )
        chrome.topToolbar = aCtx.Request.top_toolbar();

    if( aCtx.Request.has_side_toolbars() )
        chrome.sideToolbars = aCtx.Request.side_toolbars();

    if( aCtx.Request.has_status_bar() )
        chrome.statusBar = aCtx.Request.status_bar();

    embed.SetChrome( chrome );

    SetChromeResponse response;
    response.set_menubar( chrome.menubar );
    response.set_top_toolbar( chrome.topToolbar );
    response.set_side_toolbars( chrome.sideToolbars );
    response.set_status_bar( chrome.statusBar );
    return response;
}


HANDLER_RESULT<SetReservedKeysResponse> API_HANDLER_EMBED::handleSetReservedKeys(
        const HANDLER_CONTEXT<SetReservedKeys>& aCtx )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();
    std::vector<std::string> chords( aCtx.Request.chords().begin(), aCtx.Request.chords().end() );
    wxString error;

    if( !embed.SetReservedKeys( chords, &error ) )
    {
        return tl::unexpected(
                failure( ApiStatusCode::AS_BAD_REQUEST, error.ToStdString( wxConvUTF8 ) ) );
    }

    SetReservedKeysResponse response;

    for( const std::string& chord : embed.ReservedKeys() )
        response.add_chords( chord );

    return response;
}


HANDLER_RESULT<CloseResponse> API_HANDLER_EMBED::handleClose( const HANDLER_CONTEXT<Close>& aCtx )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();
    bool             everything = aCtx.Request.kind() == FK_UNKNOWN && aCtx.Request.window() == 0;
    wxFrame*         single = nullptr;

    if( !everything )
    {
        single = embed.FindFrame( static_cast<EMBED_FRAME_KIND>( aCtx.Request.kind() ),
                                  aCtx.Request.window() );

        if( !single )
            return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST, "no such frame" ) );
    }

    CloseResponse            response;
    std::vector<std::string> modifiedTitles;

    for( const EMBED_FRAME_INFO& frame : embed.AllInfo() )
    {
        if( frame.modified && ( everything || frame.window == embed.Info( single ).window ) )
        {
            fillFrame( response.add_modified(), frame );
            modifiedTitles.push_back( frame.title.ToStdString( wxConvUTF8 ) );
        }
    }

    if( aCtx.Request.mode() == CM_FAIL_IF_MODIFIED && !modifiedTitles.empty() )
    {
        std::string list;

        for( const std::string& title : modifiedTitles )
            list += ( list.empty() ? "" : ", " ) + ( "'" + title + "'" );

        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST,
                                        "unsaved changes in " + list ) );
    }

    // KiCad's own close paths run (canCloseWindow, autosave bookkeeping), and their "save
    // changes?" prompts are answered silently with the host's choice.
    embed.SetUnsavedChangesAnswer( aCtx.Request.mode() == CM_SAVE ? wxID_YES : wxID_NO );

    bool closed = false;

    if( everything )
        closed = m_kiway.PlayersClose( false );
    else
        closed = single->Close( false );

    embed.SetUnsavedChangesAnswer( std::nullopt );

    if( !closed )
    {
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST,
                                        "KiCad did not close (a save failed or a frame is busy)" ) );
    }

    if( everything )
    {
        response.set_exiting( true );

        // After the reply has gone out: closing the hidden project manager ends the process.
        if( wxFrame* manager = embed.Manager() )
        {
            manager->CallAfter(
                    [manager]()
                    {
                        BACKPLANE_EMBED::Get().SetUnsavedChangesAnswer( wxID_NO );
                        manager->Close( true );
                    } );
        }
    }

    return response;
}


HANDLER_RESULT<SetThemeResponse> API_HANDLER_EMBED::handleSetTheme( const HANDLER_CONTEXT<SetTheme>& aCtx )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    if( aCtx.Request.mode() < TM_KICAD || aCtx.Request.mode() > TM_DARK )
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST, "unknown theme mode" ) );

    embed.SetTheme( static_cast<EMBED_THEME>( aCtx.Request.mode() ) );

    SetThemeResponse response;
    response.set_mode( static_cast<ThemeMode>( embed.Theme() ) );
    response.set_gtk_dark( embed.GtkDark() );

    wxString colorTheme = embed.ColorThemeOverride();

    if( colorTheme.IsEmpty() )
        colorTheme = wxS( "(the person's own)" );

    response.set_color_theme( colorTheme.ToStdString( wxConvUTF8 ) );

    bool darkIcons = embed.Theme() == EMBED_THEME::DARK_MODE
                     || ( embed.Theme() == EMBED_THEME::OWN_SETTINGS && embed.GtkDark() );
    response.set_icon_theme( darkIcons ? "dark" : "light" );
    return response;
}


namespace
{
wxFrame* frameForDocument( FrameKind aKind, const std::string& aPath )
{
    BACKPLANE_EMBED& embed = BACKPLANE_EMBED::Get();

    if( !aPath.empty() )
        return embed.FindFrameByFile( wxString::FromUTF8( aPath ) );

    return embed.FindFrame( static_cast<EMBED_FRAME_KIND>( aKind ) );
}
} // namespace


HANDLER_RESULT<ReloadDocumentResponse> API_HANDLER_EMBED::handleReloadDocument(
        const HANDLER_CONTEXT<ReloadDocument>& aCtx )
{
    wxFrame* frame = frameForDocument( aCtx.Request.kind(), aCtx.Request.path() );

    if( !frame )
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST, "no frame has that document" ) );

    BACKPLANE_EMBED&    embed = BACKPLANE_EMBED::Get();
    EMBED_FRAME_KIND    kind = embed.Info( frame ).kind;

    if( kind == EMBED_FRAME_KIND::PCB_3D || kind == EMBED_FRAME_KIND::OTHER )
    {
        return tl::unexpected( failure( ApiStatusCode::AS_UNIMPLEMENTED,
                                        "this frame has no document to reload" ) );
    }

    EMBED_RELOAD_RESULT result = embed.Reload( frame, aCtx.Request.mode() == RM_DISCARD );

    if( result == EMBED_RELOAD_RESULT::UNKNOWN )
        return tl::unexpected( failure( ApiStatusCode::AS_INTERNAL_ERROR, "the reload failed (see KiCad's log)" ) );

    ReloadDocumentResponse response;
    response.set_result( static_cast<ReloadResult>( result ) );

    if( embed.IsEmbedded( frame ) )
        fillFrame( response.mutable_frame(), embed.Info( frame ) );

    return response;
}


HANDLER_RESULT<ResolveConflictResponse> API_HANDLER_EMBED::handleResolveConflict(
        const HANDLER_CONTEXT<ResolveConflict>& aCtx )
{
    if( aCtx.Request.choice() != CC_KEEP && aCtx.Request.choice() != CC_RELOAD )
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST, "choice must be CC_KEEP or CC_RELOAD" ) );

    wxFrame* frame = frameForDocument( aCtx.Request.kind(), aCtx.Request.path() );

    if( !frame )
        return tl::unexpected( failure( ApiStatusCode::AS_BAD_REQUEST, "no frame has that document" ) );

    BACKPLANE_EMBED&    embed = BACKPLANE_EMBED::Get();
    EMBED_RELOAD_RESULT result = embed.ResolveConflict( frame, aCtx.Request.choice() == CC_RELOAD );

    if( result == EMBED_RELOAD_RESULT::UNKNOWN )
        return tl::unexpected( failure( ApiStatusCode::AS_INTERNAL_ERROR, "the reload failed (see KiCad's log)" ) );

    ResolveConflictResponse response;
    response.set_result( static_cast<ReloadResult>( result ) );

    if( embed.IsEmbedded( frame ) )
        fillFrame( response.mutable_frame(), embed.Info( frame ) );

    return response;
}
