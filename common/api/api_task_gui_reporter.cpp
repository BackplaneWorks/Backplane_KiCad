/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#include <api/api_task_gui_reporter.h>
#include <api/api_server.h>
#include <pgm_base.h>
#include <wx/thread.h>


bool API_TASK_GUI_PROGRESS_REPORTER::updateUI()
{
    const bool result = WX_PROGRESS_REPORTER::updateUI();

    // Serve the API requests that came in while the task ran.  The server hands each request to
    // this thread as a queued wx event, and wx's YieldFor( category ) only runs queued events when
    // asked for every category (wxEventLoopBase::DoYieldFor), so yielding for the thread category
    // served none: every request waited for the whole task.  Yielding for everything would also
    // run the application's other queued events (CallAfters, another task) inside this one, so the
    // server's own queue is run instead.  The server answers reads and refuses changes while a task
    // runs (KICAD_API_SERVER::handleApiRequestString).  Unthrottled: a request costs one dispatch.
    if( wxThread::IsMain() && PgmOrNull() )
    {
        if( KICAD_API_SERVER* server = PgmOrNull()->ApiServerOrNull() )
        {
            // Bounded: requests that keep arriving are served at the next update
            for( int ii = 0; ii < 64 && server->HasPendingEvents(); ++ii )
                server->ProcessPendingEvents();
        }
    }

    return result;
}
