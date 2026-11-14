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

#include "plan_tool.h"
#include "board_plan.h"
#include "plan_view_item.h"

#include <board.h>
#include <fmt/format.h>
#include <geometry/seg.h>
#include <geometry/shape_line_chain.h>
#include <netinfo.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <tool/tool_event.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_selection_tool.h>
#include <view/view.h>
#include <view/view_controls.h>
#include <wx/choicdlg.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/regex.h>
#include <wx/textdlg.h>

#include <algorithm>


namespace
{
std::string mm( int aNm )
{
    return fmt::format( "{:.2f} mm", aNm / 1e6 );
}

double distToPath( const VECTOR2I& aP, const std::vector<VECTOR2I>& aPath, size_t* aSeg = nullptr )
{
    double best = std::numeric_limits<double>::max();

    for( size_t ii = 1; ii < aPath.size(); ++ii )
    {
        double d = SEG( aPath[ii - 1], aPath[ii] ).Distance( aP );

        if( d < best )
        {
            best = d;

            if( aSeg )
                *aSeg = ii;
        }
    }

    return best;
}

bool inside( const VECTOR2I& aP, const std::vector<VECTOR2I>& aRing )
{
    SHAPE_LINE_CHAIN chain;

    for( const VECTOR2I& p : aRing )
        chain.Append( p );

    chain.SetClosed( true );
    return chain.PointInside( aP );
}

const std::vector<std::string> ITEM_KINDS = { "c-", "p-", "n-", "s-" };
} // namespace


PLAN_TOOL::PLAN_TOOL() :
        PCB_TOOL_BASE( "pcbnew.Plan" )   // = the actions' prefix, so their hotkeys are this tool's context actions
{
}


bool PLAN_TOOL::Init()
{
    PCB_TOOL_BASE::Init();

    auto hasSelection = [this]( const SELECTION& ) { return !m_selected.empty(); };
    auto oneCorridor = [this]( const SELECTION& )
    {
        return m_selected.size() == 1 && m_selected.begin()->rfind( "c-", 0 ) == 0;
    };
    auto manyCorridors = [this]( const SELECTION& )
    {
        return std::count_if( m_selected.begin(), m_selected.end(),
                              []( const std::string& id ) { return id.rfind( "c-", 0 ) == 0; } )
               >= 2;
    };

    CONDITIONAL_MENU& menu = m_menu->GetMenu();
    menu.AddItem( PCB_ACTIONS::planAccept, hasSelection, 10 );
    menu.AddItem( PCB_ACTIONS::planReject, hasSelection, 10 );
    menu.AddItem( PCB_ACTIONS::planToggleLock, hasSelection, 10 );
    menu.AddSeparator( 20 );
    menu.AddItem( PCB_ACTIONS::planMoveToLayer, hasSelection, 20 );
    menu.AddItem( PCB_ACTIONS::planRouteEarlier, hasSelection, 20 );
    menu.AddItem( PCB_ACTIONS::planRouteLater, hasSelection, 20 );
    menu.AddItem( PCB_ACTIONS::planWider, hasSelection, 20 );
    menu.AddItem( PCB_ACTIONS::planNarrower, hasSelection, 20 );
    menu.AddSeparator( 30 );
    menu.AddItem( PCB_ACTIONS::planMergeGroups, manyCorridors, 30 );
    menu.AddItem( PCB_ACTIONS::planSplitGroup, oneCorridor, 30 );
    menu.AddItem( PCB_ACTIONS::planAddNote, SELECTION_CONDITIONS::ShowAlways, 30 );
    menu.AddItem( PCB_ACTIONS::planDelete, hasSelection, 30 );
    menu.AddSeparator( 40 );
    menu.AddItem( PCB_ACTIONS::planDrawCorridor, SELECTION_CONDITIONS::ShowAlways, 40 );
    menu.AddItem( PCB_ACTIONS::planDrawPlane, SELECTION_CONDITIONS::ShowAlways, 40 );
    menu.AddSeparator( 50 );
    menu.AddItem( PCB_ACTIONS::planApproveLayer, SELECTION_CONDITIONS::ShowAlways, 50 );
    menu.AddItem( PCB_ACTIONS::planRejectLayer, SELECTION_CONDITIONS::ShowAlways, 50 );
    menu.AddItem( PCB_ACTIONS::planRequestReplan, SELECTION_CONDITIONS::ShowAlways, 50 );
    menu.AddSeparator( 60 );
    menu.AddItem( PCB_ACTIONS::planToggleView, SELECTION_CONDITIONS::ShowAlways, 60 );

    return true;
}


void PLAN_TOOL::Reset( RESET_REASON aReason )
{
    if( aReason == MODEL_RELOAD )
        m_selected.clear();
}


void PLAN_TOOL::setTransitions()
{
    Go( &PLAN_TOOL::TogglePlanView, PCB_ACTIONS::planToggleView.MakeEvent() );
    Go( &PLAN_TOOL::ToggleShowPlan, PCB_ACTIONS::planToggleShow.MakeEvent() );
    Go( &PLAN_TOOL::EditPlan,       PCB_ACTIONS::planEdit.MakeEvent() );
    Go( &PLAN_TOOL::Accept,         PCB_ACTIONS::planAccept.MakeEvent() );
    Go( &PLAN_TOOL::Reject,         PCB_ACTIONS::planReject.MakeEvent() );
    Go( &PLAN_TOOL::ToggleLock,     PCB_ACTIONS::planToggleLock.MakeEvent() );
    Go( &PLAN_TOOL::RouteEarlier,   PCB_ACTIONS::planRouteEarlier.MakeEvent() );
    Go( &PLAN_TOOL::RouteLater,     PCB_ACTIONS::planRouteLater.MakeEvent() );
    Go( &PLAN_TOOL::Wider,          PCB_ACTIONS::planWider.MakeEvent() );
    Go( &PLAN_TOOL::Narrower,       PCB_ACTIONS::planNarrower.MakeEvent() );
    Go( &PLAN_TOOL::MoveToLayer,    PCB_ACTIONS::planMoveToLayer.MakeEvent() );
    Go( &PLAN_TOOL::MergeGroups,    PCB_ACTIONS::planMergeGroups.MakeEvent() );
    Go( &PLAN_TOOL::SplitGroup,     PCB_ACTIONS::planSplitGroup.MakeEvent() );
    Go( &PLAN_TOOL::AddNote,        PCB_ACTIONS::planAddNote.MakeEvent() );
    Go( &PLAN_TOOL::DeleteItems,    PCB_ACTIONS::planDelete.MakeEvent() );
    Go( &PLAN_TOOL::DrawCorridor,   PCB_ACTIONS::planDrawCorridor.MakeEvent() );
    Go( &PLAN_TOOL::DrawPlane,      PCB_ACTIONS::planDrawPlane.MakeEvent() );
    Go( &PLAN_TOOL::ApproveLayer,   PCB_ACTIONS::planApproveLayer.MakeEvent() );
    Go( &PLAN_TOOL::RejectLayer,    PCB_ACTIONS::planRejectLayer.MakeEvent() );
    Go( &PLAN_TOOL::RequestReplan,  PCB_ACTIONS::planRequestReplan.MakeEvent() );
}


PCB_EDIT_FRAME* PLAN_TOOL::editFrame() const
{
    return dynamic_cast<PCB_EDIT_FRAME*>( frame() );
}


PLAN_VIEW_ITEM* PLAN_TOOL::planItem() const
{
    PCB_EDIT_FRAME* f = editFrame();
    return f ? f->GetCanvas()->GetPlanView() : nullptr;
}


std::string PLAN_TOOL::activeLayerName() const
{
    return LSET::Name( frame()->GetActiveLayer() ).ToStdString( wxConvUTF8 );
}


void PLAN_TOOL::constrain( BOARD_PLAN& aPlan, const std::string& aKind, const std::string& aTarget,
                           const std::string& aText )
{
    // One constraint per (kind, target): the newest edit replaces the older wording.
    std::erase_if( aPlan.constraints,
                   [&]( const PLAN_CONSTRAINT& c )
                   { return c.kind == aKind && c.target == aTarget && c.author == "person"; } );

    PLAN_CONSTRAINT c;
    c.id = aPlan.NewId( "k" );
    c.kind = aKind;
    c.target = aTarget;
    c.author = "person";
    c.text = aText;
    aPlan.constraints.push_back( c );
}


void PLAN_TOOL::edit( const wxString& aDescription, const std::string& aOp,
                      const std::vector<std::string>& aItems,
                      const std::function<void( BOARD_PLAN& )>& aEdit )
{
    PCB_EDIT_FRAME* f = editFrame();

    if( !f )
        return;

    BOARD_PLAN next = board()->GetPlan();
    aEdit( next );
    f->CommitPlan( next, aDescription, aOp, aItems );
    syncSelection();
}


void PLAN_TOOL::syncSelection()
{
    // Drop ids that no longer exist.
    const BOARD_PLAN& plan = board()->GetPlan();
    std::set<std::string> live;

    for( const PLAN_CORRIDOR& c : plan.corridors ) live.insert( c.id );
    for( const PLAN_PLANE& p : plan.planes )       live.insert( p.id );
    for( const PLAN_NOTE& n : plan.notes )         live.insert( n.id );
    for( const PLAN_SPLIT& s : plan.splits )       live.insert( s.id );

    std::erase_if( m_selected, [&]( const std::string& id ) { return !live.count( id ); } );

    if( PLAN_VIEW_ITEM* item = planItem() )
    {
        item->SetSelected( m_selected );
        editFrame()->GetCanvas()->RefreshPlan();
    }
}


std::string PLAN_TOOL::HitTest( const VECTOR2I& aPoint, int* aVertex ) const
{
    const BOARD_PLAN& plan = board()->GetPlan();
    std::string       layer = activeLayerName();
    double            tol = getView()->ToWorld( 6.0 );

    if( aVertex )
        *aVertex = -1;

    // Notes first (on top), then vertices of selected items, then corridors in reverse routing
    // order (the topmost), then planes by priority.
    double glyph = getView()->ToWorld( 11.0 );

    for( const PLAN_NOTE& n : plan.notes )
    {
        if( n.layer != "*" && n.layer != layer )
            continue;

        double w = glyph * 0.62 * n.text.size() + glyph;

        if( aPoint.x >= n.at.x && aPoint.x <= n.at.x + w && aPoint.y >= n.at.y
            && aPoint.y <= n.at.y + glyph * 1.7 )
        {
            return n.id;
        }
    }

    for( const PLAN_CORRIDOR& c : plan.corridors )
    {
        if( c.layer != layer || !m_selected.count( c.id ) )
            continue;

        for( size_t ii = 0; ii < c.path.size(); ++ii )
        {
            if( ( c.path[ii] - aPoint ).EuclideanNorm() <= tol )
            {
                if( aVertex )
                    *aVertex = (int) ii;

                return c.id;
            }
        }
    }

    for( const PLAN_PLANE& p : plan.planes )
    {
        if( p.layer != layer || !m_selected.count( p.id ) )
            continue;

        for( size_t ii = 0; ii < p.outline.front().size(); ++ii )
        {
            if( ( p.outline.front()[ii] - aPoint ).EuclideanNorm() <= tol )
            {
                if( aVertex )
                    *aVertex = (int) ii;

                return p.id;
            }
        }
    }

    std::vector<const PLAN_CORRIDOR*> corridors;

    for( const PLAN_CORRIDOR& c : plan.corridors )
    {
        if( c.layer == layer )
            corridors.push_back( &c );
    }

    std::stable_sort( corridors.begin(), corridors.end(),
                      []( const PLAN_CORRIDOR* a, const PLAN_CORRIDOR* b )
                      { return a->order > b->order; } );

    for( const PLAN_CORRIDOR* c : corridors )
    {
        if( distToPath( aPoint, c->path ) <= c->width / 2.0 + tol )
            return c->id;
    }

    std::vector<const PLAN_PLANE*> planes;

    for( const PLAN_PLANE& p : plan.planes )
    {
        if( p.layer == layer )
            planes.push_back( &p );
    }

    std::stable_sort( planes.begin(), planes.end(),
                      []( const PLAN_PLANE* a, const PLAN_PLANE* b )
                      { return a->priority > b->priority; } );

    for( const PLAN_PLANE* p : planes )
    {
        if( inside( aPoint, p->outline.front() ) )
            return p->id;
    }

    return "";
}


void PLAN_TOOL::SelectItem( const std::string& aId, bool aZoom )
{
    m_selected = { aId };
    syncSelection();

    if( !aZoom )
        return;

    BOARD_PLAN& plan = board()->GetPlan();
    BOX2I       box;
    bool        found = false;
    PCB_LAYER_ID layer = UNDEFINED_LAYER;

    auto add = [&]( const VECTOR2I& p )
    {
        if( !found )
            box = BOX2I( p, VECTOR2I( 0, 0 ) );
        else
            box.Merge( p );

        found = true;
    };

    if( PLAN_CORRIDOR* c = plan.FindCorridor( aId ) )
    {
        for( const VECTOR2I& p : c->path )
            add( p );

        box.Inflate( c->width );
        layer = board()->GetLayerID( wxString::FromUTF8( c->layer ) );
    }
    else if( PLAN_PLANE* p = plan.FindPlane( aId ) )
    {
        for( const VECTOR2I& pt : p->outline.front() )
            add( pt );

        layer = board()->GetLayerID( wxString::FromUTF8( p->layer ) );
    }

    if( !found )
        return;

    if( layer != UNDEFINED_LAYER )
        editFrame()->SetActiveLayer( layer );

    box.Inflate( std::max( box.GetWidth(), box.GetHeight() ) / 3 + 2000000 );
    getView()->SetCenter( box.Centre() );
    getView()->SetViewport( BOX2D( box.GetOrigin(), box.GetSize() ) );
    editFrame()->GetCanvas()->RefreshPlan();
}


int PLAN_TOOL::TogglePlanView( const TOOL_EVENT& aEvent )
{
    PLAN_VIEW_ITEM* item = planItem();

    if( !item )
        return 0;

    bool on = !item->IsPlanView();
    item->SetPlanView( on );
    item->SetShown( true );
    editFrame()->GetCanvas()->RefreshPlan();

    if( on )
        m_toolMgr->PostAction( PCB_ACTIONS::planEdit );
    else
        m_toolMgr->PostAction( ACTIONS::selectionTool );

    return 0;
}


int PLAN_TOOL::ToggleShowPlan( const TOOL_EVENT& aEvent )
{
    if( PLAN_VIEW_ITEM* item = planItem() )
    {
        item->SetShown( !item->IsShown() );
        editFrame()->GetCanvas()->RefreshPlan();
    }

    return 0;
}


int PLAN_TOOL::EditPlan( const TOOL_EVENT& aEvent )
{
    PCB_EDIT_FRAME* f = editFrame();

    if( !f )
        return 0;

    Activate();
    m_toolMgr->RunAction( ACTIONS::selectionClear );
    controls()->ShowCursor( true );
    f->PushTool( aEvent );

    std::string hovered;

    while( TOOL_EVENT* evt = Wait() )
    {
        VECTOR2I pos = evt->HasPosition() ? VECTOR2I( evt->Position() )
                                          : VECTOR2I( controls()->GetMousePosition() );

        if( evt->IsCancelInteractive() )
        {
            if( !m_selected.empty() )
            {
                m_selected.clear();
                syncSelection();
                continue;
            }

            break;
        }

        if( evt->IsActivate() && !evt->IsMoveTool() )
            break;

        if( evt->IsMotion() )
        {
            std::string hit = HitTest( pos );

            if( hit != hovered )
            {
                hovered = hit;
                planItem()->SetHovered( hit );
                f->GetCanvas()->RefreshPlan();
            }
        }
        else if( evt->IsClick( BUT_LEFT ) )
        {
            std::string hit = HitTest( pos );

            if( !evt->Modifier( MD_SHIFT ) )
                m_selected.clear();

            if( !hit.empty() )
            {
                if( evt->Modifier( MD_SHIFT ) && m_selected.count( hit ) )
                    m_selected.erase( hit );
                else
                    m_selected.insert( hit );
            }

            syncSelection();
        }
        else if( evt->IsDblClick( BUT_LEFT ) )
        {
            // Add a vertex on the corridor under the cursor.
            std::string hit = HitTest( pos );

            if( hit.rfind( "c-", 0 ) == 0 )
            {
                edit( _( "Add Corridor Vertex" ), "reshape", { hit },
                      [&]( BOARD_PLAN& plan )
                      {
                          PLAN_CORRIDOR* c = plan.FindCorridor( hit );
                          size_t         seg = 1;
                          distToPath( pos, c->path, &seg );
                          c->path.insert( c->path.begin() + seg, pos );
                          c->edited = true;
                          constrain( plan, "path", hit,
                                     fmt::format( "the person reshaped {}: keep its path", hit ) );
                      } );
                m_selected = { hit };
                syncSelection();
            }
        }
        else if( evt->IsDrag( BUT_LEFT ) )
        {
            int         vertex = -1;
            std::string hit = HitTest( pos, &vertex );

            if( hit.empty() )
                continue;

            if( !m_selected.count( hit ) )
            {
                if( !evt->Modifier( MD_SHIFT ) )
                    m_selected.clear();

                m_selected.insert( hit );
                syncSelection();
            }

            dragItems( pos, hit, vertex );
        }
        else if( evt->IsClick( BUT_RIGHT ) )
        {
            std::string hit = HitTest( pos );

            if( !hit.empty() && !m_selected.count( hit ) )
            {
                m_selected = { hit };
                syncSelection();
            }

            m_menu->ShowContextMenu();
        }
        else if( evt->IsAction( &ACTIONS::doDelete ) )
        {
            DeleteItems( *evt );
        }
        else
        {
            evt->SetPassEvent();
        }
    }

    planItem()->SetHovered( "" );
    f->GetCanvas()->RefreshPlan();
    f->PopTool( aEvent );
    return 0;
}


void PLAN_TOOL::dragItems( const VECTOR2I& aStart, const std::string& aHit, int aVertex )
{
    PCB_EDIT_FRAME* f = editFrame();
    BOARD_PLAN      before = board()->GetPlan();
    VECTOR2I        delta;
    bool            moved = false;

    m_dragging = true;
    struct CLEAR { bool& flag; ~CLEAR() { flag = false; } } clear{ m_dragging };

    auto apply = [&]( BOARD_PLAN& plan, const VECTOR2I& d )
    {
        if( aVertex >= 0 )
        {
            if( PLAN_CORRIDOR* c = plan.FindCorridor( aHit ) )
                c->path[aVertex] += d;
            else if( PLAN_PLANE* p = plan.FindPlane( aHit ) )
                p->outline.front()[aVertex] += d;

            return;
        }

        for( const std::string& id : m_selected )
        {
            if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
            {
                for( VECTOR2I& p : c->path ) p += d;
                for( VECTOR2I& p : c->vias ) p += d;
            }
            else if( PLAN_PLANE* p = plan.FindPlane( id ) )
            {
                for( auto& ring : p->outline )
                    for( VECTOR2I& pt : ring ) pt += d;
            }

            for( PLAN_NOTE& n : plan.notes )
            {
                if( n.id == id )
                    n.at += d;
            }
        }
    };

    while( TOOL_EVENT* evt = Wait() )
    {
        if( evt->IsCancelInteractive() || evt->IsActivate() )
        {
            board()->GetPlan() = before;
            f->GetCanvas()->RefreshPlan();
            return;
        }

        if( evt->IsMouseUp( BUT_LEFT ) )
            break;

        if( evt->IsDrag( BUT_LEFT ) || evt->IsMotion() )
        {
            VECTOR2I pos = evt->HasPosition() ? VECTOR2I( evt->Position() )
                                              : VECTOR2I( controls()->GetMousePosition() );
            delta = pos - aStart;
            moved = true;

            // Live preview on the board's plan; committed (with undo) on release.
            BOARD_PLAN preview = before;
            apply( preview, delta );
            board()->GetPlan() = preview;
            f->GetCanvas()->RefreshPlan();
        }
    }

    // The agent may have changed the plan meanwhile (it was the board's plan we previewed on);
    // rebase: apply the person's drag to the latest plan without the preview.
    board()->GetPlan() = before;

    if( !moved || delta == VECTOR2I() )
        return;

    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );
    std::string              what = aVertex >= 0 ? "reshape" : "move";

    edit( aVertex >= 0 ? _( "Reshape Plan Item" ) : _( "Move Plan Items" ), what, ids,
          [&]( BOARD_PLAN& plan )
          {
              apply( plan, delta );

              for( const std::string& id : ( aVertex >= 0 ? std::vector<std::string>{ aHit } : ids ) )
              {
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                      c->edited = true;
                  else if( PLAN_PLANE* p = plan.FindPlane( id ) )
                      p->edited = true;

                  constrain( plan, "path", id,
                             aVertex >= 0
                                     ? fmt::format( "the person reshaped {}: keep its shape", id )
                                     : fmt::format( "the person moved {} by ({:.2f}, {:.2f}) mm: "
                                                    "keep it there",
                                                    id, delta.x / 1e6, delta.y / 1e6 ) );
              }
          } );
}


int PLAN_TOOL::Accept( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    if( ids.empty() )
        return 0;

    edit( _( "Accept Plan Items" ), "approve", ids,
          [&]( BOARD_PLAN& plan )
          {
              for( const std::string& id : ids )
              {
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                  {
                      if( c->status == PLAN_STATUS::PROPOSED || c->status == PLAN_STATUS::REJECTED )
                          c->status = PLAN_STATUS::APPROVED;
                  }
                  else if( PLAN_PLANE* p = plan.FindPlane( id ) )
                  {
                      if( p->status == PLAN_STATUS::PROPOSED || p->status == PLAN_STATUS::REJECTED )
                          p->status = PLAN_STATUS::APPROVED;
                  }
              }
          } );
    return 0;
}


int PLAN_TOOL::Reject( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    if( ids.empty() )
        return 0;

    edit( _( "Reject Plan Items" ), "reject", ids,
          [&]( BOARD_PLAN& plan )
          {
              for( const std::string& id : ids )
              {
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                      c->status = PLAN_STATUS::REJECTED;
                  else if( PLAN_PLANE* p = plan.FindPlane( id ) )
                      p->status = PLAN_STATUS::REJECTED;

                  constrain( plan, "rejected", id,
                             fmt::format( "the person rejected {}: propose something else", id ) );
              }
          } );
    return 0;
}


int PLAN_TOOL::ToggleLock( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    if( ids.empty() )
        return 0;

    edit( _( "Lock / Unlock Plan Items" ), "lock", ids,
          [&]( BOARD_PLAN& plan )
          {
              for( const std::string& id : ids )
              {
                  bool* locked = nullptr;

                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                      locked = &c->locked;
                  else if( PLAN_PLANE* p = plan.FindPlane( id ) )
                      locked = &p->locked;

                  if( locked )
                  {
                      *locked = !*locked;

                      if( *locked )
                          constrain( plan, "keep", id,
                                     fmt::format( "the person locked {}: do not change it", id ) );
                      else
                          std::erase_if( plan.constraints, [&]( const PLAN_CONSTRAINT& k )
                                         { return k.kind == "keep" && k.target == id; } );
                  }
              }
          } );
    return 0;
}


static void reorder( BOARD_PLAN& aPlan, const std::set<std::string>& aIds, int aStep )
{
    // Order is a ranking over all corridors: move the selection one step and renumber 1..n.
    std::vector<PLAN_CORRIDOR*> all;

    for( PLAN_CORRIDOR& c : aPlan.corridors )
        all.push_back( &c );

    std::stable_sort( all.begin(), all.end(),
                      []( PLAN_CORRIDOR* a, PLAN_CORRIDOR* b ) { return a->order < b->order; } );

    if( aStep < 0 )
    {
        for( size_t ii = 1; ii < all.size(); ++ii )
            if( aIds.count( all[ii]->id ) && !aIds.count( all[ii - 1]->id ) )
                std::swap( all[ii], all[ii - 1] );
    }
    else
    {
        for( size_t ii = all.size() - 1; ii-- > 0; )
            if( aIds.count( all[ii]->id ) && !aIds.count( all[ii + 1]->id ) )
                std::swap( all[ii], all[ii + 1] );
    }

    for( size_t ii = 0; ii < all.size(); ++ii )
        all[ii]->order = (int) ii + 1;
}


int PLAN_TOOL::RouteEarlier( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    edit( _( "Route Earlier" ), "order", ids,
          [&]( BOARD_PLAN& plan )
          {
              reorder( plan, m_selected, -1 );

              for( const std::string& id : ids )
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                      constrain( plan, "order", id,
                                 fmt::format( "the person wants {} routed as number {}", id, c->order ) );
          } );
    return 0;
}


int PLAN_TOOL::RouteLater( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    edit( _( "Route Later" ), "order", ids,
          [&]( BOARD_PLAN& plan )
          {
              reorder( plan, m_selected, +1 );

              for( const std::string& id : ids )
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                      constrain( plan, "order", id,
                                 fmt::format( "the person wants {} routed as number {}", id, c->order ) );
          } );
    return 0;
}


static void scaleWidth( BOARD_PLAN& aPlan, const std::set<std::string>& aIds, double aFactor )
{
    for( const std::string& id : aIds )
    {
        if( PLAN_CORRIDOR* c = aPlan.FindCorridor( id ) )
        {
            c->width = std::max( 200000, KiROUND( c->width * aFactor ) );
            c->edited = true;
        }
    }
}


int PLAN_TOOL::Wider( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    edit( _( "Widen Corridor" ), "width", ids,
          [&]( BOARD_PLAN& plan )
          {
              scaleWidth( plan, m_selected, 1.2 );

              for( const std::string& id : ids )
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                      constrain( plan, "width", id,
                                 fmt::format( "the person set {} to {} wide", id, mm( c->width ) ) );
          } );
    return 0;
}


int PLAN_TOOL::Narrower( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    edit( _( "Narrow Corridor" ), "width", ids,
          [&]( BOARD_PLAN& plan )
          {
              scaleWidth( plan, m_selected, 1 / 1.2 );

              for( const std::string& id : ids )
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                      constrain( plan, "width", id,
                                 fmt::format( "the person set {} to {} wide", id, mm( c->width ) ) );
          } );
    return 0;
}


int PLAN_TOOL::MoveToLayer( const TOOL_EVENT& aEvent )
{
    if( m_selected.empty() )
        return 0;

    wxArrayString             names;
    std::vector<PCB_LAYER_ID> ids;

    for( PCB_LAYER_ID layer : board()->GetEnabledLayers().CuStack() )
    {
        names.Add( wxString::FromUTF8( PlanLayerChip( *board(), LSET::Name( layer ).ToStdString() ) ) );
        ids.push_back( layer );
    }

    wxSingleChoiceDialog dlg( frame(), _( "Move the selected plan items to:" ), _( "Move to Layer" ),
                              names );

    if( dlg.ShowModal() != wxID_OK )
        return 0;

    std::string layer = LSET::Name( ids[dlg.GetSelection()] ).ToStdString();
    std::vector<std::string> sel( m_selected.begin(), m_selected.end() );

    edit( _( "Move Plan Items to Layer" ), "layer", sel,
          [&]( BOARD_PLAN& plan )
          {
              for( const std::string& id : sel )
              {
                  if( PLAN_CORRIDOR* c = plan.FindCorridor( id ) )
                  {
                      c->layer = layer;
                      c->edited = true;
                  }
                  else if( PLAN_PLANE* p = plan.FindPlane( id ) )
                  {
                      p->layer = layer;
                      p->edited = true;
                  }

                  constrain( plan, "layer", id,
                             fmt::format( "the person moved {} to {}", id, layer ) );
              }

              if( !plan.FindLayer( layer ) )
                  plan.layers.push_back( { layer, "signal", PLAN_STATUS::PROPOSED, "" } );
          } );

    frame()->SetActiveLayer( ids[dlg.GetSelection()] );
    return 0;
}


int PLAN_TOOL::MergeGroups( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> sel( m_selected.begin(), m_selected.end() );

    edit( _( "Merge Plan Groups" ), "merge", sel,
          [&]( BOARD_PLAN& plan )
          {
              std::string into;

              for( const std::string& id : sel )
              {
                  PLAN_CORRIDOR* c = plan.FindCorridor( id );

                  if( !c )
                      continue;

                  if( into.empty() )
                  {
                      into = c->group;
                      continue;
                  }

                  if( c->group == into )
                      continue;

                  PLAN_GROUP* from = plan.FindGroup( c->group );
                  PLAN_GROUP* to = plan.FindGroup( into );
                  std::string old = c->group;

                  if( from && to )
                  {
                      for( const std::string& net : from->nets )
                          if( std::find( to->nets.begin(), to->nets.end(), net ) == to->nets.end() )
                              to->nets.push_back( net );

                      to->diffPairs.insert( to->diffPairs.end(), from->diffPairs.begin(),
                                            from->diffPairs.end() );
                  }

                  for( PLAN_CORRIDOR& other : plan.corridors )
                      if( other.group == old )
                          other.group = into;

                  std::erase_if( plan.groups, [&]( const PLAN_GROUP& g ) { return g.id == old; } );
              }

              if( !into.empty() )
                  constrain( plan, "group", into,
                             fmt::format( "the person merged groups into {}: route them as one bus",
                                          into ) );
          } );
    return 0;
}


int PLAN_TOOL::SplitGroup( const TOOL_EVENT& aEvent )
{
    if( m_selected.size() != 1 )
        return 0;

    std::string    id = *m_selected.begin();
    BOARD_PLAN&    current = board()->GetPlan();
    PLAN_CORRIDOR* c = current.FindCorridor( id );
    PLAN_GROUP*    g = c ? current.FindGroup( c->group ) : nullptr;

    if( !g || g->nets.size() < 2 )
        return 0;

    wxArrayString nets;

    for( const std::string& net : g->nets )
        nets.Add( wxString::FromUTF8( net ) );

    wxMultiChoiceDialog dlg( frame(), _( "Nets to move into a new group:" ), _( "Split Group" ), nets );

    if( dlg.ShowModal() != wxID_OK || dlg.GetSelections().IsEmpty()
        || dlg.GetSelections().size() == nets.size() )
    {
        return 0;
    }

    std::set<std::string> moving;

    for( int idx : dlg.GetSelections() )
        moving.insert( g->nets[idx] );

    std::string newCorridor;

    edit( _( "Split Plan Group" ), "split", { id },
          [&]( BOARD_PLAN& plan )
          {
              PLAN_CORRIDOR* src = plan.FindCorridor( id );
              PLAN_GROUP*    grp = plan.FindGroup( src->group );

              PLAN_GROUP split = *grp;
              split.id = plan.NewId( "g" );
              split.name = grp->name + " (split)";
              split.nets.assign( moving.begin(), moving.end() );
              split.color = "";
              std::erase_if( grp->nets, [&]( const std::string& n ) { return moving.count( n ); } );
              plan.groups.push_back( split );

              PLAN_CORRIDOR copy = *src;
              copy.id = plan.NewId( "c" );
              copy.group = split.id;
              copy.order = (int) plan.corridors.size() + 1;
              copy.width = std::max( 400000, src->width * (int) moving.size()
                                                     / (int) ( moving.size() + grp->nets.size() ) );
              src->width = std::max( 400000, src->width - copy.width );

              // Beside the original, so both stay visible.
              VECTOR2I offset( 0, src->width / 2 + copy.width / 2 + 500000 );

              for( VECTOR2I& p : copy.path )
                  p += offset;

              copy.edited = true;
              copy.author = "person";
              src->edited = true;
              newCorridor = copy.id;
              plan.corridors.push_back( copy );

              constrain( plan, "group", split.id,
                         fmt::format( "the person split {} nets out of {} into {}", moving.size(),
                                      grp->id, split.id ) );
          } );

    if( !newCorridor.empty() )
    {
        m_selected = { newCorridor };
        syncSelection();
    }

    return 0;
}


int PLAN_TOOL::AddNote( const TOOL_EVENT& aEvent )
{
    wxTextEntryDialog dlg( frame(), _( "Note for the agent (e.g. \"keep away from U2\"):" ),
                           _( "Add Plan Note" ) );

    if( dlg.ShowModal() != wxID_OK || dlg.GetValue().Trim().IsEmpty() )
        return 0;

    std::string text = dlg.GetValue().ToStdString( wxConvUTF8 );
    VECTOR2I    at = controls()->GetCursorPosition( false );
    std::string target = m_selected.size() == 1 ? *m_selected.begin() : "";
    std::string layer = activeLayerName();

    edit( _( "Add Plan Note" ), "note", { target },
          [&]( BOARD_PLAN& plan )
          {
              PLAN_NOTE note;
              note.id = plan.NewId( "n" );
              note.at = at;
              note.layer = layer;
              note.text = text;
              note.author = "person";
              note.target = target;
              plan.notes.push_back( note );

              PLAN_CONSTRAINT c;
              c.id = plan.NewId( "k" );
              c.kind = "free";
              c.target = target;
              c.author = "person";
              c.text = text;
              plan.constraints.push_back( c );
          } );
    return 0;
}


int PLAN_TOOL::DeleteItems( const TOOL_EVENT& aEvent )
{
    std::vector<std::string> ids( m_selected.begin(), m_selected.end() );

    if( ids.empty() )
        return 0;

    edit( _( "Delete Plan Items" ), "delete", ids,
          [&]( BOARD_PLAN& plan )
          {
              std::set<std::string> gone( ids.begin(), ids.end() );

              for( const std::string& id : ids )
                  if( id.rfind( "c-", 0 ) == 0 || id.rfind( "p-", 0 ) == 0 )
                      constrain( plan, "removed", id,
                                 fmt::format( "the person deleted {}: do not propose it again", id ) );

              std::erase_if( plan.corridors, [&]( const PLAN_CORRIDOR& c ) { return gone.count( c.id ); } );
              std::erase_if( plan.planes, [&]( const PLAN_PLANE& p ) { return gone.count( p.id ); } );
              std::erase_if( plan.notes, [&]( const PLAN_NOTE& n ) { return gone.count( n.id ); } );
              std::erase_if( plan.splits, [&]( const PLAN_SPLIT& s ) { return gone.count( s.id ); } );
          } );

    m_selected.clear();
    syncSelection();
    return 0;
}


std::vector<VECTOR2I> PLAN_TOOL::collectPoints( bool aClosed )
{
    std::vector<VECTOR2I> points;
    PLAN_VIEW_ITEM*       item = planItem();
    std::string           layer = activeLayerName();
    int                   width = 1500000;

    while( TOOL_EVENT* evt = Wait() )
    {
        VECTOR2I pos = evt->HasPosition() ? VECTOR2I( evt->Position() )
                                          : VECTOR2I( controls()->GetCursorPosition() );

        if( evt->IsCancelInteractive() || evt->IsActivate() )
        {
            points.clear();
            break;
        }

        if( evt->IsClick( BUT_RIGHT ) || evt->IsDblClick( BUT_LEFT ) )
        {
            if( evt->IsDblClick( BUT_LEFT ) && ( points.empty() || points.back() != pos ) )
                points.push_back( pos );

            break;
        }

        if( evt->IsClick( BUT_LEFT ) )
            points.push_back( pos );
        else if( !evt->IsMotion() )
        {
            evt->SetPassEvent();
            continue;
        }

        std::vector<VECTOR2I> draft = points;
        draft.push_back( pos );

        if( aClosed && draft.size() >= 2 )
            draft.push_back( draft.front() );

        item->SetDraft( draft, aClosed ? getView()->ToWorld( 2.0 ) : width, layer );
        editFrame()->GetCanvas()->RefreshPlan();
    }

    item->SetDraft( {}, 0, "" );
    editFrame()->GetCanvas()->RefreshPlan();
    return points;
}


int PLAN_TOOL::DrawCorridor( const TOOL_EVENT& aEvent )
{
    if( !editFrame() )
        return 0;

    Activate();
    controls()->ShowCursor( true );
    frame()->PushTool( aEvent );

    std::vector<VECTOR2I> points = collectPoints( false );
    frame()->PopTool( aEvent );

    if( points.size() < 2 )
        return 0;

    // Which group: an existing one, or a new one from a net pattern.
    BOARD_PLAN&   plan = board()->GetPlan();
    wxArrayString choices;
    choices.Add( _( "New group from a net pattern..." ) );

    for( const PLAN_GROUP& g : plan.groups )
        choices.Add( wxString::Format( wxS( "%s (%zu nets)" ), wxString::FromUTF8( g.name ),
                                       g.nets.size() ) );

    wxSingleChoiceDialog pick( frame(), _( "Route which group along this corridor?" ),
                               _( "Draw Corridor" ), choices );

    if( pick.ShowModal() != wxID_OK )
        return 0;

    std::string groupId;
    PLAN_GROUP  newGroup;

    if( pick.GetSelection() == 0 )
    {
        wxTextEntryDialog pattern( frame(), _( "Nets (wildcards, e.g. DDR_DQ*):" ), _( "New Group" ) );

        if( pattern.ShowModal() != wxID_OK || pattern.GetValue().IsEmpty() )
            return 0;

        wxString pat = pattern.GetValue();

        for( NETINFO_ITEM* net : board()->GetNetInfo() )
        {
            if( net->GetNetCode() > 0
                && ( net->GetNetname().Matches( pat ) || net->GetNetname().Matches( wxS( "*/" ) + pat ) ) )
            {
                newGroup.nets.push_back( net->GetNetname().ToStdString( wxConvUTF8 ) );
            }
        }

        if( newGroup.nets.empty() )
        {
            wxMessageBox( _( "No net matches that pattern." ), _( "New Group" ) );
            return 0;
        }

        newGroup.name = pat.ToStdString( wxConvUTF8 );
    }
    else
    {
        groupId = plan.groups[pick.GetSelection() - 1].id;
    }

    std::string layer = activeLayerName();
    std::string newId;

    edit( _( "Draw Corridor" ), "add", {},
          [&]( BOARD_PLAN& p )
          {
              if( groupId.empty() )
              {
                  newGroup.id = p.NewId( "g" );
                  groupId = newGroup.id;
                  p.groups.push_back( newGroup );
              }

              PLAN_CORRIDOR c;
              c.id = p.NewId( "c" );
              c.group = groupId;
              c.layer = layer;
              c.path = points;
              c.width = std::max( 800000, (int) p.FindGroup( groupId )->nets.size() * 220000 );
              c.order = (int) p.corridors.size() + 1;
              c.author = "person";
              c.edited = true;
              newId = c.id;
              p.corridors.push_back( c );

              if( !p.FindLayer( layer ) )
                  p.layers.push_back( { layer, "signal", PLAN_STATUS::PROPOSED, "" } );

              constrain( p, "path", c.id,
                         fmt::format( "the person drew {} for {} on {}: route it this way", c.id,
                                      groupId, layer ) );
          } );

    m_selected = { newId };
    syncSelection();
    m_toolMgr->PostAction( PCB_ACTIONS::planEdit );
    return 0;
}


int PLAN_TOOL::DrawPlane( const TOOL_EVENT& aEvent )
{
    if( !editFrame() )
        return 0;

    Activate();
    controls()->ShowCursor( true );
    frame()->PushTool( aEvent );

    std::vector<VECTOR2I> points = collectPoints( true );
    frame()->PopTool( aEvent );

    if( points.size() < 3 )
        return 0;

    wxArrayString nets;

    for( NETINFO_ITEM* net : board()->GetNetInfo() )
    {
        if( net->GetNetCode() > 0 )
            nets.Add( net->GetNetname() );
    }

    nets.Sort();

    wxSingleChoiceDialog pick( frame(), _( "Plane net:" ), _( "Draw Plane Region" ), nets );

    if( pick.ShowModal() != wxID_OK )
        return 0;

    std::string net = pick.GetStringSelection().ToStdString( wxConvUTF8 );
    std::string layer = activeLayerName();
    std::string newId;

    edit( _( "Draw Plane Region" ), "add", {},
          [&]( BOARD_PLAN& p )
          {
              PLAN_PLANE plane;
              plane.id = p.NewId( "p" );
              plane.net = net;
              plane.layer = layer;
              plane.outline = { points };
              plane.priority = (int) p.planes.size() + 1;
              plane.author = "person";
              plane.edited = true;
              newId = plane.id;
              p.planes.push_back( plane );

              if( PLAN_LAYER* l = p.FindLayer( layer ) )
                  l->role = "power";
              else
                  p.layers.push_back( { layer, "power", PLAN_STATUS::PROPOSED, "" } );

              constrain( p, "path", plane.id,
                         fmt::format( "the person drew the {} plane region {} on {}", net,
                                      plane.id, layer ) );
          } );

    m_selected = { newId };
    syncSelection();
    m_toolMgr->PostAction( PCB_ACTIONS::planEdit );
    return 0;
}


int PLAN_TOOL::ApproveLayer( const TOOL_EVENT& aEvent )
{
    std::string layer = activeLayerName();
    edit( wxString::Format( _( "Approve %s Plan" ), layer ), "approve_layer", { layer },
          [&]( BOARD_PLAN& plan ) { plan.SetLayerStatus( layer, PLAN_STATUS::APPROVED ); } );
    return 0;
}


int PLAN_TOOL::RejectLayer( const TOOL_EVENT& aEvent )
{
    std::string layer = activeLayerName();
    edit( wxString::Format( _( "Reject %s Plan" ), layer ), "reject_layer", { layer },
          [&]( BOARD_PLAN& plan )
          {
              plan.SetLayerStatus( layer, PLAN_STATUS::REJECTED );
              constrain( plan, "rejected", layer,
                         fmt::format( "the person rejected the plan for {}: re-plan it", layer ) );
          } );
    return 0;
}


int PLAN_TOOL::RequestReplan( const TOOL_EVENT& aEvent )
{
    // Stop routing (the routing task watches for this), and put everything that was routing
    // back to approved: the agent re-plans with the person's constraints and notes.
    edit( _( "Back to Planning" ), "replan", {},
          [&]( BOARD_PLAN& plan )
          {
              // Nothing unrouted stays approved: the person approves again after the re-plan.
              for( PLAN_CORRIDOR& c : plan.corridors )
              {
                  if( c.status == PLAN_STATUS::ROUTING || c.status == PLAN_STATUS::APPROVED )
                      c.status = PLAN_STATUS::PROPOSED;
              }

              for( PLAN_PLANE& p : plan.planes )
              {
                  if( p.status == PLAN_STATUS::APPROVED )
                      p.status = PLAN_STATUS::PROPOSED;
              }

              for( PLAN_LAYER& l : plan.layers )
              {
                  if( l.status == PLAN_STATUS::APPROVED )
                      l.status = PLAN_STATUS::PROPOSED;
              }

              constrain( plan, "replan", "",
                         "the person asked for a new plan: keep every person edit, lock and note" );
          } );
    return 0;
}
