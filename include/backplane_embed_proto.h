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

/// Backplane fork: BACKPLANE_EMBED's structs as kiapi.backplane.embed messages.

#ifndef BACKPLANE_EMBED_PROTO_H_
#define BACKPLANE_EMBED_PROTO_H_

#include <api/backplane/embed.pb.h>
#include <backplane_embed.h>
#include <kicommon.h>

KICOMMON_API void FillEmbedFrame( kiapi::backplane::embed::FrameInfo* aOut,
                                  const EMBED_FRAME_INFO& aIn );
KICOMMON_API void FillEmbedItemNames( kiapi::backplane::embed::ItemNames* aOut,
                                      const EMBED_ITEM_NAMES& aIn );
KICOMMON_API void FillEmbedComment( kiapi::backplane::embed::Comment* aOut,
                                    const EMBED_COMMENT& aIn );
KICOMMON_API void FillEmbedEvent( kiapi::backplane::embed::Event* aOut, const EMBED_EVENT& aIn );

#endif // BACKPLANE_EMBED_PROTO_H_
