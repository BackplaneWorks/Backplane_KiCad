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

#include "plan_panel.h"
#include "board_plan.h"
#include "plan_tool.h"

#include <api/api_handler_pcb.h>
#include <api/backplane/plan.pb.h>
#include <api/common/envelope.pb.h>
#include <board.h>
#include <pcb_edit_frame.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/gauge.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <map>


PLAN_PANEL::PLAN_PANEL( wxWindow* aParent, PCB_EDIT_FRAME* aFrame ) :
        wxPanel( aParent ),
        m_frame( aFrame ),
        m_routeTimer( this )
{
    wxBoxSizer* main = new wxBoxSizer( wxVERTICAL );

    m_hideDone = new wxCheckBox( this, wxID_ANY, _( "Hide done" ) );
    m_hideDone->Bind( wxEVT_CHECKBOX, [this]( wxCommandEvent& ) { Rebuild(); } );
    main->Add( m_hideDone, 0, wxALL, 4 );

    m_layerSizer = new wxBoxSizer( wxVERTICAL );
    main->Add( m_layerSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 4 );

    m_list = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL );
    m_list->AppendColumn( _( "Group" ), wxLIST_FORMAT_LEFT, FromDIP( 150 ) );
    m_list->AppendColumn( _( "Layer" ), wxLIST_FORMAT_LEFT, FromDIP( 60 ) );
    m_list->AppendColumn( _( "State" ), wxLIST_FORMAT_LEFT, FromDIP( 70 ) );
    m_list->AppendColumn( _( "Progress" ), wxLIST_FORMAT_LEFT, FromDIP( 60 ) );
    m_list->AppendColumn( _( "Problems" ), wxLIST_FORMAT_LEFT, FromDIP( 60 ) );
    m_list->Bind( wxEVT_LIST_ITEM_SELECTED, &PLAN_PANEL::onRowActivated, this );
    main->Add( m_list, 1, wxEXPAND | wxALL, 4 );

    m_audit = new wxStaticText( this, wxID_ANY, wxEmptyString );
    main->Add( m_audit, 0, wxEXPAND | wxLEFT | wxRIGHT, 6 );

    wxBoxSizer* gate = new wxBoxSizer( wxHORIZONTAL );
    wxButton*   approve = new wxButton( this, wxID_ANY, _( "Approve layer" ) );
    wxButton*   reject = new wxButton( this, wxID_ANY, _( "Reject layer" ) );
    approve->Bind( wxEVT_BUTTON, &PLAN_PANEL::onApproveLayer, this );
    reject->Bind( wxEVT_BUTTON, &PLAN_PANEL::onRejectLayer, this );
    gate->Add( approve, 0, wxRIGHT, 4 );
    gate->Add( reject, 0 );
    main->Add( gate, 0, wxALL, 4 );

    wxBoxSizer* route = new wxBoxSizer( wxHORIZONTAL );
    m_route = new wxButton( this, wxID_ANY, _( "Route approved" ) );
    m_stop = new wxButton( this, wxID_ANY, _( "Stop / back to planning" ) );
    m_route->Bind( wxEVT_BUTTON, &PLAN_PANEL::onRoute, this );
    m_stop->Bind( wxEVT_BUTTON, &PLAN_PANEL::onStop, this );
    route->Add( m_route, 0, wxRIGHT, 4 );
    route->Add( m_stop, 0 );
    main->Add( route, 0, wxALL, 4 );

    Bind( wxEVT_TIMER, &PLAN_PANEL::onRouteStep, this );
    SetSizer( main );
    Rebuild();
}


void PLAN_PANEL::Rebuild()
{
    BOARD* board = m_frame->GetBoard();

    if( !board )
        return;

    const BOARD_PLAN& plan = board->GetPlan();
    PLAN_AUDIT        audit = PLAN_AUDIT::Run( plan, *board );
    PLAN_PROGRESS     progress = PLAN_PROGRESS::Run( plan, *board, audit );

    // Per-layer progress: "L6 · SIG1  3/4 groups".
    m_layerSizer->Clear( true );

    for( const auto& [layer, lp] : progress.layers )
    {
        wxBoxSizer* row = new wxBoxSizer( wxHORIZONTAL );
        row->Add( new wxStaticText( this, wxID_ANY,
                                    wxString::Format( wxS( "%s   %d/%d groups" ),
                                                      wxString::FromUTF8( PlanLayerChip( *board, layer ) ),
                                                      lp.done, lp.groups ) ),
                  1, wxALIGN_CENTER_VERTICAL );
        wxGauge* gauge = new wxGauge( this, wxID_ANY, std::max( 1, lp.groups ), wxDefaultPosition,
                                      FromDIP( wxSize( 70, 8 ) ) );
        gauge->SetValue( lp.done );
        row->Add( gauge, 0, wxALIGN_CENTER_VERTICAL );
        m_layerSizer->Add( row, 0, wxEXPAND | wxBOTTOM, 2 );
    }

    // The checklist: one row per corridor (its group) and per plane region.
    m_list->DeleteAllItems();
    m_rowIds.clear();

    auto addRow = [&]( const std::string& aId, const wxString& aName, const std::string& aLayer )
    {
        const PLAN_PROGRESS::ITEM& item = progress.items[aId];

        if( m_hideDone->GetValue() && item.state == "verified" )
            return;

        long row = m_list->InsertItem( m_list->GetItemCount(), aName );
        m_list->SetItem( row, 1, wxString::FromUTF8( aLayer ) );
        m_list->SetItem( row, 2, wxString::FromUTF8( item.state ) );
        m_list->SetItem( row, 3, item.total ? wxString::Format( wxS( "%d/%d" ), item.routed, item.total )
                                            : wxString() );
        m_list->SetItem( row, 4, item.ProblemCount() ? wxString::Format( wxS( "%d" ), item.ProblemCount() )
                                                     : wxString() );

        if( item.ProblemCount() )
            m_list->SetItemTextColour( row, wxColour( 220, 70, 60 ) );
        else if( item.state == "verified" )
            m_list->SetItemTextColour( row, wxColour( 70, 170, 90 ) );

        m_rowIds.push_back( aId );
    };

    std::vector<const PLAN_CORRIDOR*> corridors;

    for( const PLAN_CORRIDOR& c : plan.corridors )
        corridors.push_back( &c );

    std::stable_sort( corridors.begin(), corridors.end(),
                      []( const PLAN_CORRIDOR* a, const PLAN_CORRIDOR* b ) { return a->order < b->order; } );

    for( const PLAN_CORRIDOR* c : corridors )
    {
        const PLAN_GROUP* g = plan.FindGroup( c->group );
        wxString name = wxString::Format( wxS( "%d  %s (%zu)" ), c->order,
                                          wxString::FromUTF8( g ? g->name : c->id ),
                                          g ? g->nets.size() : 0 );
        addRow( c->id, name, c->layer );
    }

    for( const PLAN_PLANE& p : plan.planes )
        addRow( p.id, wxString::FromUTF8( p.net ) + _( " plane" ), p.layer );

    // The audit, in short lines.
    wxString text;

    for( const PLAN_AUDIT::WARNING& w : audit.warnings )
        text << wxS( "⚠ " ) << wxString::FromUTF8( w.text ) << wxS( "\n" );

    for( const PLAN_AUDIT::BUS& b : audit.buses )
    {
        const PLAN_GROUP* g = plan.FindGroup( b.group );
        text << wxString::Format( _( "%s: %.1f–%.1f mm, skew %.2f mm\n" ),
                                  wxString::FromUTF8( g ? g->name : b.group ), b.minLength / 1e6,
                                  b.maxLength / 1e6, b.skew / 1e6 );
    }

    m_audit->SetLabel( text );
    m_route->Enable( !m_routing );
    m_stop->Enable( true );
    Layout();
}


void PLAN_PANEL::onRowActivated( wxListEvent& aEvent )
{
    long row = aEvent.GetIndex();

    if( row < 0 || row >= (long) m_rowIds.size() )
        return;

    if( PLAN_TOOL* tool = m_frame->GetToolManager()->GetTool<PLAN_TOOL>() )
        tool->SelectItem( m_rowIds[row], true );
}


void PLAN_PANEL::onApproveLayer( wxCommandEvent& )
{
    m_frame->GetToolManager()->RunAction( PCB_ACTIONS::planApproveLayer );
}


void PLAN_PANEL::onRejectLayer( wxCommandEvent& )
{
    m_frame->GetToolManager()->RunAction( PCB_ACTIONS::planRejectLayer );
}


void PLAN_PANEL::onRoute( wxCommandEvent& )
{
    m_routing = true;
    m_route->Enable( false );
    m_routeTimer.StartOnce( 10 );
}


void PLAN_PANEL::onStop( wxCommandEvent& )
{
    m_routing = false;
    m_routeTimer.Stop();
    m_frame->GetToolManager()->RunAction( PCB_ACTIONS::planRequestReplan );
    Rebuild();
}


void PLAN_PANEL::onRouteStep( wxTimerEvent& )
{
    if( !m_routing )
        return;

    // One step of RoutePlan through the editor's own API handler, as the person.  The UI runs
    // between steps: the overlay fills, and Stop works.
    kiapi::common::ApiRequest request;
    request.mutable_header()->set_client_name( "person" );

    kiapi::backplane::plan::RoutePlan step;
    step.mutable_board()->set_type( kiapi::common::types::DOCTYPE_PCB );
    step.mutable_board()->set_board_filename(
            wxFileName( m_frame->GetBoard()->GetFileName() ).GetFullName().ToStdString( wxConvUTF8 ) );
    step.set_max_nets( 2 );
    request.mutable_message()->PackFrom( step );

    API_RESULT result = m_frame->ApiHandler()->Handle( request );

    kiapi::backplane::plan::RoutePlanResponse reply;

    if( !result.has_value() || !result->message().UnpackTo( &reply ) || reply.done() )
    {
        m_routing = false;
        Rebuild();
        return;
    }

    m_routeTimer.StartOnce( 30 );
}
