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

// Progress of long jobs in --embedded (bp.12): the progress dialogs stay hidden and report here;
// the host hears ET_PROGRESS and draws it.  See BACKPLANE_EMBED.md.

#include <backplane_embed.h>

#include <algorithm>
#include <cmath>

#include <wx/frame.h>
#include <wx/statusbr.h>
#include <wx/toplevel.h>


namespace
{
constexpr auto UPDATE_EVERY = std::chrono::milliseconds( 100 );   ///< at most 10 Hz per job
constexpr auto QUIET_START = std::chrono::milliseconds( 150 );    ///< a short job: start and done only
constexpr int64_t HOST_LISTENING_MS = 5000;                      ///< a NextEvents this recent: a host draws it


int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch() )
            .count();
}
} // namespace


uint64_t BACKPLANE_EMBED::ProgressBegin( wxWindow* aOwner, const wxString& aTitle,
                                         bool aCancellable )
{
    PROGRESS_JOB job;
    job.state.title = aTitle;
    job.state.cancellable = aCancellable;
    job.started = job.lastSent = std::chrono::steady_clock::now();

    if( aOwner )
    {
        wxFrame* frame = wxDynamicCast( wxGetTopLevelParent( aOwner ), wxFrame );

        if( frame && IsEmbedded( frame ) )
            job.frame = frame;
    }

    {
        std::lock_guard<std::mutex> lock( m_progressMutex );
        job.state.id = m_nextProgressId++;
        m_progress[job.state.id] = job;
    }

    pushProgress( job );
    return job.state.id;
}


void BACKPLANE_EMBED::ProgressUpdate( uint64_t aId, double aFraction, const wxString& aDetail,
                                      const wxString& aTitle )
{
    PROGRESS_JOB copy;

    {
        std::lock_guard<std::mutex> lock( m_progressMutex );
        auto it = m_progress.find( aId );

        if( it == m_progress.end() )
            return;

        PROGRESS_JOB& job = it->second;
        job.state.fraction = aFraction < 0.0 ? -1.0 : std::clamp( aFraction, 0.0, 1.0 );

        if( !aDetail.IsEmpty() )
            job.state.detail = aDetail;

        if( !aTitle.IsEmpty() )
            job.state.title = aTitle;

        auto now = std::chrono::steady_clock::now();

        if( now - job.started < QUIET_START || now - job.lastSent < UPDATE_EVERY )
            return;

        job.lastSent = now;
        copy = job;
    }

    pushProgress( copy );

    // No host listening: the frame's own status bar shows it instead
    std::lock_guard<std::mutex> lock( m_progressMutex );

    if( auto it = m_progress.find( aId ); it != m_progress.end() )
        showProgressInStatusBar( it->second, false );
}


void BACKPLANE_EMBED::ProgressEnd( uint64_t aId )
{
    PROGRESS_JOB job;

    {
        std::lock_guard<std::mutex> lock( m_progressMutex );
        auto it = m_progress.find( aId );

        if( it == m_progress.end() )
            return;

        job = it->second;
        job.state.done = true;
        job.state.cancelled = m_cancelRequests.erase( aId ) > 0;

        if( job.state.fraction >= 0.0 && !job.state.cancelled )
            job.state.fraction = 1.0;

        showProgressInStatusBar( it->second, true );
        m_progress.erase( it );
    }

    pushProgress( job );
}


bool BACKPLANE_EMBED::ProgressCancelled( uint64_t aId ) const
{
    std::lock_guard<std::mutex> lock( m_progressMutex );
    return m_cancelRequests.count( aId ) > 0;
}


bool BACKPLANE_EMBED::RequestCancel( uint64_t aId )
{
    std::lock_guard<std::mutex> lock( m_progressMutex );
    auto it = m_progress.find( aId );

    if( it == m_progress.end() || !it->second.state.cancellable )
        return false;

    m_cancelRequests.insert( aId );
    return true;
}


std::vector<EMBED_PROGRESS> BACKPLANE_EMBED::ActiveProgress() const
{
    std::lock_guard<std::mutex> lock( m_progressMutex );
    std::vector<EMBED_PROGRESS> result;

    for( const auto& [id, job] : m_progress )
        result.push_back( job.state );

    return result;
}


void BACKPLANE_EMBED::pushProgress( const PROGRESS_JOB& aJob )
{
    EMBED_FRAME_INFO frameInfo;

    if( aJob.frame && entry( aJob.frame ) )
        frameInfo = Info( aJob.frame );

    auto progress = std::make_shared<EMBED_PROGRESS>( aJob.state );

    {
        std::lock_guard<std::mutex> lock( m_eventMutex );

        EMBED_EVENT event;
        event.seq = m_nextSeq++;
        event.type = EMBED_EVENT_TYPE::PROGRESS;
        event.frame = frameInfo;
        event.detail = aJob.state.title.ToStdString( wxConvUTF8 );
        event.progress = progress;
        m_events.push_back( event );

        while( m_events.size() > MAX_EVENTS )
            m_events.pop_front();
    }

    m_eventCv.notify_all();
}


void BACKPLANE_EMBED::showProgressInStatusBar( PROGRESS_JOB& aJob, bool aClear )
{
    if( !aJob.frame || !entry( aJob.frame ) )
        return;

    wxStatusBar* bar = aJob.frame->GetStatusBar();

    if( !bar )
        return;

    if( aClear )
    {
        if( aJob.statusShown )
            bar->SetStatusText( wxEmptyString, 0 );

        aJob.statusShown = false;
        return;
    }

    if( steadyMs() - m_lastPollMs.load() < HOST_LISTENING_MS )
        return;   // the host draws it

    wxString text = aJob.state.title;

    if( aJob.state.fraction >= 0.0 )
        text += wxString::Format( wxS( " %d%%" ), (int) std::lround( aJob.state.fraction * 100.0 ) );

    if( !aJob.state.detail.IsEmpty() )
        text += wxS( " — " ) + aJob.state.detail;

    bar->SetStatusText( text, 0 );
    aJob.statusShown = true;
}
