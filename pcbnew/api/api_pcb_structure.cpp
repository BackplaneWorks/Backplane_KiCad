/*
 * Board physical construction and rigid-flex viewer IPC.
 *
 * The construction model is stored in the fork companion file.  IPC uses the
 * model's canonical JSON representation so clients can round-trip fields that
 * this build does not otherwise need to understand.
 */

#include <api/api_handler_pcb.h>
#include <api/backplane/structure.pb.h>

#include <3d_viewer/eda_3d_viewer_frame.h>
#include <board.h>
#include <board_structure.h>
#include <dialogs/appearance_controls_3D.h>
#include <3d_viewer/eda_3d_viewer_settings.h>
#include <pcb_base_frame.h>
#include <pcb_edit_frame.h>
#include <pcb_draw_panel_gal.h>
#include <widgets/appearance_controls.h>

#include <json_common.h>

using namespace kiapi::backplane;

namespace
{
HANDLER_RESULT<BoardStructureResponse> makeStructureResponse( const BOARD_STRUCTURE& aStructure )
{
    BoardStructureResponse response;
    response.set_json( aStructure.ToJson().dump() );

    const BOARD_STRUCTURE_VALIDATION validation = aStructure.Validate();
    response.set_valid( validation.IsValid() );

    for( const std::string& error : validation.errors )
        response.add_errors( error );

    for( const std::string& warning : validation.warnings )
        response.add_warnings( warning );

    return response;
}


HANDLER_RESULT<BOARD_STRUCTURE> parseStructure( const std::string& aJson )
{
    try
    {
        BOARD_STRUCTURE structure;
        structure.FromJson( nlohmann::json::parse( aJson ) );

        if( BOARD_STRUCTURE_VALIDATION validation = structure.Validate(); !validation.IsValid() )
        {
            ApiResponseStatus status;
            status.set_status( ApiStatusCode::AS_BAD_REQUEST );
            status.set_error_message( validation.errors.front() );
            return tl::unexpected( status );
        }

        return structure;
    }
    catch( const std::exception& error )
    {
        ApiResponseStatus status;
        status.set_status( ApiStatusCode::AS_BAD_REQUEST );
        status.set_error_message( error.what() );
        return tl::unexpected( status );
    }
}


} // namespace


HANDLER_RESULT<BoardStructureResponse> API_HANDLER_PCB::handleGetBoardStructure(
        const HANDLER_CONTEXT<GetBoardStructure>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
        return tl::unexpected( documentValidation.error() );

    return makeStructureResponse( board()->GetBoardStructure() );
}


HANDLER_RESULT<BoardStructureResponse> API_HANDLER_PCB::handleValidateBoardStructure(
        const HANDLER_CONTEXT<ValidateBoardStructure>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
        return tl::unexpected( documentValidation.error() );

    HANDLER_RESULT<BOARD_STRUCTURE> parsed = parseStructure( aCtx.Request.json() );

    if( !parsed )
        return tl::unexpected( parsed.error() );

    return makeStructureResponse( *parsed );
}


HANDLER_RESULT<BoardStructureResponse> API_HANDLER_PCB::handleSetBoardStructure(
        const HANDLER_CONTEXT<SetBoardStructure>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
        return tl::unexpected( documentValidation.error() );

    HANDLER_RESULT<BOARD_STRUCTURE> parsed = parseStructure( aCtx.Request.json() );

    if( !parsed )
        return tl::unexpected( parsed.error() );

    BoardStructureResponse response = *makeStructureResponse( *parsed );

    if( aCtx.Request.validate_only() )
        return response;

    if( !m_activeClients.empty() )
    {
        ApiResponseStatus status;
        status.set_status( ApiStatusCode::AS_BAD_REQUEST );
        status.set_error_message( "Construction changes cannot participate in an item edit transaction" );
        return tl::unexpected( status );
    }

    // Parse and validate into a temporary before assigning, so malformed or
    // physically invalid requests cannot partially alter the live board.
    if( frame() )
        frame()->CommitBoardStructure( *parsed, _( "Edit board structure through IPC" ) );
    else
    {
        board()->GetBoardStructure() = *parsed;
        board()->IncrementTimeStamp();
        onModified();
    }

    if( auto* frame = dynamic_cast<PCB_BASE_FRAME*>( m_frame ) )
        frame->Update3DView( true, true );
    else if( m_frame )
        m_frame->Refresh();

    return response;
}


HANDLER_RESULT<ViewerConstructionStateResponse> API_HANDLER_PCB::handleGetViewerConstructionState(
        const HANDLER_CONTEXT<GetViewerConstructionState>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
        return tl::unexpected( documentValidation.error() );

    ViewerConstructionStateResponse response;
    PCB_BASE_FRAME* frame = dynamic_cast<PCB_BASE_FRAME*>( m_frame );
    EDA_3D_VIEWER_FRAME* viewer = frame ? frame->GetOur3DViewerFrame() : nullptr;
    response.set_viewer_open( viewer != nullptr );

    if( viewer )
    {
        response.set_construction_view( viewer->GetAdapter().GetConstructionView() );
        response.set_folded_view( viewer->GetAdapter().GetFoldedView() );
    }

    return response;
}


HANDLER_RESULT<ViewerConstructionStateResponse> API_HANDLER_PCB::handleSetViewerConstructionState(
        const HANDLER_CONTEXT<SetViewerConstructionState>& aCtx )
{
    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
        return tl::unexpected( documentValidation.error() );

    PCB_BASE_FRAME* frame = dynamic_cast<PCB_BASE_FRAME*>( m_frame );
    EDA_3D_VIEWER_FRAME* viewer = frame ? frame->GetOur3DViewerFrame() : nullptr;

    if( !viewer && frame && aCtx.Request.open_if_needed() )
    {
        frame->CreateAndShow3D_Frame();
        viewer = frame->GetOur3DViewerFrame();
    }

    if( !viewer )
    {
        ApiResponseStatus status;
        status.set_status( ApiStatusCode::AS_BAD_REQUEST );
        status.set_error_message( "3D viewer is not open" );
        return tl::unexpected( status );
    }

    if( aCtx.Request.construction_view()
        && ( board()->GetBoardStructure().regions.empty()
             || viewer->GetAdapter().m_Cfg->m_Render.engine != RENDER_ENGINE::OPENGL ) )
    {
        ApiResponseStatus status;
        status.set_status( ApiStatusCode::AS_BAD_REQUEST );
        status.set_error_message( "Construction view requires regional construction and the OpenGL renderer" );
        return tl::unexpected( status );
    }

    // A newly created viewer may not have handled its first paint yet.
    // Bind its board before setting transient presentation flags, otherwise
    // the first ReloadRequest would reset the requested folded state.
    viewer->GetAdapter().SetBoard( board() );
    viewer->GetAdapter().SetConstructionView( aCtx.Request.construction_view() );
    viewer->GetAdapter().SetFoldedView( aCtx.Request.construction_view()
                                        && aCtx.Request.folded_view() );
    viewer->GetAppearanceManager()->UpdateLayerCtls();
    viewer->ReloadRequest();
    viewer->Redraw();

    ViewerConstructionStateResponse response;
    response.set_viewer_open( true );
    response.set_construction_view( viewer->GetAdapter().GetConstructionView() );
    response.set_folded_view( viewer->GetAdapter().GetFoldedView() );
    return response;
}

namespace
{
ConstructionLayerStateResponse layerState( PCB_EDIT_FRAME* aFrame )
{
    ConstructionLayerStateResponse response;
    auto* canvas = aFrame->GetCanvas();
    response.set_active_layer( static_cast<ConstructionLayer>( canvas->GetActiveConstructionLayer() ) );
    for( int i = 1; i <= 4; ++i )
    {
        auto* item = response.add_visibility();
        item->set_layer( static_cast<ConstructionLayer>( i ) );
        item->set_visible( canvas->IsStructureVisible(
                ConstructionLayerCategory( static_cast<CONSTRUCTION_LAYER>( i ) ) )
                && ( i < 3 || canvas->IsStructureVisible( 2 ) ) );
    }
    return response;
}
ApiResponseStatus constructionError( const std::string& aMessage )
{
    ApiResponseStatus status;
    status.set_status( ApiStatusCode::AS_BAD_REQUEST );
    status.set_error_message( aMessage );
    return status;
}
}

HANDLER_RESULT<ConstructionLayerStateResponse> API_HANDLER_PCB::handleGetConstructionLayerState(
        const HANDLER_CONTEXT<GetConstructionLayerState>& aCtx )
{
    if( auto busy = checkForBusy() ) return tl::unexpected( *busy );
    if( auto valid = validateDocument( aCtx.Request.board() ); !valid )
        return tl::unexpected( valid.error() );
    if( !frame() ) return tl::unexpected( constructionError( "Construction layers require a PCB editor" ) );
    return layerState( frame() );
}

HANDLER_RESULT<ConstructionLayerStateResponse> API_HANDLER_PCB::handleSetConstructionLayerState(
        const HANDLER_CONTEXT<SetConstructionLayerState>& aCtx )
{
    if( auto busy = checkForBusy() ) return tl::unexpected( *busy );
    if( auto valid = validateDocument( aCtx.Request.board() ); !valid )
        return tl::unexpected( valid.error() );
    if( !frame() ) return tl::unexpected( constructionError( "Construction layers require a PCB editor" ) );
    const auto& request = aCtx.Request;
    if( request.has_active_layer() && ( request.active_layer() < CL_NONE || request.active_layer() > CL_B_STIFFENER ) )
        return tl::unexpected( constructionError( "Unknown construction layer" ) );
    for( const auto& item : request.visibility() )
        if( item.layer() <= CL_NONE || item.layer() > CL_B_STIFFENER )
            return tl::unexpected( constructionError( "Unknown construction visibility layer" ) );
    if( request.has_active_layer() )
        frame()->GetAppearancePanel()->SelectConstructionLayer(
                static_cast<CONSTRUCTION_LAYER>( request.active_layer() ) );
    auto* canvas = frame()->GetCanvas();
    for( const auto& item : request.visibility() )
    {
        if( item.visible() && item.layer() >= CL_F_STIFFENER ) canvas->SetStructureVisible( 2, true );
        canvas->SetStructureVisible( ConstructionLayerCategory(
                static_cast<CONSTRUCTION_LAYER>( item.layer() ) ), item.visible() );
    }
    frame()->GetAppearancePanel()->OnLayerChanged();
    return layerState( frame() );
}

HANDLER_RESULT<BoardStructureResponse> API_HANDLER_PCB::handleDeriveBoardStructurePartition(
        const HANDLER_CONTEXT<DeriveBoardStructurePartition>& aCtx )
{
    if( auto busy = checkForBusy() ) return tl::unexpected( *busy );
    if( auto valid = validateDocument( aCtx.Request.board() ); !valid )
        return tl::unexpected( valid.error() );
    if( !aCtx.Request.validate_only() && !m_activeClients.empty() )
        return tl::unexpected( constructionError( "Construction changes cannot participate in an item edit transaction" ) );
    BOARD_STRUCTURE next = board()->GetBoardStructure();
    try
    {
        if( !aCtx.Request.json().empty() ) next.FromJson( nlohmann::json::parse( aCtx.Request.json() ) );
        SHAPE_POLY_SET outline;
        if( !board()->GetBoardPolygonOutlines( outline, false ) )
            return tl::unexpected( constructionError( "A closed valid Edge.Cuts outline is required" ) );
        std::string error;
        if( !next.DerivePartition( outline, &error ) ) return tl::unexpected( constructionError( error ) );
        const auto validation = next.Validate();
        if( !validation.IsValid() ) return tl::unexpected( constructionError( validation.errors.front() ) );
        if( !aCtx.Request.validate_only() )
        {
            if( frame() ) frame()->CommitBoardStructure( next, _( "Rebuild stackup boundaries through IPC" ) );
            else { board()->GetBoardStructure() = next; board()->IncrementTimeStamp(); onModified(); }
        }
        return makeStructureResponse( next );
    }
    catch( const std::exception& error ) { return tl::unexpected( constructionError( error.what() ) ); }
}
