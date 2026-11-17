/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
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

#ifndef __PNS_SKETCH_ROUTER_H
#define __PNS_SKETCH_ROUTER_H

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include <math/box2.h>
#include <geometry/direction45.h>
#include <geometry/shape_line_chain.h>

#include "pns_algo_base.h"
#include "pns_item.h"

class BOARD;
class BOARD_CONNECTED_ITEM;
class BOARD_ITEM;

namespace PNS {

class NODE;
class ROUTER_IFACE;
class SKETCH_FIELD;
class VIA;

/**
 * One unrouted connection (a ratsnest edge) for the sketch router to complete.
 */
struct SKETCH_CONNECTION
{
    ITEM*      m_start = nullptr;   ///< Item the route starts on (pad, via or track end)
    VECTOR2I   m_startPos;          ///< Anchor on the start item
    ITEM*      m_end = nullptr;     ///< Item the route ends on
    VECTOR2I   m_endPos;            ///< Anchor on the end item
    NET_HANDLE m_net = nullptr;
};


/**
 * A way out of a pad: a grid direction (0 = +x, counting 45 degree steps toward +y) and the
 * length the track should run straight from the anchor before it turns.
 */
struct SKETCH_EXIT
{
    int m_dir;
    int m_length;
    int m_penalty = 0;   ///< extra cost (nm) for using this exit rather than a preferred one

    ///< Shortest straight run that still clears the pad, when that is less than m_length.
    ///< Leaves routes some slack to meet an exit without a jog.
    int m_minLength = 0;

    int MinLength() const { return m_minLength > 0 ? std::min( m_minLength, m_length ) : m_length; }

    bool operator==( const SKETCH_EXIT& aOther ) const = default;
};


/**
 * A kind of via the router may place: the layers it joins, its size, and what one costs.
 */
struct SKETCH_VIA_KIND
{
    int    m_type = 0;          ///< a VIATYPE
    int    m_layerStart = 0;    ///< PNS layers
    int    m_layerEnd = 0;
    int    m_diameter = 0;
    int    m_drill = 0;
    double m_cost = 0.0;        ///< as a length of track (nm)

    ///< One of the board's own vias of this kind: a new one is made like it (filled, capped
    ///< and tented as the board's are, which a stack of micro vias has to be), or nullptr
    const BOARD_ITEM* m_like = nullptr;
};


struct SKETCH_ROUTER_OPTIONS
{
    ///< PNS layers the router may put tracks on.  Empty means every copper layer.
    std::vector<int> m_layers;

    ///< PNS layers that carry planes: no track goes there, but a stack of vias may pass
    std::vector<int> m_transitLayers;

    ///< The vias to change layer with.  Empty: a through via of each net's own size.
    std::vector<SKETCH_VIA_KIND> m_viaKinds;

    ///< Whether the board's rules allow a via of this kind, on this net, at this place (its
    ///< diameter, hole and annular ring, a disallow rule).  Unset: anywhere.
    std::function<bool( const SKETCH_VIA_KIND&, NET_HANDLE, const VECTOR2I& )> m_viaAllowed;

    ///< PNS layer the routes should run on where they can (usually the active layer), or -1.
    int m_preferredLayer = -1;

    ///< Optional sketch path.  When set, the connections are routed as a bundle along it.
    SHAPE_LINE_CHAIN m_guide;

    ///< Half width of the corridor around the sketch path.  Zero picks one from the bundle size.
    int m_guideHalfWidth = 0;

    ///< Area the routes must stay inside (usually the board outline bounding box).
    BOX2I m_boundary;

    bool m_allowVias = true;

    ///< Track width for every route (nm); 0 takes each net's own (its net class or rules).
    int m_trackWidth = 0;

    ///< A connection with no way at its net's width may be routed this narrow (nm), or as
    ///< narrow as the board's rules allow where it runs if that is wider.  0: never narrower.
    int m_minTrackWidth = 0;

    ///< Cost of a via, as an equivalent length of track (nm).
    double m_viaCost = 3000000.0;

    ///< Cost of a corner, in grid steps.
    double m_bendCost = 2.0;

    ///< Extra cost per step when running against a layer's preferred direction (fraction of the
    ///< step length).  Even routing layers prefer horizontal runs, odd ones vertical.  Not used
    ///< for sketched bundles, which follow the sketch instead.
    double m_wrongWayCost = 0.2;

    ///< Weight of the A* heuristic.  Values above 1 trade optimality for speed.
    double m_heuristicWeight = 1.3;

    ///< Number of rip-up and reorder passes when some connections fail.
    int m_maxPasses = 3;

    ///< Total time budget in milliseconds.
    int m_timeLimitMs = 30000;

    ///< Maximum number of search states expanded per connection attempt.
    int m_maxExpansions = 2000000;

    ///< Maximum size of a search window, counted as grid cells times layers.  Larger windows are
    ///< searched on a coarser grid.
    int m_maxCells = 4000000;

    ///< Run the PNS optimizer over each finished route.
    bool m_optimize = true;

    ///< When no route leaves and enters the pads straight, let it leave and enter them any way
    bool m_freeExits = true;

    ///< Keep each net's obstacles by place (SKETCH_FIELD) instead of asking the node about every
    ///< probe.  The routes are the same; off only to compare.
    bool m_obstacleField = true;
    bool m_viaField = true;     ///< and about vias

    ///< Without a sketch, route groups of connections between the same two footprints as
    ///< bundles along a path found for the whole group.
    bool m_autoBundle = true;
};


struct SKETCH_ROUTER_STATS
{
    int              m_connections = 0;
    int              m_routed = 0;
    int              m_vias = 0;
    int              m_narrowed = 0;      ///< Connections routed narrower than their net asks
    int              m_passes = 0;
    long long        m_length = 0;
    long long        m_expansions = 0;
    long long        m_collisionChecks = 0;
    double           m_elapsedMs = 0.0;
    bool             m_cancelled = false;
    bool             m_timedOut = false;  ///< The time limit ran out before the router was done
    std::vector<int> m_failed;            ///< Indices of the connections left unrouted

    ///< Lane corridors of the last pass, for debugging
    std::vector<SHAPE_LINE_CHAIN> m_lanes;
};


/**
 * Automatic router for a group of connections, in the spirit of Xpedition's Sketch Router.
 *
 * Every connection is searched with A* over an octilinear grid on each routing layer, with via
 * transitions between layers.  The search state holds the travel direction so turns can be
 * limited to 45 degrees (or 90 degrees in orthogonal corner modes) and corners can be charged
 * for.  Obstacles are probed lazily through NODE::CheckColliding(), so the router follows every
 * clearance, hole and keepout rule the interactive router follows.
 *
 * Tracks leave pads straight out along the pad's axis and only turn once clear of the pad.
 * With a sketch path the connections form a bundle: the sketch is snapped to 45 degrees and each
 * connection gets its own lane beside it, one track pitch apart, so the bundle comes out tight,
 * stays on the preferred layer and only changes layer near its pads.
 *
 * Finished routes go through the PNS optimizer and are verified again before they are added to
 * the result branch.  Connections that fail are moved to the front of the queue and the group is
 * routed again, up to SKETCH_ROUTER_OPTIONS::m_maxPasses times.  The best pass is kept.
 */
class SKETCH_ROUTER : public ALGO_BASE
{
public:
    SKETCH_ROUTER( ROUTER* aRouter );
    ~SKETCH_ROUTER();

    SKETCH_ROUTER_OPTIONS& Options() { return m_options; }

    /**
     * Set a callback reporting progress as (connections done, connections total).  Return false
     * from it to cancel routing.  The partial result of the current pass is kept.
     */
    void SetProgressCallback( std::function<bool( int, int )> aCallback )
    {
        m_progressCallback = std::move( aCallback );
    }

    /**
     * Route \a aConnections on a branch of \a aWorld.
     *
     * @return the branch holding the new tracks and vias, or nullptr if nothing was routed.  The
     *         branch is a child of \a aWorld: commit it with ROUTER::CommitRouting() or delete it.
     */
    NODE* Route( NODE* aWorld, const std::vector<SKETCH_CONNECTION>& aConnections );

    const SKETCH_ROUTER_STATS& Stats() const { return m_stats; }

    /**
     * Collect the ratsnest connections to route for a set of board items.
     *
     * Footprints contribute their pads.  If any ratsnest edge joins two of the given items then
     * only such edges are returned (routing "between" the selected pins); otherwise every edge
     * touching one of the items is returned.
     */
    static std::vector<SKETCH_CONNECTION> CollectConnections( BOARD* aBoard, NODE* aWorld,
                                                              const std::vector<BOARD_CONNECTED_ITEM*>& aItems );

    /**
     * Return the PNS layers suitable for routing: every copper layer except those defined as
     * power planes in the board stackup.
     */
    static std::vector<int> RoutableLayers( BOARD* aBoard, ROUTER_IFACE* aIface );

    /**
     * Take the board's own conventions into \a aOptions: the layers it routes on (m_layers),
     * the inner layers it keeps as planes (m_transitLayers: a pour and next to no track) and
     * the kinds of via it uses (m_viaKinds: laser micro vias between neighbouring layers on a
     * board that has them, each at the size the board mostly uses).  A board with no via yet
     * leaves m_viaKinds empty, and each net's own through via is used.
     */
    static void BoardConventions( BOARD* aBoard, ROUTER_IFACE* aIface, SKETCH_ROUTER_OPTIONS& aOptions );

    /**
     * Return the ways a track may leave \a aItem from \a aAnchor.  Pads are left square to one
     * of their edges, preferably along the long axis of elongated pads, and never back under
     * their own footprint; round pads may also be left diagonally.  Each exit runs straight until the track is clear of the
     * pad.  Items that aren't pads return no exits, meaning any direction will do.
     *
     * @param aStrict when false, every direction is allowed, but still with a straight exit.
     */
    static std::vector<SKETCH_EXIT> PadExits( const ITEM* aItem, const VECTOR2I& aAnchor,
                                              int aWidth, bool aOrthogonal, bool aStrict );

private:
    class GRID_SEARCH;

    bool routePass( NODE* aNode, const std::vector<SKETCH_CONNECTION>& aConnections,
                    const std::vector<int>& aOrder, std::vector<uint8_t>& aStatus, double& aCost,
                    int& aVias, long long& aLength );

    bool routeConnection( NODE* aNode, const SKETCH_CONNECTION& aConn, int aBundleSize,
                          double aLaneOffset, double& aCost, int& aVias, long long& aLength,
                          bool& aDetoured, int aNarrowWidth = 0 );

    /**
     * Connections routed together along one sketch.  Every member gets a lane beside the
     * sketch, one track pitch from its neighbours.
     */
    struct BUNDLE
    {
        SHAPE_LINE_CHAIN m_guide;
        int              m_lanePitch = 0;
        std::vector<int> m_members;
    };

    ///< Give each connection of a bundle its own lane beside the sketch
    void assignLanes( const std::vector<SKETCH_CONNECTION>& aConnections, BUNDLE& aBundle );

    ///< Make a bundle's sketch and lane pitch the ones the routing functions work with
    void activateBundle( int aBundle );

    ///< Track pitch (width plus clearance) of the widest member of a bundle
    int bundlePitch( const std::vector<SKETCH_CONNECTION>& aConnections,
                     const std::vector<int>& aMembers ) const;

    /**
     * Without a sketch, find groups of connections running between the same two footprints and
     * give each group a sketch of its own: the path of one fat track as wide as the whole group.
     */
    void formBundles( NODE* aWorld, std::vector<SKETCH_CONNECTION>& aConnections );

    /**
     * Search a path for one track as wide as a whole bundle, from \a aFrom to \a aTo on the
     * bundle's layer.  The bundle's own nets are no obstacle, nor is anything within the free
     * radius of either end, where the pads and fanouts are.  The ends of the path inside those
     * radii are cut off.  With a corridor, the path stays inside it, and \a aCenterCost per
     * step at its edges keeps it close to the middle.
     *
     * @return the path, or an empty chain if there is none.
     */
    SHAPE_LINE_CHAIN searchSpine( NODE* aWorld, const std::vector<SKETCH_CONNECTION>& aConnections,
                                  const std::vector<int>& aMembers, const VECTOR2I& aFrom,
                                  const VECTOR2I& aTo, int aFreeFrom, int aFreeTo, int aPitch,
                                  const SHAPE_LINE_CHAIN* aCorridor, int aCorridorHalfWidth,
                                  double aCenterCost = 0.0 );

    ///< True if the connection runs against the direction the sketch was drawn in
    bool guideReversed( const SKETCH_CONNECTION& aConn ) const;

    ///< The sketch path (optionally offset to a lane) extended to the ends of a connection,
    ///< pointing from its start to its end
    SHAPE_LINE_CHAIN orientedCorridor( const SKETCH_CONNECTION& aConn,
                                       double aLaneOffset = 0.0 ) const;

    ///< The lane of a connection with fanouts to its pads that leave them straight
    SHAPE_LINE_CHAIN laneCorridor( const SKETCH_CONNECTION& aConn, double aLaneOffset,
                                   const std::vector<SKETCH_EXIT>& aStartExits,
                                   const std::vector<SKETCH_EXIT>& aEndExits, int& aStartRoom,
                                   int& aEndRoom ) const;

    ///< How far a connection sits toward the inside of the sketch's overall bend
    double bendInnerness( const SKETCH_CONNECTION& aConn ) const;

    bool commitPath( NODE* aNode, const SKETCH_CONNECTION& aConn, int aWidth, int aClearance,
                     const std::vector<SHAPE_LINE_CHAIN>& aRuns, const std::vector<int>& aRunLayers,
                     const std::vector<VECTOR2I>& aVias, const std::vector<VIA>& aViaProtos,
                     const std::vector<SKETCH_EXIT>& aStartExits,
                     const std::vector<SKETCH_EXIT>& aEndExits, int aHoleToHole, int& aViaCount,
                     long long& aLength );

    SHAPE_LINE_CHAIN optimizeRun( NODE* aNode, const SHAPE_LINE_CHAIN& aRaw, int aLayer, int aWidth,
                                  NET_HANDLE aNet, const std::vector<SKETCH_EXIT>* aStartExits,
                                  const std::vector<SKETCH_EXIT>* aEndExits );

    SHAPE_LINE_CHAIN roundCorners( NODE* aNode, const SHAPE_LINE_CHAIN& aChain, int aLayer,
                                   int aWidth, int aClearance, NET_HANDLE aNet, int aStartStraight,
                                   int aEndStraight );

    bool isRouteShapeValid( const SHAPE_LINE_CHAIN& aChain ) const;

    bool insideCorridor( const SHAPE_LINE_CHAIN& aChain ) const;

    bool cancelled();

    ///< The obstacle field of a net and track width, made on first use
    SKETCH_FIELD* field( NODE* aNode, NET_HANDLE aNet, int aWidth );

    ///< Tell the fields that an item is about to be added to or taken from the node
    void fieldsChanged( const ITEM* aItem );

    struct FIELD_CHANGE
    {
        BOX2I m_box;
        int   m_layerStart;
        int   m_layerEnd;
    };

    ///< Report progress from within a search; false once the user has cancelled
    bool keepGoing();

    SKETCH_ROUTER_OPTIONS            m_options;
    SKETCH_ROUTER_STATS              m_stats;

    std::map<std::pair<NET_HANDLE, int>, std::unique_ptr<SKETCH_FIELD>> m_fields;
    std::vector<FIELD_CHANGE>        m_passChanges;   ///< copper the current pass added
    int                              m_narrowed = 0;  ///< see SKETCH_ROUTER_STATS

    ///< By net, kind of via and tile: 1 the rules allow it there, 0 not, -1 it depends where
    std::map<std::tuple<NET_HANDLE, int, int, int>, int8_t> m_viaAllowedTiles;
    std::function<bool( int, int )>  m_progressCallback;
    std::vector<int>                 m_layers;
    DIRECTION_45::CORNER_MODE        m_cornerMode;
    bool                             m_orthogonal;
    int64_t                          m_deadline;

    std::vector<BUNDLE>              m_bundles;
    std::vector<int>                 m_bundleOf;      ///< Bundle of each connection, or -1

    ///< Sketch path of the active bundle, snapped to 45 (or 90) degrees; empty without one
    SHAPE_LINE_CHAIN                 m_guide;

    ///< Lane offset of each connection from its bundle's sketch, and the active lane spacing
    std::vector<double>              m_laneOffsets;
    int                              m_lanePitch;

    ///< Corridor of the guided route being committed, or nullptr
    const SHAPE_LINE_CHAIN*          m_corridor;
    int                              m_corridorHalfWidth;
    int                              m_corridorStartRoom;
    int                              m_corridorEndRoom;

    ///< Bundle members still to route beside the connection being routed, by lane offset
    bool                             m_guardPlus = false;
    bool                             m_guardMinus = false;

    ///< Progress of the current pass, for reports made from within a search
    int                              m_progressDone;
    int                              m_progressTotal;
};

}

#endif
