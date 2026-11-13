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

/*
 * Steering the routing plan (docs/backplane/floorplanner.md): select, move, reshape, re-layer,
 * reorder, lock, accept/reject, merge/split, note, draw corridors and plane regions, approve a
 * layer.  Every edit is one undoable plan change by the person, and becomes a constraint the
 * agent sees.
 */

#pragma once

#include <tools/pcb_tool_base.h>

#include <functional>
#include <map>
#include <set>
#include <string>

class BOARD_PLAN;
class PCB_EDIT_FRAME;
class PLAN_VIEW_ITEM;


class PLAN_TOOL : public PCB_TOOL_BASE
{
public:
    PLAN_TOOL();

    bool Init() override;
    void Reset( RESET_REASON aReason ) override;

    /// Plan view on/off (Alt+P); entering it starts the plan editing loop.
    int TogglePlanView( const TOOL_EVENT& aEvent );
    int ToggleShowPlan( const TOOL_EVENT& aEvent );

    /// The plan editing loop (selection, drags, context menu).
    int EditPlan( const TOOL_EVENT& aEvent );

    int Accept( const TOOL_EVENT& aEvent );
    int Reject( const TOOL_EVENT& aEvent );
    int ToggleLock( const TOOL_EVENT& aEvent );
    int RouteEarlier( const TOOL_EVENT& aEvent );
    int RouteLater( const TOOL_EVENT& aEvent );
    int Wider( const TOOL_EVENT& aEvent );
    int Narrower( const TOOL_EVENT& aEvent );
    int MoveToLayer( const TOOL_EVENT& aEvent );
    int MergeGroups( const TOOL_EVENT& aEvent );
    int SplitGroup( const TOOL_EVENT& aEvent );
    int AddNote( const TOOL_EVENT& aEvent );
    int DeleteItems( const TOOL_EVENT& aEvent );
    int DrawCorridor( const TOOL_EVENT& aEvent );
    int DrawPlane( const TOOL_EVENT& aEvent );
    int ApproveLayer( const TOOL_EVENT& aEvent );
    int RejectLayer( const TOOL_EVENT& aEvent );
    int RequestReplan( const TOOL_EVENT& aEvent );

    /// Select (and optionally zoom to) a plan item from the panel.
    void SelectItem( const std::string& aId, bool aZoom );

    const std::set<std::string>& Selected() const { return m_selected; }

    /// The person is dragging plan items (previewed on the board's plan): plan writes from
    /// the API wait until the drag ends.
    bool IsDragging() const { return m_dragging; }

    /// The id of the plan item at aPoint on the active layer, or on aLayer ("" if none); vertex
    /// hits set aVertex to the path/outline index.
    std::string HitTest( const VECTOR2I& aPoint, int* aVertex = nullptr,
                         const std::string* aLayer = nullptr ) const;

    /// A corridor or plane at aPoint on another plan layer than the active one: its id, and its
    /// layer in aLayer ("" if none).
    std::string HitOtherLayer( const VECTOR2I& aPoint, std::string& aLayer ) const;

private:
    /// Plan view hides the board's own layers (the plan is the foreground; the overlay draws the
    /// outline and the parts as context), restoring what was visible on the way out.
    void hideBoardLayers( bool aHide );
    void fitBesideOverview();
    /// Whether anything is selected; if not, the infobar says what to do.
    bool needSelection() const;
    std::map<int, bool> m_savedVisibility;

    void setTransitions() override;

    PCB_EDIT_FRAME* editFrame() const;
    PLAN_VIEW_ITEM* planItem() const;
    std::string     activeLayerName() const;

    /// Apply aEdit to a copy of the plan and commit it as the person's change.
    void edit( const wxString& aDescription, const std::string& aOp,
               const std::vector<std::string>& aItems,
               const std::function<void( BOARD_PLAN& )>& aEdit );

    /// Plain-words constraint for the agent, from a person's edit.
    static void constrain( BOARD_PLAN& aPlan, const std::string& aKind, const std::string& aTarget,
                           const std::string& aText );

    void syncSelection();
    void dragItems( const VECTOR2I& aStart, const std::string& aHit, int aVertex );
    std::vector<VECTOR2I> collectPoints( bool aClosed );

    std::set<std::string> m_selected;
    bool                  m_dragging = false;
};
