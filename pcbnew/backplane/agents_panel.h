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
 * The Agents panel (docs/backplane/coedit.md, "Visibility"): what each agent is working on and
 * what it is touching, Pause / Resume and Steer without stopping it, and the agents' changes
 * with an Undo for each.
 */

#pragma once

#include <wx/panel.h>

#include <cstdint>
#include <string>
#include <vector>

class PCB_EDIT_FRAME;
class wxBoxSizer;
class wxListCtrl;
class wxListEvent;
class wxButton;


class AGENTS_PANEL : public wxPanel
{
public:
    AGENTS_PANEL( wxWindow* aParent, PCB_EDIT_FRAME* aFrame );
    ~AGENTS_PANEL() override;

    void Rebuild();

private:
    void onUndo( wxCommandEvent& aEvent );
    void onShow( wxListEvent& aEvent );

    PCB_EDIT_FRAME*       m_frame;
    wxBoxSizer*           m_agents;
    wxListCtrl*           m_log;
    wxButton*             m_undo;
    std::vector<uint64_t> m_rowSeqs;
    std::vector<std::vector<std::string>> m_rowItems;
    bool                  m_pending = false;
};
