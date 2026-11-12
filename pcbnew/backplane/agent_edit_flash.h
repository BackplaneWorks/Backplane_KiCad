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
 * Attribution of agent edits (docs/backplane/coedit.md): the items an agent just changed
 * flash in the agent colour with a label "<agent> · <description>" for a moment, then go: two
 * overlay repaints per edit, no animation.
 */

#pragma once

#include <eda_item.h>
#include <gal/color4d.h>
#include <wx/timer.h>

#include <chrono>
#include <functional>
#include <vector>

namespace KIGFX { class VIEW; }


/// Where a working agent is: the area its recent changes cover, labelled with its task.
struct AGENT_EDIT_FLASH_ZONE
{
    BOX2I    box;
    wxString label;
    bool     paused = false;
};


class AGENT_EDIT_FLASH : public EDA_ITEM, public wxEvtHandler
{
public:
    AGENT_EDIT_FLASH( KIGFX::VIEW* aView, std::function<void()> aRefresh );
    ~AGENT_EDIT_FLASH() override;

    /// Starts a flash over these boxes (board coordinates).
    void Flash( const std::vector<BOX2I>& aBoxes, const wxString& aLabel );

    using ZONE = AGENT_EDIT_FLASH_ZONE;

    void SetZones( const std::vector<ZONE>& aZones );

    /// "See both" for a collision: the agent's version of an item (dashed, labelled) with an
    /// arrow from the person's.  Stays until cleared (aShow false).
    void SetGhost( bool aShow, const BOX2I& aMine = BOX2I(), const BOX2I& aAgents = BOX2I(),
                   const wxString& aLabel = wxEmptyString );

    const BOX2I      ViewBBox() const override;
    void             ViewDraw( int aLayer, KIGFX::VIEW* aView ) const override;
    std::vector<int> ViewGetLayers() const override;

    wxString GetClass() const override { return wxT( "AGENT_EDIT_FLASH" ); }
    bool HitTest( const VECTOR2I&, int = 0 ) const override { return false; }

    /// The colour of agent attribution everywhere (flash, labels).
    static KIGFX::COLOR4D AgentColor() { return KIGFX::COLOR4D( 0.55, 0.40, 1.0, 1.0 ); }

    /// A short name for an actor ("agent:t123" -> "agent t123").
    static wxString ActorLabel( const std::string& aActor );

private:
    struct ENTRY
    {
        std::vector<BOX2I>                    boxes;
        wxString                              label;
        std::chrono::steady_clock::time_point start;
    };

    double alpha( const ENTRY& aEntry ) const;
    void   repaint();
    void   onTimer( wxTimerEvent& aEvent );

    std::vector<ZONE>     m_zones;
    bool                  m_ghostShown = false;
    BOX2I                 m_ghostMine;
    BOX2I                 m_ghostAgents;
    wxString              m_ghostLabel;

    KIGFX::VIEW*          m_view;
    std::function<void()> m_refresh;
    std::vector<ENTRY> m_entries;
    wxTimer            m_timer;
    bool               m_added = false;
};
