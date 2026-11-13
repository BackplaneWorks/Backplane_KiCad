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

#ifndef PREVIEW_ITEMS_HOST_MARKERS_ITEM_H
#define PREVIEW_ITEMS_HOST_MARKERS_ITEM_H

#include <eda_item.h>
#include <kiid.h>
#include <layer_ids.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace KIGFX
{
class VIEW;
}

/**
 * An embedding host's review markers (kiapi.backplane.embed SetHostMarkers): host state drawn in
 * the editor's overlay, never part of the document, DRC/ERC results or undo.  Drawn as rings in
 * the host's colours (not KiCad's DRC arrow), a fixed size on screen.
 */
struct HOST_MARKER
{
    std::string       set;
    std::string       id;
    std::string       title;
    std::string       detail;
    int               severity = 0;   ///< 0 info, 1 warning, 2 error (kiapi MarkerSeverity)
    VECTOR2I          pos;            ///< internal units of the editor
    std::vector<KIID> items;
    KIID_PATH         sheet;          ///< schematic: the sheet it is on (empty: every sheet)
    std::string       sheetPath;      ///< as the host sent it
    std::string       layer;
};


class HOST_MARKERS_ITEM : public EDA_ITEM
{
public:
    HOST_MARKERS_ITEM();

    /// Replace one named set; an empty list clears it.  Returns the markers now in the set.
    int SetMarkers( const std::string& aSet, std::vector<HOST_MARKER> aMarkers );

    /// Every marker, in set order.
    std::vector<const HOST_MARKER*> All() const;

    /// Schematic: only markers on the sheet shown (or on no sheet) are drawn and hit.
    void SetSheetProvider( std::function<KIID_PATH()> aCurrentSheet ) { m_currentSheet = std::move( aCurrentSheet ); }

    /// The marker under aWorld within aRadius (internal units), nearest first.
    const HOST_MARKER* HitMarker( const VECTOR2I& aWorld, int aRadius ) const;

    bool Empty() const;

    const BOX2I      ViewBBox() const override;
    std::vector<int> ViewGetLayers() const override { return { LAYER_GP_OVERLAY }; }
    void             ViewDraw( int aLayer, KIGFX::VIEW* aView ) const override;

    wxString GetClass() const override { return wxT( "HOST_MARKERS_ITEM" ); }

#if defined( DEBUG )
    void Show( int, std::ostream& ) const override {}
#endif

private:
    bool shown( const HOST_MARKER& aMarker ) const
    {
        return !m_currentSheet || aMarker.sheet.empty() || aMarker.sheet == m_currentSheet();
    }

    std::map<std::string, std::vector<HOST_MARKER>> m_sets;
    std::function<KIID_PATH()>                      m_currentSheet;
};

#endif
