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

#ifndef SKETCH_ROUTE_BOARD_H
#define SKETCH_ROUTE_BOARD_H

#include <functional>
#include <vector>

#include <geometry/direction45.h>
#include <geometry/shape_line_chain.h>
#include <layer_ids.h>

#include "pns_sketch_router.h"

class BOARD;
class BOARD_CONNECTED_ITEM;
class BOARD_ITEM;
class COMMIT;


/**
 * Sketch routing without an editor: what to route and how.
 */
struct SKETCH_ROUTE_REQUEST
{
    /// Pads, tracks or vias whose unrouted connections to route.  Callers expand footprints to
    /// their pads.
    std::vector<BOARD_CONNECTED_ITEM*> m_items;

    /// Path for the routes to follow as a bundle; empty to find one automatically
    SHAPE_LINE_CHAIN m_guide;

    /// Copper layer the routes should keep to, or UNDEFINED_LAYER to use any
    PCB_LAYER_ID m_preferredLayer = UNDEFINED_LAYER;

    DIRECTION_45::CORNER_MODE m_cornerMode = DIRECTION_45::MITERED_45;
    bool m_allowVias = true;

    /// 0 keeps the router's default
    int m_timeLimitMs = 0;

    /// Track width for every route (nm); 0 takes each net's own (net class, rules)
    int m_trackWidth = 0;

    /// A connection with no way at its width may be routed this narrow (nm) where the
    /// board's rules allow; 0 never routes narrower than the net asks
    int m_minTrackWidth = 0;

    /// Called with (done, total); return false to cancel
    std::function<bool( int, int )> m_progress;

    /// Called with the router options just before routing, for callers that need more control
    std::function<void( PNS::SKETCH_ROUTER_OPTIONS& )> m_configure;
};


struct SKETCH_ROUTE_RESULT
{
    PNS::SKETCH_ROUTER_STATS m_stats;

    /// Unrouted connections found among the items
    int m_connections = 0;

    /// Tracks, arcs and vias created, in the order they were added
    std::vector<BOARD_ITEM*> m_added;

    /// A connection left unrouted, and why
    struct FAILURE
    {
        wxString    m_net;
        BOARD_ITEM* m_start = nullptr;
        BOARD_ITEM* m_end = nullptr;
        VECTOR2I    m_startPos;
        VECTOR2I    m_endPos;
        wxString    m_reason;
    };

    std::vector<FAILURE> m_failures;
};


/**
 * Route the unrouted connections between \a aRequest's items with the sketch router, on a
 * router of its own rather than an editor's.
 *
 * The new items are added to \a aCommit, which the caller pushes or reverts.  Without a commit
 * they go straight onto the board, which is then responsible for them.
 */
SKETCH_ROUTE_RESULT RouteSketchOnBoard( BOARD* aBoard, const SKETCH_ROUTE_REQUEST& aRequest,
                                        COMMIT* aCommit );

#endif // SKETCH_ROUTE_BOARD_H
