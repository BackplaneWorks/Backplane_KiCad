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
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HOST_MARKERS_TOOL_H
#define HOST_MARKERS_TOOL_H

#include <tool/tool_interactive.h>
#include <preview_items/host_markers_item.h>

#include <memory>

/**
 * Keeps an embedding host's review markers (HOST_MARKERS_ITEM) in the editor's view: the view is
 * cleared on a sheet change or a board load, and tools re-add their items on the reset that
 * follows.
 */
class HOST_MARKERS_TOOL : public TOOL_INTERACTIVE
{
public:
    HOST_MARKERS_TOOL();
    ~HOST_MARKERS_TOOL() override;

    void Reset( RESET_REASON aReason ) override;

    /// The markers, added to the view on first use.
    HOST_MARKERS_ITEM& Markers();

    /// The markers if any were set, else null.
    HOST_MARKERS_ITEM* Existing() const { return m_item.get(); }

    /// Redraw after a change.
    void Changed();

private:
    std::unique_ptr<HOST_MARKERS_ITEM> m_item;
};

#endif
