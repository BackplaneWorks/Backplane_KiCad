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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Backplane fork: "Comment to Backplane..." on a selection (bp.10), the parts shared by the PCB
 * and schematic editors.  The editors fill an EMBED_COMMENT from their selection; see
 * BACKPLANE_EMBED.md.
 */

#ifndef BACKPLANE_COMMENT_H_
#define BACKPLANE_COMMENT_H_

#include <backplane_embed.h>
#include <math/box2.h>

class EDA_DRAW_FRAME;
class wxWindow;

/// A box in internal units as millimetres.
EMBED_BOX_MM BackplaneBoxMm( const BOX2I& aBox, double aIuPerMm );

/// Grow aBox (the union of item boxes) by aOther; an empty aBox takes aOther.
void BackplaneMergeBox( BOX2I& aBox, bool& aHasBox, const BOX2I& aOther );

/**
 * Ask the person for a note.  aSummary says what is selected ("U3, R7 and 2 more").  Returns
 * false on Cancel or an empty note.
 */
bool BackplaneCommentDialog( wxWindow* aParent, const wxString& aSummary, wxString& aNote );

/// "U3, R7 and 2 more", "3 items", "the area in view".
wxString BackplaneCommentSummary( const EMBED_COMMENT& aComment );

/**
 * Fill aComment's screenshot fields with a PNG of aFrame's canvas around aBox (internal units,
 * padded; at most MAX_SIDE pixels on its long side).  The view is changed for the shot only if
 * the region is not already in view at a useful size, and is put back.  Nothing is done if the
 * canvas is not shown or there is no Backplane socket.
 */
void BackplaneCommentScreenshot( EDA_DRAW_FRAME* aFrame, const BOX2I& aBox, double aIuPerMm,
                                 EMBED_COMMENT& aComment );

constexpr int BACKPLANE_COMMENT_MAX_SIDE = 800;

#endif // BACKPLANE_COMMENT_H_
