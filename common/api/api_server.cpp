/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#include <fmt/format.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <wx/app.h>
#include <wx/datetime.h>
#include <wx/event.h>
#include <wx/stdpaths.h>

#include <advanced_config.h>
#include <backplane_embed.h>
#include <api/api_handler.h>
#include <api/api_task_scheduler.h>
#include <api/api_utils.h> // traceApi
#include <api/api_server.h>
#include <set>
#include <backplane_coedit.h>
#include <kiid.h>
#include <kinng.h>
#include <paths.h>
#include <pgm_base.h>
#include <settings/common_settings.h>
#include <string_utils.h>

#include <api/common/envelope.pb.h>
#include <api/backplane/embed.pb.h>

#ifdef __UNIX__
#include <sys/file.h>
#endif

using kiapi::common::ApiRequest, kiapi::common::ApiResponse, kiapi::common::ApiStatusCode;


wxString KICAD_API_SERVER::s_logFileName = "api.log";


wxDEFINE_EVENT( API_REQUEST_EVENT, KICAD_API_REQUEST_EVENT );


/**
 * Requests that only read state, and so may run while a modal dialog's nested event loop is
 * dispatching (a changing request there could pull a document out from under the dialog).
 */
static bool isReadOnlyRequest( const std::string& aType )
{
    static const std::string embedPackage = "kiapi.backplane.embed.";

    if( aType.compare( 0, embedPackage.size(), embedPackage ) == 0 )
    {
        // Window placement and the event feed must keep working; ShowFrame checks itself
        // whether it would have to open a frame.
        for( const char* changing : { "Close", "SelectItems", "ReloadDocument", "ResolveConflict",
                                       "CommentSelection" } )
        {
            if( aType == embedPackage + changing )
                return false;
        }

        return true;
    }

    std::string name = aType.substr( aType.rfind( '.' ) + 1 );

    for( const char* prefix : { "Get", "List", "Ping", "HitTest", "Check", "Expand" } )
    {
        if( name.compare( 0, std::strlen( prefix ), prefix ) == 0 )
            return true;
    }

    return false;
}


KICAD_API_SERVER::KICAD_API_SERVER( bool aAutoStart ) :
        wxEvtHandler(),
        m_token( KIID().AsStdString() ),
        m_readyToReply( false )
{
    if( !aAutoStart )
        return;

    if( !Pgm().GetCommonSettings()->m_Api.enable_server )
    {
        wxLogTrace( traceApi, "Server: disabled by user preferences." );
        return;
    }

    Start();
}


KICAD_API_SERVER::~KICAD_API_SERVER()
{
    Stop();
}


wxFileName KICAD_API_SERVER::StandardSocketPath()
{
    wxFileName socket;

#ifdef __WXMAC__
    socket.AssignDir( wxS( "/tmp" ) );
#else
    socket.AssignDir( wxStandardPaths::Get().GetTempDir() );
#endif

    socket.AppendDir( wxS( "kicad" ) );
    socket.SetFullName( wxS( "api.sock" ) );

    return socket;
}


std::string KICAD_API_SERVER::StandardSocketUrl()
{
    return fmt::format( "ipc://{}", StandardSocketPath().GetFullPath().ToUTF8().data() );
}


bool KICAD_API_SERVER::Start()
{
    if( Running() )
        return true;

    wxFileName socket;

    if( m_socketPathOverride.IsEmpty() )
    {
        socket = StandardSocketPath();
    }
    else
    {
        socket.Assign( m_socketPathOverride );

        if( !socket.IsAbsolute() )
            socket.MakeAbsolute();
    }

    if( !PATHS::EnsurePathExists( socket.GetPath() ) )
    {
        wxLogTrace( traceApi, wxString::Format( "Server: socket path %s could not be created",
                                                socket.GetPath() ) );
        return false;
    }

#ifndef __WINDOWS__
    // We use non-abstract sockets because macOS and some other non-Linux platforms don't support
    // abstract sockets, which means there might be an old socket to unlink.  In order to try to
    // recover this, we lock a file (which will be unlocked on process exit) and if we get the lock,
    // we know the old socket is orphaned and can be removed.
    wxFileName lockFilePath( socket.GetPath(), wxS( "api.lock" ) );

    int lockFile = open( lockFilePath.GetFullPath().c_str(), O_RDONLY | O_CREAT, 0600 );

    if( lockFile >= 0 && flock( lockFile, LOCK_EX | LOCK_NB ) == 0 )
    {
        if( socket.Exists() )
        {
            wxLogTrace( traceApi, wxString::Format( "Server: cleaning up stale socket path %s",
                                                    socket.GetFullPath() ) );
            wxRemoveFile( socket.GetFullPath() );
        }
    }
#endif

    if( socket.Exists() )
    {
        // A caller that chose the path (and tells its clients about it) must not silently get
        // a different one.
        if( m_socketPathStrict )
        {
            wxLogTrace( traceApi, wxString::Format( "Server: socket path %s is in use",
                                                    socket.GetFullPath() ) );
            return false;
        }

        socket.SetFullName( wxString::Format( wxS( "api-%lu.sock" ), ::wxGetProcessId() ) );

        if( socket.Exists() )
        {
            wxLogTrace( traceApi, wxString::Format( "Server: PID socket path %s already exists!",
                                                    socket.GetFullPath() ) );
            return false;
        }
    }

    m_server = std::make_unique<KINNG_REQUEST_SERVER>(
            fmt::format( "ipc://{}", socket.GetFullPath().ToStdString() ) );
    m_server->SetCallback( [&]( std::string* aRequest ) { onApiRequest( aRequest ); } );

    if( !m_server->Start() )
    {
        wxLogTrace( traceApi, "Server: failed to start KINNG listener thread" );
        m_server.reset( nullptr );
        return false;
    }

    m_logFilePath.AssignDir( PATHS::GetLogsPath() );
    m_logFilePath.SetName( s_logFileName );

    if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
    {
        PATHS::EnsurePathExists( PATHS::GetLogsPath() );
        log( fmt::format( "--- KiCad API server started at {} ---\n", SocketPath() ) );
    }

    wxLogTrace( traceApi, wxString::Format( "Server: listening at %s", SocketPath() ) );
    Bind( API_REQUEST_EVENT, &KICAD_API_SERVER::handleApiEvent, this );
    return true;
}


void KICAD_API_SERVER::Stop()
{
    if( !Running() )
        return;

    wxLogTrace( traceApi, "Stopping server" );
    Unbind( API_REQUEST_EVENT, &KICAD_API_SERVER::handleApiEvent, this );

    m_server->Stop();
    m_server.reset( nullptr );
}


bool KICAD_API_SERVER::Running() const
{
    return m_server && m_server->Running();
}


std::vector<std::string> KICAD_API_SERVER::RegisteredCommands() const
{
    std::vector<std::string> commands;

    for( API_HANDLER* handler : m_handlers )
    {
        std::vector<std::string> handlerCommands = handler->RegisteredCommands();
        commands.insert( commands.end(), handlerCommands.begin(), handlerCommands.end() );
    }

    std::sort( commands.begin(), commands.end() );
    commands.erase( std::unique( commands.begin(), commands.end() ), commands.end() );
    return commands;
}


void KICAD_API_SERVER::RegisterHandler( API_HANDLER* aHandler )
{
    wxCHECK( aHandler, /* void */ );

    aHandler->setApiServer( this );
    m_handlers.insert( aHandler );
}


void KICAD_API_SERVER::DeregisterHandler( API_HANDLER* aHandler )
{
    if( aHandler )
        aHandler->setApiServer( nullptr );

    m_handlers.erase( aHandler );
}


void KICAD_API_SERVER::NotifyNetSettingsChanged()
{
    for( API_HANDLER* handler : m_handlers )
        handler->onNetSettingsChanged();
}


void KICAD_API_SERVER::NotifyDocumentRenamed( kiapi::common::types::DocumentType aType,
                                              const wxString& aOldPath, const wxString& aNewPath )
{
    if( m_renameListener )
        m_renameListener( aType, aOldPath, aNewPath );
}


std::string KICAD_API_SERVER::SocketPath() const
{
    return m_server ? m_server->SocketPath() : "";
}


void KICAD_API_SERVER::onApiRequest( std::string* aRequest )
{
    if( !m_readyToReply.load( std::memory_order_acquire ) )
    {
        ApiResponse notHandled;
        notHandled.mutable_status()->set_status( ApiStatusCode::AS_NOT_READY );
        notHandled.mutable_status()->set_error_message( "KiCad is not ready to reply" );
        m_server->Reply( notHandled.SerializeAsString() );
        log( "Got incoming request but was not yet ready to reply." );
        return;
    }

    // CancelProgress is answered here, on the server's thread: the job it stops is what keeps
    // the UI thread busy (BACKPLANE_EMBED::RequestCancel is thread-safe).
    if( aRequest->find( "CancelProgress" ) != std::string::npos )
    {
        ApiRequest  request;
        std::string type;

        if( request.ParseFromString( *aRequest )
            && google::protobuf::Any::ParseAnyTypeUrl( request.message().type_url(), &type )
            && type == "kiapi.backplane.embed.CancelProgress" )
        {
            ApiResponse response;
            response.mutable_header()->set_kicad_token( m_token );
            kiapi::backplane::embed::CancelProgress cancel;

            if( !request.header().kicad_token().empty() && request.header().kicad_token() != m_token )
            {
                response.mutable_status()->set_status( ApiStatusCode::AS_TOKEN_MISMATCH );
                response.mutable_status()->set_error_message(
                        "the provided kicad_token did not match this KiCad instance's token" );
            }
            else if( request.message().UnpackTo( &cancel )
                     && BACKPLANE_EMBED::Get().RequestCancel( cancel.id() ) )
            {
                kiapi::backplane::embed::CancelProgressResponse reply;
                reply.set_cancelling( true );
                response.mutable_status()->set_status( ApiStatusCode::AS_OK );
                response.mutable_message()->PackFrom( reply );
            }
            else
            {
                response.mutable_status()->set_status( ApiStatusCode::AS_BAD_REQUEST );
                response.mutable_status()->set_error_message(
                        "no running cancellable job with that id" );
            }

            m_server->Reply( response.SerializeAsString() );
            return;
        }
    }

    KICAD_API_REQUEST_EVENT* evt = new KICAD_API_REQUEST_EVENT( API_REQUEST_EVENT );

    // We don't actually need write access to this string, but client data is non-const
    evt->SetClientData( static_cast<void*>( aRequest ) );

    // Takes ownership and frees the wxCommandEvent
    QueueEvent( evt );
}


void KICAD_API_SERVER::handleApiEvent( wxCommandEvent& aEvent )
{
    std::string& requestString = *static_cast<std::string*>( aEvent.GetClientData() );
    handleApiRequestString( requestString );
}


void KICAD_API_SERVER::handleApiRequestString( std::string& aRequestString )
{
    std::unique_ptr<BACKPLANE_EMBED::TRACE_SCOPE> trace;

    m_handledRequestCount.fetch_add( 1, std::memory_order_relaxed );

    ApiRequest request;

    if( !request.ParseFromString( aRequestString ) )
    {
        ApiResponse error;
        error.mutable_header()->set_kicad_token( m_token );
        error.mutable_status()->set_status( ApiStatusCode::AS_BAD_REQUEST );
        error.mutable_status()->set_error_message( "request could not be parsed" );
        m_server->Reply( error.SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response (ERROR): " + error.Utf8DebugString() );

        return;
    }

    if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
        log( "Request: " + request.Utf8DebugString() );

    if( BACKPLANE_EMBED::Get().Tracing() )
    {
        std::string type = request.message().type_url();
        trace = std::make_unique<BACKPLANE_EMBED::TRACE_SCOPE>( "ipc:" + type.substr( type.rfind( '.' ) + 1 ) );
    }

    if( !request.header().kicad_token().empty() &&
        request.header().kicad_token().compare( m_token ) != 0 )
    {
        ApiResponse error;
        error.mutable_header()->set_kicad_token( m_token );
        error.mutable_status()->set_status( ApiStatusCode::AS_TOKEN_MISMATCH );
        error.mutable_status()->set_error_message(
                "the provided kicad_token did not match this KiCad instance's token" );
        m_server->Reply( error.SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response (ERROR): " + error.Utf8DebugString() );

        return;
    }

    // Only allow-listed API requests can be handled while a task is running; anything else
    // is assumed to possible mess with state that the task might be depending on not changing.
    if( request.has_message() && API_TASK_SCHEDULER::Instance().AnyTaskRunning() )
    {
        std::string requestType;

        if( google::protobuf::Any::ParseAnyTypeUrl( request.message().type_url(), &requestType ) )
        {
            bool typeHandled = false;
            bool allowedDuringTask = false;

            for( API_HANDLER* handler : m_handlers )
            {
                if( handler->CanHandle( requestType ) )
                {
                    typeHandled = true;
                    allowedDuringTask = handler->AllowsDuringTask( requestType );
                    break;
                }
            }

            if( typeHandled && !allowedDuringTask )
            {
                ApiResponse error;
                error.mutable_header()->set_kicad_token( m_token );
                error.mutable_status()->set_status( ApiStatusCode::AS_BUSY );
                error.mutable_status()->set_error_message(
                        "a task is running; only task status and result queries are available until it completes" );
                m_server->Reply( error.SerializeAsString() );

                if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
                    log( "Response (ERROR): " + error.Utf8DebugString() );

                return;
            }
        }
    }

    // A modal or quasi-modal dialog runs a nested event loop, which also dispatches API requests.
    // Changing state from inside it can invalidate what the dialog is editing, so only reads
    // are served until it closes.
    if( request.has_message() )
    {
        std::string requestType;

        if( google::protobuf::Any::ParseAnyTypeUrl( request.message().type_url(), &requestType )
            && !isReadOnlyRequest( requestType ) )
        {
            // Co-editing (docs/backplane/coedit.md): an ordinary modal only holds what it
            // edits (the editors' handlers defer those items); only a structural dialog, one
            // that rewrites the document wholesale, refuses every write.
            std::optional<COEDIT::MODAL> modal = COEDIT::OpenModal();
            std::optional<wxString>      dialog;

            // Under an ordinary dialog only the hold-aware item operations go through (they defer
            // what the dialog may be editing); tool actions and other changes still wait.
            static const std::set<std::string> holdAware = {
                "kiapi.common.commands.CreateItems",
                "kiapi.common.commands.UpdateItems",
                "kiapi.common.commands.DeleteItems",
                "kiapi.common.commands.BeginCommit",
                "kiapi.common.commands.EndCommit",
                "kiapi.backplane.coedit.SetAgentActivity",
                "kiapi.backplane.coedit.EditCustomRules",
            };

            if( modal && ( modal->structural || !holdAware.count( requestType ) ) )
                dialog = modal->title;

            if( dialog )
            {
                ApiResponse error;
                error.mutable_header()->set_kicad_token( m_token );
                error.mutable_status()->set_status( ApiStatusCode::AS_BUSY );
                error.mutable_status()->set_error_message( fmt::format(
                        "KiCad is showing the dialog '{}'; only reads and item edits are served "
                        "until it is closed",
                        dialog->ToStdString( wxConvUTF8 ) ) );
                m_server->Reply( error.SerializeAsString() );

                if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
                    log( "Response (ERROR): " + error.Utf8DebugString() );

                return;
            }
        }
    }

    API_RESULT result;
    bool notifyNetSettings = false;

    // An exception escaping a handler would otherwise be swallowed by the event loop and the
    // client would wait for a reply forever.  Answer with an internal error instead.
    auto internalError =
            [&]( const std::string& aWhat )
            {
                ApiResponseStatus status;
                status.set_status( ApiStatusCode::AS_INTERNAL_ERROR );
                status.set_error_message( fmt::format( "{} failed: {}",
                                                       request.message().type_url(), aWhat ) );
                result = tl::unexpected( status );
            };

    // Every change made while handling this request is the client's (its undo group, its
    // change-log entry, its attribution).
    std::string actor = request.header().client_name();

    if( actor.empty() )
        actor = "api";

    // A client named "person:<anything>" acts for the person (Backplane's own UI: a BOM edit, a
    // panel's button): the person's change, in their undo.  Every other name is an actor of its own
    // ("agent:<thread>" for agents), with its own undo, as before.
    if( actor.rfind( "person:", 0 ) == 0 )
        actor.clear();

    COEDIT::ACTOR_SCOPE actorScope( actor );

    std::string handledType;
    google::protobuf::Any::ParseAnyTypeUrl( request.message().type_url(), &handledType );
    COEDIT::READ_SCOPE  readScope( !handledType.empty() && isReadOnlyRequest( handledType ) );

    try
    {
        for( API_HANDLER* handler : m_handlers )
        {
            result = handler->Handle( request );
            notifyNetSettings |= handler->clearNetSettingsNotification();

            if( result.has_value() )
                break;
            else if( result.error().status() != ApiStatusCode::AS_UNHANDLED )
                break;
        }
    }
    catch( const std::exception& e )
    {
        internalError( e.what() );
    }
    catch( ... )
    {
        internalError( "unknown exception" );
    }

    if( notifyNetSettings )
        NotifyNetSettingsChanged();

    // Co-editing: what this client has seen (an update it sends later is judged against it).
    if( result.has_value() )
    {
        std::string requestType;

        if( google::protobuf::Any::ParseAnyTypeUrl( request.message().type_url(), &requestType )
            && requestType.find( ".Get" ) != std::string::npos )
        {
            COEDIT::NoteRead( actor );
        }
    }

    // Note: at the point we call Reply(), we no longer own requestString.

    if( result.has_value() )
    {
        result->mutable_header()->set_kicad_token( m_token );
        m_server->Reply( result->SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response: " + result->Utf8DebugString() );
    }
    else
    {
        ApiResponse error;
        error.mutable_status()->CopyFrom( result.error() );
        error.mutable_header()->set_kicad_token( m_token );

        if( result.error().status() == ApiStatusCode::AS_UNHANDLED )
        {
            std::string type = "<unparseable Any>";
            google::protobuf::Any::ParseAnyTypeUrl( request.message().type_url(), &type );
            std::string msg = fmt::format( "no handler available for request of type {}", type );
            error.mutable_status()->set_error_message( msg );
        }

        m_server->Reply( error.SerializeAsString() );

        if( ADVANCED_CFG::GetCfg().m_EnableAPILogging )
            log( "Response (ERROR): " + error.Utf8DebugString() );
    }
}


void KICAD_API_SERVER::log( const std::string& aOutput )
{
    FILE* fp = wxFopen( m_logFilePath.GetFullPath(), wxT( "a" ) );

    if( !fp )
        return;

    wxString out;
    wxDateTime now = wxDateTime::Now();

    fprintf( fp, "%s", TO_UTF8( out.Format( wxS( "%s: %s" ),
                                            now.FormatISOCombined(), aOutput ) ) );
    fclose( fp );
}
