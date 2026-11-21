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
 * The Routing plan panel (docs/backplane/floorplanner.md): the board checklist (every group
 * with its state, progress and problems; a row selects and zooms to it; Hide done), per-layer
 * progress, the audit, and the gate: approve / reject a layer, route the approved plan, and
 * Stop / back to planning while routing runs.
 */

#pragma once

#include <wx/panel.h>
#include <wx/timer.h>

#include <string>
#include <vector>

class PCB_EDIT_FRAME;
class wxButton;
class wxCheckBox;
class wxListCtrl;
class wxListEvent;
class wxStaticText;
class wxBoxSizer;


class PLAN_PANEL : public wxPanel
{
public:
    PLAN_PANEL( wxWindow* aParent, PCB_EDIT_FRAME* aFrame );

    /// Rebuild from the board's plan (after any plan change or layer switch).
    void Rebuild();

    bool IsRouting() const { return m_routing; }

private:
    void onRowActivated( wxListEvent& aEvent );
    void onApproveLayer( wxCommandEvent& aEvent );
    void onRejectLayer( wxCommandEvent& aEvent );
    void onRoute( wxCommandEvent& aEvent );
    void onStop( wxCommandEvent& aEvent );
    void onRouteStep( wxTimerEvent& aEvent );

    PCB_EDIT_FRAME*          m_frame;
    wxCheckBox*              m_hideDone;
    wxBoxSizer*              m_layerSizer;
    wxListCtrl*              m_list;
    wxStaticText*            m_audit;
    wxButton*                m_route;
    wxButton*                m_stop;
    wxTimer                  m_routeTimer;
    bool                     m_routing = false;
    std::vector<std::string> m_rowIds;
};
