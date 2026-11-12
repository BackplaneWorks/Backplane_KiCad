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
 * Live co-editing (fork-only, docs/backplane/coedit.md): who is making a change (the actor),
 * which commits the person has open, the per-document change log and the classification of
 * modal dialogs.  Everything here runs on the UI thread.
 */

#ifndef BACKPLANE_COEDIT_H
#define BACKPLANE_COEDIT_H

#include <kicommon.h>
#include <kiid.h>
#include <wx/string.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

class COMMIT;
class EDA_ITEM;
class PICKED_ITEMS_LIST;


namespace COEDIT
{

/// The actor of the change being made now: empty for the person, the API client's name while
/// an API request is handled.
KICOMMON_API const std::string& CurrentActor();

inline bool AgentActing() { return !CurrentActor().empty(); }

/// Sets the current actor for its lifetime (nests).
class KICOMMON_API ACTOR_SCOPE
{
public:
    explicit ACTOR_SCOPE( const std::string& aActor );
    ~ACTOR_SCOPE();

    ACTOR_SCOPE( const ACTOR_SCOPE& ) = delete;
    ACTOR_SCOPE& operator=( const ACTOR_SCOPE& ) = delete;

private:
    std::string m_previous;
};


/// While alive, the request being handled only reads (Get*, List*, HitTest*...): never refused
/// as busy, whatever the person is doing.
class KICOMMON_API READ_SCOPE
{
public:
    explicit READ_SCOPE( bool aRead );
    ~READ_SCOPE();
    static bool Active();

private:
    bool m_previous;
};


/// Every live COMMIT registers itself here, so holds can see what the person is editing.
KICOMMON_API void RegisterCommit( COMMIT* aCommit );
KICOMMON_API void UnregisterCommit( COMMIT* aCommit );
KICOMMON_API const std::set<COMMIT*>& LiveCommits();


/// One pushed change (an undo group), person or agent.
struct KICOMMON_API CHANGE
{
    uint64_t                 seq = 0;
    std::string              doc;          ///< "pcb" or "sch"
    std::string              actor;        ///< empty = the person
    std::string              description;
    std::vector<std::string> created;      ///< KIIDs
    std::vector<std::string> modified;
    std::vector<std::string> deleted;
    bool                     rebased = false;
    bool                     undo = false; ///< an undo or redo of an earlier change
    int64_t                  at = 0;       ///< when, ms since the epoch (set by RecordChange)
};

/// Records a change, assigns its seq and notifies listeners (the embed event stream).
KICOMMON_API uint64_t RecordChange( CHANGE aChange );

/// Builds a CHANGE from a pushed undo list (defined in undo_redo_container.cpp, common) (picker statuses give created/modified/deleted).
CHANGE ChangeFromUndoList( const PICKED_ITEMS_LIST& aList, const std::string& aDoc );

/// Changes after aSince for aDoc (empty = all).  aLost is set when some were dropped.
KICOMMON_API std::vector<CHANGE> ChangesSince( uint64_t aSince, const std::string& aDoc,
                                               bool& aLost );

KICOMMON_API uint64_t LastSeq();

/// A client read the document (any Get* request): what it knows is as of this seq.
KICOMMON_API void     NoteRead( const std::string& aClient );

/// The change seq as of the client's last read (0 = it never read).
KICOMMON_API uint64_t LastRead( const std::string& aClient );

/// The person's newest change touching aId made after aSince, if any.
KICOMMON_API std::optional<CHANGE> PersonChangedAfter( const std::string& aId, uint64_t aSince );

/// While alive, recorded changes are marked as rebased agent operations.
class KICOMMON_API REBASE_SCOPE
{
public:
    REBASE_SCOPE();
    ~REBASE_SCOPE();
    static bool Active();
};

/// Listener for recorded changes and notices (one: the embed layer).
KICOMMON_API void SetChangeListener( std::function<void( const CHANGE& )> aListener );

/// A notice for an agent: an operation was deferred and rebased, dropped, or its undo dropped.
struct KICOMMON_API NOTICE
{
    std::string kind;    ///< "rebased", "dropped", "undo_dropped", "deferred"
    std::string actor;   ///< the agent it is for
    std::string doc;
    std::string json;    ///< details
};

KICOMMON_API void PostNotice( const NOTICE& aNotice );
KICOMMON_API void SetNoticeListener( std::function<void( const NOTICE& )> aListener );
KICOMMON_API std::vector<NOTICE> NoticesFor( const std::string& aActor, bool aClear );


/// What an agent is doing, as its host says (SetAgentActivity) and as its changes show.
struct KICOMMON_API ACTIVITY
{
    std::string actor;
    std::string task;             ///< "Reroute the DDR bus"
    std::string state;            ///< "working", "paused", "idle"
    int64_t     updated = 0;      ///< ms
};

KICOMMON_API void SetActivity( const ACTIVITY& aActivity );
KICOMMON_API std::vector<ACTIVITY> Activities();

/// Items an actor changed in the last aWindowMs (newest first), from the change log.
KICOMMON_API std::vector<std::string> RecentItems( const std::string& aActor, int64_t aWindowMs );

/// Agent changes, newest first (at most aMax).
KICOMMON_API std::vector<CHANGE> AgentChanges( size_t aMax );

/// The person pauses, resumes or steers an agent: tells the host (an embed event).
KICOMMON_API void SetControlListener( std::function<void( const std::string& aActor,
                                                          const std::string& aAction,
                                                          const std::string& aText )> aListener );
KICOMMON_API void PostControl( const std::string& aActor, const std::string& aAction,
                               const std::string& aText );

/// Listeners for activity changes (the panels).
KICOMMON_API void AddActivityListener( void* aOwner, std::function<void()> aListener );
KICOMMON_API void RemoveActivityListener( void* aOwner );
KICOMMON_API void NotifyActivity();

/// The modal dialog (or modal window) that is open now, if any.
struct KICOMMON_API MODAL
{
    wxString title;
    bool     structural = false;  ///< rewrites the document: an exclusive hold
};

KICOMMON_API std::optional<MODAL> OpenModal();

/// True for a dialog class (by RTTI name) that rewrites the document wholesale.
KICOMMON_API bool IsStructuralDialog( const std::string& aTypeName );


/// What the person holds now in one editor (computed on demand by the editor's API handler).
struct KICOMMON_API HOLDS
{
    bool                 exclusive = false;   ///< no agent writes at all
    wxString             reason;              ///< the operation or dialog, for messages
    bool                 deferDeletes = false;///< agent deletes wait for the hold to end
    std::set<KIID>       items;               ///< agent updates of these wait
    bool                 allCopper = false;   ///< the router holds every connected item

    bool Active() const { return exclusive || deferDeletes || allCopper || !items.empty(); }
};

} // namespace COEDIT

#endif
