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

#include "agents_panel.h"
#include "agent_edit_flash.h"

#include <backplane_coedit.h>
#include <board.h>
#include <pcb_edit_frame.h>

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/time.h>
#include <wx/datetime.h>


namespace
{
wxString agentName( const std::string& aActor )
{
    return AGENT_EDIT_FLASH::ActorLabel( aActor );
}

wxString ago( int64_t aMs )
{
    // The clock time: rows are not redrawn, so a relative "2 min" would go stale.
    return wxDateTime( (wxLongLong) aMs ).Format( wxS( "%H:%M:%S" ) );
}
} // namespace


AGENTS_PANEL::AGENTS_PANEL( wxWindow* aParent, PCB_EDIT_FRAME* aFrame ) :
        wxPanel( aParent ),
        m_frame( aFrame )
{
    wxBoxSizer* main = new wxBoxSizer( wxVERTICAL );

    m_agents = new wxBoxSizer( wxVERTICAL );
    main->Add( m_agents, 0, wxEXPAND | wxALL, 4 );

    m_log = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL );
    m_log->AppendColumn( _( "When" ), wxLIST_FORMAT_LEFT, FromDIP( 50 ) );
    m_log->AppendColumn( _( "Agent" ), wxLIST_FORMAT_LEFT, FromDIP( 80 ) );
    m_log->AppendColumn( _( "Change" ), wxLIST_FORMAT_LEFT, FromDIP( 220 ) );
    m_log->Bind( wxEVT_LIST_ITEM_ACTIVATED, &AGENTS_PANEL::onShow, this );
    main->Add( m_log, 1, wxEXPAND | wxLEFT | wxRIGHT, 4 );

    m_undo = new wxButton( this, wxID_ANY, _( "Undo this change" ) );
    m_undo->Bind( wxEVT_BUTTON, &AGENTS_PANEL::onUndo, this );
    main->Add( m_undo, 0, wxALL, 4 );

    SetSizer( main );

    // Rebuild at most once per idle: changes come in bursts.
    COEDIT::AddActivityListener( this,
                                 [this]()
                                 {
                                     if( m_pending )
                                         return;

                                     m_pending = true;
                                     CallAfter( [this]() { m_pending = false; Rebuild(); } );
                                 } );
    Rebuild();
}


AGENTS_PANEL::~AGENTS_PANEL()
{
    COEDIT::RemoveActivityListener( this );
}


void AGENTS_PANEL::Rebuild()
{
    // Cheap on every agent edit: the agent rows are rebuilt only when what they show changed,
    // and the change list only gets the new rows at its top.
    rebuildAgents();
    addChanges();
    m_frame->RefreshAgentZones();
}


void AGENTS_PANEL::rebuildAgents()
{
    std::string key;

    for( const COEDIT::ACTIVITY& a : COEDIT::Activities() )
    {
        if( a.state != "idle" )
            key += a.actor + "|" + a.task + "|" + a.state + "|"
                   + std::to_string( COEDIT::RecentItems( a.actor, 10 * 60 * 1000 ).size() ) + "\n";
    }

    if( key == m_activityKey )
        return;

    m_activityKey = key;
    m_agents->Clear( true );

    for( const COEDIT::ACTIVITY& a : COEDIT::Activities() )
    {
        if( a.state == "idle" )
            continue;

        size_t   touching = COEDIT::RecentItems( a.actor, 10 * 60 * 1000 ).size();
        wxString text = wxString::Format( wxS( "%s%s — %s" ), agentName( a.actor ),
                                          a.state == "paused" ? _( " (paused)" ) : wxString(),
                                          a.task.empty() ? _( "working" )
                                                         : wxString::FromUTF8( a.task ) );

        if( touching )
            text << wxString::Format( _( ", touching %zu items" ), touching );

        wxBoxSizer* row = new wxBoxSizer( wxHORIZONTAL );
        row->Add( new wxStaticText( this, wxID_ANY, text ), 1, wxALIGN_CENTER_VERTICAL );

        std::string actor = a.actor;
        bool        paused = a.state == "paused";
        wxButton*   pause = new wxButton( this, wxID_ANY, paused ? _( "Resume" ) : _( "Pause" ),
                                          wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT );
        pause->Bind( wxEVT_BUTTON, [actor, paused]( wxCommandEvent& )
                     { COEDIT::PostControl( actor, paused ? "resume" : "pause", "" ); } );

        wxButton* steer = new wxButton( this, wxID_ANY, _( "Steer..." ), wxDefaultPosition,
                                        wxDefaultSize, wxBU_EXACTFIT );
        steer->Bind( wxEVT_BUTTON,
                     [this, actor]( wxCommandEvent& )
                     {
                         wxTextEntryDialog dlg( this, _( "Tell the agent (it keeps working):" ),
                                                agentName( actor ) );

                         if( dlg.ShowModal() == wxID_OK && !dlg.GetValue().Trim().IsEmpty() )
                             COEDIT::PostControl( actor, "steer",
                                                  dlg.GetValue().ToStdString( wxConvUTF8 ) );
                     } );

        row->Add( pause, 0, wxLEFT, 4 );
        row->Add( steer, 0, wxLEFT, 4 );
        m_agents->Add( row, 0, wxEXPAND | wxBOTTOM, 2 );
    }

    Layout();
}


void AGENTS_PANEL::addChanges()
{
    // New agent changes, oldest first, each inserted at the top.
    std::vector<COEDIT::CHANGE> fresh;

    for( const COEDIT::CHANGE& c : COEDIT::AgentChanges( 200 ) )
    {
        if( c.seq <= m_lastSeq )
            break;

        if( c.doc == "pcb" || c.doc == "rules" || c.doc == "project" )
            fresh.push_back( c );
    }

    if( !COEDIT::AgentChanges( 1 ).empty() )
        m_lastSeq = std::max( m_lastSeq, COEDIT::AgentChanges( 1 ).front().seq );

    if( fresh.empty() )
        return;

    m_log->Freeze();

    for( auto it = fresh.rbegin(); it != fresh.rend(); ++it )
    {
        const COEDIT::CHANGE& c = *it;
        long row = m_log->InsertItem( 0, ago( c.at ) );
        m_log->SetItem( row, 1, agentName( c.actor ) );
        wxString what = wxString::FromUTF8( c.description );

        if( c.rebased )
            what << _( " (after your edit)" );

        m_log->SetItem( row, 2, what );
        m_rowSeqs.insert( m_rowSeqs.begin(), c.seq );

        std::vector<std::string> items = c.created;
        items.insert( items.end(), c.modified.begin(), c.modified.end() );
        m_rowItems.insert( m_rowItems.begin(), items );
    }

    // Keep the newest 200.
    while( m_log->GetItemCount() > 200 )
    {
        m_log->DeleteItem( m_log->GetItemCount() - 1 );
        m_rowSeqs.pop_back();
        m_rowItems.pop_back();
    }

    m_log->Thaw();
}


void AGENTS_PANEL::onUndo( wxCommandEvent& )
{
    long row = m_log->GetNextItem( -1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED );

    if( row < 0 || row >= (long) m_rowSeqs.size() )
        return;

    std::string error;

    if( !m_frame->UndoChangeBySeq( m_rowSeqs[row], error ) )
        m_frame->ShowInfoBarMsg( wxString::FromUTF8( error ) );
}


void AGENTS_PANEL::onShow( wxListEvent& aEvent )
{
    long row = aEvent.GetIndex();

    if( row < 0 || row >= (long) m_rowItems.size() )
        return;

    m_frame->ShowAgentItems( m_rowItems[row] );
}
