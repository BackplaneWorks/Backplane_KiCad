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

#include <drc/drc_engine.h>
#include <fstream>
#include <kiplatform/io.h>
#include <backplane_coedit.h>
#include <backplane_merge.h>
#include <backplane_embed.h>
#include <backplane_embed_proto.h>
#include <backplane_comment_pcb.h>
#include <magic_enum.hpp>
#include <memory>
#include <pgm_base.h>
#include <properties/property.h>
#include <ranges>

#include <common.h>
#include <fmt.h>
#include <api/api_handler_pcb.h>
#include <api/proto_merge.h>
#include <api/api_pcb_utils.h>
#include <api/api_enums.h>
#include <api/api_server.h>
#include <api/api_utils.h>
#include <api/common/commands/library_commands.pb.h>
#include <api/cross_probe_client.h>
#include <wx/log.h>
#include <base_screen.h>
#include <board_commit.h>
#include <board_connected_item.h>
#include <board_design_settings.h>
#include <core/kicad_algo.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <kicad_clipboard.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <pcb_group.h>
#include <pcb_reference_image.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_textbox.h>
#include <pcb_track.h>
#include <pcbnew_id.h>
#include <pcb_marker.h>
#include <preview_items/host_markers_item.h>
#include <tool/host_markers_tool.h>
#include <view/view_controls.h>
#include <pcb_3d_body.h>
#include <pcb_point.h>
#include <kiway.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <jobs/job_export_pcb_3d.h>
#include <jobs/job_export_pcb_dxf.h>
#include <jobs/job_export_pcb_drill.h>
#include <jobs/job_export_pcb_gencad.h>
#include <jobs/job_export_pcb_specctra_dsn.h>
#include <jobs/job_export_pcb_gerber.h>
#include <jobs/job_export_pcb_gerbers.h>
#include <jobs/job_export_pcb_idf.h>
#include <jobs/job_export_pcb_ipc2581.h>
#include <jobs/job_export_pcb_ipcd356.h>
#include <jobs/job_export_pcb_odb.h>
#include <jobs/job_export_pcb_pdf.h>
#include <netlist_reader/pcb_netlist_utils.h>
#include <project_pcb.h>
#include <jobs/job_export_pcb_pos.h>
#include <jobs/job_export_pcb_ps.h>
#include <jobs/job_export_pcb_stats.h>
#include <jobs/job_export_pcb_svg.h>
#include <jobs/job_export_pcb_png.h>
#include <pcb_plot_params.h>
#include <plotters/plotter_png.h>
#include <jobs/job_pcb_render.h>
#include <layer_ids.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/pcb_netlist.h>
#include <project.h>
#include <project/project_file.h>
#include <tool/actions.h>
#include <view/view.h>
#include <string_utils.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_selection_tool.h>
#include <tools/zone_filler_tool.h>
#include <tools/generator_tool.h>
#include <zone.h>
#include <zone_filler.h>

#include <api/common/types/base_types.pb.h>

#include <specctra_import_export/specctra.h>
#include <reporter.h>
#include <api/board/board_rules.pb.h>
#include <connectivity/connectivity_data.h>
#include <google/protobuf/util/json_util.h>
#include <drc/drc_rule_condition.h>
#include <drc/drc_rule_parser.h>
#include <widgets/appearance_controls.h>
#include <widgets/report_severity.h>
#include <drc/rule_editor/drc_re_rule_loader.h>
#include <trace_helpers.h>
#include <wx/ffile.h>

using namespace kiapi::common::commands;
using namespace kiapi::board::commands;
using types::CommandStatus;
using types::DocumentType;
using types::ItemRequestStatus;


API_HANDLER_PCB::API_HANDLER_PCB( PCB_EDIT_FRAME* aFrame ) :
        API_HANDLER_PCB( CreatePcbFrameContext( aFrame ), aFrame )
{
}


API_HANDLER_PCB::~API_HANDLER_PCB()
{
    *m_alive = false;
}


HOST_MARKERS_ITEM* API_HANDLER_PCB::hostMarkers( bool aCreate ) const
{
    PCB_EDIT_FRAME*    editor = frame();
    HOST_MARKERS_TOOL* tool = editor ? editor->GetToolManager()->GetTool<HOST_MARKERS_TOOL>() : nullptr;

    if( !tool )
        return nullptr;

    return aCreate ? &tool->Markers() : tool->Existing();
}


API_HANDLER_PCB::API_HANDLER_PCB( std::shared_ptr<PCB_CONTEXT> aContext, PCB_EDIT_FRAME* aFrame ) :
        API_HANDLER_BOARD( std::move( aContext ), aFrame )
{
    // Embedded: a click on a host or DRC marker tells the host (ET_MARKER_CLICKED)
    if( aFrame && aFrame->GetCanvas() && BACKPLANE_EMBED::Get().Enabled() )
    {
        std::weak_ptr<bool> alive = m_alive;

        aFrame->GetCanvas()->Bind( wxEVT_LEFT_UP,
                                   [this, alive]( wxMouseEvent& aEvent )
                                   {
                                       aEvent.Skip();

                                       if( std::shared_ptr<bool> on = alive.lock(); on && *on )
                                           onCanvasClick();
                                   } );
    }

    registerHandler<kiapi::backplane::GetBoardStructure, kiapi::backplane::BoardStructureResponse>(
            &API_HANDLER_PCB::handleGetBoardStructure );
    registerHandler<kiapi::backplane::SetBoardStructure, kiapi::backplane::BoardStructureResponse>(
            &API_HANDLER_PCB::handleSetBoardStructure );
    registerHandler<kiapi::backplane::ValidateBoardStructure, kiapi::backplane::BoardStructureResponse>(
            &API_HANDLER_PCB::handleValidateBoardStructure );
    registerHandler<kiapi::backplane::GetViewerConstructionState, kiapi::backplane::ViewerConstructionStateResponse>(
            &API_HANDLER_PCB::handleGetViewerConstructionState );
    registerHandler<kiapi::backplane::SetViewerConstructionState, kiapi::backplane::ViewerConstructionStateResponse>(
            &API_HANDLER_PCB::handleSetViewerConstructionState );
    registerHandler<kiapi::backplane::GetConstructionLayerState, kiapi::backplane::ConstructionLayerStateResponse>(
            &API_HANDLER_PCB::handleGetConstructionLayerState );
    registerHandler<kiapi::backplane::SetConstructionLayerState, kiapi::backplane::ConstructionLayerStateResponse>(
            &API_HANDLER_PCB::handleSetConstructionLayerState );
    registerHandler<kiapi::backplane::DeriveBoardStructurePartition, kiapi::backplane::BoardStructureResponse>(
            &API_HANDLER_PCB::handleDeriveBoardStructurePartition );
    registerHandler<kiapi::backplane::RunBoardStructureExport3D, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardStructureExport3D );

    registerHandler<GetOpenDocuments, GetOpenDocumentsResponse>(
            &API_HANDLER_PCB::handleGetOpenDocuments );
    registerHandler<SaveDocument, Empty>( &API_HANDLER_PCB::handleSaveDocument );
    registerHandler<SaveDocumentAs, Empty>( &API_HANDLER_PCB::handleSaveDocumentAs );
    registerHandler<SaveCopyOfDocument, Empty>( &API_HANDLER_PCB::handleSaveCopyOfDocument );
    registerHandler<RevertDocument, Empty>( &API_HANDLER_PCB::handleRevertDocument );

    registerHandler<GetItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetItems );

    registerHandler<SetBoardEnabledLayers, BoardEnabledLayersResponse>( &API_HANDLER_PCB::handleSetBoardEnabledLayers );
    registerHandler<GetBoardDesignRules, BoardDesignRulesResponse>( &API_HANDLER_PCB::handleGetBoardDesignRules );
    registerHandler<SetBoardDesignRules, BoardDesignRulesResponse>( &API_HANDLER_PCB::handleSetBoardDesignRules );
    registerHandler<GetCustomDesignRules, CustomRulesResponse>( &API_HANDLER_PCB::handleGetCustomDesignRules );
    registerHandler<SetCustomDesignRules, CustomRulesResponse>( &API_HANDLER_PCB::handleSetCustomDesignRules );
    registerHandler<kiapi::backplane::coedit::EditCustomRules, CustomRulesResponse>(
            &API_HANDLER_PCB::handleEditCustomRules );
    registerHandler<GetEmbeddedFiles, common::types::EmbeddedFiles>( &API_HANDLER_PCB::handleGetEmbeddedFiles );
    registerHandler<AddEmbeddedFiles, Empty>( &API_HANDLER_PCB::handleAddEmbeddedFiles );
    registerHandler<SetEmbeddedFiles, Empty>( &API_HANDLER_PCB::handleSetEmbeddedFiles );
    registerHandler<GetBoardOrigin, types::Vector2>( &API_HANDLER_PCB::handleGetBoardOrigin );
    registerHandler<SetBoardOrigin, Empty>( &API_HANDLER_PCB::handleSetBoardOrigin );
    registerHandler<GetBoardLayerName, BoardLayerNameResponse>( &API_HANDLER_PCB::handleGetBoardLayerName );
    registerHandler<GetBoardLayerByName, BoardLayerResponse>( &API_HANDLER_PCB::handleGetBoardLayerByName );

    registerHandler<GetNets, NetsResponse>( &API_HANDLER_PCB::handleGetNets );
    registerHandler<GetConnectedItems, GetItemsResponse>( &API_HANDLER_PCB::handleGetConnectedItems );
    registerHandler<GetItemsByNet, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNet );
    registerHandler<GetItemsByNetClass, GetItemsResponse>( &API_HANDLER_PCB::handleGetItemsByNetClass );
    registerHandler<GetNetClassForNets, NetClassForNetsResponse>(
            &API_HANDLER_PCB::handleGetNetClassForNets );
    registerHandler<RefillZones, Empty>( &API_HANDLER_PCB::handleRefillZones );
    registerHandler<kiapi::backplane::SketchRoute, kiapi::backplane::SketchRouteResponse>(
            &API_HANDLER_PCB::handleSketchRoute );

    {
        using namespace kiapi::backplane::plan;
        registerHandler<GetPlan, PlanResponse>( &API_HANDLER_PCB::handleGetPlan );
        registerHandler<ProposePlan, ProposePlanResponse>( &API_HANDLER_PCB::handleProposePlan );
        registerHandler<UpdatePlan, UpdatePlanResponse>( &API_HANDLER_PCB::handleUpdatePlan );
        registerHandler<ApprovePlan, PlanResponse>( &API_HANDLER_PCB::handleApprovePlan );
        registerHandler<RoutePlan, RoutePlanResponse>( &API_HANDLER_PCB::handleRoutePlan );
        registerHandler<StopPlanRouting, PlanResponse>( &API_HANDLER_PCB::handleStopPlanRouting );
        registerHandler<VerifyPlan, VerifyPlanResponse>( &API_HANDLER_PCB::handleVerifyPlan );
    }
    registerHandler<kiapi::backplane::ListAssemblyComponents, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleListAssemblyComponents );
    registerHandler<kiapi::backplane::MoveAssemblyComponent, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleMoveAssemblyComponent );
    registerHandler<kiapi::backplane::SyncAssemblyElectrical, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleSyncAssemblyElectrical );
    registerHandler<kiapi::backplane::OpenAssemblyTab, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleOpenAssemblyTab );
    registerHandler<kiapi::backplane::ListAssemblyTabs, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleListAssemblyTabs );
    registerHandler<kiapi::backplane::ShowAssembly3D, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleShowAssembly3D );
    registerHandler<kiapi::backplane::GetAssembly, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleGetAssembly );
    registerHandler<kiapi::backplane::SaveAssembly, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleSaveAssembly );
    registerHandler<kiapi::backplane::ResolveAssemblyMates, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleResolveAssemblyMates );
    registerHandler<kiapi::backplane::CheckAssemblyCollisions, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleCheckAssemblyCollisions );
    registerHandler<kiapi::backplane::ExportAssemblyStep, kiapi::backplane::AssemblyResponse>(
            &API_HANDLER_PCB::handleExportAssemblyStep );
    registerHandler<ImportNetlist, ImportNetlistResponse>( &API_HANDLER_PCB::handleImportNetlist );
    registerHandler<ImportSpecctraSession, ImportSpecctraSessionResponse>(
            &API_HANDLER_PCB::handleImportSpecctraSession );
    registerHandler<kiapi::board::tasks::StartRefillZonesTask, kiapi::common::tasks::StartTaskResponse>(
            &API_HANDLER_PCB::handleStartRefillZonesTask );
    registerHandler<kiapi::board::tasks::StartDrcTask,
                    kiapi::common::tasks::StartTaskResponse>(
            &API_HANDLER_PCB::handleStartDrcTask );
    registerHandler<kiapi::board::tasks::GetDrcTaskResult, kiapi::board::tasks::DrcTaskResult>(
            &API_HANDLER_PCB::handleGetDrcTaskResult, true );

    registerHandler<GetBoardEditorAppearanceSettings, BoardEditorAppearanceSettings>(
            &API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings );
    registerHandler<SetBoardEditorAppearanceSettings, Empty>(
            &API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings );
    registerHandler<GetBoardPlotSettings, BoardPlotSettingsResponse>( &API_HANDLER_PCB::handleGetBoardPlotSettings );
    registerHandler<SetBoardPlotSettings, Empty>( &API_HANDLER_PCB::handleSetBoardPlotSettings );
    registerHandler<InjectDrcError, InjectDrcErrorResponse>(
            &API_HANDLER_PCB::handleInjectDrcError );

    registerHandler<GetVariants, VariantsResponse>( &API_HANDLER_PCB::handleGetVariants );
    registerHandler<AddVariant, Empty>( &API_HANDLER_PCB::handleAddVariant );
    registerHandler<DeleteVariant, Empty>( &API_HANDLER_PCB::handleDeleteVariant );
    registerHandler<RenameVariant, Empty>( &API_HANDLER_PCB::handleRenameVariant );
    registerHandler<CopyVariant, Empty>( &API_HANDLER_PCB::handleCopyVariant );
    registerHandler<SetVariantDescription, Empty>( &API_HANDLER_PCB::handleSetVariantDescription );
    registerHandler<SetCurrentVariant, Empty>( &API_HANDLER_PCB::handleSetCurrentVariant );
    registerHandler<GetCurrentVariant, CurrentVariantResponse>(
            &API_HANDLER_PCB::handleGetCurrentVariant );

    registerHandler<RunBoardJobExport3D, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExport3D );
    registerHandler<RunBoardJobExportRender, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportRender );
    registerHandler<RunBoardJobExportSvg, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportSvg );
    registerHandler<RunBoardJobExportDxf, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportDxf );
    registerHandler<RunBoardJobExportPdf, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPdf );
    registerHandler<RunBoardJobExportPs, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPs );
    registerHandler<RunBoardJobExportPng, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPng );
    registerHandler<RunBoardJobExportGerbers, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportGerbers );
    registerHandler<RunBoardJobExportDrill, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportDrill );
    registerHandler<RunBoardJobExportPosition, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportPosition );
    registerHandler<RunBoardJobExportSpecctraDsn, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportSpecctraDsn );
    registerHandler<RunBoardJobExportGencad, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportGencad );
    registerHandler<RunBoardJobExportIpc2581, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportIpc2581 );
    registerHandler<RunBoardJobExportIpcD356, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportIpcD356 );
    registerHandler<RunBoardJobExportODB, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportODB );
    registerHandler<RunBoardJobExportStats, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportStats );
    registerHandler<RunBoardJobExportIdf, types::RunJobResponse>(
            &API_HANDLER_PCB::handleRunBoardJobExportIdf );

    registerHandler<GetPageSettings, types::PageSettings>( &API_HANDLER_PCB::handleGetPageSettings );
    registerHandler<SetPageSettings, types::PageSettings>( &API_HANDLER_PCB::handleSetPageSettings );

    registerHandler<CrossProbeAnnounce, CrossProbeAnnounceResponse>( &API_HANDLER_PCB::handleCrossProbeAnnounce );
    registerHandler<SyncSelection, SyncSelectionResponse>( &API_HANDLER_PCB::handleSyncSelection );
    registerHandler<kiapi::backplane::embed::SelectItems, kiapi::backplane::embed::SelectItemsResponse>(
            &API_HANDLER_PCB::handleSelectItems );
    registerHandler<kiapi::backplane::embed::RunChecks, kiapi::backplane::embed::RunChecksResponse>(
            &API_HANDLER_PCB::handleRunChecks );
    registerHandler<kiapi::backplane::embed::GetMarkers, kiapi::backplane::embed::GetMarkersResponse>(
            &API_HANDLER_PCB::handleGetMarkers, true );
    registerHandler<kiapi::backplane::embed::SetHostMarkers, kiapi::backplane::embed::SetHostMarkersResponse>(
            &API_HANDLER_PCB::handleSetHostMarkers );
    registerHandler<kiapi::backplane::embed::CommentSelection,
                    kiapi::backplane::embed::CommentSelectionResponse>(
            &API_HANDLER_PCB::handleCommentSelection );
    registerHandler<HighlightNets, HighlightNetsResponse>( &API_HANDLER_PCB::handleHighlightNets );

    registerHandler<PlaceFootprintFromLibrary, PlaceFromLibraryResponse>(
            &API_HANDLER_PCB::handlePlaceFootprintFromLibrary );
}


PCB_EDIT_FRAME* API_HANDLER_PCB::frame() const
{
    return static_cast<PCB_EDIT_FRAME*>( m_frame );
}


HANDLER_RESULT<GetOpenDocumentsResponse> API_HANDLER_PCB::handleGetOpenDocuments(
        const HANDLER_CONTEXT<GetOpenDocuments>& aCtx )
{
    if( aCtx.Request.type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    GetOpenDocumentsResponse response;
    common::types::DocumentSpecifier doc;

    wxFileName fn( pcbContext()->GetCurrentFileName() );

    doc.set_type( DocumentType::DOCTYPE_PCB );
    doc.set_board_filename( fn.GetFullName() );

    doc.mutable_project()->set_name( project().GetProjectName().ToStdString() );
    doc.mutable_project()->set_path( project().GetProjectDirectory().ToStdString() );

    response.mutable_documents()->Add( std::move( doc ) );
    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveDocument(
        const HANDLER_CONTEXT<SaveDocument>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( board()->GetFileName().IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "the board is untitled; use SaveDocumentAs to save it to a new file" );
        return tl::unexpected( e );
    }

    if( !pcbContext()->SaveBoard() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_INTERNAL_ERROR );
        e.set_error_message( "board could not be saved" );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveDocumentAs( const HANDLER_CONTEXT<SaveDocumentAs>& aCtx )
{
    if( Pgm().IsGUI() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "SaveDocumentAs is not available in GUI mode" );
        return tl::unexpected( e );
    }

    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    wxString pathStr = wxString::FromUTF8( aCtx.Request.path() );

    if( pathStr.IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "path is required" );
        return tl::unexpected( e );
    }

    wxFileName oldBoardFile( pcbContext()->GetCurrentFileName() );

    if( !pcbContext()->SaveBoardAs( pathStr ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "failed to save board to '{}'", pathStr.ToStdString() ) );
        return tl::unexpected( e );
    }

    if( KICAD_API_SERVER* server = apiServer() )
    {
        wxFileName newBoardFile( pathStr );
        newBoardFile.SetExt( FILEEXT::KiCadPcbFileExtension );

        server->NotifyDocumentRenamed( thisDocumentType(), oldBoardFile.GetFullPath(), newBoardFile.GetFullPath() );
    }

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSaveCopyOfDocument(
        const HANDLER_CONTEXT<SaveCopyOfDocument>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    wxFileName boardPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.path() ) ) );

    if( !boardPath.IsOk() || !boardPath.IsDirWritable() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' could not be opened",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.FileExists()
        && ( !boardPath.IsFileWritable() || !aCtx.Request.options().overwrite() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' exists and cannot be overwritten",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( boardPath.GetExt() != FILEEXT::KiCadPcbFileExtension )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "save path '{}' must have a kicad_pcb extension",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    BOARD* board = this->board();

    if( board->GetFileName().Matches( boardPath.GetFullPath() ) )
    {
        if( !pcbContext()->SaveBoard() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_INTERNAL_ERROR );
            e.set_error_message( "board could not be saved" );
            return tl::unexpected( e );
        }

        return Empty();
    }

    bool includeProject = true;

    if( aCtx.Request.has_options() )
        includeProject = aCtx.Request.options().include_project();

    if( !pcbContext()->SavePcbCopy( boardPath.GetFullPath(), includeProject, /* aHeadless = */ true ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_INTERNAL_ERROR );
        e.set_error_message( fmt::format( "board could not be saved to '{}'",
                                          boardPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRevertDocument(
        const HANDLER_CONTEXT<RevertDocument>& aCtx )
{
    // Validate first so a request meant for another editor's document is not captured here
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // Reloading frees every item, so refuse while any client transaction is open; the staged
    // commit would otherwise dangle
    if( !m_commits.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BUSY );
        e.set_error_message( "cannot revert while a commit is open" );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    if( !pcbContext()->RevertToSaved() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "could not revert: there is no saved file on disk to revert to" );
        return tl::unexpected( e );
    }

    return Empty();
}


tl::expected<bool, ApiResponseStatus> API_HANDLER_PCB::validateDocumentInternal( const DocumentSpecifier& aDocument ) const
{
    if( aDocument.type() != DocumentType::DOCTYPE_PCB )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    wxFileName fn( pcbContext()->GetCurrentFileName() );

    if( aDocument.board_filename().compare( fn.GetFullName() ) != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested document {} is not open",
                                          aDocument.board_filename() ) );
        return tl::unexpected( e );
    }

    return true;
}


// Board types that are directly retrievable by GetItems
static const std::vector<KICAD_T> s_allowedBoardTypes = {
    PCB_FOOTPRINT_T,
    PCB_PAD_T,
    PCB_SHAPE_T,
    PCB_REFERENCE_IMAGE_T,
    PCB_TEXT_T,
    PCB_TEXTBOX_T,
    PCB_TABLE_T,
    PCB_DRILL_CHART_T,
    PCB_STACKUP_TABLE_T,
    PCB_DRILL_MAP_T,
    PCB_TRACE_T,
    PCB_ARC_T,
    PCB_VIA_T,
    PCB_DIMENSION_T,
    PCB_ZONE_T,
    PCB_GROUP_T,
    PCB_GENERATOR_T,
    PCB_BARCODE_T,
    PCB_CONSTRAINT_T,
    PCB_GRID_ITEM_T,
    PCB_POINT_T,
    PCB_3D_BODY_T
};


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItems( const HANDLER_CONTEXT<GetItems>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        // No message needed for AS_UNHANDLED; this is an internal flag for the API server
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    GetItemsResponse response;

    BOARD* board = this->board();
    std::vector<BOARD_ITEM*> items;
    std::set<KICAD_T> typesRequested, typesHandled;

    std::vector<KICAD_T> requestedTypes = parseRequestedItemTypes( aCtx.Request.types() );

    if( aCtx.Request.types().empty() )
        requestedTypes.assign( s_allowedBoardTypes.begin(), s_allowedBoardTypes.end() );

    for( KICAD_T type : requestedTypes )
    {
        typesRequested.emplace( type );

        if( typesHandled.count( type ) )
            continue;

        switch( type )
        {
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
            std::ranges::copy( board->Tracks(), std::back_inserter( items ) );
            typesHandled.insert( { PCB_TRACE_T, PCB_ARC_T, PCB_VIA_T } );
            break;

        case PCB_PAD_T:
            for( FOOTPRINT* fp : board->Footprints() )
                std::ranges::copy( fp->Pads(), std::back_inserter( items ) );

            typesHandled.insert( PCB_PAD_T );
            break;

        case PCB_FOOTPRINT_T:
            std::ranges::copy( board->Footprints(), std::back_inserter( items ) );
            typesHandled.insert( PCB_FOOTPRINT_T );
            break;

        case PCB_SHAPE_T:
        case PCB_TABLE_T:
        case PCB_DRILL_CHART_T:
        case PCB_STACKUP_TABLE_T:
        case PCB_DRILL_MAP_T:
        case PCB_TEXT_T:
        case PCB_TEXTBOX_T:
        case PCB_BARCODE_T:
        case PCB_REFERENCE_IMAGE_T:
        case PCB_GRID_ITEM_T:
        case PCB_DIMENSION_T:
            for( BOARD_ITEM* item : board->Drawings() )
            {
                if( item->Type() == type || BaseType( item->Type() ) == type )
                    items.emplace_back( item );
            }

            typesHandled.insert( type );
            break;

        case PCB_ZONE_T:
            std::ranges::copy( board->Zones(), std::back_inserter( items ) );
            typesHandled.insert( PCB_ZONE_T );
            break;

        case PCB_GROUP_T:
            std::ranges::copy( board->Groups(), std::back_inserter( items ) );
            typesHandled.insert( PCB_GROUP_T );
            break;

        case PCB_POINT_T:
            std::ranges::copy( board->Points(), std::back_inserter( items ) );
            typesHandled.insert( PCB_POINT_T );
            break;

        case PCB_3D_BODY_T:
            // Not supported on a board context yet
            break;

        case PCB_GENERATOR_T:
            std::ranges::copy( board->Generators(), std::back_inserter( items ) );
            typesHandled.insert( PCB_GENERATOR_T );
            break;

        case PCB_CONSTRAINT_T:
            std::ranges::copy( board->Constraints(), std::back_inserter( items ) );
            typesHandled.insert( PCB_CONSTRAINT_T );
            break;

        default:
            break;
        }
    }

    if( typesHandled.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    for( const BOARD_ITEM* item : items )
    {
        if( !typesRequested.count( item->Type() ) && !typesRequested.count( BaseType( item->Type() ) ) )
            continue;

        google::protobuf::Any itemBuf;
        item->Serialize( itemBuf );
        response.mutable_items()->Add( std::move( itemBuf ) );
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<BoardEnabledLayersResponse> API_HANDLER_PCB::handleSetBoardEnabledLayers(
        const HANDLER_CONTEXT<SetBoardEnabledLayers>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( aCtx.Request.copper_layer_count() < 2 || aCtx.Request.copper_layer_count() % 2 != 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "copper_layer_count must be an even number of at least 2" );
        return tl::unexpected( e );
    }

    if( aCtx.Request.copper_layer_count() > MAX_CU_LAYERS )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "copper_layer_count must be below %d", MAX_CU_LAYERS ) );
        return tl::unexpected( e );
    }

    int copperLayerCount = static_cast<int>( aCtx.Request.copper_layer_count() );
    LSET enabled = board::UnpackLayerSet( aCtx.Request.layers() );

    // Sanitize the input
    enabled |= LSET( { Edge_Cuts, Margin, F_CrtYd, B_CrtYd } );
    enabled &= ~LSET::AllCuMask();
    enabled |= LSET::AllCuMask( copperLayerCount );

    BOARD* board = this->board();

    LSET previousEnabled = board->GetEnabledLayers();
    LSET changedLayers = enabled ^ previousEnabled;

    board->SetEnabledLayers( enabled );
    board->SetVisibleLayers( board->GetVisibleLayers() | changedLayers );

    LSEQ removedLayers;

    for( PCB_LAYER_ID layer_id : previousEnabled )
    {
        if( !enabled[layer_id] && board->HasItemsOnLayer( layer_id ) )
            removedLayers.push_back( layer_id );
    }

    bool modified = false;

    if( !removedLayers.empty() )
    {
        toolManager()->RunAction( PCB_ACTIONS::selectionClear );

        for( PCB_LAYER_ID layer_id : removedLayers )
            modified |= board->RemoveAllItemsOnLayer( layer_id );
    }

    if( enabled != previousEnabled || modified )
        onModified();

    BoardEnabledLayersResponse response;

    response.set_copper_layer_count( copperLayerCount );
    board::PackLayerSet( *response.mutable_layers(), enabled );

    return response;
}


HANDLER_RESULT<common::types::EmbeddedFiles>
API_HANDLER_PCB::handleGetEmbeddedFiles( const HANDLER_CONTEXT<GetEmbeddedFiles>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    common::types::EmbeddedFiles response;
    board::PackEmbeddedFiles( response, *board()->GetEmbeddedFiles() );
    return response;
}


HANDLER_RESULT<Empty> unpackEmbeddedFiles( EMBEDDED_FILES& aOutput, const common::types::EmbeddedFiles& aProto )
{
    if( !board::UnpackEmbeddedFiles( aOutput, aProto ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "embedded file validation failed" );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleAddEmbeddedFiles( const HANDLER_CONTEXT<AddEmbeddedFiles>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    EMBEDDED_FILES files;
    HANDLER_RESULT<Empty> result = unpackEmbeddedFiles( files, aCtx.Request.files() );

    if( !result.has_value() )
        return result;

    EMBEDDED_FILES* boardFiles = board()->GetEmbeddedFiles();

    for( const std::shared_ptr<EMBEDDED_FILES::EMBEDDED_FILE>& file : files.EmbeddedFileMap() | std::views::values )
    {
        auto copy = std::make_shared<EMBEDDED_FILES::EMBEDDED_FILE>( *file );
        boardFiles->AddFile( copy );
    }

    onModified();
    return result;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetEmbeddedFiles( const HANDLER_CONTEXT<SetEmbeddedFiles>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    HANDLER_RESULT<Empty> result = unpackEmbeddedFiles( *board()->GetEmbeddedFiles(),
                                                        aCtx.Request.files() );

    if( !result.has_value() )
        return result;

    onModified();
    return result;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleGetBoardDesignRules(
        const HANDLER_CONTEXT<GetBoardDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();
    BoardDesignRulesResponse     response;
    kiapi::board::BoardDesignRules* rules = response.mutable_rules();

    kiapi::board::MinimumConstraints* constraints = rules->mutable_constraints();

    constraints->mutable_min_clearance()->set_value_nm( bds.m_MinClearance );
    constraints->mutable_min_groove_width()->set_value_nm( bds.m_MinGrooveWidth );
    constraints->mutable_min_connection_width()->set_value_nm( bds.m_MinConn );
    constraints->mutable_min_track_width()->set_value_nm( bds.m_TrackMinWidth );
    constraints->mutable_min_via_annular_width()->set_value_nm( bds.m_ViasMinAnnularWidth );
    constraints->mutable_min_via_size()->set_value_nm( bds.m_ViasMinSize );
    constraints->mutable_min_through_drill()->set_value_nm( bds.m_MinThroughDrill );
    constraints->mutable_min_microvia_size()->set_value_nm( bds.m_MicroViasMinSize );
    constraints->mutable_min_microvia_drill()->set_value_nm( bds.m_MicroViasMinDrill );
    constraints->mutable_copper_edge_clearance()->set_value_nm( bds.m_CopperEdgeClearance );
    constraints->mutable_hole_clearance()->set_value_nm( bds.m_HoleClearance );
    constraints->mutable_hole_to_hole_min()->set_value_nm( bds.m_HoleToHoleMin );
    constraints->mutable_silk_clearance()->set_value_nm( bds.m_SilkClearance );
    constraints->set_min_resolved_spokes( bds.m_MinResolvedSpokes );
    constraints->mutable_min_silk_text_height()->set_value_nm( bds.m_MinSilkTextHeight );
    constraints->mutable_min_silk_text_thickness()->set_value_nm( bds.m_MinSilkTextThickness );

    kiapi::board::PredefinedSizes* sizes = rules->mutable_predefined_sizes();

    for( size_t ii = 1; ii < bds.m_TrackWidthList.size(); ++ii )
        sizes->add_tracks()->mutable_width()->set_value_nm( bds.m_TrackWidthList[ii] );

    for( size_t ii = 1; ii < bds.m_ViasDimensionsList.size(); ++ii )
    {
        kiapi::board::PresetViaDimension* via = sizes->add_vias();
        via->mutable_diameter()->set_value_nm( bds.m_ViasDimensionsList[ii].m_Diameter );
        via->mutable_drill()->set_value_nm( bds.m_ViasDimensionsList[ii].m_Drill );
    }

    for( size_t ii = 1; ii < bds.m_DiffPairDimensionsList.size(); ++ii )
    {
        kiapi::board::PresetDiffPairDimension* pair = sizes->add_diff_pairs();
        pair->mutable_width()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_Width );
        pair->mutable_gap()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_Gap );
        pair->mutable_via_gap()->set_value_nm( bds.m_DiffPairDimensionsList[ii].m_ViaGap );
    }

    kiapi::board::SolderMaskPasteDefaults* maskPaste = rules->mutable_solder_mask_paste();

    maskPaste->mutable_mask_expansion()->set_value_nm( bds.m_SolderMaskExpansion );
    maskPaste->mutable_mask_min_width()->set_value_nm( bds.m_SolderMaskMinWidth );
    maskPaste->mutable_mask_to_copper_clearance()->set_value_nm( bds.m_SolderMaskToCopperClearance );
    maskPaste->mutable_paste_margin()->set_value_nm( bds.m_SolderPasteMargin );
    maskPaste->set_paste_margin_ratio( bds.m_SolderPasteMarginRatio );
    maskPaste->set_allow_soldermask_bridges_in_footprints( bds.m_AllowSoldermaskBridgesInFPs );

    kiapi::board::TeardropDefaults* teardrops = rules->mutable_teardrops();

    teardrops->set_target_vias( bds.m_TeardropParamsList.m_TargetVias );
    teardrops->set_target_pth_pads( bds.m_TeardropParamsList.m_TargetPTHPads );
    teardrops->set_target_smd_pads( bds.m_TeardropParamsList.m_TargetSMDPads );
    teardrops->set_target_track_to_track( bds.m_TeardropParamsList.m_TargetTrack2Track );
    teardrops->set_use_round_shapes_only( bds.m_TeardropParamsList.m_UseRoundShapesOnly );

    TEARDROP_PARAMETERS_LIST& tdList = const_cast<TEARDROP_PARAMETERS_LIST&>( bds.m_TeardropParamsList );

    for( int target = TARGET_ROUND; target <= TARGET_TRACK; ++target )
    {
        const TEARDROP_PARAMETERS* params = tdList.GetParameters( static_cast<TARGET_TD>( target ) );
        kiapi::board::TeardropTargetEntry* entry = teardrops->add_target_params();

        entry->set_target( ToProtoEnum<TARGET_TD, kiapi::board::TeardropTarget>(
            static_cast<TARGET_TD>( target ) ) );
        entry->mutable_params()->set_enabled( params->m_Enabled );
        entry->mutable_params()->mutable_max_length()->set_value_nm( params->m_TdMaxLen );
        entry->mutable_params()->mutable_max_width()->set_value_nm( params->m_TdMaxWidth );
        entry->mutable_params()->set_best_length_ratio( params->m_BestLengthRatio );
        entry->mutable_params()->set_best_width_ratio( params->m_BestWidthRatio );
        entry->mutable_params()->set_width_to_size_filter_ratio( params->m_WidthtoSizeFilterRatio );
        entry->mutable_params()->set_curved_edges( params->m_CurvedEdges );
        entry->mutable_params()->set_allow_two_tracks( params->m_AllowUseTwoTracks );
        entry->mutable_params()->set_on_pads_in_zones( params->m_TdOnPadsInZones );
    }

    kiapi::board::ViaProtectionDefaults* viaProtection = rules->mutable_via_protection();

    viaProtection->set_tent_front( bds.m_TentViasFront );
    viaProtection->set_tent_back( bds.m_TentViasBack );
    viaProtection->set_cover_front( bds.m_CoverViasFront );
    viaProtection->set_cover_back( bds.m_CoverViasBack );
    viaProtection->set_plug_front( bds.m_PlugViasFront );
    viaProtection->set_plug_back( bds.m_PlugViasBack );
    viaProtection->set_cap( bds.m_CapVias );
    viaProtection->set_fill( bds.m_FillVias );

    for( const auto& [errorCode, severity] : bds.m_DRCSeverities )
    {
        board::DrcSeveritySetting* setting = rules->add_severities();
        setting->set_rule_type(
                ToProtoEnum<PCB_DRC_CODE, board::DrcErrorType>( static_cast<PCB_DRC_CODE>( errorCode ) ) );
        setting->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( severity ) );
    }

    for( const DRC_EXCLUSION& exclusion : bds.m_DrcExclusions )
        rules->add_exclusions()->CopyFrom( exclusion.ToProto() );

    response.set_custom_rules_status( CRS_NONE );

    wxString rulesPath = board()->GetDesignRulesPath();

    if( !rulesPath.IsEmpty() && wxFileName::IsFileReadable( rulesPath ) )
    {
        wxFFile file( rulesPath, "r" );
        wxString content;
        std::vector<std::shared_ptr<DRC_RULE>> parsedRules;

        if( !file.IsOpened() )
        {
            response.set_custom_rules_status( CRS_INVALID );
            return response;
        }

        file.ReadAll( &content );
        file.Close();

        try
        {
            DRC_RULES_PARSER parser( content, "File" );
            parser.Parse( parsedRules, nullptr );
            response.set_custom_rules_status( CRS_VALID );
        }
        catch( const IO_ERROR& )
        {
            response.set_custom_rules_status( CRS_INVALID );
        }
    }

    return response;
}


HANDLER_RESULT<BoardDesignRulesResponse> API_HANDLER_PCB::handleSetBoardDesignRules(
        const HANDLER_CONTEXT<SetBoardDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    BOARD_DESIGN_SETTINGS newSettings( board()->GetDesignSettings() );
    const kiapi::board::BoardDesignRules& rules = aCtx.Request.rules();

    if( rules.has_constraints() )
    {
        const kiapi::board::MinimumConstraints& constraints = rules.constraints();

        newSettings.m_MinClearance = constraints.min_clearance().value_nm();
        newSettings.m_MinGrooveWidth = constraints.min_groove_width().value_nm();
        newSettings.m_MinConn = constraints.min_connection_width().value_nm();
        newSettings.m_TrackMinWidth = constraints.min_track_width().value_nm();
        newSettings.m_ViasMinAnnularWidth = constraints.min_via_annular_width().value_nm();
        newSettings.m_ViasMinSize = constraints.min_via_size().value_nm();
        newSettings.m_MinThroughDrill = constraints.min_through_drill().value_nm();
        newSettings.m_MicroViasMinSize = constraints.min_microvia_size().value_nm();
        newSettings.m_MicroViasMinDrill = constraints.min_microvia_drill().value_nm();
        newSettings.m_CopperEdgeClearance = constraints.copper_edge_clearance().value_nm();
        newSettings.m_HoleClearance = constraints.hole_clearance().value_nm();
        newSettings.m_HoleToHoleMin = constraints.hole_to_hole_min().value_nm();
        newSettings.m_SilkClearance = constraints.silk_clearance().value_nm();
        newSettings.m_MinResolvedSpokes = constraints.min_resolved_spokes();
        newSettings.m_MinSilkTextHeight = constraints.min_silk_text_height().value_nm();
        newSettings.m_MinSilkTextThickness = constraints.min_silk_text_thickness().value_nm();
    }

    if( rules.has_predefined_sizes() )
    {
        newSettings.m_TrackWidthList.clear();
        newSettings.m_TrackWidthList.emplace_back( 0 );

        for( const kiapi::board::PresetTrackWidth& track : rules.predefined_sizes().tracks() )
            newSettings.m_TrackWidthList.emplace_back( track.width().value_nm() );

        newSettings.m_ViasDimensionsList.clear();
        newSettings.m_ViasDimensionsList.emplace_back( 0, 0 );

        for( const kiapi::board::PresetViaDimension& via : rules.predefined_sizes().vias() )
        {
            newSettings.m_ViasDimensionsList.emplace_back( static_cast<int>( via.diameter().value_nm() ),
                                                           static_cast<int>( via.drill().value_nm() ) );
        }

        newSettings.m_DiffPairDimensionsList.clear();
        newSettings.m_DiffPairDimensionsList.emplace_back( 0, 0, 0 );

        for( const kiapi::board::PresetDiffPairDimension& pair : rules.predefined_sizes().diff_pairs() )
        {
            newSettings.m_DiffPairDimensionsList.emplace_back(
                    static_cast<int>( pair.width().value_nm() ),
                    static_cast<int>( pair.gap().value_nm() ),
                    static_cast<int>( pair.via_gap().value_nm() ) );
        }
    }

    if( rules.has_solder_mask_paste() )
    {
        const kiapi::board::SolderMaskPasteDefaults& maskPaste = rules.solder_mask_paste();

        newSettings.m_SolderMaskExpansion = maskPaste.mask_expansion().value_nm();
        newSettings.m_SolderMaskMinWidth = maskPaste.mask_min_width().value_nm();
        newSettings.m_SolderMaskToCopperClearance = maskPaste.mask_to_copper_clearance().value_nm();
        newSettings.m_SolderPasteMargin = maskPaste.paste_margin().value_nm();
        newSettings.m_SolderPasteMarginRatio = maskPaste.paste_margin_ratio();
        newSettings.m_AllowSoldermaskBridgesInFPs =
                maskPaste.allow_soldermask_bridges_in_footprints();
    }

    if( rules.has_teardrops() )
    {
        const kiapi::board::TeardropDefaults& teardrops = rules.teardrops();

        newSettings.m_TeardropParamsList.m_TargetVias = teardrops.target_vias();
        newSettings.m_TeardropParamsList.m_TargetPTHPads = teardrops.target_pth_pads();
        newSettings.m_TeardropParamsList.m_TargetSMDPads = teardrops.target_smd_pads();
        newSettings.m_TeardropParamsList.m_TargetTrack2Track = teardrops.target_track_to_track();
        newSettings.m_TeardropParamsList.m_UseRoundShapesOnly = teardrops.use_round_shapes_only();

        for( const kiapi::board::TeardropTargetEntry& entry : teardrops.target_params() )
        {
            if( entry.target() == kiapi::board::TeardropTarget::TDT_UNKNOWN )
                continue;

            TARGET_TD target = FromProtoEnum<TARGET_TD, kiapi::board::TeardropTarget>(
                    entry.target() );

            if( target == TARGET_UNKNOWN )
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( "unknown teardrop target" );
                return tl::unexpected( e );
            }

            TEARDROP_PARAMETERS* params = newSettings.m_TeardropParamsList.GetParameters( target );

            params->m_Enabled = entry.params().enabled();
            params->m_TdMaxLen = entry.params().max_length().value_nm();
            params->m_TdMaxWidth = entry.params().max_width().value_nm();
            params->m_BestLengthRatio = entry.params().best_length_ratio();
            params->m_BestWidthRatio = entry.params().best_width_ratio();
            params->m_WidthtoSizeFilterRatio = entry.params().width_to_size_filter_ratio();
            params->m_CurvedEdges = entry.params().curved_edges();
            params->m_AllowUseTwoTracks = entry.params().allow_two_tracks();
            params->m_TdOnPadsInZones = entry.params().on_pads_in_zones();
        }
    }

    if( rules.has_via_protection() )
    {
        const kiapi::board::ViaProtectionDefaults& viaProtection = rules.via_protection();

        newSettings.m_TentViasFront = viaProtection.tent_front();
        newSettings.m_TentViasBack = viaProtection.tent_back();
        newSettings.m_CoverViasFront = viaProtection.cover_front();
        newSettings.m_CoverViasBack = viaProtection.cover_back();
        newSettings.m_PlugViasFront = viaProtection.plug_front();
        newSettings.m_PlugViasBack = viaProtection.plug_back();
        newSettings.m_CapVias = viaProtection.cap();
        newSettings.m_FillVias = viaProtection.fill();
    }

    newSettings.m_DRCSeverities.clear();

    for( const kiapi::board::DrcSeveritySetting& severitySetting : rules.severities() )
    {
        PCB_DRC_CODE ruleType =
                FromProtoEnum<PCB_DRC_CODE, kiapi::board::DrcErrorType>( severitySetting.rule_type() );

        const std::unordered_set<SEVERITY> permitted( { RPT_SEVERITY_ERROR, RPT_SEVERITY_WARNING, RPT_SEVERITY_IGNORE } );
        SEVERITY setting = FromProtoEnum<SEVERITY, kiapi::common::types::RuleSeverity>( severitySetting.severity() );

        if( !permitted.contains( setting ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "DRC severity must be error, warning, or ignore" ) );
            return tl::unexpected( e );
        }

        newSettings.m_DRCSeverities[ruleType] = setting;
    }

    newSettings.m_DrcExclusions.clear();

    for( const kiapi::board::DrcExclusion& exclusion : rules.exclusions() )
        newSettings.m_DrcExclusions.insert( DRC_EXCLUSION::FromProto( exclusion ) );

    std::vector<BOARD_DESIGN_SETTINGS::VALIDATION_ERROR> errors = newSettings.ValidateDesignRules();

    if( !errors.empty() )
    {
        const BOARD_DESIGN_SETTINGS::VALIDATION_ERROR& error = errors.front();

        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "Invalid board design rules: {}: {}",
                                          error.setting_name.ToStdString(),
                                          error.error_message.ToStdString() ) );
        return tl::unexpected( e );
    }

    board()->SetDesignSettings( newSettings );

    onModified();

    HANDLER_CONTEXT<GetBoardDesignRules> getCtx = { aCtx.ClientName, GetBoardDesignRules() };
    *getCtx.Request.mutable_board() = aCtx.Request.board();

    return handleGetBoardDesignRules( getCtx );
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::handleGetCustomDesignRules(
        const HANDLER_CONTEXT<GetCustomDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CustomRulesResponse response;
    response.set_status( CRS_NONE );

    wxString rulesPath = board()->GetDesignRulesPath();

    if( rulesPath.IsEmpty() || !wxFileName::IsFileReadable( rulesPath ) )
        return response;

    wxFFile file( rulesPath, "r" );

    if( !file.IsOpened() )
    {
        response.set_status( CRS_INVALID );
        response.set_error_text( "Failed to open custom rules file" );
        return response;
    }

    wxString content;
    file.ReadAll( &content );
    file.Close();

    std::vector<std::shared_ptr<DRC_RULE>> parsedRules;

    try
    {
        DRC_RULES_PARSER parser( content, "File" );
        parser.Parse( parsedRules, nullptr );
    }
    catch( const IO_ERROR& ioe )
    {
        response.set_status( CRS_INVALID );
        response.set_error_text( ioe.What().ToStdString() );
        return response;
    }

    m_rulesBase[aCtx.ClientName] = std::string( content.ToUTF8() );

    CustomRulesResponse built = rulesToProto( content, parsedRules );
    *response.mutable_rules() = built.rules();
    response.set_status( CRS_VALID );
    return response;
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::handleSetCustomDesignRules(
        const HANDLER_CONTEXT<SetCustomDesignRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    // Co-editing (docs/backplane/coedit.md): merge per rule, never overwrite the file.  The
    // base is what this client last read; rules it did not change keep their exact text.
    std::vector<std::string> upserts;
    std::set<std::string>    named;

    for( const board::CustomRule& rule : aCtx.Request.rules() )
    {
        wxString serializationError;
        wxString serializedRule = DRC_RULE::FormatRuleFromProto( rule, &serializationError );

        if( serializedRule.IsEmpty() )
        {
            CustomRulesResponse response;
            response.set_status( CRS_INVALID );
            response.set_error_text( serializationError.IsEmpty()
                                             ? std::string( "Failed to serialize custom rule" )
                                             : std::string( serializationError.ToUTF8() ) );
            return response;
        }

        named.insert( rule.name() );
        upserts.push_back( serializedRule.ToStdString( wxConvUTF8 ) );
    }

    std::string disk = readRulesFile();
    std::string base = m_rulesBase.count( aCtx.ClientName ) ? m_rulesBase[aCtx.ClientName] : disk;

    // Unchanged rules (same canonical text as in the base) keep the base's text and comments.
    std::map<std::string, std::string> canonicalBase = canonicalRules( base );
    std::vector<std::string>           changed;

    for( const std::string& text : upserts )
    {
        std::string name = BACKPLANE_MERGE::RuleName( text );
        auto        it = canonicalBase.find( name );

        if( it == canonicalBase.end() || it->second != text )
            changed.push_back( text );
    }

    std::vector<std::string> removed;

    for( const auto& [name, text] : canonicalBase )
        if( !named.count( name ) )
            removed.push_back( name );

    std::string ours = BACKPLANE_MERGE::EditRules( base, changed, removed );

    if( HANDLER_RESULT<CustomRulesResponse> written = writeMergedRules( aCtx.ClientName, base, ours,
                                                                       disk, "Set custom rules" );
        !written || written->status() == CRS_INVALID )
    {
        return written;
    }

    HANDLER_CONTEXT<GetCustomDesignRules> getCtx = { aCtx.ClientName, GetCustomDesignRules() };
    *getCtx.Request.mutable_board() = aCtx.Request.board();
    HANDLER_RESULT<CustomRulesResponse> reply = handleGetCustomDesignRules( getCtx );
    m_rulesBase[aCtx.ClientName] = ours;   // what it set, not the others' rules it was handed back
    return reply;
}


CustomRulesResponse API_HANDLER_PCB::rulesToProto( const wxString& content,
                                                  const std::vector<std::shared_ptr<DRC_RULE>>& parsedRules )
{
    CustomRulesResponse response;

    for( const std::shared_ptr<DRC_RULE>& rule : parsedRules )
    {
        // TODO(JE) since we now need this for both here and the rules editor, maybe it's time
        // to just make comment parsing part of the parser?
        wxString text = DRC_RULE_LOADER::ExtractRuleText( content, rule->m_Name );
        wxString comment = DRC_RULE_LOADER::ExtractRuleComment( text );

        kiapi::board::CustomRule* customRule = response.add_rules();

        if( rule->m_Condition )
            customRule->set_condition( rule->m_Condition->GetExpression().ToUTF8() );

        for( const DRC_CONSTRAINT& constraint : rule->m_Constraints )
        {
            board::CustomRuleConstraint* constraintProto = customRule->add_constraints();
            constraint.ToProto( *constraintProto );
        }

        customRule->set_severity( ToProtoEnum<SEVERITY, types::RuleSeverity>( rule->m_Severity ) );
        customRule->set_name( rule->m_Name.ToUTF8() );

        if( rule->m_LayerSource.CmpNoCase( wxS( "outer" ) ) == 0 )
        {
            customRule->set_layer_mode( kiapi::board::CRLM_OUTER );
        }
        else if( rule->m_LayerSource.CmpNoCase( wxS( "inner" ) ) == 0 )
        {
            customRule->set_layer_mode( kiapi::board::CRLM_INNER );
        }
        else if( !rule->m_LayerSource.IsEmpty() )
        {
            int layer = LSET::NameToLayer( rule->m_LayerSource );

            if( layer != UNDEFINED_LAYER && layer != UNSELECTED_LAYER && layer < PCB_LAYER_ID_COUNT )
            {
                customRule->set_single_layer(
                        ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( static_cast<PCB_LAYER_ID>( layer ) ) );
            }
        }

        if( !comment.IsEmpty() )
            customRule->set_comments( comment );
    }

    return response;
}


std::string API_HANDLER_PCB::readRulesFile()
{
    wxString rulesPath = board()->GetDesignRulesPath();

    if( rulesPath.IsEmpty() || !wxFileName::IsFileReadable( rulesPath ) )
        return "";

    std::ifstream in( rulesPath.fn_str(), std::ios::in | std::ios::binary );
    return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
}


std::map<std::string, std::string> API_HANDLER_PCB::canonicalRules( const std::string& aText )
{
    // Each rule as FormatRuleFromProto would write it: comparable with what a client sends.
    std::map<std::string, std::string> out;

    if( aText.empty() )
        return out;

    HANDLER_CONTEXT<GetCustomDesignRules> ctx;
    std::vector<std::shared_ptr<DRC_RULE>> parsed;
    wxString content = wxString::FromUTF8( aText );

    try
    {
        DRC_RULES_PARSER parser( content, "File" );
        parser.Parse( parsed, nullptr );
    }
    catch( const IO_ERROR& )
    {
        return out;
    }

    CustomRulesResponse protos = rulesToProto( content, parsed );

    for( const board::CustomRule& rule : protos.rules() )
    {
        wxString text = DRC_RULE::FormatRuleFromProto( rule, nullptr );

        if( !text.IsEmpty() )
            out[rule.name()] = text.ToStdString( wxConvUTF8 );
    }

    return out;
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::writeMergedRules( const std::string& aClient,
                                                                      const std::string& aBase,
                                                                      const std::string& aOurs,
                                                                      const std::string& aDisk,
                                                                      const std::string& aWhat )
{
    std::vector<BACKPLANE_MERGE::CONFLICT> conflicts;
    std::string merged = BACKPLANE_MERGE::MergeRules( aBase, aOurs, aDisk, conflicts );

    // Validate before writing so callers get parser errors in the response.
    try
    {
        std::vector<std::shared_ptr<DRC_RULE>> parsedRules;
        DRC_RULES_PARSER parser( wxString::FromUTF8( merged ), "EditCustomRules" );
        parser.Parse( parsedRules, nullptr );
    }
    catch( const IO_ERROR& ioe )
    {
        CustomRulesResponse response;
        response.set_status( CRS_INVALID );
        response.set_error_text( ioe.What().ToStdString() );
        return response;
    }

    wxString rulesPath = board()->GetDesignRulesPath();
    wxString writeError;

    if( merged != aDisk
        && !KIPLATFORM::IO::AtomicWriteFile( rulesPath, merged.data(), merged.size(), &writeError ) )
    {
        CustomRulesResponse response;
        response.set_status( CRS_INVALID );
        response.set_error_text( "Failed to write custom rules file: " + writeError.ToStdString() );
        return response;
    }

    // What this client now believes the rules are: what it read plus its own edits (not the
    // other writers' changes it has not seen, or a later whole-list Set would undo them).
    m_rulesBase[aClient] = aOurs;

    // The open editor's DRC engine (router, live DRC) uses the rules at once.
    if( PCB_EDIT_FRAME* f = frame() )
        f->ReloadCustomRules();
    else
        board()->GetDesignSettings().m_DRCEngine->InitEngine( rulesPath );

    COEDIT::CHANGE change;
    change.doc = "rules";
    change.actor = aClient;
    change.description = aWhat;

    for( const BACKPLANE_MERGE::CONFLICT& c : conflicts )
        change.modified.push_back( c.path );

    COEDIT::RecordChange( change );

    if( !conflicts.empty() )
    {
        COEDIT::NOTICE notice;
        notice.kind = "rules_conflict";
        notice.actor = aClient;
        notice.doc = "rules";
        nlohmann::json list = nlohmann::json::array();

        for( const BACKPLANE_MERGE::CONFLICT& c : conflicts )
            list.push_back( { { "rule", c.path }, { "kept_yours", c.ours }, { "file_had", c.theirs } } );

        notice.json = list.dump();
        COEDIT::PostNotice( notice );
    }

    CustomRulesResponse response;
    response.set_status( CRS_VALID );
    return response;
}


HANDLER_RESULT<CustomRulesResponse> API_HANDLER_PCB::handleEditCustomRules(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::EditCustomRules>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    std::vector<std::string> upserts;

    for( const board::CustomRule& rule : aCtx.Request.upsert() )
    {
        wxString error;
        wxString text = DRC_RULE::FormatRuleFromProto( rule, &error );

        if( text.IsEmpty() )
        {
            CustomRulesResponse response;
            response.set_status( CRS_INVALID );
            response.set_error_text( "rule '" + rule.name() + "': " + error.ToStdString() );
            return response;
        }

        upserts.push_back( text.ToStdString( wxConvUTF8 ) );
    }

    std::vector<std::string> removes( aCtx.Request.remove().begin(), aCtx.Request.remove().end() );
    std::string disk = readRulesFile();
    std::string ours = BACKPLANE_MERGE::EditRules( disk, upserts, removes );

    // What this client believes afterwards: what it read before plus these edits.
    std::string belief = BACKPLANE_MERGE::EditRules(
            m_rulesBase.count( aCtx.ClientName ) ? m_rulesBase[aCtx.ClientName] : disk, upserts,
            removes );

    HANDLER_RESULT<CustomRulesResponse> written = writeMergedRules( aCtx.ClientName, disk, ours, disk,
                                                                   "Edit custom rules" );

    if( !written || written->status() == CRS_INVALID )
        return written;

    HANDLER_CONTEXT<GetCustomDesignRules> getCtx = { aCtx.ClientName, GetCustomDesignRules() };
    *getCtx.Request.mutable_board() = aCtx.Request.board();
    HANDLER_RESULT<CustomRulesResponse> reply = handleGetCustomDesignRules( getCtx );
    m_rulesBase[aCtx.ClientName] = belief;   // the reply above is not a read the agent asked for
    return reply;
}


HANDLER_RESULT<types::Vector2> API_HANDLER_PCB::handleGetBoardOrigin(
        const HANDLER_CONTEXT<GetBoardOrigin>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    VECTOR2I origin;
    const BOARD_DESIGN_SETTINGS& settings = board()->GetDesignSettings();

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
        origin = settings.GetGridOrigin();
        break;

    case BOT_DRILL:
        origin = settings.GetAuxOrigin();
        break;

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    types::Vector2 reply;
    PackVector2( reply, origin );
    return reply;
}

HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardOrigin(
        const HANDLER_CONTEXT<SetBoardOrigin>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    VECTOR2I origin = UnpackVector2( aCtx.Request.origin() );

    switch( aCtx.Request.type() )
    {
    case BOT_GRID:
    {
        PCB_EDIT_FRAME* f = frame();

        if( f )
        {
            frame()->CallAfter(
                    [f, origin]()
                    {
                        // gridSetOrigin takes ownership and frees this
                        VECTOR2D*     dorigin = new VECTOR2D( origin );
                        TOOL_MANAGER* mgr = f->GetToolManager();
                        mgr->RunAction( PCB_ACTIONS::gridSetOrigin, dorigin );
                        f->Refresh();
                    } );
        }
        else
        {
            board()->GetDesignSettings().SetGridOrigin( origin );
            pcbContext()->SetContentModified();
        }

        break;
    }

    case BOT_DRILL:
    {
        PCB_EDIT_FRAME* f = frame();

        if( f )
        {
            frame()->CallAfter(
                    [f, origin]()
                    {
                        TOOL_MANAGER* mgr = f->GetToolManager();
                        mgr->RunAction( PCB_ACTIONS::drillSetOrigin, origin );
                        f->Refresh();
                    } );
        }
        else
        {
            board()->GetDesignSettings().SetAuxOrigin( origin );
            pcbContext()->SetContentModified();
        }

        break;
    }

    default:
    case BOT_UNKNOWN:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Unexpected origin type" );
        return tl::unexpected( e );
    }
    }

    return Empty();
}


HANDLER_RESULT<BoardLayerNameResponse> API_HANDLER_PCB::handleGetBoardLayerName(
            const HANDLER_CONTEXT<GetBoardLayerName>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    BoardLayerNameResponse response;

    PCB_LAYER_ID id = FromProtoEnum<PCB_LAYER_ID>( aCtx.Request.layer() );

    response.set_name( board()->GetLayerName( id ) );

    return response;
}


HANDLER_RESULT<BoardLayerResponse> API_HANDLER_PCB::handleGetBoardLayerByName(
            const HANDLER_CONTEXT<GetBoardLayerByName>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );
        !documentValidation )
    {
        return tl::unexpected( documentValidation.error() );
    }

    BoardLayerResponse response;

    PCB_LAYER_ID id = board()->GetLayerID( wxString::FromUTF8( aCtx.Request.name() ) );
    response.set_layer( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( id ) );

    return response;
}


std::optional<TITLE_BLOCK*> API_HANDLER_PCB::getTitleBlock( const DocumentSpecifier& aDocument )
{
    return &context()->GetBoard()->GetTitleBlock();
}


std::optional<PAGE_INFO> API_HANDLER_PCB::getPageSettings( const DocumentSpecifier& aDocument )
{
    return context()->GetBoard()->GetPageSettings();
}


bool API_HANDLER_PCB::setPageSettings( const DocumentSpecifier& aDocument, const PAGE_INFO& aPageInfo )
{
    context()->GetBoard()->SetPageSettings( aPageInfo );
    return true;
}


wxString API_HANDLER_PCB::getDrawingSheetFileName()
{
    return BASE_SCREEN::m_DrawingSheetFileName;
}


void API_HANDLER_PCB::setDrawingSheetFileName( const wxString& aFileName )
{
    BASE_SCREEN::m_DrawingSheetFileName = aFileName;

    if( frame() )
    {
        frame()->Prj().GetProjectFile().m_BoardDrawingSheetFile = aFileName;
        frame()->LoadDrawingSheet();
    }
}


void API_HANDLER_PCB::onModified()
{
    pcbContext()->SetContentModified();

    if( frame() )
    {
        frame()->Refresh();
        frame()->OnModify();
        frame()->UpdateUserInterface();
    }
}


void API_HANDLER_PCB::onItemsCommitted()
{
    // BOARD_COMMIT::Push already updated the view, the connectivity, the ratsnest and the
    // message panel and called OnModify; only the document's own flag is left
    pcbContext()->SetContentModified();

    if( !frame() )
        return;

    // A commit made without the frame (no OnModify) still has to show the board as modified
    if( !frame()->IsContentModified() )
        frame()->OnModify();
}


void API_HANDLER_PCB::onNetSettingsChanged()
{
    if( BOARD* brd = board() )
        brd->SynchronizeNetsAndNetClasses( false );

    // Refresh UI that depends on netclasses, such as the properties panel
    if( TOOL_MANAGER* mgr = toolManager() )
        mgr->ProcessEvent( EVENTS::SelectedItemsModified );

    if( frame() )
    {
        frame()->Refresh();
        frame()->UpdateUserInterface();
    }
}


HANDLER_RESULT<GetDocumentModifiedStateResponse>
API_HANDLER_PCB::handleGetDocumentModifiedState( const HANDLER_CONTEXT<GetDocumentModifiedState>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    GetDocumentModifiedStateResponse response;
    response.set_state( pcbContext()->IsContentModified() ? DocumentModifiedState::DMS_MODIFIED
                                                          : DocumentModifiedState::DMS_UNMODIFIED );
    return response;
}


HANDLER_RESULT<NetsResponse> API_HANDLER_PCB::handleGetNets( const HANDLER_CONTEXT<GetNets>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    NetsResponse response;
    BOARD* board = this->board();

    std::set<wxString> netclassFilter;

    for( const std::string& nc : aCtx.Request.netclass_filter() )
        netclassFilter.insert( wxString( nc.c_str(), wxConvUTF8 ) );

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        NETCLASS* nc = net->GetNetClass();

        if( !netclassFilter.empty() && nc )
        {
            bool inClass = false;

            for( const wxString& filter : netclassFilter )
            {
                if( nc->ContainsNetclassWithName( filter ) )
                {
                    inClass = true;
                    break;
                }
            }

            if( !inClass )
                continue;
        }

        board::types::Net* netProto = response.add_nets();
        netProto->set_name( net->GetNetname() );
        netProto->mutable_code()->set_value( net->GetNetCode() );
    }

    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetConnectedItems(
        const HANDLER_CONTEXT<GetConnectedItems>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    std::set<KICAD_T> typeFilter( types.begin(), types.end() );
    std::vector<BOARD_CONNECTED_ITEM*> sourceItems;

    for( const types::KIID& id : aCtx.Request.items() )
    {
        if( std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) ) )
        {
            if( BOARD_CONNECTED_ITEM* connected = dynamic_cast<BOARD_CONNECTED_ITEM*>( *item ) )
                sourceItems.emplace_back( connected );
        }
    }

    if( sourceItems.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested IDs were found or valid connected items" );
        return tl::unexpected( e );
    }

    GetItemsResponse response;
    std::shared_ptr<CONNECTIVITY_DATA> conn = board()->GetConnectivity();
    std::set<KIID> insertedItems;

    for( BOARD_CONNECTED_ITEM* source : sourceItems )
    {
        for( BOARD_CONNECTED_ITEM* connected : conn->GetConnectedItems( source ) )
        {
            if( filterByType && !typeFilter.contains( connected->Type() ) )
                continue;

            if( !insertedItems.insert( connected->m_Uuid ).second )
                continue;

            connected->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsByNet(
        const HANDLER_CONTEXT<GetItemsByNet>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    if( !filterByType )
        types.assign( { PCB_PAD_T, PCB_VIA_T, PCB_TRACE_T, PCB_ARC_T, PCB_SHAPE_T, PCB_ZONE_T } );

    GetItemsResponse response;
    BOARD* board = this->board();
    std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity();
    std::set<KIID> insertedItems;

    const NETINFO_LIST& nets = board->GetNetInfo();

    for( const board::types::Net& net : aCtx.Request.nets() )
    {
        NETINFO_ITEM* netInfo = nets.GetNetItem( wxString::FromUTF8( net.name() ) );

        if( !netInfo )
            continue;

        for( BOARD_CONNECTED_ITEM* item : conn->GetNetItems( netInfo->GetNetCode(), types ) )
        {
            if( !insertedItems.insert( item->m_Uuid ).second )
                continue;

            item->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<GetItemsResponse> API_HANDLER_PCB::handleGetItemsByNetClass(
        const HANDLER_CONTEXT<GetItemsByNetClass>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    std::vector<KICAD_T> types = parseRequestedItemTypes( aCtx.Request.types() );
    const bool filterByType = aCtx.Request.types_size() > 0;

    if( filterByType && types.empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "none of the requested types are valid for a Board object" );
        return tl::unexpected( e );
    }

    if( !filterByType )
        types.assign( { PCB_PAD_T, PCB_VIA_T, PCB_TRACE_T, PCB_ARC_T, PCB_SHAPE_T, PCB_ZONE_T } );

    std::set<wxString> requestedClasses;

    for( const std::string& netClass : aCtx.Request.net_classes() )
        requestedClasses.insert( wxString( netClass.c_str(), wxConvUTF8 ) );

    GetItemsResponse response;
    BOARD* board = this->board();
    std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity();
    std::set<KIID> insertedItems;

    for( NETINFO_ITEM* net : board->GetNetInfo() )
    {
        if( !net )
            continue;

        NETCLASS* nc = net->GetNetClass();

        if( !requestedClasses.empty() )
        {
            if( !nc )
                continue;

            bool inClass = false;

            for( const wxString& filter : requestedClasses )
            {
                if( nc->ContainsNetclassWithName( filter ) )
                {
                    inClass = true;
                    break;
                }
            }

            if( !inClass )
                continue;
        }

        for( BOARD_CONNECTED_ITEM* item : conn->GetNetItems( net->GetNetCode(), types ) )
        {
            if( !insertedItems.insert( item->m_Uuid ).second )
                continue;

            item->Serialize( *response.add_items() );
        }
    }

    response.set_status( ItemRequestStatus::IRS_OK );
    return response;
}


HANDLER_RESULT<NetClassForNetsResponse> API_HANDLER_PCB::handleGetNetClassForNets(
            const HANDLER_CONTEXT<GetNetClassForNets>& aCtx )
{
    NetClassForNetsResponse response;

    BOARD* board = this->board();
    const NETINFO_LIST& nets = board->GetNetInfo();

    for( const board::types::Net& net : aCtx.Request.net() )
    {
        NETINFO_ITEM* netInfo = nets.GetNetItem( wxString::FromUTF8( net.name() ) );

        if( !netInfo )
            continue;

        auto [pair, rc] = response.mutable_classes()->insert( { net.name(), {} } );
        netInfo->GetNetClass()->Serialize( pair->second );
    }

    return response;
}


HANDLER_RESULT<std::vector<ZONE*>> API_HANDLER_PCB::resolveZonesToFill(
        const google::protobuf::RepeatedPtrField<kiapi::common::types::KIID>& aZoneIds ) const
{
    std::vector<ZONE*> toFill;

    for( const types::KIID& id : aZoneIds )
    {
        std::optional<BOARD_ITEM*> item = getItemById( KIID( id.value() ) );

        if( !item || ( *item )->Type() != PCB_ZONE_T )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "zone with ID {} not found on the board", id.value() ) );
            return tl::unexpected( e );
        }

        ZONE* zone = static_cast<ZONE*>( *item );

        // The filler silently skips rule areas, which would turn this into a false success
        if( zone->GetIsRuleArea() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "zone with ID {} is a rule area and cannot be filled",
                                              id.value() ) );
            return tl::unexpected( e );
        }

        // A repeated id would enqueue concurrent fill tasks for the same zone
        if( !alg::contains( toFill, zone ) )
            toFill.push_back( zone );
    }

    return toFill;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRefillZones( const HANDLER_CONTEXT<RefillZones>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    TOOL_MANAGER* mgr = toolManager();

    // A frame's tool manager always carries the zone filler tool; headless sessions start with a
    // bare tool manager and register it on first use, like the CLI jobs do.
    if( !mgr->FindTool( ZONE_FILLER_TOOL_NAME ) )
        mgr->RegisterTool( new ZONE_FILLER_TOOL );

    if( aCtx.Request.zones().empty() )
    {
        if( frame() )
        {
            frame()->CallAfter( [mgr]()
                                {
                                    mgr->RunAction( PCB_ACTIONS::zoneFillAll );
                                } );
        }
        else
        {
            // Headless sessions have no event loop to defer to; fill synchronously through the
            // same tool the CLI jobs use.
            mgr->GetTool<ZONE_FILLER_TOOL>()->FillAllZones( nullptr, nullptr, true );
        }
    }
    else
    {
        HANDLER_RESULT<std::vector<ZONE*>> toFill = resolveZonesToFill( aCtx.Request.zones() );

        if( !toFill )
            return tl::unexpected( toFill.error() );

        std::unique_ptr<COMMIT> commit = createCommit();
        ZONE_FILLER             filler( board(), commit.get() );

        if( !filler.Fill( *toFill ) )
        {
            commit->Revert();

            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_UNKNOWN );
            e.set_error_message( "zone fill failed" );
            return tl::unexpected( e );
        }

        commit->Push( _( "Fill Zone(s)" ), SKIP_CONNECTIVITY | ZONE_FILL_OP );

        // Push skipped connectivity, so run the same post-fill refresh as the interactive fill
        mgr->GetTool<ZONE_FILLER_TOOL>()->PostFillRefresh( frame() == nullptr );
    }

    onModified();
    return Empty();
}


HANDLER_RESULT<ImportNetlistResponse> API_HANDLER_PCB::handleImportNetlist( const HANDLER_CONTEXT<ImportNetlist>& aCtx )
{
    HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() );

    if( !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    wxFileName netlistPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.netlist_path() ) ) );

    if( !netlistPath.IsOk() || !netlistPath.FileExists() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message(
                fmt::format( "netlist file '{}' could not be opened", netlistPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    PCB_CONTEXT*       ctx = pcbContext();
    WX_STRING_REPORTER reporter;

    const bool lookupByTimestamp = aCtx.Request.match_mode() != NetlistMatchMode::NMM_REFERENCE;

    NETLIST netlist;
    netlist.SetFindByTimeStamp( lookupByTimestamp );
    netlist.SetReplaceFootprints( aCtx.Request.update_footprints() );

    if( !ctx->ReadNetlistFromFile( netlistPath.GetFullPath(), netlist, reporter ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "unable to handle netlist file '{}': {}",
                                          netlistPath.GetFullPath().ToStdString(),
                                          reporter.GetMessages().ToStdString() ) );
        return tl::unexpected( e );
    }

    std::unique_ptr<BOARD_NETLIST_UPDATER> updater = ctx->MakeNetlistUpdater();

    updater->SetReporter( &reporter );
    updater->SetIsDryRun( aCtx.Request.dry_run() );
    updater->SetWillDrag( false );
    updater->SetLookupByTimestamp( lookupByTimestamp );
    updater->SetDeleteUnusedFootprints( aCtx.Request.delete_extra_footprints() );
    updater->SetReplaceFootprints( aCtx.Request.update_footprints() );
    updater->SetTransferGroups( aCtx.Request.transfer_groups() );
    updater->SetOverrideLocks( aCtx.Request.override_locks() );
    updater->SetUpdateFields( true );

    const bool success = updater->UpdateNetlist( netlist );

    if( !aCtx.Request.dry_run() && success )
        ctx->OnNetlistChanged( *updater );

    ImportNetlistResponse response;
    response.set_report( reporter.GetMessages().ToUTF8() );
    response.set_error_count( updater->GetErrorCount() );
    response.set_warning_count( updater->GetWarningCount() );
    response.set_new_footprint_count( updater->GetNewFootprintCount() );
    return response;
}


HANDLER_RESULT<ImportSpecctraSessionResponse>
API_HANDLER_PCB::handleImportSpecctraSession( const HANDLER_CONTEXT<ImportSpecctraSession>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    wxFileName sesPath( project().AbsolutePath( wxString::FromUTF8( aCtx.Request.session_path() ) ) );

    if( !sesPath.IsOk() || !sesPath.FileExists() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message(
                fmt::format( "session file '{}' could not be opened", sesPath.GetFullPath().ToStdString() ) );
        return tl::unexpected( e );
    }

    int                skippedCount = 0;
    BOARD_COMMIT*      commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    WX_STRING_REPORTER reporter;

    try
    {
        DSN::ImportSpecctraSession( board(), sesPath.GetFullPath(), *commit, &reporter, &skippedCount );
    }
    catch( const IO_ERROR& ioe )
    {
        commit->Revert();

        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "failed to import session '{}': {}", sesPath.GetFullPath().ToStdString(),
                                          ioe.What().ToStdString() ) );
        return tl::unexpected( e );
    }

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Imported Specctra session via API" ) );

    ImportSpecctraSessionResponse response;
    response.set_report( reporter.GetMessages().ToUTF8() );
    response.set_skipped_count( skippedCount );
    return response;
}


HANDLER_RESULT<BoardEditorAppearanceSettings> API_HANDLER_PCB::handleGetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<GetBoardEditorAppearanceSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetBoardEditorAppearanceSettings" ) )
        return tl::unexpected( *headless );

    BoardEditorAppearanceSettings reply;

    // TODO: might be nice to put all these things in one place and have it derive SERIALIZABLE

    const PCB_DISPLAY_OPTIONS& displayOptions = frame()->GetDisplayOptions();

    reply.set_inactive_layer_display( ToProtoEnum<HIGH_CONTRAST_MODE, InactiveLayerDisplayMode>(
            displayOptions.m_ContrastModeDisplay ) );
    reply.set_net_color_display(
            ToProtoEnum<NET_COLOR_MODE, NetColorDisplayMode>( displayOptions.m_NetColorMode ) );

    reply.set_board_flip( frame()->GetCanvas()->GetView()->IsMirroredX()
                                  ? BoardFlipMode::BFM_FLIPPED_X
                                  : BoardFlipMode::BFM_NORMAL );

    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();

    reply.set_ratsnest_display( ToProtoEnum<RATSNEST_MODE, RatsnestDisplayMode>(
            editorSettings->m_Display.m_RatsnestMode ) );

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardEditorAppearanceSettings(
        const HANDLER_CONTEXT<SetBoardEditorAppearanceSettings>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SetBoardEditorAppearanceSettings" ) )
        return tl::unexpected( *headless );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    PCB_DISPLAY_OPTIONS options = frame()->GetDisplayOptions();
    PCBNEW_SETTINGS* editorSettings = frame()->GetPcbNewSettings();
    const BoardEditorAppearanceSettings& newSettings = aCtx.Request.settings();

    options.m_ContrastModeDisplay =
            FromProtoEnum<HIGH_CONTRAST_MODE>( newSettings.inactive_layer_display() );
    options.m_NetColorMode =
            FromProtoEnum<NET_COLOR_MODE>( newSettings.net_color_display() );
    options.m_FlipBoardView = newSettings.board_flip() == BoardFlipMode::BFM_FLIPPED_X;

    editorSettings->m_Display.m_RatsnestMode =
            FromProtoEnum<RATSNEST_MODE>( newSettings.ratsnest_display() );

    frame()->SetDisplayOptions( options );
    frame()->GetCanvas()->GetView()->UpdateAllLayersColor();
    frame()->GetCanvas()->Refresh();

    return Empty();
}


HANDLER_RESULT<BoardPlotSettingsResponse>
API_HANDLER_PCB::handleGetBoardPlotSettings( const HANDLER_CONTEXT<GetBoardPlotSettings>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    const PCB_PLOT_PARAMS& plotOpts = board()->GetPlotOptions();

    BoardPlotSettingsResponse response;
    BoardPlotSettings*        settings = response.mutable_plot_settings();

    board::PackLayerSet( *settings->mutable_layers(), plotOpts.GetLayerSelection() );

    for( PCB_LAYER_ID layer : plotOpts.GetPlotOnAllLayersSequence() )
        settings->add_common_layers( ToProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( layer ) );

    settings->set_mirror( plotOpts.GetMirror() );
    settings->set_black_and_white( plotOpts.GetBlackAndWhite() );
    settings->set_negative( plotOpts.GetNegative() );
    settings->set_scale( plotOpts.GetScale() );

    settings->set_sketch_pads_on_fab_layers( plotOpts.GetSketchPadsOnFabLayers() );
    settings->set_hide_dnp_footprints_on_fab_layers( plotOpts.GetHideDNPFPsOnFabLayers() );
    settings->set_sketch_dnp_footprints_on_fab_layers( plotOpts.GetSketchDNPFPsOnFabLayers() );
    settings->set_crossout_dnp_footprints_on_fab_layers( plotOpts.GetCrossoutDNPFPsOnFabLayers() );

    settings->set_plot_footprint_values( plotOpts.GetPlotValue() );
    settings->set_plot_reference_designators( plotOpts.GetPlotReference() );
    settings->set_plot_drawing_sheet( plotOpts.GetPlotFrameRef() );
    settings->set_subtract_solder_mask_from_silk( plotOpts.GetSubtractMaskFromSilk() );
    settings->set_plot_pad_numbers( plotOpts.GetPlotPadNumbers() );

    settings->set_drill_marks( ToProtoEnum<DRILL_MARKS, PlotDrillMarks>( plotOpts.GetDrillMarksType() ) );
    settings->set_use_drill_origin( plotOpts.GetUseAuxOrigin() );

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetBoardPlotSettings( const HANDLER_CONTEXT<SetBoardPlotSettings>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    const BoardPlotSettings& settings = aCtx.Request.plot_settings();
    PCB_PLOT_PARAMS          plotOpts = board()->GetPlotOptions();

    plotOpts.SetLayerSelection( board::UnpackLayerSet( settings.layers() ) );

    LSEQ commonLayers;

    for( int layer : settings.common_layers() )
    {
        PCB_LAYER_ID layerId =
                FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( static_cast<board::types::BoardLayer>( layer ) );

        if( !IsPcbLayer( layerId ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "SetBoardPlotSettings contains an invalid layer {}",
                                              magic_enum::enum_name( layerId ) ) );
            return tl::unexpected( e );
        }

        commonLayers.push_back( layerId );
    }

    plotOpts.SetPlotOnAllLayersSequence( commonLayers );

    plotOpts.SetMirror( settings.mirror() );
    plotOpts.SetBlackAndWhite( settings.black_and_white() );
    plotOpts.SetNegative( settings.negative() );
    plotOpts.SetScale( settings.scale() );

    plotOpts.SetSketchPadsOnFabLayers( settings.sketch_pads_on_fab_layers() );
    plotOpts.SetHideDNPFPsOnFabLayers( settings.hide_dnp_footprints_on_fab_layers() );
    plotOpts.SetSketchDNPFPsOnFabLayers( settings.sketch_dnp_footprints_on_fab_layers() );
    plotOpts.SetCrossoutDNPFPsOnFabLayers( settings.crossout_dnp_footprints_on_fab_layers() );

    plotOpts.SetPlotValue( settings.plot_footprint_values() );
    plotOpts.SetPlotReference( settings.plot_reference_designators() );
    plotOpts.SetPlotFrameRef( settings.plot_drawing_sheet() );
    plotOpts.SetSubtractMaskFromSilk( settings.subtract_solder_mask_from_silk() );
    plotOpts.SetPlotPadNumbers( settings.plot_pad_numbers() );

    plotOpts.SetDrillMarksType( FromProtoEnum<DRILL_MARKS>( settings.drill_marks() ) );
    plotOpts.SetUseAuxOrigin( settings.use_drill_origin() );

    board()->SetPlotOptions( plotOpts );

    onModified();

    return Empty();
}


HANDLER_RESULT<InjectDrcErrorResponse> API_HANDLER_PCB::handleInjectDrcError(
        const HANDLER_CONTEXT<InjectDrcError>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.board() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    SEVERITY severity = FromProtoEnum<SEVERITY>( aCtx.Request.severity() );
    int      layer = severity == RPT_SEVERITY_WARNING ? LAYER_DRC_WARNING : LAYER_DRC_ERROR;
    int      code = severity == RPT_SEVERITY_WARNING ? DRCE_GENERIC_WARNING : DRCE_GENERIC_ERROR;

    std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( code );

    drcItem->SetErrorMessage( wxString::FromUTF8( aCtx.Request.message() ) );

    std::vector<KIID> ids;

    for( const auto& id : aCtx.Request.items() )
        ids.emplace_back( KIID( id.value() ) );

    if( !ids.empty() )
        drcItem->SetItems( ids );

    const auto& pos = aCtx.Request.position();
    VECTOR2I    position( static_cast<int>( pos.x_nm() ), static_cast<int>( pos.y_nm() ) );

    PCB_MARKER* marker = new PCB_MARKER( drcItem, position, layer );

    COMMIT* commit = getCurrentCommit( aCtx.ClientName );
    commit->Add( marker );

    if( !m_activeClients.count( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "API injected DRC marker" ) );

    InjectDrcErrorResponse response;
    response.mutable_marker()->set_value( marker->GetUUID().AsStdString() );

    return response;
}


std::optional<ApiResponseStatus> ValidateUnitsInchMm( types::Units aUnits,
                                                      const std::string& aCommandName )
{
    if( aUnits == types::Units::U_INCH || aUnits == types::Units::U_MM
        || aUnits == types::Units::U_UNKNOWN )
    {
        return std::nullopt;
    }

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( fmt::format( "{} supports only inch and mm units", aCommandName ) );
    return e;
}


std::optional<ApiResponseStatus>
ValidatePaginationModeForSingleOrPerFile( kiapi::board::jobs::BoardJobPaginationMode aMode,
                                          const std::string& aCommandName )
{
    if( aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_UNKNOWN
        || aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_ALL_LAYERS_ONE_PAGE
        || aMode == kiapi::board::jobs::BoardJobPaginationMode::BJPM_EACH_LAYER_OWN_FILE )
    {
        return std::nullopt;
    }

    ApiResponseStatus e;
    e.set_status( ApiStatusCode::AS_BAD_REQUEST );
    e.set_error_message( fmt::format( "{} does not support EACH_LAYER_OWN_PAGE pagination mode",
                                      aCommandName ) );
    return e;
}


std::optional<ApiResponseStatus> ApplyBoardPlotSettings( const BoardPlotSettings& aSettings,
                                                         JOB_EXPORT_PCB_PLOT& aJob )
{
    for( int layer : aSettings.layers() )
    {
        PCB_LAYER_ID layerId = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                static_cast<board::types::BoardLayer>( layer ) );

        if( !IsPcbLayer( static_cast<int>( layerId ) ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Board plot settings contain an invalid layer" );
            return e;
        }

        aJob.m_plotLayerSequence.push_back( layerId );
    }

    for( int layer : aSettings.common_layers() )
    {
        PCB_LAYER_ID layerId = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>(
                static_cast<board::types::BoardLayer>( layer ) );

        if( !IsPcbLayer( static_cast<int>( layerId ) ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Board plot settings contain an invalid common layer" );
            return e;
        }

        aJob.m_plotOnAllLayersSequence.push_back( layerId );
    }

    aJob.m_colorTheme = wxString::FromUTF8( aSettings.color_theme() );
    aJob.m_drawingSheet = wxString::FromUTF8( aSettings.drawing_sheet() );
    aJob.m_variant = wxString::FromUTF8( aSettings.variant() );

    aJob.m_mirror = aSettings.mirror();
    aJob.m_blackAndWhite = aSettings.black_and_white();
    aJob.m_negative = aSettings.negative();
    aJob.m_scale = aSettings.scale();

    aJob.m_sketchPadsOnFabLayers = aSettings.sketch_pads_on_fab_layers();
    aJob.m_hideDNPFPsOnFabLayers = aSettings.hide_dnp_footprints_on_fab_layers();
    aJob.m_sketchDNPFPsOnFabLayers = aSettings.sketch_dnp_footprints_on_fab_layers();
    aJob.m_crossoutDNPFPsOnFabLayers = aSettings.crossout_dnp_footprints_on_fab_layers();

    aJob.m_plotFootprintValues = aSettings.plot_footprint_values();
    aJob.m_plotRefDes = aSettings.plot_reference_designators();
    aJob.m_plotDrawingSheet = aSettings.plot_drawing_sheet();
    aJob.m_subtractSolderMaskFromSilk = aSettings.subtract_solder_mask_from_silk();
    aJob.m_plotPadNumbers = aSettings.plot_pad_numbers();

    aJob.m_drillShapeOption = FromProtoEnum<DRILL_MARKS>( aSettings.drill_marks() );

    aJob.m_useDrillOrigin = aSettings.use_drill_origin();
    aJob.m_checkZonesBeforePlot = aSettings.check_zones_before_plot();

    return std::nullopt;
}


HANDLER_RESULT<types::RunJobResponse> ExecuteBoardJob( PCB_CONTEXT* aContext, JOB& aJob )
{
    types::RunJobResponse response;
    WX_STRING_REPORTER reporter;

    if( !aContext || !aContext->GetKiway() )
    {
        response.set_status( types::JobStatus::JS_ERROR );
        response.set_message( "Internal error" );
        wxCHECK_MSG( false, response, "context missing valid kiway in ExecuteBoardJob?" );
        return response;
    }

    int exitCode = aContext->GetKiway()->ProcessJob( KIWAY::FACE_PCB, &aJob, &reporter );

    for( const JOB_OUTPUT& output : aJob.GetOutputs() )
        response.add_output_path( output.m_outputPath.ToUTF8() );

    if( exitCode == 0 )
    {
        response.set_status( types::JobStatus::JS_SUCCESS );
        return response;
    }

    response.set_status( types::JobStatus::JS_ERROR );
    response.set_message( fmt::format( "Board export job '{}' failed with exit code {}: {}",
                                       aJob.GetType(), exitCode,
                                       reporter.GetMessages().ToStdString() ) );
    return response;
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExport3D(
        const HANDLER_CONTEXT<RunBoardJobExport3D>& aCtx )
{
    return runBoardJobExport3D( aCtx.Request, false );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardStructureExport3D(
        const HANDLER_CONTEXT<kiapi::backplane::RunBoardStructureExport3D>& aCtx )
{
    return runBoardJobExport3D( aCtx.Request.job(), aCtx.Request.folded() );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::runBoardJobExport3D(
        const RunBoardJobExport3D& aRequest, bool aFolded )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aRequest.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_3D job;
    job.m_3dparams.m_Folded = aFolded;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aRequest.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_3D::FORMAT>( aRequest.format() );

    job.m_variant = wxString::FromUTF8( aRequest.variant() );
    job.m_3dparams.m_NetFilter = wxString::FromUTF8( aRequest.net_filter() );
    job.m_3dparams.m_ComponentFilter = wxString::FromUTF8( aRequest.component_filter() );

    job.m_hasUserOrigin = aRequest.has_user_origin();
    job.m_3dparams.m_Origin = VECTOR2D( aRequest.origin().x_nm(), aRequest.origin().y_nm() );

    job.m_3dparams.m_Overwrite = aRequest.overwrite();
    job.m_3dparams.m_UseGridOrigin = aRequest.use_grid_origin();
    job.m_3dparams.m_UseDrillOrigin = aRequest.use_drill_origin();
    job.m_3dparams.m_UseDefinedOrigin = aRequest.use_defined_origin() || aRequest.has_user_origin();
    job.m_3dparams.m_UsePcbCenterOrigin = aRequest.use_pcb_center_origin();

    job.m_3dparams.m_IncludeUnspecified = aRequest.include_unspecified();
    job.m_3dparams.m_IncludeDNP = aRequest.include_dnp();
    job.m_3dparams.m_SubstModels = aRequest.substitute_models();

    job.m_3dparams.m_BoardOutlinesChainingEpsilon = aRequest.board_outlines_chaining_epsilon();
    job.m_3dparams.m_BoardOnly = aRequest.board_only();
    job.m_3dparams.m_CutViasInBody = aRequest.cut_vias_in_body();
    job.m_3dparams.m_ExportBoardBody = aRequest.export_board_body();
    job.m_3dparams.m_ExportComponents = aRequest.export_components();
    job.m_3dparams.m_ExportTracksVias = aRequest.export_tracks_and_vias();
    job.m_3dparams.m_ExportPads = aRequest.export_pads();
    job.m_3dparams.m_ExportZones = aRequest.export_zones();
    job.m_3dparams.m_ExportInnerCopper = aRequest.export_inner_copper();
    job.m_3dparams.m_ExportSilkscreen = aRequest.export_silkscreen();
    job.m_3dparams.m_ExportSoldermask = aRequest.export_soldermask();
    job.m_3dparams.m_FuseShapes = aRequest.fuse_shapes();
    job.m_3dparams.m_FillAllVias = aRequest.fill_all_vias();
    job.m_3dparams.m_OptimizeStep = aRequest.optimize_step();
    job.m_3dparams.m_ExtraPadThickness = aRequest.extra_pad_thickness();

    job.m_vrmlUnits = FromProtoEnum<JOB_EXPORT_PCB_3D::VRML_UNITS>( aRequest.vrml_units() );

    job.m_vrmlModelDir = wxString::FromUTF8( aRequest.vrml_model_dir() );
    job.m_vrmlRelativePaths = aRequest.vrml_relative_paths();

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportRender(
        const HANDLER_CONTEXT<RunBoardJobExportRender>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_PCB_RENDER job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_PCB_RENDER::FORMAT>( aCtx.Request.format() );
    job.m_quality = FromProtoEnum<JOB_PCB_RENDER::QUALITY>( aCtx.Request.quality() );
    job.m_bgStyle = FromProtoEnum<JOB_PCB_RENDER::BG_STYLE>( aCtx.Request.background_style() );

    job.m_width = aCtx.Request.width();
    job.m_height = aCtx.Request.height();
    job.m_appearancePreset = aCtx.Request.appearance_preset();
    job.m_useBoardStackupColors = aCtx.Request.use_board_stackup_colors();

    job.m_side = FromProtoEnum<JOB_PCB_RENDER::SIDE>( aCtx.Request.side() );

    job.m_zoom = aCtx.Request.zoom();
    job.m_perspective = aCtx.Request.perspective();

    job.m_rotation = UnpackVector3D( aCtx.Request.rotation() );
    job.m_pan = UnpackVector3D( aCtx.Request.pan() );
    job.m_pivot = UnpackVector3D( aCtx.Request.pivot() );

    job.m_proceduralTextures = aCtx.Request.procedural_textures();
    job.m_floor = aCtx.Request.floor();
    job.m_antiAlias = aCtx.Request.anti_alias();
    job.m_postProcess = aCtx.Request.post_process();

    job.m_lightTopIntensity = UnpackVector3D( aCtx.Request.light_top_intensity() );
    job.m_lightBottomIntensity = UnpackVector3D( aCtx.Request.light_bottom_intensity() );
    job.m_lightCameraIntensity = UnpackVector3D( aCtx.Request.light_camera_intensity() );
    job.m_lightSideIntensity = UnpackVector3D( aCtx.Request.light_side_intensity() );
    job.m_lightSideElevation = aCtx.Request.light_side_elevation();

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportSvg(
        const HANDLER_CONTEXT<RunBoardJobExportSvg>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_SVG job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_fitPageToBoard = aCtx.Request.fit_page_to_board();
    job.m_precision = aCtx.Request.precision();

        if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportSvg" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_SVG::GEN_MODE>( aCtx.Request.page_mode() );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportDxf(
        const HANDLER_CONTEXT<RunBoardJobExportDxf>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_DXF job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_plotGraphicItemsUsingContours = aCtx.Request.plot_graphic_items_using_contours();
    job.m_polygonMode = aCtx.Request.polygon_mode();

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportDxf" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_dxfUnits = FromProtoEnum<JOB_EXPORT_PCB_DXF::DXF_UNITS>( aCtx.Request.units() );

        if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportDxf" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_DXF::GEN_MODE>( aCtx.Request.page_mode() );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPdf(
        const HANDLER_CONTEXT<RunBoardJobExportPdf>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_PDF job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    job.m_pdfFrontFPPropertyPopups = aCtx.Request.front_footprint_property_popups();
    job.m_pdfBackFPPropertyPopups = aCtx.Request.back_footprint_property_popups();
    job.m_pdfMetadata = aCtx.Request.include_metadata();
    job.m_pdfSingle = aCtx.Request.single_document();
    job.m_pdfBackgroundColor = wxString::FromUTF8( aCtx.Request.background_color() );

    job.m_pdfGenMode = FromProtoEnum<JOB_EXPORT_PCB_PDF::GEN_MODE>( aCtx.Request.page_mode() );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPs(
        const HANDLER_CONTEXT<RunBoardJobExportPs>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_PS job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    if( std::optional<ApiResponseStatus> paginationError =
            ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(),
                                                      "RunBoardJobExportPs" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_PS::GEN_MODE>( aCtx.Request.page_mode() );

    job.m_trackWidthCorrection = aCtx.Request.track_width_correction();
    job.m_XScaleAdjust = aCtx.Request.x_scale_adjust();
    job.m_YScaleAdjust = aCtx.Request.y_scale_adjust();
    job.m_forceA4 = aCtx.Request.force_a4();
    job.m_useGlobalSettings = aCtx.Request.use_global_settings();

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse>
API_HANDLER_PCB::handleRunBoardJobExportPng( const HANDLER_CONTEXT<RunBoardJobExportPng>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_PNG job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
        return tl::unexpected( *err );

    if( std::optional<ApiResponseStatus> paginationError =
                ValidatePaginationModeForSingleOrPerFile( aCtx.Request.page_mode(), "RunBoardJobExportPng" ) )
    {
        return tl::unexpected( *paginationError );
    }

    job.m_genMode = FromProtoEnum<JOB_EXPORT_PCB_PNG::GEN_MODE>( aCtx.Request.page_mode() );

    if( aCtx.Request.has_dpi() )
    {
        int dpi = aCtx.Request.dpi();

        if( dpi < MIN_PNG_DPI || dpi > MAX_PNG_DPI )
        {
            ApiResponseStatus status;
            status.set_status( ApiStatusCode::AS_BAD_REQUEST );
            status.set_error_message( fmt::format( "dpi must be between {} and {}", MIN_PNG_DPI, MAX_PNG_DPI ) );
            return tl::unexpected( status );
        }

        job.m_dpi = dpi;
    }

    // Unknown -> default AA on
    job.m_antialias = aCtx.Request.antialiasing() != types::AntialiasingMode::AAM_NONE;
    job.m_useBackgroundColor = aCtx.Request.plot_background_color();

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportGerbers(
        const HANDLER_CONTEXT<RunBoardJobExportGerbers>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_GERBERS job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_useBoardPlotParams = aCtx.Request.use_board_plot_params();

    if( !job.m_useBoardPlotParams )
    {
        if( std::optional<ApiResponseStatus> err = ApplyBoardPlotSettings( aCtx.Request.plot_settings(), job ) )
            return tl::unexpected( *err );
    }

    job.m_createJobsFile = aCtx.Request.create_gerber_job_file();
    job.m_includeNetlistAttributes = aCtx.Request.include_netlist_attributes();
    job.m_useX2Format = aCtx.Request.use_x2_format();
    job.m_disableApertureMacros = aCtx.Request.disable_aperture_macros();
    job.m_useProtelFileExtension = aCtx.Request.use_protel_file_extensions();

    switch( aCtx.Request.precision() )
    {
    default:
    case GerberPrecision::GP_5: job.m_precision = 5; break;
    case GerberPrecision::GP_6: job.m_precision = 6; break;
    }

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportDrill(
        const HANDLER_CONTEXT<RunBoardJobExportDrill>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_DRILL job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_FORMAT>( aCtx.Request.format() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportDrill" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_drillUnits = FromProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_UNITS>( aCtx.Request.units() );
    job.m_drillOrigin = FromProtoEnum<JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN>( aCtx.Request.origin() );
    job.m_zeroFormat = FromProtoEnum<JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT>( aCtx.Request.zeros_format() );

    if( aCtx.Request.has_excellon() )
    {
        const ExcellonFormatOptions& excellonOptions = aCtx.Request.excellon();

        if( excellonOptions.has_mirror_y() )
            job.m_excellonMirrorY = excellonOptions.mirror_y();

        if( excellonOptions.has_minimal_header() )
            job.m_excellonMinimalHeader = excellonOptions.minimal_header();

        if( excellonOptions.has_combine_pth_npth() )
            job.m_excellonCombinePTHNPTH = excellonOptions.combine_pth_npth();

        if( excellonOptions.has_route_oval_holes() )
            job.m_excellonOvalDrillRoute = excellonOptions.route_oval_holes();
    }

    if( aCtx.Request.map_format() != DrillMapFormat::DMF_UNKNOWN )
    {
        job.m_generateMap = true;
        job.m_mapFormat = FromProtoEnum<JOB_EXPORT_PCB_DRILL::MAP_FORMAT>( aCtx.Request.map_format() );
    }

    job.m_gerberPrecision = aCtx.Request.gerber_precision() == DrillGerberPrecision::DGP_4_5 ? 5 : 6;

    if( aCtx.Request.has_gerber_generate_tenting() )
        job.m_generateTenting = aCtx.Request.gerber_generate_tenting();

    if( aCtx.Request.report_format() != DrillReportFormat::DRF_UNKNOWN )
    {
        job.m_generateReport = true;

        if( aCtx.Request.has_report_filename() )
            job.m_reportPath = wxString::FromUTF8( aCtx.Request.report_filename() );
    }

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportPosition(
        const HANDLER_CONTEXT<RunBoardJobExportPosition>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_POS job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( aCtx.Request.has_use_drill_place_file_origin() )
        job.m_useDrillPlaceFileOrigin = aCtx.Request.use_drill_place_file_origin();

    job.m_smdOnly = aCtx.Request.smd_only();
    job.m_excludeFootprintsWithTh = aCtx.Request.exclude_footprints_with_th();
    job.m_excludeDNP = aCtx.Request.exclude_dnp();
    job.m_excludeBOM = aCtx.Request.exclude_from_bom();
    job.m_negateBottomX = aCtx.Request.negate_bottom_x();
    job.m_singleFile = aCtx.Request.single_file();
    job.m_nakedFilename = aCtx.Request.naked_filename();
    if( aCtx.Request.has_include_board_edge_for_gerber() )
        job.m_gerberBoardEdge = aCtx.Request.include_board_edge_for_gerber();

    job.m_variant = wxString::FromUTF8( aCtx.Request.variant() );

    job.m_side = FromProtoEnum<JOB_EXPORT_PCB_POS::SIDE>( aCtx.Request.side() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportPosition" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_POS::UNITS>( aCtx.Request.units() );
    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_POS::FORMAT>( aCtx.Request.format() );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportGencad(
        const HANDLER_CONTEXT<RunBoardJobExportGencad>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_GENCAD job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_flipBottomPads = aCtx.Request.flip_bottom_pads();
    job.m_useIndividualShapes = aCtx.Request.use_individual_shapes();
    job.m_storeOriginCoords = aCtx.Request.store_origin_coords();
    job.m_useDrillOrigin = aCtx.Request.use_drill_origin();
    job.m_useUniquePins = aCtx.Request.use_unique_pins();

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportSpecctraDsn(
        const HANDLER_CONTEXT<RunBoardJobExportSpecctraDsn>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_SPECCTRA_DSN job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportIpc2581(
        const HANDLER_CONTEXT<RunBoardJobExportIpc2581>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_IPC2581 job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_drawingSheet = wxString::FromUTF8( aCtx.Request.drawing_sheet() );
    if( !aCtx.Request.variant().empty() )
    {
        job.m_variantNames.push_back( wxString::FromUTF8( aCtx.Request.variant() ) );
    }

    if( aCtx.Request.has_precision() )
        job.m_precision = aCtx.Request.precision();

    job.m_compress = aCtx.Request.compress();
    job.m_colInternalId = wxString::FromUTF8( aCtx.Request.internal_id_column() );
    job.m_colMfgPn = wxString::FromUTF8( aCtx.Request.manufacturer_part_number_column() );
    job.m_colMfg = wxString::FromUTF8( aCtx.Request.manufacturer_column() );
    job.m_colDistPn = wxString::FromUTF8( aCtx.Request.distributor_part_number_column() );
    job.m_colDist = wxString::FromUTF8( aCtx.Request.distributor_column() );
    job.m_bomRev = wxString::FromUTF8( aCtx.Request.bom_revision() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportIpc2581" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_FAB::UNITS>( aCtx.Request.units() );
    job.m_checkZonesBeforeExport = aCtx.Request.check_zones();
    job.m_version = FromProtoEnum<JOB_EXPORT_PCB_IPC2581::IPC2581_VERSION>( aCtx.Request.version() );
    job.m_refDes = FromProtoEnum<JOB_EXPORT_PCB_IPC2581::REF_DES>( aCtx.Request.ref_des() );
    ApplyFabExportContent( aCtx.Request.content(), job );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportIpcD356(
        const HANDLER_CONTEXT<RunBoardJobExportIpcD356>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_IPCD356 job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    return ExecuteBoardJob( pcbContext(), job );
}


void ApplyFabExportContent( const FabExportContent& aContent, JOB_EXPORT_PCB_FAB& aJob )
{
    aJob.m_dataSet = FromProtoEnum<JOB_EXPORT_PCB_FAB::DATA_SET>( aContent.data_set() );
    aJob.m_sections = aContent.sections().empty()
                              ? std::nullopt
                              : std::optional<wxString>( wxString::FromUTF8( aContent.sections() ) );
    aJob.m_netNames = FromProtoEnum<JOB_EXPORT_PCB_FAB::NET_NAMES>( aContent.net_names() );
}


void ApplyOdbJobRequest( const RunBoardJobExportODB& aRequest, JOB_EXPORT_PCB_ODB& aJob )
{
    aJob.m_drawingSheet = wxString::FromUTF8( aRequest.drawing_sheet() );

    if( aRequest.variant_names_size() > 0 )
    {
        for( const std::string& name : aRequest.variant_names() )
            aJob.m_variantNames.push_back( wxString::FromUTF8( name ) );
    }
    else if( !aRequest.variant().empty() )
    {
        aJob.m_variantNames.push_back( wxString::FromUTF8( aRequest.variant() ) );
    }

    aJob.m_variantPackaging = FromProtoEnum<JOB_EXPORT_PCB_ODB::VARIANT_PACKAGING>(
            aRequest.variant_packaging() );

    if( aRequest.has_precision() )
        aJob.m_precision = aRequest.precision();

    aJob.m_units = FromProtoEnum<JOB_EXPORT_PCB_FAB::UNITS>( aRequest.units() );
    aJob.m_checkZonesBeforeExport = aRequest.check_zones();
    aJob.m_compressionMode = FromProtoEnum<JOB_EXPORT_PCB_ODB::ODB_COMPRESSION>( aRequest.compression() );
    aJob.m_colMfgPn = wxString::FromUTF8( aRequest.manufacturer_part_number_column() );
    aJob.m_origin = FromProtoEnum<JOB_EXPORT_PCB_ODB::ORIGIN>( aRequest.origin() );
    aJob.m_productName = wxString::FromUTF8( aRequest.product_name() );
    ApplyFabExportContent( aRequest.content(), aJob );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportODB(
        const HANDLER_CONTEXT<RunBoardJobExportODB>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_ODB job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportODB" ) )
    {
        return tl::unexpected( *unitError );
    }

    ApplyOdbJobRequest( aCtx.Request, job );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse> API_HANDLER_PCB::handleRunBoardJobExportStats(
        const HANDLER_CONTEXT<RunBoardJobExportStats>& aCtx )
{
    if( HANDLER_RESULT<bool> validation = validateDocument( aCtx.Request.job_settings().document() ); !validation )
        return tl::unexpected( validation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    JOB_EXPORT_PCB_STATS job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_format = FromProtoEnum<JOB_EXPORT_PCB_STATS::OUTPUT_FORMAT>( aCtx.Request.format() );

    if( std::optional<ApiResponseStatus> unitError =
            ValidateUnitsInchMm( aCtx.Request.units(), "RunBoardJobExportStats" ) )
    {
        return tl::unexpected( *unitError );
    }

    job.m_units = FromProtoEnum<JOB_EXPORT_PCB_STATS::UNITS>( aCtx.Request.units() );

    job.m_excludeFootprintsWithoutPads = aCtx.Request.exclude_footprints_without_pads();
    job.m_subtractHolesFromBoardArea = aCtx.Request.subtract_holes_from_board_area();
    job.m_subtractHolesFromCopperAreas = aCtx.Request.subtract_holes_from_copper_areas();

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<types::RunJobResponse>
API_HANDLER_PCB::handleRunBoardJobExportIdf( const HANDLER_CONTEXT<RunBoardJobExportIdf>& aCtx )
{
    if( HANDLER_RESULT<bool> valid = validateDocument( aCtx.Request.job_settings().document() ); !valid )
        return tl::unexpected( valid.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    switch( aCtx.Request.units() )
    {
    case types::Units::U_UNKNOWN:
    case types::Units::U_MM:
    case types::Units::U_MILS:
        break;

    default:
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "RunBoardJobExportIdf supports only mm and mils units" );
        return tl::unexpected( e );
    }
    }

    JOB_EXPORT_PCB_IDF job;
    job.m_filename = pcbContext()->GetCurrentFileName();
    job.SetConfiguredOutputPath( wxString::FromUTF8( aCtx.Request.job_settings().output_path() ) );

    job.m_units = FromProtoEnum<IDF_SETTINGS::UNITS>( aCtx.Request.units() );
    job.m_originMode = FromProtoEnum<IDF_SETTINGS::COORD_ORIGIN>( aCtx.Request.origin_mode() );

    if( aCtx.Request.origin_mode() == IdfOriginMode::IOM_USER )
    {
        // m_userOrigin is expressed in the units selected by m_units
        double scale = job.m_units == IDF_SETTINGS::UNITS::MM ? pcbIUScale.MM_PER_IU : pcbIUScale.MILS_PER_IU;
        job.m_userOrigin = UnpackVector2( aCtx.Request.user_origin() ) * scale;
    }

    job.m_includeUnspecified = aCtx.Request.include_unspecified();
    job.m_includeDNP = aCtx.Request.include_dnp();
    job.m_calculateHeightFromModels = !aCtx.Request.only_use_explicit_heights();

    if( aCtx.Request.has_part_number_field() )
        job.m_partNumberField = wxString::FromUTF8( aCtx.Request.part_number_field() );
    else
        job.m_partNumberField = wxS( "Value" );

    return ExecuteBoardJob( pcbContext(), job );
}


HANDLER_RESULT<CrossProbeAnnounceResponse> API_HANDLER_PCB::handleCrossProbeAnnounce(
        const HANDLER_CONTEXT<CrossProbeAnnounce>& aCtx )
{
    wxLogTrace( traceApi, "Received announce from frame %d at %s",
                aCtx.Request.frame_type(), aCtx.Request.socket_path() );

    CROSS_PROBE_CLIENT::RegisterPeer( static_cast<FRAME_T>( aCtx.Request.frame_type() ),
                                      aCtx.Request.socket_path() );

    CrossProbeAnnounceResponse response;
    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<kiapi::backplane::embed::SelectItemsResponse>
API_HANDLER_PCB::handleSelectItems( const HANDLER_CONTEXT<kiapi::backplane::embed::SelectItems>& aCtx )
{
    namespace embed = kiapi::backplane::embed;

    // Routed by frame kind: the schematic editor's handler serves FK_SCH.
    if( aCtx.Request.kind() != embed::FK_PCB )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SelectItems" ) )
        return tl::unexpected( *headless );

    embed::SelectItemsResponse response;
    response.set_kind( embed::FK_PCB );

    std::vector<EDA_ITEM*> items;
    std::vector<wxString>  nets;
    BOX2I                  bbox;

    auto grow = [&]( const BOX2I& aBox )
                {
                    if( bbox.GetWidth() == 0 && bbox.GetHeight() == 0 )
                        bbox = aBox;
                    else
                        bbox.Merge( aBox );
                };

    for( const std::string& ref : aCtx.Request.references() )
    {
        if( FOOTPRINT* fp = board()->FindFootprintByReference( wxString::FromUTF8( ref ) ) )
        {
            items.push_back( fp );
            grow( fp->GetBoundingBox() );
            response.mutable_found()->add_references( ref );
        }
        else
        {
            response.mutable_not_found()->add_references( ref );
        }
    }

    for( const std::string& pinName : aCtx.Request.pins() )
    {
        wxString text = wxString::FromUTF8( pinName );
        int      dot = text.Find( '.', true );
        bool     found = false;

        if( dot > 0 )
        {
            if( FOOTPRINT* fp = board()->FindFootprintByReference( text.Left( dot ) ) )
            {
                for( PAD* pad : fp->Pads() )
                {
                    if( pad->GetNumber() == text.Mid( dot + 1 ) )
                    {
                        items.push_back( pad );
                        grow( pad->GetBoundingBox() );
                        found = true;
                    }
                }
            }
        }

        if( found )
            response.mutable_found()->add_pins( pinName );
        else
            response.mutable_not_found()->add_pins( pinName );
    }

    for( const std::string& netName : aCtx.Request.nets() )
    {
        wxString      name = wxString::FromUTF8( netName );
        NETINFO_ITEM* net = board()->FindNet( name );

        if( net && net->GetNetCode() > 0 )
        {
            nets.push_back( name );
            response.mutable_found()->add_nets( netName );

            for( BOARD_CONNECTED_ITEM* item : board()->AllConnectedItems() )
            {
                if( item->GetNetCode() == net->GetNetCode() )
                    grow( item->GetBoundingBox() );
            }
        }
        else
        {
            response.mutable_not_found()->add_nets( netName );
        }
    }

    // bp.13: exact items by KIID (tracks, vias, zones, markers, ...)
    for( const kiapi::common::types::KIID& id : aCtx.Request.ids() )
    {
        BOARD_ITEM* item = board()->ResolveItem( KIID( id.value() ), true );

        if( item && item != DELETED_BOARD_ITEM::GetInstance() )
        {
            items.push_back( item );
            grow( item->GetBoundingBox() );
        }
        else
        {
            response.add_ids_not_found()->set_value( id.value() );
        }
    }

    {
        // The host's selection is not reported back to it; KiCad's own cross-probe to the
        // schematic still runs.
        BACKPLANE_EMBED::SELECTION_SCOPE quiet;
        TOOL_MANAGER* mgr = frame()->GetToolManager();

        mgr->RunAction( ACTIONS::selectionClear );

        if( !items.empty() )
            mgr->RunAction<std::vector<EDA_ITEM*>*>( ACTIONS::selectItems, &items );

        frame()->HandleRemoteNetHighlight( nets );
    }

    KIGFX::VIEW* view = frame()->GetCanvas()->GetView();

    if( aCtx.Request.has_zoom_box_nm() && aCtx.Request.zoom_box_nm().has_size()
        && aCtx.Request.zoom_box_nm().size().x_nm() > 0 && aCtx.Request.zoom_box_nm().size().y_nm() > 0 )
    {
        const kiapi::common::types::Box2& box = aCtx.Request.zoom_box_nm();
        view->SetViewport( BOX2D( VECTOR2D( box.position().x_nm(), box.position().y_nm() ),
                                  VECTOR2D( box.size().x_nm(), box.size().y_nm() ) ) );
    }
    else if( aCtx.Request.zoom() && ( bbox.GetWidth() > 0 || bbox.GetHeight() > 0 ) )
    {
        bbox.Inflate( std::max( bbox.GetWidth(), bbox.GetHeight() ) / 5 + pcbIUScale.mmToIU( 1 ) );
        view->SetViewport( BOX2D( bbox.GetOrigin(), bbox.GetSize() ) );
    }
    else if( aCtx.Request.has_at_nm() )
    {
        view->SetCenter( VECTOR2D( aCtx.Request.at_nm().x_nm(), aCtx.Request.at_nm().y_nm() ) );
    }

    frame()->GetCanvas()->Refresh();
    return response;
}


void API_HANDLER_PCB::FillEmbedMarker( kiapi::backplane::embed::Marker* aOut, PCB_MARKER* aMarker,
                                       BOARD* aBoard )
{
    std::shared_ptr<RC_ITEM> rcItem = aMarker->GetRCItem();

    if( !rcItem )
        return;

    aOut->set_type( rcItem->GetSettingsKey().ToStdString( wxConvUTF8 ) );

    SEVERITY severity = aMarker->GetSeverity();
    aOut->set_excluded( aMarker->IsExcluded() );
    aOut->set_severity( aMarker->IsExcluded() || severity == RPT_SEVERITY_EXCLUSION ? "exclusion"
                        : severity == RPT_SEVERITY_ERROR                          ? "error"
                                                                                  : "warning" );
    aOut->set_message( rcItem->GetErrorMessage( false ).ToStdString( wxConvUTF8 ) );
    aOut->mutable_at_nm()->set_x_nm( aMarker->GetPosition().x );
    aOut->mutable_at_nm()->set_y_nm( aMarker->GetPosition().y );

    for( const KIID& id : rcItem->GetIDs() )
    {
        if( id != niluuid )
            aOut->add_items()->set_value( id.AsStdString() );
    }

    aOut->mutable_id()->set_value( aMarker->m_Uuid.AsStdString() );

    if( DRC_ITEM* drcItem = dynamic_cast<DRC_ITEM*>( rcItem.get() ) )
    {
        if( DRC_RULE* rule = drcItem->GetViolatingRule(); rule && !rule->IsImplicit() )
            aOut->set_rule( rule->m_Name.ToStdString( wxConvUTF8 ) );
    }

    if( aMarker->GetLayer() != UNDEFINED_LAYER )
        aOut->set_layer( aBoard->GetLayerName( aMarker->GetLayer() ).ToStdString( wxConvUTF8 ) );
}


HANDLER_RESULT<kiapi::backplane::embed::SetHostMarkersResponse>
API_HANDLER_PCB::handleSetHostMarkers( const HANDLER_CONTEXT<kiapi::backplane::embed::SetHostMarkers>& aCtx )
{
    namespace embed = kiapi::backplane::embed;

    if( aCtx.Request.kind() != embed::FK_PCB )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SetHostMarkers" ) )
        return tl::unexpected( *headless );

    HOST_MARKERS_ITEM* hostItem = hostMarkers( true );

    if( !hostItem )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_INTERNAL_ERROR );
        e.set_error_message( "the PCB editor has no host-markers tool" );
        return tl::unexpected( e );
    }

    std::vector<HOST_MARKER> markers;

    for( const embed::HostMarker& in : aCtx.Request.markers() )
    {
        HOST_MARKER marker;
        marker.set = aCtx.Request.set();
        marker.id = in.id();
        marker.title = in.title();
        marker.detail = in.detail();
        marker.severity = static_cast<int>( in.severity() );
        marker.layer = in.layer();

        BOX2I itemsBox;
        bool  any = false;

        for( const kiapi::common::types::KIID& id : in.items() )
        {
            marker.items.emplace_back( id.value() );

            if( BOARD_ITEM* item = board()->ResolveItem( KIID( id.value() ), true );
                item && item != DELETED_BOARD_ITEM::GetInstance() )
            {
                if( any )
                    itemsBox.Merge( item->GetBoundingBox() );
                else
                    itemsBox = item->GetBoundingBox();

                any = true;
            }
        }

        if( in.has_at_nm() )
            marker.pos = VECTOR2I( in.at_nm().x_nm(), in.at_nm().y_nm() );
        else if( any )
            marker.pos = itemsBox.Centre();
        else
            continue;   // nowhere to draw it

        markers.push_back( std::move( marker ) );
    }

    embed::SetHostMarkersResponse response;
    response.set_count( hostItem->SetMarkers( aCtx.Request.set(), std::move( markers ) ) );

    frame()->GetToolManager()->GetTool<HOST_MARKERS_TOOL>()->Changed();
    frame()->GetCanvas()->Refresh();
    return response;
}


void API_HANDLER_PCB::onCanvasClick()
{
    PCB_EDIT_FRAME* editor = frame();

    if( !editor || !editor->GetCanvas() || !board() )
        return;

    KIGFX::VIEW* view = editor->GetCanvas()->GetView();
    VECTOR2I     at( editor->GetCanvas()->GetViewControls()->GetMousePosition( true ) );
    int          radius = KiROUND( view->ToWorld( 10.0 ) );

    if( HOST_MARKERS_ITEM* hostItem = hostMarkers( false ) )
    {
        if( const HOST_MARKER* hit = hostItem->HitTest( at, radius ) )
        {
            BACKPLANE_EMBED::Get().MarkerClicked( editor, hit->set, hit->id, std::string() );
            return;
        }
    }

    for( PCB_MARKER* marker : board()->Markers() )
    {
        if( view->IsVisible( marker ) && marker->HitTest( at, radius ) )
        {
            kiapi::backplane::embed::Marker out;
            FillEmbedMarker( &out, marker, board() );
            BACKPLANE_EMBED::Get().MarkerClicked( editor, std::string(), std::string(), out.SerializeAsString() );
            return;
        }
    }
}


HANDLER_RESULT<kiapi::backplane::embed::GetMarkersResponse>
API_HANDLER_PCB::handleGetMarkers( const HANDLER_CONTEXT<kiapi::backplane::embed::GetMarkers>& aCtx )
{
    namespace embed = kiapi::backplane::embed;

    if( aCtx.Request.kind() != embed::FK_PCB )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "GetMarkers" ) )
        return tl::unexpected( *headless );

    embed::GetMarkersResponse response;

    for( PCB_MARKER* marker : board()->Markers() )
    {
        if( marker->GetRCItem() )
            FillEmbedMarker( response.add_markers(), marker, board() );
    }

    if( HOST_MARKERS_ITEM* hostItem = aCtx.Request.include_host() ? hostMarkers( false ) : nullptr )
    {
        for( const HOST_MARKER* host : hostItem->All() )
        {
            embed::Marker* out = response.add_markers();
            out->set_type( "host" );
            out->set_severity( host->severity == 2 ? "error" : host->severity == 1 ? "warning" : "info" );
            out->set_message( host->title );
            out->mutable_at_nm()->set_x_nm( host->pos.x );
            out->mutable_at_nm()->set_y_nm( host->pos.y );

            for( const KIID& id : host->items )
                out->add_items()->set_value( id.AsStdString() );

            out->mutable_host()->set_set( host->set );
            out->mutable_host()->set_id( host->id );
            out->set_layer( host->layer );
        }
    }

    return response;
}


HANDLER_RESULT<kiapi::backplane::embed::CommentSelectionResponse>
API_HANDLER_PCB::handleCommentSelection(
        const HANDLER_CONTEXT<kiapi::backplane::embed::CommentSelection>& aCtx )
{
    namespace embed = kiapi::backplane::embed;

    // Routed by frame kind: the schematic editor's handler serves FK_SCH.
    if( aCtx.Request.kind() != embed::FK_PCB )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "CommentSelection" ) )
        return tl::unexpected( *headless );

    BOX2I         box;
    EMBED_COMMENT comment = BackplaneBuildPcbComment( frame(), box );
    comment.note = aCtx.Request.note();
    comment.fromApi = true;

    embed::CommentSelectionResponse response;
    response.set_seq( BackplaneSendPcbComment( frame(), comment, box ) );
    FillEmbedComment( response.mutable_comment(), comment );
    return response;
}


HANDLER_RESULT<SyncSelectionResponse> API_HANDLER_PCB::handleSyncSelection( const HANDLER_CONTEXT<SyncSelection>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "SyncSelection" ) )
        return tl::unexpected( *headless );

    SyncSelectionResponse response;

    const CROSS_PROBING_SETTINGS& settings = frame()->GetPcbNewSettings()->m_CrossProbing;

    if( !settings.on_selection && aCtx.Request.context() != SyncSelectionContext::SSC_EXPLICIT )
    {
        response.set_status( CPS_DISABLED );
        response.set_message( "implicit selection sync disabled by user" );
        return response;
    }

    std::vector<BOARD_ITEM*> items =
            kiapi::board::FindItemsFromSyncSelection( board(), aCtx.Request.items() );

    frame()->m_ProbingSchToPcb = true; // recursion guard

    if( aCtx.Request.mode() == SyncSelectionMode::SSM_ITEMS_AND_NETS )
        frame()->GetToolManager()->RunAction( PCB_ACTIONS::syncSelectionWithNets, &items );
    else
        frame()->GetToolManager()->RunAction( PCB_ACTIONS::syncSelection, &items );

    // Update 3D viewer highlighting
    frame()->Update3DView( false, frame()->GetPcbNewSettings()->m_Display.m_Live3DRefresh );

    frame()->m_ProbingSchToPcb = false;

    if( settings.flash_selection )
    {
        wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE) PCB: flash enabled, items=%zu", items.size() );
        if( items.empty() )
        {
            wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE) PCB: nothing to flash" );
        }
        else
        {
            std::vector<BOARD_ITEM*> boardItems;
            std::copy( items.begin(), items.end(), std::back_inserter( boardItems ) );
            frame()->StartCrossProbeFlash( boardItems );
        }
    }
    else
    {
        wxLogTrace( traceCrossProbeFlash, "MAIL_SELECTION(_FORCE) PCB: flash disabled" );
    }

    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<HighlightNetsResponse> API_HANDLER_PCB::handleHighlightNets(
        const HANDLER_CONTEXT<HighlightNets>& aCtx )
{
    if( std::optional<ApiResponseStatus> headless = checkForHeadless( "HighlightNets" ) )
        return tl::unexpected( *headless );

    HighlightNetsResponse response;
    CROSS_PROBING_SETTINGS& crossProbingSettings = frame()->GetPcbNewSettings()->m_CrossProbing;

    if( aCtx.ClientName == StandaloneCrossProbeClientName
        || aCtx.ClientName == KiwayClientName )
    {
        if( !crossProbingSettings.auto_highlight )
        {
            response.set_status( CPS_DISABLED );
            response.set_message( "net highlight cross-probing disabled by user" );
            return response;
        }
    }

    std::vector<wxString> nets;

    for( const std::string& name : aCtx.Request.net_name() )
        nets.emplace_back( wxString::FromUTF8( name ) );

    frame()->HandleRemoteNetHighlight( nets );

    response.set_status( CPS_OK );
    return response;
}


HANDLER_RESULT<VariantsResponse> API_HANDLER_PCB::handleGetVariants( const HANDLER_CONTEXT<GetVariants>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    BOARD* board = pcbContext()->GetBoard();
    VariantsResponse response;

    response.mutable_document()->CopyFrom( aCtx.Request.document() );

    for( const wxString& name : board->GetVariantNames() )
    {
        types::DesignVariant* var = response.add_variants();
        var->set_name( name.ToUTF8() );
        var->set_description( board->GetVariantDescription( name ).ToUTF8() );
    }

    return response;
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleAddVariant( const HANDLER_CONTEXT<AddVariant>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    BOARD* board = pcbContext()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->AddVariant( name );

    if( aCtx.Request.has_description() )
        board->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    onModified();

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleDeleteVariant( const HANDLER_CONTEXT<DeleteVariant>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    BOARD* board = pcbContext()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->DeleteVariant( name );

    onModified();

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleRenameVariant( const HANDLER_CONTEXT<RenameVariant>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    BOARD* board = pcbContext()->GetBoard();

    wxString oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString newName = wxString::FromUTF8( aCtx.Request.new_name() );

    if( oldName.IsEmpty() || oldName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( newName.IsEmpty() || newName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    board->RenameVariant( oldName, newName );

    onModified();

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleCopyVariant( const HANDLER_CONTEXT<CopyVariant>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    BOARD* board = pcbContext()->GetBoard();

    wxString oldName = wxString::FromUTF8( aCtx.Request.old_name() );
    wxString newName = wxString::FromUTF8( aCtx.Request.new_name() );

    if( oldName.IsEmpty() || oldName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( newName.IsEmpty() || newName.CmpNoCase( GetDefaultVariantName() ) == 0 )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "'{}' is not a valid variant name", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    if( !board->HasVariant( oldName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.old_name() ) );
        return tl::unexpected( e );
    }

    if( board->HasVariant( newName ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "a variant named '{}' already exists", aCtx.Request.new_name() ) );
        return tl::unexpected( e );
    }

    board->CopyVariant( oldName, newName,
                        aCtx.Request.has_new_description() ? wxString::FromUTF8( aCtx.Request.new_description() )
                                                           : wxString() );

    onModified();

    if( frame() )
        frame()->UpdateVariantSelectionCtrl();

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetVariantDescription( const HANDLER_CONTEXT<SetVariantDescription>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    BOARD* board = pcbContext()->GetBoard();

    wxString name = wxString::FromUTF8( aCtx.Request.name() );

    if( name.IsEmpty() || name.CmpNoCase( GetDefaultVariantName() ) == 0 || !board->HasVariant( name ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
        return tl::unexpected( e );
    }

    board->SetVariantDescription( name, wxString::FromUTF8( aCtx.Request.description() ) );

    onModified();

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_PCB::handleSetCurrentVariant( const HANDLER_CONTEXT<SetCurrentVariant>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    BOARD* board = pcbContext()->GetBoard();

    if( aCtx.Request.has_name() && !aCtx.Request.name().empty() )
    {
        if( wxString name = wxString::FromUTF8( aCtx.Request.name() ); !board->HasVariant( name ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "no variant named '{}' exists", aCtx.Request.name() ) );
            return tl::unexpected( e );
        }
    }

    wxString varName = aCtx.Request.has_name() ? wxString::FromUTF8( aCtx.Request.name() ) : wxString();

    if( frame() )
        frame()->SetCurrentVariant( varName );
    else
        board->SetCurrentVariant( varName );

    return Empty();
}


HANDLER_RESULT<CurrentVariantResponse>
API_HANDLER_PCB::handleGetCurrentVariant( const HANDLER_CONTEXT<GetCurrentVariant>& aCtx )
{
    if( HANDLER_RESULT<bool> documentValidation = validateDocument( aCtx.Request.document() ); !documentValidation )
        return tl::unexpected( documentValidation.error() );

    CurrentVariantResponse response;

    if( wxString current = pcbContext()->GetBoard()->GetCurrentVariant(); !current.IsEmpty() )
        response.set_name( current.ToUTF8() );

    return response;
}


HANDLER_RESULT<PlaceFromLibraryResponse>
API_HANDLER_PCB::handlePlaceFootprintFromLibrary( const HANDLER_CONTEXT<PlaceFootprintFromLibrary>& aCtx )
{
    if( !validateItemHeaderDocument( aCtx.Request.header() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        return tl::unexpected( e );
    }

    if( std::optional<ApiResponseStatus> busy = checkForBusy() )
        return tl::unexpected( *busy );

    LIB_ID libId = UnpackLibId( aCtx.Request.lib_id() );

    if( !libId.IsValid() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "lib_id must specify both a library nickname and an entry name" );
        return tl::unexpected( e );
    }

    PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID, board::types::BoardLayer>( aCtx.Request.layer() );

    if( layer == UNDEFINED_LAYER )
        layer = F_Cu;

    // TODO update if we support inner layer footprints in the future
    if( !IsExternalCopperLayer( layer ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "layer must be F_Cu or B_Cu" );
        return tl::unexpected( e );
    }

    std::unique_ptr<FOOTPRINT> footprint( LoadFootprintFromProject( board(), libId ) );

    if( !footprint )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "footprint '{}' not found", libId.Format().wx_str() ) );
        return tl::unexpected( e );
    }

    footprint->SetUuid( KIID() );
    footprint->RunOnChildren(
            []( BOARD_ITEM* aChild )
            {
                aChild->ResetUuid();
            },
            RECURSE_MODE::RECURSE );

    footprint->SetParent( board() );

    footprint->SetPosition( UnpackVector2( aCtx.Request.position() ) );

    if( aCtx.Request.has_orientation() )
        footprint->SetOrientationDegrees( aCtx.Request.orientation().value_degrees() );

    footprint->SetLayerAndFlip( layer );

    BOARD_COMMIT* commit = static_cast<BOARD_COMMIT*>( getCurrentCommit( aCtx.ClientName ) );
    FOOTPRINT*    placed = footprint.release();
    commit->Add( placed );

    if( !m_activeClients.contains( aCtx.ClientName ) )
        pushCurrentCommit( aCtx.ClientName, _( "Placed footprint via API" ) );

    PlaceFromLibraryResponse response;
    response.mutable_header()->CopyFrom( aCtx.Request.header() );
    placed->Serialize( *response.mutable_item() );

    return response;
}


COEDIT::HOLDS API_HANDLER_PCB::currentHolds()
{
    COEDIT::HOLDS   holds;
    PCB_EDIT_FRAME* editFrame = frame();

    if( !editFrame )
        return holds;   // headless: nobody to hold anything

    bool holdSelection = false;

    if( std::optional<COEDIT::MODAL> modal = COEDIT::OpenModal() )
    {
        if( modal->structural )
        {
            holds.exclusive = true;
            holds.reason = modal->title;
            return holds;
        }

        // An ordinary modal edits the selection; it may keep pointers to anything it shows.
        holds.reason = modal->title;
        holds.deferDeletes = true;
        holdSelection = true;
    }

    if( editFrame->InteractiveOperationInProgress() )
    {
        holds.deferDeletes = true;
        holdSelection = true;

        if( holds.reason.IsEmpty() && toolManager() && toolManager()->GetCurrentTool() )
            holds.reason = wxString::FromUTF8( toolManager()->GetCurrentTool()->GetName() );
    }

    if( editFrame->CopperOperationInProgress() )
        holds.allCopper = true;

    if( holdSelection )
    {
        for( const KIID& id : editFrame->SelectedItemIds() )
            holds.items.insert( id );
    }

    // Whatever an open person commit has staged is mid-edit.
    for( COMMIT* commit : COEDIT::LiveCommits() )
    {
        if( !commit->GetActor().empty() || commit->Empty() )
            continue;

        for( const auto& [item, type] : commit->StagedItems() )
        {
            if( item )
                holds.items.insert( item->m_Uuid );
        }
    }

    return holds;
}


bool API_HANDLER_PCB::isHeld( const COEDIT::HOLDS& aHolds, EDA_ITEM* aItem ) const
{
    if( API_HANDLER_EDITOR::isHeld( aHolds, aItem ) )
        return true;

    if( aHolds.allCopper && aItem )
    {
        switch( aItem->Type() )
        {
        case PCB_TRACE_T:
        case PCB_ARC_T:
        case PCB_VIA_T:
        case PCB_PAD_T:
        case PCB_FOOTPRINT_T:
        case PCB_ZONE_T:
            return true;

        default:
            break;
        }
    }

    return false;
}


void API_HANDLER_PCB::showAgentGhost( const google::protobuf::Any& aAgent, const wxString& aLabel,
                                      bool aShow )
{
    PCB_EDIT_FRAME* f = frame();

    if( !f )
        return;

    if( !aShow )
    {
        f->ShowAgentGhost( false );
        return;
    }

    std::optional<std::string> id = PROTO_MERGE::ItemId( aAgent );
    EDA_ITEM* live = id ? resolveLiveItem( KIID( *id ) ) : nullptr;
    BOARD_ITEM* item = dynamic_cast<BOARD_ITEM*>( live );

    if( !item )
        return;

    // The agent's version: the live item's box moved to the agent's position.
    BOX2I    mine = item->GetBoundingBox();
    BOX2I    theirs = mine;
    VECTOR2I agentPos = item->GetPosition();

    if( std::optional<VECTOR2I> pos = PROTO_MERGE::Position( aAgent ) )
        agentPos = *pos;

    theirs.Move( agentPos - item->GetPosition() );
    f->ShowAgentGhost( true, mine, theirs, aLabel );
}


bool API_HANDLER_PCB::undoChangeBySeq( uint64_t aSeq, std::string& aError )
{
    if( PCB_EDIT_FRAME* f = frame() )
        return f->UndoChangeBySeq( aSeq, aError );

    aError = "the headless server keeps no undo history";
    return false;
}
