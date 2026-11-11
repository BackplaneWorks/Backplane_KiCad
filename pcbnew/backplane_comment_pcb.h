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

/// Backplane fork: "Comment to Backplane..." in the PCB editor (bp.10).

#ifndef BACKPLANE_COMMENT_PCB_H_
#define BACKPLANE_COMMENT_PCB_H_

#include <backplane_embed.h>
#include <math/box2.h>

class PCB_EDIT_FRAME;

/// The comment for the PCB editor's current selection (no note, no screenshot yet).  aBox is
/// set to the selection's box (or the view's, with nothing selected), internal units.
EMBED_COMMENT BackplaneBuildPcbComment( PCB_EDIT_FRAME* aFrame, BOX2I& aBox );

/// Take the screenshot and push ET_COMMENT.  Returns the event's seq.
uint64_t BackplaneSendPcbComment( PCB_EDIT_FRAME* aFrame, EMBED_COMMENT& aComment,
                                  const BOX2I& aBox );

#endif // BACKPLANE_COMMENT_PCB_H_
