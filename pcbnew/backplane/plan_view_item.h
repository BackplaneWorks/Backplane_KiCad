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
 * The routing plan overlay (docs/backplane/floorplanner.md): corridors, plane regions, splits,
 * notes and audit marks of the active copper layer, drawn on the GAL above the board; and the
 * plan view, which shows the board the way the plan reads (dark board, outlined parts with
 * their references, the active layer's plan at full strength).
 */

#pragma once

#include <backplane/board_plan.h>
#include <eda_item.h>
#include <geometry/shape_poly_set.h>
#include <gal/color4d.h>
#include <layer_ids.h>

#include <set>
#include <string>

class BOARD;
class BOARD_PLAN;
struct PLAN_CORRIDOR;
struct PLAN_PLANE;
namespace KIGFX { class GAL; class VIEW; }


class PLAN_VIEW_ITEM : public EDA_ITEM
{
public:
    explicit PLAN_VIEW_ITEM( BOARD* aBoard );

    void SetBoard( BOARD* aBoard ) { m_board = aBoard; }

    void         SetActiveLayer( PCB_LAYER_ID aLayer ) { m_activeLayer = aLayer; }
    PCB_LAYER_ID GetActiveLayer() const { return m_activeLayer; }

    /// The plan view: the board drawn the way the plan reads.
    void SetPlanView( bool aOn ) { m_planView = aOn; }
    bool IsPlanView() const { return m_planView; }

    /// Show the plan overlay at all (the "Routing plan" appearance row).
    void SetShown( bool aShown ) { m_shown = aShown; }
    bool IsShown() const { return m_shown; }

    void SetSelected( const std::set<std::string>& aIds ) { m_selected = aIds; }
    const std::set<std::string>& GetSelected() const { return m_selected; }

    void SetHovered( const std::string& aId ) { m_hovered = aId; }

    /// A corridor being drawn (rubber band), board coordinates; empty = none.
    void SetDraft( const std::vector<VECTOR2I>& aPoints, int aWidth, const std::string& aLayer )
    {
        m_draft = aPoints;
        m_draftWidth = aWidth;
        m_draftLayer = aLayer;
    }

    const BOX2I      ViewBBox() const override;
    void             ViewDraw( int aLayer, KIGFX::VIEW* aView ) const override;
    std::vector<int> ViewGetLayers() const override;

    wxString GetClass() const override { return wxT( "PLAN_VIEW_ITEM" ); }
    bool HitTest( const VECTOR2I&, int = 0 ) const override { return false; }

#if defined( DEBUG )
    void Show( int, std::ostream& ) const override {}
#endif

    /// The group colour of a corridor (its group's, or a stable colour from its id).
    static KIGFX::COLOR4D CorridorColor( const BOARD_PLAN& aPlan, const PLAN_CORRIDOR& aCorridor );
    static KIGFX::COLOR4D PlaneColor( const PLAN_PLANE& aPlane );
    wxString        ProblemText( const PLAN_PROGRESS::ITEM& aItem ) const;
    VECTOR2D        labelSpot( const SHAPE_POLY_SET& aPoly, double aW, double aH ) const;
    static wxString Ordinal( int aN );
    static KIGFX::COLOR4D ParseColor( const std::string& aHex, const KIGFX::COLOR4D& aDefault );

private:
    void drawPlanBackdrop( KIGFX::GAL* aGal, KIGFX::VIEW* aView ) const;
    void drawOverview( KIGFX::GAL* aGal, KIGFX::VIEW* aView, const std::string& aActive ) const;
    void drawPlanes( KIGFX::GAL* aGal, KIGFX::VIEW* aView, const std::string& aLayer ) const;
    void drawCorridors( KIGFX::GAL* aGal, KIGFX::VIEW* aView, const std::string& aLayer,
                        bool aFaint ) const;
    void drawNotesAndAudit( KIGFX::GAL* aGal, KIGFX::VIEW* aView, const std::string& aLayer ) const;
    void drawHeader( KIGFX::GAL* aGal, KIGFX::VIEW* aView, const std::string& aLayer ) const;

    BOARD*                m_board;
    PCB_LAYER_ID          m_activeLayer = F_Cu;
    bool                  m_planView = false;
    bool                  m_shown = true;
    std::set<std::string> m_selected;
    std::string           m_hovered;
    std::vector<VECTOR2I> m_draft;
    int                   m_draftWidth = 0;
    std::string           m_draftLayer;

    // Derived for the current draw (audit, progress).
    mutable PLAN_AUDIT    m_audit;
    mutable PLAN_PROGRESS m_progress;
};
