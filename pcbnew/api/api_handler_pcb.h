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

#ifndef KICAD_API_HANDLER_PCB_H
#define KICAD_API_HANDLER_PCB_H

#include <api/backplane/embed.pb.h>
#include <api/api_handler_board.h>
#include <api/pcb_context.h>
#include <api/backplane/structure.pb.h>
#include <api/backplane/assembly.pb.h>
#include <api/backplane/sketch_route.pb.h>
#include <api/backplane/plan.pb.h>
#include <api/board/board_jobs.pb.h>
#include <api/board/board_tasks.pb.h>
#include <api/common/tasks.pb.h>
#include <api/common/commands/cross_probe_commands.pb.h>
#include <api/common/commands/project_commands.pb.h>
#include <api/common/commands/variant_commands.pb.h>
#include <api/common/commands/library_commands.pb.h>
#include <properties/property_mgr.h>

using namespace kiapi::board::jobs;


class PCB_EDIT_FRAME;
class PCB_TRACK;
class PROPERTY_BASE;
class ZONE;
class JOB_EXPORT_PCB_FAB;
class JOB_EXPORT_PCB_ODB;


void ApplyFabExportContent( const kiapi::board::jobs::FabExportContent& aContent, JOB_EXPORT_PCB_FAB& aJob );

void ApplyOdbJobRequest( const kiapi::board::jobs::RunBoardJobExportODB& aRequest,
                         JOB_EXPORT_PCB_ODB& aJob );


class BOARD_PLAN;
class DRC_RULE;
class HOST_MARKERS_ITEM;
class PCB_MARKER;

class API_HANDLER_PCB : public API_HANDLER_BOARD
{
public:
    int DispatchRank() const override { return RANK_PCB_EDITOR; }

    API_HANDLER_PCB( PCB_EDIT_FRAME* aFrame );
    API_HANDLER_PCB( std::shared_ptr<PCB_CONTEXT> aContext, PCB_EDIT_FRAME* aFrame = nullptr );
    ~API_HANDLER_PCB() override;

    /// kiapi.backplane.embed.Marker for one of the board's DRC markers.
    static void FillEmbedMarker( kiapi::backplane::embed::Marker* aOut, PCB_MARKER* aMarker,
                                 BOARD* aBoard );

private:
    HANDLER_RESULT<kiapi::backplane::BoardStructureResponse> handleGetBoardStructure(
            const HANDLER_CONTEXT<kiapi::backplane::GetBoardStructure>& aCtx );
    HANDLER_RESULT<kiapi::backplane::BoardStructureResponse> handleSetBoardStructure(
            const HANDLER_CONTEXT<kiapi::backplane::SetBoardStructure>& aCtx );
    HANDLER_RESULT<kiapi::backplane::BoardStructureResponse> handleValidateBoardStructure(
            const HANDLER_CONTEXT<kiapi::backplane::ValidateBoardStructure>& aCtx );
    HANDLER_RESULT<kiapi::backplane::ViewerConstructionStateResponse> handleGetViewerConstructionState(
            const HANDLER_CONTEXT<kiapi::backplane::GetViewerConstructionState>& aCtx );
    HANDLER_RESULT<kiapi::backplane::ViewerConstructionStateResponse> handleSetViewerConstructionState(
            const HANDLER_CONTEXT<kiapi::backplane::SetViewerConstructionState>& aCtx );
    HANDLER_RESULT<kiapi::backplane::ConstructionLayerStateResponse> handleGetConstructionLayerState(
            const HANDLER_CONTEXT<kiapi::backplane::GetConstructionLayerState>& aCtx );
    HANDLER_RESULT<kiapi::backplane::ConstructionLayerStateResponse> handleSetConstructionLayerState(
            const HANDLER_CONTEXT<kiapi::backplane::SetConstructionLayerState>& aCtx );
    HANDLER_RESULT<kiapi::backplane::BoardStructureResponse> handleDeriveBoardStructurePartition(
            const HANDLER_CONTEXT<kiapi::backplane::DeriveBoardStructurePartition>& aCtx );
    HANDLER_RESULT<types::RunJobResponse> handleRunBoardStructureExport3D(
            const HANDLER_CONTEXT<kiapi::backplane::RunBoardStructureExport3D>& aCtx );
    HANDLER_RESULT<types::RunJobResponse> runBoardJobExport3D(
            const RunBoardJobExport3D& aRequest, bool aFolded );

    typedef std::map<std::string, PROPERTY_BASE*> PROTO_PROPERTY_MAP;

    HANDLER_RESULT<commands::GetOpenDocumentsResponse> handleGetOpenDocuments(
            const HANDLER_CONTEXT<commands::GetOpenDocuments>& aCtx );

    HANDLER_RESULT<Empty> handleSaveDocument( const HANDLER_CONTEXT<commands::SaveDocument>& aCtx );
    HANDLER_RESULT<Empty> handleSaveDocumentAs( const HANDLER_CONTEXT<commands::SaveDocumentAs>& aCtx );

    HANDLER_RESULT<Empty> handleSaveCopyOfDocument(
            const HANDLER_CONTEXT<commands::SaveCopyOfDocument>& aCtx );

    HANDLER_RESULT<Empty> handleRevertDocument(
            const HANDLER_CONTEXT<commands::RevertDocument>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItems(
            const HANDLER_CONTEXT<commands::GetItems>& aCtx );

    HANDLER_RESULT<BoardEnabledLayersResponse> handleSetBoardEnabledLayers(
            const HANDLER_CONTEXT<SetBoardEnabledLayers>& aCtx );

    HANDLER_RESULT<BoardDesignRulesResponse> handleGetBoardDesignRules(
            const HANDLER_CONTEXT<GetBoardDesignRules>& aCtx );

    HANDLER_RESULT<BoardDesignRulesResponse> handleSetBoardDesignRules(
            const HANDLER_CONTEXT<SetBoardDesignRules>& aCtx );

    HANDLER_RESULT<CustomRulesResponse> handleGetCustomDesignRules(
            const HANDLER_CONTEXT<GetCustomDesignRules>& aCtx );

    HANDLER_RESULT<CustomRulesResponse> handleSetCustomDesignRules(
            const HANDLER_CONTEXT<SetCustomDesignRules>& aCtx );

    HANDLER_RESULT<common::types::EmbeddedFiles> handleGetEmbeddedFiles(
            const HANDLER_CONTEXT<GetEmbeddedFiles>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleAddEmbeddedFiles(
            const HANDLER_CONTEXT<AddEmbeddedFiles>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleSetEmbeddedFiles(
            const HANDLER_CONTEXT<SetEmbeddedFiles>& aCtx );

    HANDLER_RESULT<types::Vector2> handleGetBoardOrigin(
            const HANDLER_CONTEXT<GetBoardOrigin>& aCtx );

    HANDLER_RESULT<Empty> handleSetBoardOrigin( const HANDLER_CONTEXT<SetBoardOrigin>& aCtx );

    HANDLER_RESULT<BoardLayerNameResponse> handleGetBoardLayerName(
            const HANDLER_CONTEXT<GetBoardLayerName>& aCtx );

    HANDLER_RESULT<BoardLayerResponse> handleGetBoardLayerByName(
            const HANDLER_CONTEXT<GetBoardLayerByName>& aCtx );

    HANDLER_RESULT<NetsResponse> handleGetNets( const HANDLER_CONTEXT<GetNets>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetConnectedItems(
            const HANDLER_CONTEXT<GetConnectedItems>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItemsByNet(
            const HANDLER_CONTEXT<GetItemsByNet>& aCtx );

    HANDLER_RESULT<commands::GetItemsResponse> handleGetItemsByNetClass(
            const HANDLER_CONTEXT<GetItemsByNetClass>& aCtx );

    HANDLER_RESULT<NetClassForNetsResponse> handleGetNetClassForNets(
            const HANDLER_CONTEXT<GetNetClassForNets>& aCtx );

    HANDLER_RESULT<Empty> handleRefillZones( const HANDLER_CONTEXT<RefillZones>& aCtx );

    HANDLER_RESULT<kiapi::backplane::SketchRouteResponse> handleSketchRoute(
            const HANDLER_CONTEXT<kiapi::backplane::SketchRoute>& aCtx );

    // Routing plan (docs/backplane/floorplanner.md), api_pcb_plan.cpp
    HANDLER_RESULT<kiapi::backplane::plan::PlanResponse> handleGetPlan(
            const HANDLER_CONTEXT<kiapi::backplane::plan::GetPlan>& aCtx );
    HANDLER_RESULT<kiapi::backplane::plan::ProposePlanResponse> handleProposePlan(
            const HANDLER_CONTEXT<kiapi::backplane::plan::ProposePlan>& aCtx );
    HANDLER_RESULT<kiapi::backplane::plan::UpdatePlanResponse> handleUpdatePlan(
            const HANDLER_CONTEXT<kiapi::backplane::plan::UpdatePlan>& aCtx );
    HANDLER_RESULT<kiapi::backplane::plan::PlanResponse> handleApprovePlan(
            const HANDLER_CONTEXT<kiapi::backplane::plan::ApprovePlan>& aCtx );
    HANDLER_RESULT<kiapi::backplane::plan::RoutePlanResponse> handleRoutePlan(
            const HANDLER_CONTEXT<kiapi::backplane::plan::RoutePlan>& aCtx );
    HANDLER_RESULT<kiapi::backplane::plan::PlanResponse> handleStopPlanRouting(
            const HANDLER_CONTEXT<kiapi::backplane::plan::StopPlanRouting>& aCtx );
    HANDLER_RESULT<kiapi::backplane::plan::VerifyPlanResponse> handleVerifyPlan(
            const HANDLER_CONTEXT<kiapi::backplane::plan::VerifyPlan>& aCtx );

    // Custom rules, merged per rule (docs/backplane/coedit.md)
    HANDLER_RESULT<CustomRulesResponse> handleEditCustomRules(
            const HANDLER_CONTEXT<kiapi::backplane::coedit::EditCustomRules>& aCtx );
    std::string readRulesFile();
    std::map<std::string, std::string> canonicalRules( const std::string& aText );
    CustomRulesResponse rulesToProto( const wxString& aContent,
                                                const std::vector<std::shared_ptr<DRC_RULE>>& aRules );
    HANDLER_RESULT<CustomRulesResponse> writeMergedRules( const std::string& aClient,
            const std::string& aBase, const std::string& aOurs, const std::string& aDisk,
            const std::string& aWhat );
    std::map<std::string, std::string> m_rulesBase;   ///< per client: the rules it last read

    bool planBusy( std::string& aReason );
    void commitPlan( const BOARD_PLAN& aPlan, const std::string& aClient,
                     const wxString& aDescription, const std::string& aOp,
                     const std::vector<std::string>& aItems );

    std::string                                     m_planRouting;
    std::map<std::string, std::set<std::string>>    m_planTried;

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleListAssemblyComponents(
            const HANDLER_CONTEXT<kiapi::backplane::ListAssemblyComponents>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleMoveAssemblyComponent(
            const HANDLER_CONTEXT<kiapi::backplane::MoveAssemblyComponent>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleSyncAssemblyElectrical(
            const HANDLER_CONTEXT<kiapi::backplane::SyncAssemblyElectrical>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleOpenAssemblyTab(
            const HANDLER_CONTEXT<kiapi::backplane::OpenAssemblyTab>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleListAssemblyTabs(
            const HANDLER_CONTEXT<kiapi::backplane::ListAssemblyTabs>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleShowAssembly3D(
            const HANDLER_CONTEXT<kiapi::backplane::ShowAssembly3D>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleGetAssembly(
            const HANDLER_CONTEXT<kiapi::backplane::GetAssembly>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleSaveAssembly(
            const HANDLER_CONTEXT<kiapi::backplane::SaveAssembly>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleResolveAssemblyMates(
            const HANDLER_CONTEXT<kiapi::backplane::ResolveAssemblyMates>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleCheckAssemblyCollisions(
            const HANDLER_CONTEXT<kiapi::backplane::CheckAssemblyCollisions>& aCtx );

    HANDLER_RESULT<kiapi::backplane::AssemblyResponse> handleExportAssemblyStep(
            const HANDLER_CONTEXT<kiapi::backplane::ExportAssemblyStep>& aCtx );

    HANDLER_RESULT<ImportNetlistResponse> handleImportNetlist(
            const HANDLER_CONTEXT<ImportNetlist>& aCtx );

    HANDLER_RESULT<ImportSpecctraSessionResponse> handleImportSpecctraSession(
            const HANDLER_CONTEXT<ImportSpecctraSession>& aCtx );
    HANDLER_RESULT<kiapi::common::tasks::StartTaskResponse> handleStartRefillZonesTask(
            const HANDLER_CONTEXT<kiapi::board::tasks::StartRefillZonesTask>& aCtx );

    HANDLER_RESULT<kiapi::common::tasks::StartTaskResponse> handleStartDrcTask(
            const HANDLER_CONTEXT<kiapi::board::tasks::StartDrcTask>& aCtx );

    HANDLER_RESULT<kiapi::board::tasks::DrcTaskResult> handleGetDrcTaskResult(
            const HANDLER_CONTEXT<kiapi::board::tasks::GetDrcTaskResult>& aCtx );

    class REFILL_ZONES_TASK;
    class DRC_TASK;

    HANDLER_RESULT<BoardEditorAppearanceSettings> handleGetBoardEditorAppearanceSettings(
            const HANDLER_CONTEXT<GetBoardEditorAppearanceSettings>& aCtx );

    HANDLER_RESULT<Empty> handleSetBoardEditorAppearanceSettings(
            const HANDLER_CONTEXT<SetBoardEditorAppearanceSettings>& aCtx );

    HANDLER_RESULT<BoardPlotSettingsResponse>
    handleGetBoardPlotSettings( const HANDLER_CONTEXT<GetBoardPlotSettings>& aCtx );

    HANDLER_RESULT<Empty> handleSetBoardPlotSettings( const HANDLER_CONTEXT<SetBoardPlotSettings>& aCtx );

    HANDLER_RESULT<InjectDrcErrorResponse> handleInjectDrcError(
            const HANDLER_CONTEXT<InjectDrcError>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExport3D(
            const HANDLER_CONTEXT<RunBoardJobExport3D>& aCtx );

    HANDLER_RESULT<commands::VariantsResponse> handleGetVariants( const HANDLER_CONTEXT<commands::GetVariants>& aCtx );
    HANDLER_RESULT<Empty> handleAddVariant( const HANDLER_CONTEXT<commands::AddVariant>& aCtx );
    HANDLER_RESULT<Empty> handleDeleteVariant( const HANDLER_CONTEXT<commands::DeleteVariant>& aCtx );
    HANDLER_RESULT<Empty> handleRenameVariant( const HANDLER_CONTEXT<commands::RenameVariant>& aCtx );
    HANDLER_RESULT<Empty> handleCopyVariant( const HANDLER_CONTEXT<commands::CopyVariant>& aCtx );
    HANDLER_RESULT<Empty> handleSetVariantDescription( const HANDLER_CONTEXT<commands::SetVariantDescription>& aCtx );
    HANDLER_RESULT<Empty> handleSetCurrentVariant( const HANDLER_CONTEXT<commands::SetCurrentVariant>& aCtx );
    HANDLER_RESULT<commands::CurrentVariantResponse>
    handleGetCurrentVariant( const HANDLER_CONTEXT<commands::GetCurrentVariant>& aCtx );

    HANDLER_RESULT<kiapi::common::commands::PlaceFromLibraryResponse> handlePlaceFootprintFromLibrary(
            const HANDLER_CONTEXT<kiapi::board::commands::PlaceFootprintFromLibrary>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportRender(
            const HANDLER_CONTEXT<RunBoardJobExportRender>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportSvg(
            const HANDLER_CONTEXT<RunBoardJobExportSvg>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportDxf(
            const HANDLER_CONTEXT<RunBoardJobExportDxf>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportPdf(
            const HANDLER_CONTEXT<RunBoardJobExportPdf>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportPs(
            const HANDLER_CONTEXT<RunBoardJobExportPs>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportPng(
            const HANDLER_CONTEXT<RunBoardJobExportPng>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportGerbers(
            const HANDLER_CONTEXT<RunBoardJobExportGerbers>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportDrill(
            const HANDLER_CONTEXT<RunBoardJobExportDrill>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportPosition(
            const HANDLER_CONTEXT<RunBoardJobExportPosition>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportGencad(
            const HANDLER_CONTEXT<RunBoardJobExportGencad>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportSpecctraDsn(
            const HANDLER_CONTEXT<RunBoardJobExportSpecctraDsn>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportIpc2581(
            const HANDLER_CONTEXT<RunBoardJobExportIpc2581>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportIpcD356(
            const HANDLER_CONTEXT<RunBoardJobExportIpcD356>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportODB(
            const HANDLER_CONTEXT<RunBoardJobExportODB>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportIdf(
            const HANDLER_CONTEXT<RunBoardJobExportIdf>& aCtx );

    HANDLER_RESULT<types::RunJobResponse> handleRunBoardJobExportStats(
            const HANDLER_CONTEXT<RunBoardJobExportStats>& aCtx );

    HANDLER_RESULT<commands::CrossProbeAnnounceResponse> handleCrossProbeAnnounce(
            const HANDLER_CONTEXT<commands::CrossProbeAnnounce>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::SelectItemsResponse> handleSelectItems(
            const HANDLER_CONTEXT<kiapi::backplane::embed::SelectItems>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::RunChecksResponse> handleRunChecks(
            const HANDLER_CONTEXT<kiapi::backplane::embed::RunChecks>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::GetMarkersResponse> handleGetMarkers(
            const HANDLER_CONTEXT<kiapi::backplane::embed::GetMarkers>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::SetHostMarkersResponse> handleSetHostMarkers(
            const HANDLER_CONTEXT<kiapi::backplane::embed::SetHostMarkers>& aCtx );

    /// A click in the canvas (embedded): on a host or DRC marker, ET_MARKER_CLICKED.
    void onCanvasClick();

    /// The host's markers (HOST_MARKERS_TOOL keeps them in the view), or null
    HOST_MARKERS_ITEM* hostMarkers( bool aCreate ) const;

    std::shared_ptr<bool> m_alive = std::make_shared<bool>( true );

    HANDLER_RESULT<kiapi::backplane::embed::CommentSelectionResponse> handleCommentSelection(
            const HANDLER_CONTEXT<kiapi::backplane::embed::CommentSelection>& aCtx );

    HANDLER_RESULT<commands::SyncSelectionResponse> handleSyncSelection(
            const HANDLER_CONTEXT<commands::SyncSelection>& aCtx );

    HANDLER_RESULT<commands::HighlightNetsResponse> handleHighlightNets(
            const HANDLER_CONTEXT<commands::HighlightNets>& aCtx );

protected:
    kiapi::common::types::DocumentType thisDocumentType() const override
    {
        return kiapi::common::types::DOCTYPE_PCB;
    }

    tl::expected<bool, ApiResponseStatus> validateDocumentInternal( const DocumentSpecifier& aDocument ) const override;

    std::optional<TITLE_BLOCK*> getTitleBlock( const DocumentSpecifier& aDocument ) override;

    std::optional<PAGE_INFO> getPageSettings( const DocumentSpecifier& aDocument ) override;

    bool setPageSettings( const DocumentSpecifier& aDocument, const PAGE_INFO& aPageInfo ) override;

    wxString getDrawingSheetFileName() override;

    void setDrawingSheetFileName( const wxString& aFileName ) override;

    void onModified() override;

    COEDIT::HOLDS currentHolds() override;

    bool isHeld( const COEDIT::HOLDS& aHolds, EDA_ITEM* aItem ) const override;

    std::string coeditDocKind() const override { return "pcb"; }

    void showAgentGhost( const google::protobuf::Any& aAgent, const wxString& aLabel,
                         bool aShow ) override;

    bool undoChangeBySeq( uint64_t aSeq, std::string& aError ) override;

    void onItemsCommitted() override;

    void onNetSettingsChanged() override;

    HANDLER_RESULT<commands::GetDocumentModifiedStateResponse>
    handleGetDocumentModifiedState( const HANDLER_CONTEXT<commands::GetDocumentModifiedState>& aCtx ) override;

private:
    PCB_CONTEXT* pcbContext() const { return static_cast<PCB_CONTEXT*>( context() ); }

    PCB_EDIT_FRAME* frame() const;

    HANDLER_RESULT<std::vector<ZONE*>>
    resolveZonesToFill( const google::protobuf::RepeatedPtrField<kiapi::common::types::KIID>& aZoneIds ) const;
};

#endif //KICAD_API_HANDLER_PCB_H
