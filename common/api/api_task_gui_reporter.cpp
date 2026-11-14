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


bool API_TASK_GUI_PROGRESS_REPORTER::updateUI()
{
    const bool result = WX_PROGRESS_REPORTER::updateUI();

    // Serve the API requests that came in while the task ran (the server's own queue: wx's
    // YieldFor( wxEVT_CATEGORY_THREAD ) never ran them, so every request waited for the whole task)
    KICAD_API_SERVER::ServeDuringTask();

    return result;
}
