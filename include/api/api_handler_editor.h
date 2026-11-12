/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
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

#ifndef KICAD_API_HANDLER_EDITOR_H
#define KICAD_API_HANDLER_EDITOR_H

#include <api/api_handler.h>
#include <backplane_coedit.h>
#include <api/common/commands/editor_commands.pb.h>
#include <api/backplane/coedit.pb.h>
#include <base_units.h>
#include <commit.h>
#include <google/protobuf/empty.pb.h>
#include <kiid.h>
#include <deque>
#include <page_info.h>

using namespace kiapi::common;
using kiapi::common::types::DocumentSpecifier;
using kiapi::common::types::ItemRequestStatus;
using kiapi::common::commands::ItemDeletionStatus;

class EDA_BASE_FRAME;
class TITLE_BLOCK;
class wxTimer;

/**
 * Base class for API handlers related to editor frames
 */
class API_HANDLER_EDITOR : public API_HANDLER
{
public:
    API_HANDLER_EDITOR( EDA_BASE_FRAME* aFrame = nullptr );
    ~API_HANDLER_EDITOR() override;

    /// Applies deferred agent operations whose holds have ended (docs/backplane/coedit.md).
    /// Called by a timer while any are queued; public for tests.
    void ProcessDeferred();

    size_t DeferredCount() const { return m_deferred.size(); }

    /// Co-editing collisions waiting for the person (same item, same property).
    size_t CollisionCount() const { return m_collisions.size(); }

    /// Answer the oldest collision: 0 keep the person's, 1 take the agent's, 2 show both.
    void ResolveCollision( int aChoice );

protected:
    /// If the header is valid, returns the item container
    HANDLER_RESULT<std::optional<KIID>> validateItemHeaderDocument(
            const kiapi::common::types::ItemHeader& aHeader );

    HANDLER_RESULT<bool> validateDocument( const DocumentSpecifier& aDocument );

    /**
     * Checks if the editor can accept commands
     * @return an error status if busy, std::nullopt if not busy
     */
    virtual std::optional<ApiResponseStatus> checkForBusy();

    /**
     * Co-editing (docs/backplane/coedit.md): item operations (create, update, delete, commit)
     * are refused only under an exclusive hold; held items are deferred instead.
     */
    std::optional<ApiResponseStatus> checkForExclusive( const COEDIT::HOLDS& aHolds );

    /// What the person holds now.  Default: everything while the frame refuses API commands.
    virtual COEDIT::HOLDS currentHolds();

    /// True if an agent update of aItem must wait for aHolds to end.
    virtual bool isHeld( const COEDIT::HOLDS& aHolds, EDA_ITEM* aItem ) const;

    /// True when an update of aItem replaces the object (removes it and adds a new one): such
    /// an update waits like a delete while the person is in a tool or a dialog.
    virtual bool updateReplacesObject( EDA_ITEM* aItem ) const { return false; }

    /// "pcb", "sch", "footprint" or "symbol": the change log's document kind.
    virtual std::string coeditDocKind() const { return "doc"; }

    /// The item as it was before the person's in-progress edit (the staged copy in an open
    /// person commit), or the item itself when no commit has staged it.
    const EDA_ITEM* heldBase( EDA_ITEM* aItem ) const;

    /// The live item with this id in the editor's own document, or null.
    virtual EDA_ITEM* resolveLiveItem( const KIID& aId ) { return nullptr; }

    HANDLER_RESULT<kiapi::backplane::coedit::GetChangesResponse> handleGetChanges(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::GetChanges>& aCtx );

    HANDLER_RESULT<kiapi::backplane::coedit::GetNoticesResponse> handleGetNotices(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::GetNotices>& aCtx );

    HANDLER_RESULT<kiapi::backplane::coedit::GetHoldsResponse> handleGetHolds(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::GetHolds>& aCtx );

    HANDLER_RESULT<kiapi::backplane::coedit::SetAgentActivityResponse> handleSetAgentActivity(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::SetAgentActivity>& aCtx );

    HANDLER_RESULT<kiapi::backplane::coedit::ResolveCollisionResponse> handleResolveCollision(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::ResolveCollision>& aCtx );

    HANDLER_RESULT<kiapi::backplane::coedit::UndoAgentChangeResponse> handleUndoAgentChange(
        const HANDLER_CONTEXT<kiapi::backplane::coedit::UndoAgentChange>& aCtx );

    /// Undo the change with this change-log seq (and newer changes on the same items).
    virtual bool undoChangeBySeq( uint64_t aSeq, std::string& aError )
    {
        aError = "this editor cannot undo agent changes";
        return false;
    }

    HANDLER_RESULT<commands::BeginCommitResponse> handleBeginCommit(
        const HANDLER_CONTEXT<commands::BeginCommit>& aCtx );

    HANDLER_RESULT<commands::EndCommitResponse> handleEndCommit(
        const HANDLER_CONTEXT<commands::EndCommit>& aCtx );

    COMMIT* getCurrentCommit( const std::string& aClientName );

    virtual void pushCurrentCommit( const std::string& aClientName, const wxString& aMessage );

    HANDLER_RESULT<commands::CreateItemsResponse> handleCreateItems(
        const HANDLER_CONTEXT<commands::CreateItems>& aCtx );

    HANDLER_RESULT<commands::UpdateItemsResponse> handleUpdateItems(
        const HANDLER_CONTEXT<commands::UpdateItems>& aCtx );

    HANDLER_RESULT<commands::DeleteItemsResponse> handleDeleteItems(
        const HANDLER_CONTEXT<commands::DeleteItems>& aCtx );

    HANDLER_RESULT<commands::HitTestResponse> handleHitTest(
        const HANDLER_CONTEXT<commands::HitTest>& aCtx );

    virtual HANDLER_RESULT<commands::GetDocumentModifiedStateResponse>
    handleGetDocumentModifiedState( const HANDLER_CONTEXT<commands::GetDocumentModifiedState>& aCtx );

    HANDLER_RESULT<types::TitleBlockInfo> handleGetTitleBlockInfo(
            const HANDLER_CONTEXT<commands::GetTitleBlockInfo>& aCtx );

    HANDLER_RESULT<google::protobuf::Empty> handleSetTitleBlockInfo(
            const HANDLER_CONTEXT<commands::SetTitleBlockInfo>& aCtx );

    HANDLER_RESULT<types::PageSettings> handleGetPageSettings(
            const HANDLER_CONTEXT<commands::GetPageSettings>& aCtx );

    HANDLER_RESULT<types::PageSettings> handleSetPageSettings(
            const HANDLER_CONTEXT<commands::SetPageSettings>& aCtx );

    /**
     * Override this to create an appropriate COMMIT subclass for the frame in question
     * @return a new COMMIT, bound to the editor frame
     */
    virtual std::unique_ptr<COMMIT> createCommit() = 0;

    /**
     * Override this to specify which document type this editor handles
     */
    virtual types::DocumentType thisDocumentType() const = 0;

    /**
     * @return true if the given document is valid for this editor and is currently open
     */
    virtual tl::expected<bool, ApiResponseStatus> validateDocumentInternal( const DocumentSpecifier& aDocument ) const = 0;

    /**
     * Returns the internal-unit scale that the concrete editor uses. API wire coordinates
     * are always in nanometers, so this scale drives conversion to the editor's native IU.
     * Defaults to pcbIUScale; schematic-like editors must override.
     */
    virtual const EDA_IU_SCALE& getIuScale() const { return pcbIUScale; }

    virtual HANDLER_RESULT<ItemRequestStatus> handleCreateUpdateItemsInternal( bool aCreate,
        const std::string& aClientName,
        const types::ItemHeader &aHeader,
        const google::protobuf::RepeatedPtrField<google::protobuf::Any>& aItems,
        std::function<void( commands::ItemStatus, google::protobuf::Any )> aItemHandler ) = 0;

    virtual void deleteItemsInternal( std::map<KIID, ItemDeletionStatus>& aItemsToDelete,
                                      const std::string& aClientName ) = 0;

    virtual std::optional<EDA_ITEM*> getItemFromDocument( const DocumentSpecifier& aDocument,
                                                          const KIID& aId ) = 0;

    static std::vector<KICAD_T> parseRequestedItemTypes( const google::protobuf::RepeatedField<int>& aTypes );

    virtual std::optional<TITLE_BLOCK*> getTitleBlock( const DocumentSpecifier& aDocument ) { return std::nullopt; }

    virtual std::optional<PAGE_INFO> getPageSettings( const DocumentSpecifier& aDocument ) { return std::nullopt; }

    virtual bool setPageSettings( const DocumentSpecifier& aDocument, const PAGE_INFO& aPageInfo ) { return false; }

    virtual wxString getDrawingSheetFileName() { return wxEmptyString; }

    virtual void setDrawingSheetFileName( const wxString& aFileName ) {}

    virtual void onModified() {}

    /// After a pushed commit that only added, changed or removed items: the commit already
    /// updated the view, connectivity and the frame's modified state.
    virtual void onItemsCommitted() { onModified(); }

protected:
    struct DEFERRED_OP
    {
        std::string           client;
        bool                  remove = false;
        types::ItemHeader     header;
        KIID                  id;
        google::protobuf::Any agent;   ///< the agent's item (update)
        google::protobuf::Any base;    ///< the item before the person's edit (update)
    };

    void deferOp( DEFERRED_OP aOp );

    /// A real collision: the agent changed fields the person changed too (since the agent last
    /// read the item, or while the person held it).  The rest of the agent's change is already
    /// applied; these fields wait for the person.
    struct COLLISION
    {
        std::string              client;
        types::ItemHeader        header;
        KIID                     id;
        wxString                 what;        ///< "U3"
        std::vector<std::string> fields;      ///< "position", "orientation"...
        google::protobuf::Any    base;
        google::protobuf::Any    agent;
        int64_t                  personAt = 0; ///< when the person changed it (ms)
    };

    void raiseCollision( COLLISION aCollision );
    void showCollision();
    wxString collisionText( const COLLISION& aCollision ) const;

    /// The item as the person's first change after aSince found it (that change's undo copy):
    /// the base an agent's update made from an older read merges from.  Null if none.
    const EDA_ITEM* personBaseSince( EDA_ITEM* aLive, uint64_t aSince ) const;

    /// A short name for an item in a collision ("U3", "track on F.Cu").
    virtual wxString describeItem( EDA_ITEM* aItem ) const;

    /// Show (or hide, aShow false) the agent's version of an item over the person's.
    virtual void showAgentGhost( const google::protobuf::Any& aAgent, const wxString& aLabel,
                                 bool aShow ) {}

    /// Rebase one agent update made from a stale read; may raise a collision.  Returns the item
    /// to apply.
    google::protobuf::Any rebaseStale( const std::string& aClient, const types::ItemHeader& aHeader,
                                       const google::protobuf::Any& aItem );

    std::deque<COLLISION> m_collisions;

    std::vector<DEFERRED_OP>  m_deferred;
    std::unique_ptr<wxTimer>  m_deferTimer;

    std::map<std::string, std::pair<KIID, std::unique_ptr<COMMIT>>> m_commits;

    std::set<std::string> m_activeClients;

    EDA_BASE_FRAME* m_frame;
};

#endif //KICAD_API_HANDLER_EDITOR_H
