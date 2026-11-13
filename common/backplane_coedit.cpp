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

#include <backplane_coedit.h>
#include <backplane_embed.h>
#include <dialog_shim.h>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/evtloop.h>
#include <wx/frame.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <typeinfo>


namespace COEDIT
{

namespace
{
std::string& actorSlot()
{
    static std::string actor;
    return actor;
}

std::set<COMMIT*>& commits()
{
    // Never destroyed: commits owned by tools may outlive static destruction order.
    static std::set<COMMIT*>* live = new std::set<COMMIT*>();
    return *live;
}

constexpr size_t LOG_CAPACITY = 4096;
constexpr size_t NOTICE_CAPACITY = 256;

struct LOG
{
    uint64_t                              next = 1;
    std::deque<CHANGE>                    changes;
    std::deque<NOTICE>                    notices;
    std::function<void( const CHANGE& )>  changeListener;
    std::function<void( const NOTICE& )>  noticeListener;
};

LOG& log()
{
    static LOG l;
    return l;
}
} // namespace


const std::string& CurrentActor()
{
    return actorSlot();
}


ACTOR_SCOPE::ACTOR_SCOPE( const std::string& aActor ) :
        m_previous( actorSlot() )
{
    actorSlot() = aActor;
}


ACTOR_SCOPE::~ACTOR_SCOPE()
{
    actorSlot() = m_previous;
}


namespace
{
bool g_reading = false;
}

READ_SCOPE::READ_SCOPE( bool aRead ) : m_previous( g_reading ) { g_reading = aRead; }
READ_SCOPE::~READ_SCOPE() { g_reading = m_previous; }
bool READ_SCOPE::Active() { return g_reading; }


void RegisterCommit( COMMIT* aCommit )
{
    commits().insert( aCommit );
}


void UnregisterCommit( COMMIT* aCommit )
{
    commits().erase( aCommit );
}


const std::set<COMMIT*>& LiveCommits()
{
    return commits();
}


uint64_t RecordChange( CHANGE aChange )
{
    LOG& l = log();
    aChange.seq = l.next++;
    aChange.at = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch() ).count();
    l.changes.push_back( std::move( aChange ) );

    while( l.changes.size() > LOG_CAPACITY )
        l.changes.pop_front();

    if( l.changeListener )
        l.changeListener( l.changes.back() );

    uint64_t seq = l.changes.back().seq;
    NotifyActivity();
    return seq;
}


std::vector<CHANGE> ChangesSince( uint64_t aSince, const std::string& aDoc, bool& aLost )
{
    LOG&                l = log();
    std::vector<CHANGE> out;

    aLost = !l.changes.empty() && aSince + 1 < l.changes.front().seq;

    for( const CHANGE& c : l.changes )
    {
        if( c.seq > aSince && ( aDoc.empty() || c.doc == aDoc ) )
            out.push_back( c );
    }

    return out;
}


namespace
{
int g_rebasing = 0;
}

REBASE_SCOPE::REBASE_SCOPE()  { ++g_rebasing; }
REBASE_SCOPE::~REBASE_SCOPE() { --g_rebasing; }
bool REBASE_SCOPE::Active()   { return g_rebasing > 0; }


namespace
{
std::map<std::string, uint64_t>& reads()
{
    static auto* r = new std::map<std::string, uint64_t>();
    return *r;
}
} // namespace


std::string ActorForClient( const std::string& aClient )
{
    if( aClient.empty() )
        return "api";

    if( aClient.rfind( "person:", 0 ) == 0 )
        return std::string();

    return aClient;
}


void NoteRead( const std::string& aClient )
{
    reads()[aClient] = log().next - 1;
}


uint64_t LastRead( const std::string& aClient )
{
    auto it = reads().find( aClient );
    return it == reads().end() ? 0 : it->second;
}


std::optional<CHANGE> PersonChangedAfter( const std::string& aId, uint64_t aSince )
{
    const LOG& l = log();

    for( auto it = l.changes.rbegin(); it != l.changes.rend() && it->seq > aSince; ++it )
    {
        if( !it->actor.empty() || it->undo )
            continue;

        if( std::find( it->modified.begin(), it->modified.end(), aId ) != it->modified.end() )
            return *it;
    }

    return std::nullopt;
}


uint64_t LastSeq()
{
    return log().next - 1;
}


void SetChangeListener( std::function<void( const CHANGE& )> aListener )
{
    log().changeListener = std::move( aListener );
}


void PostNotice( const NOTICE& aNotice )
{
    LOG& l = log();
    l.notices.push_back( aNotice );

    while( l.notices.size() > NOTICE_CAPACITY )
        l.notices.pop_front();

    if( l.noticeListener )
        l.noticeListener( aNotice );
}


void SetNoticeListener( std::function<void( const NOTICE& )> aListener )
{
    log().noticeListener = std::move( aListener );
}


std::vector<NOTICE> NoticesFor( const std::string& aActor, bool aClear )
{
    LOG&                l = log();
    std::vector<NOTICE> out;

    for( auto it = l.notices.begin(); it != l.notices.end(); )
    {
        if( aActor.empty() || it->actor == aActor )
        {
            out.push_back( *it );

            if( aClear )
            {
                it = l.notices.erase( it );
                continue;
            }
        }

        ++it;
    }

    return out;
}


namespace
{
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch() ).count();
}

std::map<std::string, ACTIVITY>& activities()
{
    static auto* a = new std::map<std::string, ACTIVITY>();
    return *a;
}

std::map<void*, std::function<void()>>& activityListeners()
{
    static auto* l = new std::map<void*, std::function<void()>>();
    return *l;
}

std::function<void( const std::string&, const std::string&, const std::string& )>& controlListener()
{
    static auto* l = new std::function<void( const std::string&, const std::string&, const std::string& )>();
    return *l;
}
} // namespace


void SetActivity( const ACTIVITY& aActivity )
{
    ACTIVITY a = aActivity;
    a.updated = nowMs();
    activities()[a.actor] = a;
    NotifyActivity();
}


std::vector<ACTIVITY> Activities()
{
    std::vector<ACTIVITY> out;

    for( const auto& [actor, a] : activities() )
        out.push_back( a );

    return out;
}


std::vector<std::string> RecentItems( const std::string& aActor, int64_t aWindowMs )
{
    std::vector<std::string> out;
    std::set<std::string>    seen;
    int64_t                  since = nowMs() - aWindowMs;

    for( auto it = log().changes.rbegin(); it != log().changes.rend() && it->at >= since; ++it )
    {
        std::string actor = it->actor;

        if( auto hash = actor.find( '#' ); hash != std::string::npos )
            actor = actor.substr( 0, hash );

        if( actor != aActor || it->undo )
            continue;

        for( const auto* list : { &it->created, &it->modified } )
            for( const std::string& id : *list )
                if( seen.insert( id ).second )
                    out.push_back( id );
    }

    return out;
}


std::vector<CHANGE> AgentChanges( size_t aMax )
{
    std::vector<CHANGE> out;

    for( auto it = log().changes.rbegin(); it != log().changes.rend() && out.size() < aMax; ++it )
    {
        if( !it->actor.empty() && it->actor != "file" && !it->undo )
            out.push_back( *it );
    }

    return out;
}


void SetControlListener( std::function<void( const std::string&, const std::string&,
                                             const std::string& )> aListener )
{
    controlListener() = std::move( aListener );
}


void PostControl( const std::string& aActor, const std::string& aAction, const std::string& aText )
{
    if( aAction == "pause" || aAction == "resume" )
    {
        ACTIVITY& a = activities()[aActor];
        a.actor = aActor;
        a.state = aAction == "pause" ? "paused" : "working";
        a.updated = nowMs();
    }

    if( controlListener() )
        controlListener()( aActor, aAction, aText );

    NotifyActivity();
}


void AddActivityListener( void* aOwner, std::function<void()> aListener )
{
    activityListeners()[aOwner] = std::move( aListener );
}


void RemoveActivityListener( void* aOwner )
{
    activityListeners().erase( aOwner );
}


void NotifyActivity()
{
    // Copy: a listener may remove itself.
    std::map<void*, std::function<void()>> listeners = activityListeners();

    for( auto& [owner, fn] : listeners )
        fn();
}


bool IsStructuralDialog( const std::string& aTypeName )
{
    // Dialogs whose OK rewrites the document wholesale (annotation, netlist/schematic sync,
    // global edits, setup).  Matched against the mangled RTTI name, which contains the class
    // identifier verbatim.
    static const char* const structural[] = {
        "DIALOG_ANNOTATE",
        "DIALOG_BOARD_REANNOTATE",
        "DIALOG_UPDATE_PCB",
        "DIALOG_UPDATE_FROM_PCB",
        "DIALOG_IMPORT_NETLIST",
        "DIALOG_SYMBOL_FIELDS_TABLE",
        "DIALOG_CHANGE_SYMBOLS",
        "DIALOG_EXCHANGE_FOOTPRINTS",
        "DIALOG_GLOBAL_DELETION",
        "DIALOG_CLEANUP_TRACKS_AND_VIAS",
        "DIALOG_CLEANUP_GRAPHICS",
        "DIALOG_GLOBAL_EDIT_TRACKS_AND_VIAS",
        "DIALOG_GLOBAL_EDIT_TEXT_AND_GRAPHICS",
        "DIALOG_GLOBAL_SYM_LIB_TABLE_CONFIG",
        "DIALOG_BOARD_SETUP",
        "DIALOG_SCHEMATIC_SETUP",
        "DIALOG_SWAP_LAYERS",
        "DIALOG_EDIT_SYMBOLS_LIBID",
        "DIALOG_FIELD_PROPERTIES_GLOBAL",
        "DIALOG_GLOBAL_EDIT_TEXT_AND_GRAPHICS_BASE",
        // Filter- or board-wide edits, and generators that rebuild their children
        "DIALOG_GLOBAL_EDIT_TEARDROPS",
        "DIALOG_UNUSED_PAD_LAYERS",
        "DIALOG_GENERATORS",
        "DIALOG_TUNING_PATTERN_PROPERTIES",
        "DIALOG_MULTICHANNEL_GENERATE_RULE_AREAS",
        "DIALOG_GENERATED_TABLE_PROPERTIES",
        // Footprint editor: edits the whole footprint, not the selection
        "DIALOG_ENUM_PADS",
    };

    for( const char* name : structural )
    {
        if( aTypeName.find( name ) != std::string::npos )
            return true;
    }

    return false;
}


std::optional<MODAL> OpenModal()
{
    if( !wxTheApp )
        return std::nullopt;

    std::optional<MODAL> found;

    for( wxWindow* window : wxTopLevelWindows )
    {
        if( !window->IsShown() )
            continue;

        bool    modal = false;
        wxString title;

        if( DIALOG_SHIM* shim = dynamic_cast<DIALOG_SHIM*>( window ) )
        {
            modal = shim->IsQuasiModal() || shim->IsModal();
            title = shim->GetTitle();
        }
        else if( wxDialog* dialog = dynamic_cast<wxDialog*>( window ) )
        {
            modal = dialog->IsModal();
            title = dialog->GetTitle();
        }

        if( !modal )
            continue;

        bool structural = IsStructuralDialog( typeid( *window ).name() );

        if( !found || ( structural && !found->structural ) )
            found = MODAL{ title, structural };
    }

    if( found )
        return found;

    // Native modals (a file chooser, a message box) and modal frames (choosers): ordinary.
    if( std::optional<wxString> other = BACKPLANE_EMBED::ModalBlocker() )
        return MODAL{ *other, false };

    return std::nullopt;
}

} // namespace COEDIT
