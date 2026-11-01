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

#include "pns_sketch_router.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>

#include <board.h>
#include <board_connected_item.h>
#include <footprint.h>
#include <pad.h>
#include <trigo.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_algo.h>
#include <ratsnest/ratsnest_data.h>
#include <geometry/shape_arc.h>

#include "pns_line.h"
#include "pns_node.h"
#include "pns_optimizer.h"
#include "pns_router.h"
#include "pns_routing_settings.h"
#include "pns_segment.h"
#include "pns_sizes_settings.h"
#include "pns_solid.h"
#include "pns_via.h"

namespace PNS {

namespace {

constexpr int   DIR_NONE = 8;
constexpr int   DIR_SLOTS = 9;
constexpr float COST_INF = std::numeric_limits<float>::max();
constexpr float SQRT2 = 1.41421356f;

/// How much more straying toward a lane still to be routed costs than straying the other way
constexpr double GUARD_COST = 4.0;

// Per connection outcome of a routing pass
constexpr uint8_t ROUTE_FAILED = 0;
constexpr uint8_t ROUTE_OK = 1;
constexpr uint8_t ROUTE_DETOURED = 2;   ///< routed, but outside the sketch corridor

// Grid directions, indexed so that d and d + 4 are opposite and odd indices are diagonals
const int DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
const int DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };


int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
}


/// Return the grid direction of an octilinear vector, or -1 if it has none.
int directionOf( const VECTOR2I& aVec )
{
    if( aVec.x == 0 && aVec.y == 0 )
        return -1;

    if( aVec.x != 0 && aVec.y != 0 && std::abs( aVec.x ) != std::abs( aVec.y ) )
        return -1;

    const int sx = ( aVec.x > 0 ) - ( aVec.x < 0 );
    const int sy = ( aVec.y > 0 ) - ( aVec.y < 0 );

    for( int d = 0; d < 8; d++ )
    {
        if( DX[d] == sx && DY[d] == sy )
            return d;
    }

    return -1;
}


/// Number of 45 degree steps between two directions (0..4).
int turnSteps( int aFrom, int aTo )
{
    int t = ( aTo - aFrom + 8 ) % 8;
    return t > 4 ? 8 - t : t;
}


/// A turn that the corner mode allows: at most 45 degrees, or exactly 90 in orthogonal modes.
bool turnAllowed( int aSteps, bool aOrthogonal )
{
    return aOrthogonal ? ( aSteps == 0 || aSteps == 2 ) : aSteps <= 1;
}


/// Length of a grid direction's unit step.
double dirLength( int aDir )
{
    return ( aDir & 1 ) ? M_SQRT2 : 1.0;
}


/**
 * Drop repeated points and points in the middle of straight runs.  Unlike
 * SHAPE_LINE_CHAIN::Simplify() this has no tolerance, so it never merges a short jog into a
 * neighbouring segment and knocks it off its 45 degree angle.  Only for chains without arcs.
 */
SHAPE_LINE_CHAIN simplifyExact( const SHAPE_LINE_CHAIN& aChain )
{
    std::vector<VECTOR2I> pts;

    for( int i = 0; i < aChain.PointCount(); i++ )
    {
        const VECTOR2I& p = aChain.CPoint( i );

        if( !pts.empty() && pts.back() == p )
            continue;

        while( pts.size() >= 2 )
        {
            const VECTOR2I& p0 = pts[pts.size() - 2];
            const VECTOR2I& p1 = pts[pts.size() - 1];
            const int64_t   ax = int64_t( p1.x ) - p0.x;
            const int64_t   ay = int64_t( p1.y ) - p0.y;
            const int64_t   bx = int64_t( p.x ) - p1.x;
            const int64_t   by = int64_t( p.y ) - p1.y;

            if( ax * by - ay * bx != 0 || ax * bx + ay * by <= 0 )
                break;

            pts.pop_back();
        }

        pts.push_back( p );
    }

    SHAPE_LINE_CHAIN out;

    for( const VECTOR2I& p : pts )
        out.Append( p );

    return out;
}


/// True if every segment of a chain is octilinear (only horizontal and vertical if orthogonal).
bool isOctilinear( const SHAPE_LINE_CHAIN& aChain, bool aOrthogonal )
{
    for( int i = 0; i < aChain.SegmentCount(); i++ )
    {
        int dir = directionOf( aChain.CSegment( i ).B - aChain.CSegment( i ).A );

        if( dir < 0 || ( aOrthogonal && ( dir & 1 ) ) )
            return false;
    }

    return true;
}


/**
 * Short octilinear paths from \a aFrom into \a aTo.
 *
 * Without exits these are the two two-segment traces of DIRECTION_45::BuildInitialTrace().  With
 * exits the path has to arrive through one of them: its last segment runs straight into \a aTo
 * against the exit direction for at least the exit length, and at most one segment leads from
 * \a aFrom to the start of that stretch.  All points stay on integer coordinates, so every
 * segment is exactly octilinear.
 *
 * @param aMinSegment shortest segment worth drawing, besides the ones exits require.
 */
std::vector<SHAPE_LINE_CHAIN> approachPaths( const VECTOR2I& aFrom, const VECTOR2I& aTo,
                                             const std::vector<SKETCH_EXIT>& aExits,
                                             bool aOrthogonal, int aMinSegment )
{
    std::vector<SHAPE_LINE_CHAIN> paths;

    auto addPath =
            [&]( SHAPE_LINE_CHAIN aPath )
            {
                aPath = simplifyExact( aPath );

                if( aPath.PointCount() < 2 || !isOctilinear( aPath, aOrthogonal ) )
                    return;

                for( const SHAPE_LINE_CHAIN& other : paths )
                {
                    if( other.CompareGeometry( aPath ) )
                        return;
                }

                paths.push_back( aPath );
            };

    if( aFrom == aTo )
    {
        if( aExits.empty() )
        {
            SHAPE_LINE_CHAIN single;
            single.Append( aTo );
            paths.push_back( single );
        }

        return paths;
    }

    if( aExits.empty() )
    {
        DIRECTION_45::CORNER_MODE mode = aOrthogonal ? DIRECTION_45::MITERED_90
                                                     : DIRECTION_45::MITERED_45;

        for( bool diagonalFirst : { false, true } )
        {
            SHAPE_LINE_CHAIN path = DIRECTION_45().BuildInitialTrace( aFrom, aTo, diagonalFirst, mode );
            bool             slivers = false;

            for( int i = 0; i < path.SegmentCount(); i++ )
                slivers |= path.CSegment( i ).Length() < aMinSegment;

            if( !slivers )
                addPath( path );
        }

        return paths;
    }

    const VECTOR2I delta = aTo - aFrom;

    for( const SKETCH_EXIT& exit : aExits )
    {
        // The path arrives at aTo heading against the exit
        const VECTOR2I e( DX[exit.m_dir], DY[exit.m_dir] );
        const double   eLen = dirLength( exit.m_dir );

        for( int d = 0; d < 8; d++ )
        {
            if( aOrthogonal && ( d & 1 ) )
                continue;

            if( !turnAllowed( turnSteps( d, ( exit.m_dir + 4 ) % 8 ), aOrthogonal ) )
                continue;

            // Solve aFrom + a * dv == aTo + s * e for the corner at a, s along each direction
            const VECTOR2I dv( DX[d], DY[d] );
            const int64_t  det = int64_t( dv.y ) * e.x - int64_t( dv.x ) * e.y;
            int64_t        a;
            int64_t        s;

            if( det == 0 )
            {
                // Parallel: only works when aFrom already sits on the exit ray
                if( int64_t( delta.x ) * e.y - int64_t( delta.y ) * e.x != 0 )
                    continue;

                a = 0;
                s = e.x != 0 ? -int64_t( delta.x ) / e.x : -int64_t( delta.y ) / e.y;
            }
            else
            {
                const int64_t aNum = int64_t( delta.y ) * e.x - int64_t( delta.x ) * e.y;
                const int64_t sNum = int64_t( delta.y ) * dv.x - int64_t( delta.x ) * dv.y;

                if( aNum % det != 0 || sNum % det != 0 )
                    continue;

                a = aNum / det;
                s = sNum / det;
            }

            if( a < 0 || s * eLen < exit.MinLength() )
                continue;

            if( a > 0 && a * dirLength( d ) < aMinSegment )
                continue;

            const VECTOR2I corner( aTo.x + s * e.x, aTo.y + s * e.y );

            SHAPE_LINE_CHAIN path;
            path.Append( aFrom );
            path.Append( corner );
            path.Append( aTo );
            addPath( path );
        }
    }

    return paths;
}


/**
 * Offset an open polyline sideways by \a aOffset, with mitered joints.  Positive offsets go to
 * the side given by the normal (-dy, dx) of each segment.
 */
SHAPE_LINE_CHAIN offsetPolyline( const SHAPE_LINE_CHAIN& aPath, double aOffset )
{
    const int             n = aPath.PointCount();
    std::vector<VECTOR2D> normals;

    for( int i = 0; i < n - 1; i++ )
    {
        VECTOR2D d( aPath.CPoint( i + 1 ) - aPath.CPoint( i ) );
        double   len = d.EuclideanNorm();

        if( len > 0 )
            normals.emplace_back( -d.y / len, d.x / len );
        else
            normals.push_back( normals.empty() ? VECTOR2D( 0, 0 ) : normals.back() );
    }

    SHAPE_LINE_CHAIN out;

    for( int j = 0; j < n; j++ )
    {
        VECTOR2D offset;

        if( normals.empty() )
        {
            offset = VECTOR2D( 0, 0 );
        }
        else if( j == 0 )
        {
            offset = normals.front();
        }
        else if( j == n - 1 )
        {
            offset = normals.back();
        }
        else
        {
            const VECTOR2D& a = normals[j - 1];
            const VECTOR2D& b = normals[j];
            double          denom = 1.0 + a.Dot( b );

            // Very sharp turns would push the miter far out; clamp it
            offset = ( a + b ) / std::max( denom, 0.25 );
        }

        out.Append( KiROUND( VECTOR2D( aPath.CPoint( j ) ) + offset * aOffset ) );
    }

    return out;
}


/// Grid direction closest to a vector.
int nearestDirection( const VECTOR2D& aVec )
{
    int    best = -1;
    double bestDot = -2.0;

    if( aVec.EuclideanNorm() == 0 )
        return -1;

    const VECTOR2D unit = aVec.Resize( 1.0 );

    for( int d = 0; d < 8; d++ )
    {
        double dot = unit.Dot( VECTOR2D( DX[d], DY[d] ).Resize( 1.0 ) );

        if( dot > bestDot )
        {
            bestDot = dot;
            best = d;
        }
    }

    return best;
}


/**
 * Turn a sketch into a 45 (or 90) degree path.  Freehand input is reduced to its main vertices
 * first, then consecutive vertices are joined with two segment traces, keeping the direction
 * the previous stretch was heading in where there is a choice.
 */
SHAPE_LINE_CHAIN octilinearGuide( const SHAPE_LINE_CHAIN& aGuide, bool aOrthogonal, int aTolerance )
{
    SHAPE_LINE_CHAIN simple = aGuide;
    simple.Simplify( aTolerance );

    if( simple.PointCount() < 2 )
        return simple;

    DIRECTION_45::CORNER_MODE mode = aOrthogonal ? DIRECTION_45::MITERED_90 : DIRECTION_45::MITERED_45;
    SHAPE_LINE_CHAIN          out;
    int                       lastDir = -1;

    out.Append( simple.CPoint( 0 ) );

    for( int i = 1; i < simple.PointCount(); i++ )
    {
        const VECTOR2I   a = out.CLastPoint();
        const VECTOR2I   b = simple.CPoint( i );
        SHAPE_LINE_CHAIN pick;

        if( a == b )
            continue;

        for( bool diagonalFirst : { false, true } )
        {
            SHAPE_LINE_CHAIN path = DIRECTION_45().BuildInitialTrace( a, b, diagonalFirst, mode );

            if( pick.PointCount() == 0 )
                pick = path;

            const SEG first = path.CSegment( 0 );

            if( lastDir >= 0 && nearestDirection( VECTOR2D( first.B - first.A ) ) == lastDir )
                pick = path;
        }

        for( int j = 1; j < pick.PointCount(); j++ )
            out.Append( pick.CPoint( j ) );

        const SEG last = pick.CSegment( -1 );
        lastDir = nearestDirection( VECTOR2D( last.B - last.A ) );
    }

    return simplifyExact( out );
}


/**
 * Cut the parts of a sketch that a bundle \a aMinGap wide can't follow: hairpins and small loops
 * where the sketch comes back closer to itself than the bundle is wide, and wiggles smaller than
 * the bundle.  Big loops are left alone even if they come close to themselves; going round those
 * is what the sketch asks for.
 */
SHAPE_LINE_CHAIN cutTightLoops( const SHAPE_LINE_CHAIN& aGuide, int aMinGap )
{
    const double total = aGuide.Length();

    if( aGuide.PointCount() < 3 || aMinGap <= 0 || total <= 0.0 )
        return aGuide;

    // Evenly spaced points, with the distance along the sketch to each
    const double          step = std::max( aMinGap / 4.0, total / 600.0 );
    std::vector<VECTOR2I> pts;
    std::vector<double>   along;
    double                done = 0.0;

    for( int i = 0; i < aGuide.SegmentCount(); i++ )
    {
        const SEG    seg = aGuide.CSegment( i );
        const double len = seg.Length();

        for( double t = pts.empty() ? 0.0 : along.back() + step - done; t < len; t += step )
        {
            pts.push_back( seg.A + KiROUND( VECTOR2D( seg.B - seg.A ) * ( t / len ) ) );
            along.push_back( done + t );
        }

        done += len;
    }

    pts.push_back( aGuide.CLastPoint() );
    along.push_back( total );

    // Only stretches up to this long are cut; a longer stretch is a loop the bundle can go round
    const double     maxCut = 4.0 * aMinGap;
    const int        n = (int) pts.size();
    SHAPE_LINE_CHAIN out;

    out.Append( pts[0] );

    for( int i = 0; i < n - 1; )
    {
        int next = i + 1;

        for( int j = n - 1; j > i + 1; j-- )
        {
            const double stretch = along[j] - along[i];

            if( stretch > maxCut )
                continue;

            const double gap = ( pts[j] - pts[i] ).EuclideanNorm();

            // Twice as far round as across: the sketch doubled back on itself
            if( gap < aMinGap && stretch > 2.0 * gap + step )
            {
                next = j;
                break;
            }
        }

        out.Append( pts[next] );
        i = next;
    }

    return out;
}


/**
 * An octilinear path from \a aFrom, heading off along \a aDir, to \a aTo, arriving in a
 * direction that turns onto \a aArriveDir within the corner mode's limits.  Uses at most three
 * segments and never turns more than the corner mode allows.  For corridors only: the points are
 * rounded, so the segments may be a hair off their angles.
 *
 * @return an empty chain when no such path exists.
 */
SHAPE_LINE_CHAIN directedPath( const VECTOR2I& aFrom, int aDir, const VECTOR2I& aTo, int aArriveDir,
                               bool aOrthogonal )
{
    const VECTOR2D delta( aTo - aFrom );
    SHAPE_LINE_CHAIN best;
    int              bestSegments = std::numeric_limits<int>::max();
    double           bestLength = std::numeric_limits<double>::max();

    auto unit =
            []( int aD )
            {
                return VECTOR2D( DX[aD], DY[aD] ).Resize( 1.0 );
            };

    auto arrives =
            [&]( int aD )
            {
                return aD == aArriveDir || turnAllowed( turnSteps( aD, aArriveDir ), aOrthogonal );
            };

    // Solve aU * a + aV * b == aDelta
    auto solve =
            []( const VECTOR2D& aU, const VECTOR2D& aV, const VECTOR2D& aDelta, double& aA, double& aB )
            {
                const double det = aU.x * aV.y - aU.y * aV.x;

                if( std::abs( det ) < 1e-9 )
                    return false;

                aA = ( aDelta.x * aV.y - aDelta.y * aV.x ) / det;
                aB = ( aU.x * aDelta.y - aU.y * aDelta.x ) / det;
                return true;
            };

    auto consider =
            [&]( const std::vector<VECTOR2D>& aPoints )
            {
                double length = 0.0;

                for( size_t i = 1; i < aPoints.size(); i++ )
                    length += ( aPoints[i] - aPoints[i - 1] ).EuclideanNorm();

                const int segments = (int) aPoints.size() - 1;

                if( segments < bestSegments || ( segments == bestSegments && length < bestLength ) )
                {
                    best.Clear();

                    for( const VECTOR2D& pt : aPoints )
                        best.Append( KiROUND( pt ) );

                    bestSegments = segments;
                    bestLength = length;
                }
            };

    if( delta.EuclideanNorm() == 0 )
        return best;

    const VECTOR2D from( aFrom );
    const VECTOR2D u0 = unit( aDir );

    // Straight on
    if( std::abs( u0.Cross( delta ) ) < 1.0 && u0.Dot( delta ) > 0 && arrives( aDir ) )
        consider( { from, VECTOR2D( aTo ) } );

    for( int d1 = 0; d1 < 8; d1++ )
    {
        if( d1 == aDir || ( aOrthogonal && ( d1 & 1 ) )
                || !turnAllowed( turnSteps( aDir, d1 ), aOrthogonal ) )
        {
            continue;
        }

        const VECTOR2D u1 = unit( d1 );
        double         l0, l1;

        // One turn
        if( arrives( d1 ) && solve( u0, u1, delta, l0, l1 ) && l0 >= 0 && l1 > 0 )
            consider( { from, from + u0 * l0, VECTOR2D( aTo ) } );

        // Two turns, the middle segment cutting the corner between the outer two
        for( int d2 = 0; d2 < 8; d2++ )
        {
            if( d2 == d1 || ( aOrthogonal && ( d2 & 1 ) ) || !arrives( d2 )
                    || !turnAllowed( turnSteps( d1, d2 ), aOrthogonal ) )
            {
                continue;
            }

            const VECTOR2D u2 = unit( d2 );
            double         a, b, ca, cb;

            if( !solve( u0, u2, delta, a, b ) || a < 0 || b <= 0 || !solve( u0, u2, u1, ca, cb ) )
                continue;

            // Give the middle segment half the length it could take
            double cMax = std::numeric_limits<double>::max();

            if( ca > 1e-9 )
                cMax = std::min( cMax, a / ca );

            if( cb > 1e-9 )
                cMax = std::min( cMax, b / cb );

            if( cMax == std::numeric_limits<double>::max() || cMax <= 0 )
                continue;

            const double   c = cMax / 2.0;
            const VECTOR2D p1 = from + u0 * ( a - c * ca );
            const VECTOR2D p2 = p1 + u1 * c;

            consider( { from, p1, p2, VECTOR2D( aTo ) } );
        }
    }

    return best;
}


struct OPEN_ENTRY
{
    float   f;
    float   g;
    int32_t rec;
};


struct OPEN_CMP
{
    bool operator()( const OPEN_ENTRY& aA, const OPEN_ENTRY& aB ) const
    {
        if( aA.f != aB.f )
            return aA.f > aB.f;

        // Among equal estimates, go deeper first.  Octilinear grids have many equivalent paths
        // and this keeps the search from widening across all of them.
        return aA.g < aB.g;
    }
};


/**
 * Temporarily switch the router corner mode to its mitered equivalent.  The optimizer builds
 * its shortcuts using the router settings, and we want those without arcs; corners are rounded
 * afterwards when requested.
 */
class MITERED_CORNERS_GUARD
{
public:
    MITERED_CORNERS_GUARD( ROUTER* aRouter, bool aOrthogonal )
    {
        DIRECTION_45::CORNER_MODE mode = aOrthogonal ? DIRECTION_45::MITERED_90
                                                     : DIRECTION_45::MITERED_45;
        // PNS algorithms read their settings through the router singleton
        for( ROUTER* router : { aRouter, ROUTER::GetInstance() } )
        {
            if( !router || std::find( m_routers.begin(), m_routers.end(), router ) != m_routers.end() )
                continue;

            m_routers.push_back( router );
            m_modes.push_back( router->Settings().GetCornerMode() );
            router->Settings().SetCornerMode( mode );
        }
    }

    ~MITERED_CORNERS_GUARD()
    {
        for( size_t i = 0; i < m_routers.size(); i++ )
            m_routers[i]->Settings().SetCornerMode( m_modes[i] );
    }

private:
    std::vector<ROUTER*>                   m_routers;
    std::vector<DIRECTION_45::CORNER_MODE> m_modes;
};


/// Check that a route leaves and enters its pads through one of their exits.
bool exitsRespected( const SHAPE_LINE_CHAIN& aChain, const std::vector<SKETCH_EXIT>* aStartExits,
                     const std::vector<SKETCH_EXIT>* aEndExits )
{
    auto check =
            [&]( const SEG& aSeg, const std::vector<SKETCH_EXIT>* aExits )
            {
                if( !aExits || aExits->empty() )
                    return true;

                const int dir = directionOf( aSeg.B - aSeg.A );

                for( const SKETCH_EXIT& exit : *aExits )
                {
                    if( exit.m_dir == dir && aSeg.Length() >= exit.MinLength() - 1 )
                        return true;
                }

                return false;
            };

    if( aChain.SegmentCount() < 1 )
        return false;

    const SEG first = aChain.CSegment( 0 );
    const SEG last = aChain.CSegment( -1 );

    return check( first, aStartExits ) && check( SEG( last.B, last.A ), aEndExits );
}

/**
 * Check a via's hole against every other hole, whatever their nets, without the clearance
 * epsilon the router usually allows.  DRC measures hole to hole spacing exactly.
 */
bool viaHolesClear( NODE* aNode, const VIA& aVia, int aHoleToHole )
{
    if( aHoleToHole <= 0 || !aVia.Hole() )
        return true;

    COLLISION_SEARCH_OPTIONS opts;
    opts.m_differentNetsOnly = false;
    opts.SetFixedClearanceOverride( aHoleToHole );
    opts.m_limitCount = 1;
    opts.m_kindMask = ITEM::HOLE_T;
    opts.m_useClearanceEpsilon = false;

    return !aNode->CheckColliding( aVia.Hole(), opts );
}

} // anonymous namespace


/**
 * A* search for one connection over a window of the octilinear routing grid.
 *
 * The grid origin is the start anchor, so the route leaves it on a grid line.  With exits it
 * first runs straight out of the pad along one of them.  The end anchor is usually off-grid; it
 * is reached from nearby grid points with a short octilinear approach (see approachPaths()).
 */
class SKETCH_ROUTER::GRID_SEARCH
{
public:
    struct PARAMS
    {
        NODE*                    m_node = nullptr;
        NET_HANDLE               m_net = nullptr;
        int                      m_width = 0;
        int                      m_pitch = 0;
        const VIA*               m_viaProto = nullptr;
        std::vector<int>         m_layers;           ///< PNS layer of each local layer
        std::vector<bool>        m_startLayers;      ///< Local layers the start may be left on
        std::vector<bool>        m_endLayers;        ///< Local layers the end may be reached on
        std::vector<SKETCH_EXIT> m_startExits;       ///< Empty: leave the start any way
        std::vector<SKETCH_EXIT> m_endExits;         ///< Empty: reach the end any way
        VECTOR2I                 m_start;
        VECTOR2I                 m_end;
        int                      m_endRadius = 0;
        int                      m_minViaSpacing = 0;    ///< between vias of one route
        int                      m_holeToHole = 0;       ///< minimum spacing between holes
        int                      m_viaZoneRadius = 0;    ///< vias only this close to an end (0: anywhere)
        int                      m_preferredLayer = -1;  ///< local layer index, or -1
        double                   m_offLayerCost = 0.0;   ///< extra cost per step off that layer
        BOX2I                    m_window;
        const SHAPE_LINE_CHAIN*  m_corridor = nullptr;
        int                      m_corridorHalfWidth = 0;
        double                   m_centerCost = 0.0;     ///< per step, at the corridor's edge
        bool                     m_followOrder = false;  ///< go along the corridor, not across it

        ///< Sides of the corridor (left and right of its direction) with neighbours still to
        ///< route.  Straying toward them costs more, so that routes don't creep into the next
        ///< lane one after another and squeeze the last one out.
        bool                     m_guardLeft = false;
        bool                     m_guardRight = false;

        ///< Edges this close to the start (or end) aren't checked for collisions.  Used when
        ///< searching a bundle's path, whose ends lie among the bundle's own pads.
        int                      m_freeRadiusStart = 0;
        int                      m_freeRadiusEnd = 0;

        ///< Room around the start and end the corridor always allows (0: just the end radius)
        int                      m_corridorStartRoom = 0;
        int                      m_corridorEndRoom = 0;

        ///< Items on these nets are not obstacles
        const std::set<NET_HANDLE>* m_ignoreNets = nullptr;
        bool                     m_orthogonal = false;
        bool                     m_allowVias = true;
        double                   m_viaCost = 20.0;   ///< in grid steps
        double                   m_bendCost = 2.0;   ///< in grid steps
        double                   m_wrongWayCost = 0.2;
        double                   m_heuristicWeight = 1.3;
        int                      m_maxExpansions = 1000000;
        int64_t                  m_deadline = 0;

        ///< Polled during the search; returning false abandons it
        std::function<bool()>    m_keepGoing;
    };

    struct RESULT
    {
        std::vector<SHAPE_LINE_CHAIN> m_runs;
        std::vector<int>              m_runLayers;
        std::vector<VECTOR2I>         m_vias;
        double                        m_cost = 0.0;
    };

    enum class STATUS
    {
        FOUND,
        UNREACHABLE,   ///< search space exhausted
        BUDGET         ///< ran out of expansions or time
    };

    GRID_SEARCH( const PARAMS& aParams, SKETCH_ROUTER_STATS& aStats ) :
            m_p( aParams ),
            m_stats( aStats )
    {
        const int     p = m_p.m_pitch;
        const BOX2I&  w = m_p.m_window;

        const int ixMin = (int) std::floor( double( w.GetLeft() - m_p.m_start.x ) / p );
        const int iyMin = (int) std::floor( double( w.GetTop() - m_p.m_start.y ) / p );
        const int ixMax = (int) std::ceil( double( w.GetRight() - m_p.m_start.x ) / p );
        const int iyMax = (int) std::ceil( double( w.GetBottom() - m_p.m_start.y ) / p );

        m_origin = VECTOR2I( m_p.m_start.x + ixMin * p, m_p.m_start.y + iyMin * p );
        m_w = std::max( 1, ixMax - ixMin + 1 );
        m_h = std::max( 1, iyMax - iyMin + 1 );
        m_startIx = -ixMin;
        m_startIy = -iyMin;
        m_nLayers = (int) m_p.m_layers.size();

        m_tx = double( m_p.m_end.x - m_origin.x ) / p;
        m_ty = double( m_p.m_end.y - m_origin.y ) / p;

        const size_t cells = size_t( m_w ) * m_h;

        m_head.assign( cells * m_nLayers, -1 );
        m_edges.assign( cells * m_nLayers, 0 );
        m_viaState.assign( cells, 0 );

        if( m_p.m_corridor )
            buildCorridor();
    }

    static size_t CellCount( const BOX2I& aWindow, int aPitch )
    {
        return size_t( aWindow.GetWidth() / aPitch + 2 ) * size_t( aWindow.GetHeight() / aPitch + 2 );
    }

    STATUS Run( RESULT& aResult )
    {
        std::priority_queue<OPEN_ENTRY, std::vector<OPEN_ENTRY>, OPEN_CMP> open;

        seedStart( open );

        float   bestGoal = COST_INF;
        int32_t bestGoalRec = -1;
        int     bestApproach = -1;
        int     expansions = 0;
        STATUS  status = STATUS::UNREACHABLE;

        while( !open.empty() )
        {
            OPEN_ENTRY e = open.top();
            open.pop();

            if( e.rec < 0 )
            {
                status = STATUS::FOUND;
                break;
            }

            if( m_recs[e.rec].closed || e.g > m_recs[e.rec].g )
                continue;

            m_recs[e.rec].closed = true;

            if( ++expansions > m_p.m_maxExpansions
                    || ( ( expansions & 0x3FF ) == 0 && nowMs() > m_p.m_deadline )
                    || ( ( expansions & 0x3FFF ) == 0 && m_p.m_keepGoing && !m_p.m_keepGoing() ) )
            {
                status = STATUS::BUDGET;
                break;
            }

            const REC  cur = m_recs[e.rec];
            const int  ix = cur.ix;
            const int  iy = cur.iy;
            const int  k = cur.layer;
            const int  d = cur.dir;

            // Can we finish from here?
            if( m_p.m_endLayers[k] && isGoalCandidate( ix, iy ) )
            {
                const std::vector<APPROACH>& approaches = goalApproaches( ix, iy, k );

                for( int i = 0; i < (int) approaches.size(); i++ )
                {
                    const APPROACH& app = approaches[i];
                    float           turnCost = 0.0f;

                    if( d != DIR_NONE && app.m_firstDir >= 0 )
                    {
                        int t = turnSteps( d, app.m_firstDir );

                        if( !turnAllowed( t, m_p.m_orthogonal ) )
                            continue;

                        if( t )
                            turnCost = (float) m_p.m_bendCost;
                    }

                    float total = cur.g + app.m_cost + turnCost;

                    if( total < bestGoal )
                    {
                        bestGoal = total;
                        bestGoalRec = e.rec;
                        bestApproach = i;
                        open.push( { total, total, -1 } );
                    }
                }
            }

            // Moves along the current layer
            for( int nd = 0; nd < 8; nd++ )
            {
                if( m_p.m_orthogonal && ( nd & 1 ) )
                    continue;

                int t = 0;

                if( d != DIR_NONE )
                {
                    t = turnSteps( d, nd );

                    if( !turnAllowed( t, m_p.m_orthogonal ) )
                        continue;
                }

                const int nx = ix + DX[nd];
                const int ny = iy + DY[nd];

                if( nx < 0 || ny < 0 || nx >= m_w || ny >= m_h )
                {
                    m_hitWindowEdge = true;
                    continue;
                }

                if( !m_allowed.empty() && !m_allowed[size_t( ny ) * m_w + nx] )
                    continue;

                // Don't skip from one stretch of the corridor to a much later one where it
                // loops back close to itself
                if( !m_along.empty() )
                {
                    const float from = m_along[size_t( iy ) * m_w + ix];
                    const float to = m_along[size_t( ny ) * m_w + nx];

                    if( from >= 0 && to >= 0 && std::abs( to - from ) > m_maxAlongJump )
                        continue;
                }

                if( !edgeFree( ix, iy, nd, k ) )
                    continue;

                float ng = cur.g + stepCost( k, nd );

                // Keep to the middle of the corridor, which is the track's lane in a bundle
                if( !m_offCenter.empty() )
                    ng += float( m_p.m_centerCost * m_offCenter[size_t( ny ) * m_w + nx] / 255.0 )
                          * ( ( nd & 1 ) ? SQRT2 : 1.0f );

                if( !m_guard.empty() )
                    ng += float( GUARD_COST * m_p.m_centerCost * m_guard[size_t( ny ) * m_w + nx] / 255.0 )
                          * ( ( nd & 1 ) ? SQRT2 : 1.0f );

                if( t )
                    ng += (float) m_p.m_bendCost;

                relax( nx, ny, k, nd, ng, e.rec, cur.lastVia, open );
            }

            // Layer changes.  A via may not follow another via or start the route, and it has to
            // keep clear of the previous via on the path, which isn't in the world yet.
            if( m_p.m_allowVias && m_nLayers > 1 && d != DIR_NONE && inViaZone( ix, iy )
                    && viaSpaced( cur ) && viaFree( ix, iy ) )
            {
                float   ng = cur.g + (float) m_p.m_viaCost;
                int32_t cell = (int32_t) cellIndex( ix, iy );

                for( int k2 = 0; k2 < m_nLayers; k2++ )
                {
                    if( k2 != k )
                        relax( ix, iy, k2, DIR_NONE, ng, e.rec, cell, open );
                }
            }
        }

        m_stats.m_expansions += expansions;

        // Out of budget with a route already in hand: it may not be the best, but it is a route
        if( status == STATUS::BUDGET && bestGoalRec >= 0 )
            status = STATUS::FOUND;

        if( status != STATUS::FOUND || bestGoalRec < 0 )
            return status == STATUS::FOUND ? STATUS::UNREACHABLE : status;

        const REC goal = m_recs[bestGoalRec];
        buildResult( bestGoalRec, goalApproaches( goal.ix, goal.iy, goal.layer )[bestApproach],
                     aResult );
        aResult.m_cost = bestGoal;
        return STATUS::FOUND;
    }

    /// True if the search ran into the edge of its window, so a larger window might help.
    bool HitWindowEdge() const { return m_hitWindowEdge; }

    /// Check that the end can be reached at all, so hopeless searches fail immediately.
    bool EndReachable()
    {
        const int r = m_p.m_endRadius / m_p.m_pitch + 1;
        const int cx = (int) std::lround( m_tx );
        const int cy = (int) std::lround( m_ty );

        for( int k = 0; k < m_nLayers; k++ )
        {
            if( !m_p.m_endLayers[k] )
                continue;

            for( int iy = std::max( 0, cy - r ); iy <= std::min( m_h - 1, cy + r ); iy++ )
            {
                for( int ix = std::max( 0, cx - r ); ix <= std::min( m_w - 1, cx + r ); ix++ )
                {
                    if( isGoalCandidate( ix, iy ) && !goalApproaches( ix, iy, k ).empty() )
                        return true;
                }
            }
        }

        return false;
    }

private:
    struct REC
    {
        int32_t ix;
        int32_t iy;
        float   g;
        int32_t parent;
        int32_t lastVia;    ///< cell of the latest via on the best path here, or -1
        uint8_t layer;
        uint8_t dir;
        bool    closed;
    };

    struct APPROACH
    {
        SHAPE_LINE_CHAIN m_path;
        int              m_firstDir;
        float            m_cost;
    };

    size_t cellIndex( int aIx, int aIy ) const { return size_t( aIy ) * m_w + aIx; }

    VECTOR2I cellPos( int aIx, int aIy ) const
    {
        return VECTOR2I( m_origin.x + aIx * m_p.m_pitch, m_origin.y + aIy * m_p.m_pitch );
    }

    float stepCost( int aLayer, int aDir ) const
    {
        float cost = ( aDir & 1 ) ? SQRT2 : 1.0f;
        float extra = 0.0f;

        if( m_p.m_preferredLayer >= 0 && aLayer != m_p.m_preferredLayer )
            extra += (float) m_p.m_offLayerCost;

        if( m_nLayers >= 2 && m_p.m_wrongWayCost > 0.0 )
        {
            const bool horizontalLayer = ( aLayer % 2 ) == 0;

            if( aDir & 1 )
                extra += float( m_p.m_wrongWayCost * 0.5 );
            else if( ( aDir == 0 || aDir == 4 ) != horizontalLayer )
                extra += (float) m_p.m_wrongWayCost;
        }

        return cost * ( 1.0f + extra );
    }

    float heuristic( int aIx, int aIy, int aLayer ) const
    {
        const double dx = std::abs( aIx - m_tx );
        const double dy = std::abs( aIy - m_ty );
        double       h;

        if( m_p.m_orthogonal )
            h = dx + dy;
        else
            h = std::max( dx, dy ) + ( SQRT2 - 1.0 ) * std::min( dx, dy );

        if( !m_p.m_endLayers[aLayer] )
            h += m_p.m_viaCost;

        return float( h * m_p.m_heuristicWeight );
    }

    int32_t rec( int aIx, int aIy, int aLayer, int aDir )
    {
        const size_t cl = cellIndex( aIx, aIy ) * m_nLayers + aLayer;

        if( m_head[cl] < 0 )
        {
            m_head[cl] = (int32_t) ( m_blocks.size() / DIR_SLOTS );
            m_blocks.resize( m_blocks.size() + DIR_SLOTS, -1 );
        }

        const size_t slot = size_t( m_head[cl] ) * DIR_SLOTS + aDir;

        if( m_blocks[slot] < 0 )
        {
            m_blocks[slot] = (int32_t) m_recs.size();
            m_recs.push_back( { aIx, aIy, COST_INF, -1, -1, (uint8_t) aLayer, (uint8_t) aDir, false } );
        }

        return m_blocks[slot];
    }

    void relax( int aIx, int aIy, int aLayer, int aDir, float aG, int32_t aParent, int32_t aLastVia,
                std::priority_queue<OPEN_ENTRY, std::vector<OPEN_ENTRY>, OPEN_CMP>& aOpen )
    {
        int32_t idx = rec( aIx, aIy, aLayer, aDir );
        REC&    r = m_recs[idx];

        if( r.closed || aG >= r.g )
            return;

        r.g = aG;
        r.parent = aParent;
        r.lastVia = aLastVia;
        aOpen.push( { aG + heuristic( aIx, aIy, aLayer ), aG, idx } );
    }

    /**
     * Put the start states in the queue.  Without exits the route may leave the start in any
     * direction; with exits it first runs straight out along one of them, far enough to clear
     * the pad, and the search continues from the end of that stretch.
     */
    void seedStart( std::priority_queue<OPEN_ENTRY, std::vector<OPEN_ENTRY>, OPEN_CMP>& aOpen )
    {
        for( int k = 0; k < m_nLayers; k++ )
        {
            if( !m_p.m_startLayers[k] )
                continue;

            if( m_p.m_startExits.empty() )
            {
                relax( m_startIx, m_startIy, k, DIR_NONE, 0.0f, -1, -1, aOpen );
                continue;
            }

            for( const SKETCH_EXIT& exit : m_p.m_startExits )
            {
                const int d = exit.m_dir;

                if( m_p.m_orthogonal && ( d & 1 ) )
                    continue;

                const int steps = std::max( 1, (int) std::ceil( exit.m_length / ( dirLength( d ) * m_p.m_pitch ) ) );
                int       ix = m_startIx;
                int       iy = m_startIy;
                bool      ok = true;
                float     g = float( double( exit.m_penalty ) / m_p.m_pitch );

                for( int i = 0; i < steps && ok; i++ )
                {
                    const int nx = ix + DX[d];
                    const int ny = iy + DY[d];

                    ok = nx >= 0 && ny >= 0 && nx < m_w && ny < m_h && edgeFree( ix, iy, d, k );
                    g += stepCost( k, d );
                    ix = nx;
                    iy = ny;
                }

                if( ok )
                    relax( ix, iy, k, d, g, -1, -1, aOpen );
            }
        }
    }

    bool inFreeZone( const VECTOR2I& aPt ) const
    {
        const VECTOR2D pt( aPt );

        return ( m_p.m_freeRadiusStart > 0
                 && ( pt - VECTOR2D( m_p.m_start ) ).EuclideanNorm() <= m_p.m_freeRadiusStart )
               || ( m_p.m_freeRadiusEnd > 0
                    && ( pt - VECTOR2D( m_p.m_end ) ).EuclideanNorm() <= m_p.m_freeRadiusEnd );
    }

    bool segmentFree( const VECTOR2I& aA, const VECTOR2I& aB, int aLayer )
    {
        if( inFreeZone( ( aA + aB ) / 2 ) )
            return true;

        SEGMENT seg( SEG( aA, aB ), m_p.m_net );
        seg.SetWidth( m_p.m_width );
        seg.SetLayer( m_p.m_layers[aLayer] );

        m_stats.m_collisionChecks++;

        if( m_p.m_ignoreNets )
        {
            COLLISION_SEARCH_OPTIONS    opts;
            const std::set<NET_HANDLE>* nets = m_p.m_ignoreNets;

            opts.m_limitCount = 1;
            opts.m_filter = [nets]( const ITEM* aCandidate, const ITEM* )
                            {
                                return !nets->count( aCandidate->Net() );
                            };

            return !m_p.m_node->CheckColliding( &seg, opts );
        }

        return !m_p.m_node->CheckColliding( &seg );
    }

    bool edgeFree( int aIx, int aIy, int aDir, int aLayer )
    {
        // Each edge is cached once, on the cell it leaves in directions 0..3
        if( aDir >= 4 )
        {
            aIx += DX[aDir];
            aIy += DY[aDir];
            aDir -= 4;
        }

        uint8_t&  state = m_edges[cellIndex( aIx, aIy ) * m_nLayers + aLayer];
        const int shift = aDir * 2;
        const int known = ( state >> shift ) & 3;

        if( known )
            return known == 1;

        const VECTOR2I a = cellPos( aIx, aIy );
        const VECTOR2I b = cellPos( aIx + DX[aDir], aIy + DY[aDir] );
        const bool     free = segmentFree( a, b, aLayer );

        state |= ( free ? 1 : 2 ) << shift;
        return free;
    }

    bool inViaZone( int aIx, int aIy ) const
    {
        if( m_p.m_viaZoneRadius <= 0 )
            return true;

        const double   r2 = double( m_p.m_viaZoneRadius ) * m_p.m_viaZoneRadius;
        const VECTOR2D pos( cellPos( aIx, aIy ) );

        return ( pos - VECTOR2D( m_p.m_start ) ).SquaredEuclideanNorm() <= r2
               || ( pos - VECTOR2D( m_p.m_end ) ).SquaredEuclideanNorm() <= r2;
    }

    bool viaSpaced( const REC& aRec ) const
    {
        if( aRec.lastVia < 0 )
            return true;

        const double dx = double( aRec.lastVia % m_w - aRec.ix ) * m_p.m_pitch;
        const double dy = double( aRec.lastVia / m_w - aRec.iy ) * m_p.m_pitch;

        return dx * dx + dy * dy >= double( m_p.m_minViaSpacing ) * m_p.m_minViaSpacing;
    }

    bool viaFree( int aIx, int aIy )
    {
        uint8_t& state = m_viaState[cellIndex( aIx, aIy )];

        if( state )
            return state == 1;

        VIA via( *m_p.m_viaProto );
        via.SetPos( cellPos( aIx, aIy ) );

        bool free = !m_p.m_node->CheckColliding( &via );

        if( free )
        {
            // Keep vias off pads and other vias, including those of our own net
            COLLISION_SEARCH_OPTIONS opts;
            opts.m_differentNetsOnly = false;
            opts.SetFixedClearanceOverride( 0 );
            opts.m_limitCount = 1;
            opts.m_kindMask = ITEM::SOLID_T | ITEM::VIA_T;

            NET_HANDLE net = m_p.m_net;
            opts.m_filter = [net]( const ITEM* aCandidate, const ITEM* )
                            {
                                return aCandidate->Net() == net;
                            };

            free = !m_p.m_node->CheckColliding( &via, opts );
        }

        if( free )
            free = viaHolesClear( m_p.m_node, via, m_p.m_holeToHole );

        m_stats.m_collisionChecks++;
        state = free ? 1 : 2;
        return free;
    }

    bool isGoalCandidate( int aIx, int aIy ) const
    {
        const double dx = ( aIx - m_tx ) * m_p.m_pitch;
        const double dy = ( aIy - m_ty ) * m_p.m_pitch;
        const double r = m_p.m_endRadius;

        return dx * dx + dy * dy <= r * r;
    }

    const std::vector<APPROACH>& goalApproaches( int aIx, int aIy, int aLayer )
    {
        const int64_t key = int64_t( cellIndex( aIx, aIy ) ) * m_nLayers + aLayer;
        auto          it = m_approaches.find( key );

        if( it != m_approaches.end() )
            return it->second;

        std::vector<APPROACH>& approaches = m_approaches[key];

        // Anything shorter than the track is wide reads as a jog
        for( const SHAPE_LINE_CHAIN& path : approachPaths( cellPos( aIx, aIy ), m_p.m_end,
                                                           m_p.m_endExits, m_p.m_orthogonal,
                                                           inFreeZone( m_p.m_end )
                                                                   ? 0
                                                                   : std::max( m_p.m_pitch / 4, m_p.m_width ) ) )
        {
            if( path.PointCount() >= 2 && !inFreeZone( m_p.m_end ) )
            {
                LINE line;
                line.SetShape( path );
                line.SetWidth( m_p.m_width );
                line.SetLayer( m_p.m_layers[aLayer] );
                line.SetNet( m_p.m_net );

                m_stats.m_collisionChecks++;

                if( m_p.m_node->CheckColliding( &line ) )
                    continue;
            }

            APPROACH app;
            app.m_path = path;
            app.m_firstDir = path.PointCount() >= 2 ? directionOf( path.CPoint( 1 ) - path.CPoint( 0 ) )
                                                    : -1;
            app.m_cost = float( path.Length() / m_p.m_pitch
                                + std::max( 0, path.SegmentCount() - 1 ) * m_p.m_bendCost );

            if( path.SegmentCount() >= 1 )
            {
                const SEG last = path.CSegment( -1 );
                const int out = directionOf( last.A - last.B );

                for( const SKETCH_EXIT& exit : m_p.m_endExits )
                {
                    if( exit.m_dir == out )
                        app.m_cost += float( double( exit.m_penalty ) / m_p.m_pitch );
                }
            }
            approaches.push_back( app );
        }

        return approaches;
    }

    void buildCorridor()
    {
        const SHAPE_LINE_CHAIN& path = *m_p.m_corridor;
        const int               hw = m_p.m_corridorHalfWidth;
        const int               p = m_p.m_pitch;

        m_allowed.assign( size_t( m_w ) * m_h, 0 );

        if( m_p.m_centerCost > 0.0 )
            m_offCenter.assign( size_t( m_w ) * m_h, 255 );

        // For each cell, how far along the corridor the closest point of its middle is.  Across
        // the inside of a corner that jumps by up to about twice the half width.
        std::vector<float> nearest;

        if( m_p.m_followOrder )
        {
            m_along.assign( size_t( m_w ) * m_h, -1.0f );
            m_maxAlongJump = float( 2 * hw + 2 * p );
        }

        const bool guarded = m_p.m_centerCost > 0.0 && ( m_p.m_guardLeft || m_p.m_guardRight );

        if( guarded )
            m_guard.assign( size_t( m_w ) * m_h, 0 );

        if( m_p.m_followOrder || guarded )
            nearest.assign( size_t( m_w ) * m_h, std::numeric_limits<float>::max() );

        double segAlong = 0.0;

        auto markSeg =
                [&]( const SEG& aSeg, int aHalfWidth, bool aCenterLine )
                {
                    BOX2I bb( aSeg.A, VECTOR2I( 0, 0 ) );
                    bb.Merge( aSeg.B );
                    bb.Inflate( aHalfWidth );

                    const int x0 = std::max( 0, int( ( bb.GetLeft() - m_origin.x ) / p ) - 1 );
                    const int y0 = std::max( 0, int( ( bb.GetTop() - m_origin.y ) / p ) - 1 );
                    const int x1 = std::min( m_w - 1, int( ( bb.GetRight() - m_origin.x ) / p ) + 1 );
                    const int y1 = std::min( m_h - 1, int( ( bb.GetBottom() - m_origin.y ) / p ) + 1 );
                    const SEG::ecoord limit = SEG::Square( aHalfWidth );

                    for( int iy = y0; iy <= y1; iy++ )
                    {
                        for( int ix = x0; ix <= x1; ix++ )
                        {
                            const SEG::ecoord d2 = aSeg.SquaredDistance( cellPos( ix, iy ) );

                            if( d2 > limit )
                                continue;

                            m_allowed[cellIndex( ix, iy )] = 1;

                            if( aCenterLine && !m_offCenter.empty() )
                            {
                                uint8_t& off = m_offCenter[cellIndex( ix, iy )];
                                // Within a grid step of the middle is as good as the middle; otherwise
                                // the route jogs back and forth to follow a lane between grid lines
                                const double dist = std::max( 0.0, std::sqrt( double( d2 ) ) - p );
                                const double span = std::max( 1, aHalfWidth - p );

                                off = std::min<uint8_t>( off, uint8_t( std::min( 255.0, 255.0 * dist / span ) ) );
                            }

                            if( aCenterLine && !nearest.empty() )
                            {
                                const size_t idx = cellIndex( ix, iy );
                                const float  d = float( std::sqrt( double( d2 ) ) );

                                if( d < nearest[idx] )
                                {
                                    const VECTOR2I pos = cellPos( ix, iy );
                                    const VECTOR2I pt = aSeg.NearestPoint( pos );

                                    nearest[idx] = d;

                                    if( !m_along.empty() )
                                        m_along[idx] = float( segAlong + ( pt - aSeg.A ).EuclideanNorm() );

                                    if( !m_guard.empty() )
                                    {
                                        // Screen coordinates: y grows downward, so a positive
                                        // cross product is to the right of the direction
                                        const VECTOR2L dir( aSeg.B - aSeg.A );
                                        const VECTOR2L rel( pos - aSeg.A );
                                        const int64_t  cross = dir.x * rel.y - dir.y * rel.x;
                                        const bool     right = cross > 0;
                                        const bool     left = cross < 0;

                                        // Half a grid step is as close to the middle as a
                                        // route can always get
                                        const double dist = std::max( 0.0, double( d ) - p / 2.0 );
                                        const double span = std::max( 1.0, aHalfWidth - p / 2.0 );

                                        m_guard[idx] = ( right && m_p.m_guardRight ) || ( left && m_p.m_guardLeft )
                                                               ? uint8_t( std::min( 255.0, 255.0 * dist / span ) )
                                                               : 0;
                                    }
                                }
                            }
                        }
                    }
                };

        for( int i = 0; i < path.SegmentCount(); i++ )
        {
            markSeg( path.CSegment( i ), hw, true );
            segAlong += path.CSegment( i ).Length();
        }

        // Always leave room to get out of the start and into the end
        const int endRoom = std::max( hw, m_p.m_endRadius + 2 * p );
        markSeg( SEG( m_p.m_start, m_p.m_start ), std::max( endRoom, m_p.m_corridorStartRoom ), false );
        markSeg( SEG( m_p.m_end, m_p.m_end ), std::max( endRoom, m_p.m_corridorEndRoom ), false );
    }

    void buildResult( int32_t aGoalRec, const APPROACH& aApproach, RESULT& aResult )
    {
        std::vector<int32_t> chain;

        for( int32_t r = aGoalRec; r >= 0; r = m_recs[r].parent )
            chain.push_back( r );

        std::reverse( chain.begin(), chain.end() );

        SHAPE_LINE_CHAIN run;
        int              layer = m_recs[chain.front()].layer;

        // The first state sits at the end of the straight exit when the route has one
        run.Append( m_p.m_start );
        run.Append( cellPos( m_recs[chain.front()].ix, m_recs[chain.front()].iy ) );

        for( size_t i = 1; i < chain.size(); i++ )
        {
            const REC& r = m_recs[chain[i]];
            VECTOR2I   pos = cellPos( r.ix, r.iy );

            if( r.layer != layer )
            {
                aResult.m_runs.push_back( run );
                aResult.m_runLayers.push_back( m_p.m_layers[layer] );
                aResult.m_vias.push_back( pos );

                run.Clear();
                layer = r.layer;
            }

            run.Append( pos );
        }

        for( int i = 0; i < aApproach.m_path.PointCount(); i++ )
            run.Append( aApproach.m_path.CPoint( i ) );

        aResult.m_runs.push_back( run );
        aResult.m_runLayers.push_back( m_p.m_layers[layer] );

        for( SHAPE_LINE_CHAIN& r : aResult.m_runs )
            r = simplifyExact( r );
    }

private:
    const PARAMS&           m_p;
    SKETCH_ROUTER_STATS&    m_stats;

    VECTOR2I                m_origin;
    int                     m_w = 0;
    int                     m_h = 0;
    int                     m_startIx = 0;
    int                     m_startIy = 0;
    int                     m_nLayers = 0;
    double                  m_tx = 0.0;
    double                  m_ty = 0.0;
    bool                    m_hitWindowEdge = false;

    std::vector<int32_t>    m_head;       ///< Per cell and layer: block of per-direction records
    std::vector<int32_t>    m_blocks;
    std::vector<REC>        m_recs;
    std::vector<uint8_t>    m_edges;      ///< Per cell and layer: 2 bit state of 4 edges
    std::vector<uint8_t>    m_viaState;   ///< Per cell: 0 unknown, 1 free, 2 blocked
    std::vector<uint8_t>    m_allowed;    ///< Per cell corridor mask (empty when unused)
    std::vector<uint8_t>    m_offCenter;  ///< Per cell distance from the corridor's middle, 0..255
    std::vector<float>      m_along;      ///< Per cell position along the corridor, -1 off it
    std::vector<uint8_t>    m_guard;      ///< Per cell distance toward a guarded side, 0..255
    float                   m_maxAlongJump = 0.0f;

    std::unordered_map<int64_t, std::vector<APPROACH>> m_approaches;
};


SKETCH_ROUTER::SKETCH_ROUTER( ROUTER* aRouter ) :
        ALGO_BASE( aRouter ),
        m_cornerMode( DIRECTION_45::MITERED_45 ),
        m_orthogonal( false ),
        m_deadline( 0 ),
        m_lanePitch( 0 ),
        m_corridor( nullptr ),
        m_corridorHalfWidth( 0 ),
        m_corridorStartRoom( 0 ),
        m_corridorEndRoom( 0 ),
        m_progressDone( 0 ),
        m_progressTotal( 0 )
{
}


SKETCH_ROUTER::~SKETCH_ROUTER()
{
}


bool SKETCH_ROUTER::cancelled()
{
    if( !m_stats.m_cancelled && nowMs() > m_deadline )
        m_stats.m_timedOut = true;

    return m_stats.m_cancelled || m_stats.m_timedOut;
}


bool SKETCH_ROUTER::keepGoing()
{
    if( m_progressCallback && !m_stats.m_cancelled
            && !m_progressCallback( m_progressDone, m_progressTotal ) )
    {
        m_stats.m_cancelled = true;
    }

    return !m_stats.m_cancelled;
}


NODE* SKETCH_ROUTER::Route( NODE* aWorld, const std::vector<SKETCH_CONNECTION>& aConnections )
{
    const int64_t start = nowMs();

    m_stats = SKETCH_ROUTER_STATS();
    m_stats.m_connections = (int) aConnections.size();
    m_deadline = start + std::max( 1, m_options.m_timeLimitMs );
    m_cornerMode = Settings().GetCornerMode();
    m_orthogonal = m_cornerMode == DIRECTION_45::MITERED_90
                   || m_cornerMode == DIRECTION_45::ROUNDED_90;

    m_layers = m_options.m_layers;

    if( m_layers.empty() )
    {
        int count = Router()->GetInterface()->GetPNSLayerFromBoardLayer( B_Cu ) + 1;

        for( int i = 0; i < count; i++ )
            m_layers.push_back( i );
    }

    std::sort( m_layers.begin(), m_layers.end() );

    if( aConnections.empty() )
        return nullptr;

    // Connections may be turned around so that a bundle runs one way
    std::vector<SKETCH_CONNECTION> conns( aConnections );

    // Group the connections into bundles.  A sketch makes one bundle of everything; without
    // one, groups of connections between the same two footprints get a sketch of their own.
    m_bundles.clear();
    m_bundleOf.assign( conns.size(), -1 );
    m_laneOffsets.assign( conns.size(), 0.0 );

    if( m_options.m_guide.PointCount() >= 2 )
    {
        BUNDLE bundle;
        bundle.m_guide = m_options.m_guide;

        for( size_t i = 0; i < conns.size(); i++ )
            bundle.m_members.push_back( (int) i );

        // A sketch is drawn quickly and may clip an obstacle or wobble.  Fit a path for the
        // whole bundle to it: where the sketch is clear the path follows it, and where it isn't
        // the path goes round as closely as it can.
        const int pitch = bundlePitch( conns, bundle.m_members );

        if( pitch > 0 )
        {
            // As wide as searchSpine() makes the bundle
            const int width = (int) ( bundle.m_members.size() + 1 ) * pitch;
            const int ends = width / 2 + 2 * pitch;

            bundle.m_guide = cutTightLoops( bundle.m_guide, width );

            const SHAPE_LINE_CHAIN snapped = octilinearGuide( bundle.m_guide, m_orthogonal,
                                                              std::max( 300000, pitch ) );

            // Hold close to the sketch first, and only stray further when that's blocked
            const std::pair<int, double> bands[] = { { width / 2 + 2 * pitch, 4.0 },
                                                     { std::max( 3000000, 2 * width ), 2.0 } };

            for( const auto& [halfWidth, centerCost] : bands )
            {
                if( snapped.PointCount() < 2 )
                    break;

                SHAPE_LINE_CHAIN fitted = searchSpine( aWorld, conns, bundle.m_members, snapped.CPoint( 0 ),
                                                       snapped.CLastPoint(), ends, ends, pitch, &snapped,
                                                       halfWidth, centerCost );

                wxLogTrace( wxT( "PNS_SKETCH" ), wxT( "sketch fit, band %d: %s" ), halfWidth,
                            fitted.PointCount() >= 2 ? wxT( "found" ) : wxT( "blocked" ) );

                if( fitted.PointCount() >= 2 )
                {
                    bundle.m_guide = fitted;
                    break;
                }
            }
        }

        m_bundles.push_back( bundle );
    }
    else if( m_options.m_autoBundle )
    {
        formBundles( aWorld, conns );
    }

    for( size_t b = 0; b < m_bundles.size(); b++ )
    {
        for( int member : m_bundles[b].m_members )
            m_bundleOf[member] = (int) b;

        assignLanes( conns, m_bundles[b] );
    }

    // Shortest connections first: they have the fewest alternatives.  A bundle following a
    // sketch that bends is routed from the inside of the bend out instead, so that each route
    // nests around the ones before it.
    std::vector<double> length( conns.size() );
    std::vector<double> innerness( conns.size(), 0.0 );

    for( size_t i = 0; i < conns.size(); i++ )
    {
        const SKETCH_CONNECTION& conn = conns[i];

        length[i] = ( conn.m_endPos - conn.m_startPos ).EuclideanNorm();
        activateBundle( m_bundleOf[i] );

        if( m_guide.PointCount() >= 2 )
            innerness[i] = bendInnerness( conn );
    }

    std::vector<int> order( aConnections.size() );

    for( size_t i = 0; i < order.size(); i++ )
        order[i] = (int) i;

    std::stable_sort( order.begin(), order.end(),
                      [&]( int aA, int aB )
                      {
                          if( innerness[aA] != innerness[aB] )
                              return innerness[aA] > innerness[aB];

                          return length[aA] < length[aB];
                      } );

    // Passes are ranked by routed count, then by routes that had to leave the sketch corridor,
    // then by total cost
    NODE*                best = nullptr;
    int                  bestRouted = -1;
    int                  bestDetoured = 0;
    double               bestCost = 0.0;
    std::vector<uint8_t> bestStatus;
    int                  bestVias = 0;
    long long            bestLength = 0;

    for( int pass = 0; pass < std::max( 1, m_options.m_maxPasses ); pass++ )
    {
        NODE*                branch = aWorld->Branch();
        std::vector<uint8_t> status( aConnections.size(), ROUTE_FAILED );

        m_stats.m_lanes.clear();
        double               cost = 0.0;
        int                  vias = 0;
        long long            routedLength = 0;

        routePass( branch, conns, order, status, cost, vias, routedLength );
        m_stats.m_passes = pass + 1;

        const int routed = (int) std::count_if( status.begin(), status.end(),
                                                []( uint8_t aStatus )
                                                {
                                                    return aStatus != ROUTE_FAILED;
                                                } );
        const int detoured = (int) std::count( status.begin(), status.end(), ROUTE_DETOURED );

        if( routed > bestRouted || ( routed == bestRouted && detoured < bestDetoured )
                || ( routed == bestRouted && detoured == bestDetoured && cost < bestCost ) )
        {
            delete best;
            best = branch;
            bestRouted = routed;
            bestDetoured = detoured;
            bestCost = cost;
            bestStatus = status;
            bestVias = vias;
            bestLength = routedLength;
        }
        else
        {
            delete branch;
        }

        if( ( bestRouted == (int) aConnections.size() && bestDetoured == 0 ) || cancelled() )
            break;

        // Rip up and try again with the failures first, then the detours
        std::vector<int> next;

        for( uint8_t wanted : { ROUTE_FAILED, ROUTE_DETOURED, ROUTE_OK } )
        {
            for( int idx : order )
            {
                if( status[idx] == wanted )
                    next.push_back( idx );
            }
        }

        if( next == order )
            break;

        order = next;
    }

    m_stats.m_routed = std::max( 0, bestRouted );
    m_stats.m_vias = bestVias;
    m_stats.m_length = bestLength;

    for( size_t i = 0; i < bestStatus.size(); i++ )
    {
        if( bestStatus[i] == ROUTE_FAILED )
            m_stats.m_failed.push_back( (int) i );
    }

    m_stats.m_elapsedMs = double( nowMs() - start );

    if( best && bestRouted <= 0 )
    {
        delete best;
        best = nullptr;
    }

    return best;
}


void SKETCH_ROUTER::formBundles( NODE* aWorld, std::vector<SKETCH_CONNECTION>& aConnections )
{
    auto footprintOf =
            []( const ITEM* aItem ) -> const void*
            {
                if( aItem && aItem->Parent() && aItem->Parent()->Type() == PCB_PAD_T )
                    return static_cast<const PAD*>( aItem->Parent() )->GetParentFootprint();

                return nullptr;
            };

    std::map<std::pair<const void*, const void*>, std::vector<int>> groups;

    for( size_t i = 0; i < aConnections.size(); i++ )
    {
        const void* a = footprintOf( aConnections[i].m_start );
        const void* b = footprintOf( aConnections[i].m_end );

        if( a && b && a != b )
            groups[std::minmax( a, b )].push_back( (int) i );
    }

    for( auto& [key, members] : groups )
    {
        if( members.size() < 3 )
            continue;

        // Run every member away from the same footprint
        for( int i : members )
        {
            SKETCH_CONNECTION& conn = aConnections[i];

            if( footprintOf( conn.m_start ) != key.first )
            {
                std::swap( conn.m_start, conn.m_end );
                std::swap( conn.m_startPos, conn.m_endPos );
            }
        }

        VECTOR2D startCenter( 0, 0 );
        VECTOR2D endCenter( 0, 0 );

        for( int i : members )
        {
            startCenter += VECTOR2D( aConnections[i].m_startPos );
            endCenter += VECTOR2D( aConnections[i].m_endPos );
        }

        startCenter = startCenter / (double) members.size();
        endCenter = endCenter / (double) members.size();

        double startSpread = 0.0;
        double endSpread = 0.0;

        for( int i : members )
        {
            startSpread = std::max( startSpread, ( VECTOR2D( aConnections[i].m_startPos ) - startCenter ).EuclideanNorm() );
            endSpread = std::max( endSpread, ( VECTOR2D( aConnections[i].m_endPos ) - endCenter ).EuclideanNorm() );
        }

        // Parts this close together are better joined by independent fanouts
        const double span = ( endCenter - startCenter ).EuclideanNorm();

        if( span < 1.5 * ( startSpread + endSpread ) )
            continue;

        const int pitch = bundlePitch( aConnections, members );

        if( pitch <= 0 )
            continue;

        // Search a path for a track as wide as the whole bundle.  Around its ends lie the
        // bundle's own pads and fanouts, which are left to the individual routes.
        SHAPE_LINE_CHAIN spine = searchSpine( aWorld, aConnections, members, KiROUND( startCenter ),
                                              KiROUND( endCenter ), int( startSpread ) + 2 * pitch,
                                              int( endSpread ) + 2 * pitch, pitch, nullptr, 0 );

        if( spine.PointCount() < 2 )
        {
            wxLogTrace( wxT( "PNS_SKETCH" ), wxT( "no path for a bundle of %d" ), (int) members.size() );
            continue;
        }

        BUNDLE bundle;
        bundle.m_guide = spine;
        bundle.m_members = members;
        m_bundles.push_back( bundle );

        wxLogTrace( wxT( "PNS_SKETCH" ), wxT( "bundle of %d along %d points" ), (int) members.size(),
                    spine.PointCount() );
    }
}


SHAPE_LINE_CHAIN SKETCH_ROUTER::searchSpine( NODE* aWorld, const std::vector<SKETCH_CONNECTION>& aConnections,
                                             const std::vector<int>& aMembers, const VECTOR2I& aFrom,
                                             const VECTOR2I& aTo, int aFreeFrom, int aFreeTo, int aPitch,
                                             const SHAPE_LINE_CHAIN* aCorridor, int aCorridorHalfWidth,
                                             double aCenterCost )
{
    // The bundle runs on the preferred layer, or on the layer of its pads
    int layer = m_options.m_preferredLayer;

    if( layer < 0 )
    {
        const ITEM* start = aConnections[aMembers.front()].m_start;
        layer = start->Layers().Start() == start->Layers().End() ? start->Layers().Start()
                                                                 : m_layers.front();
    }

    std::set<NET_HANDLE> nets;

    for( int i : aMembers )
        nets.insert( aConnections[i].m_net );

    // Half a pitch of slack on either side: the member routes snap to a grid and can sit a little off
    // their lane centres, which would otherwise squeeze the outer lanes out wherever the spine
    // hugs an obstacle.
    const int width = (int) ( aMembers.size() + 1 ) * aPitch;
    VIA       noVia;

    GRID_SEARCH::PARAMS params;
    params.m_node = aWorld;
    params.m_width = width;
    params.m_pitch = std::max( aPitch, width / 4 );
    params.m_viaProto = &noVia;
    params.m_layers = { layer };
    params.m_startLayers = { true };
    params.m_endLayers = { true };
    params.m_start = aFrom;
    params.m_end = aTo;
    params.m_freeRadiusStart = aFreeFrom;
    params.m_freeRadiusEnd = aFreeTo;
    params.m_ignoreNets = &nets;
    params.m_orthogonal = m_orthogonal;
    params.m_allowVias = false;
    params.m_bendCost = m_options.m_bendCost;
    params.m_wrongWayCost = 0.0;
    params.m_heuristicWeight = 1.5;
    params.m_maxExpansions = 400000;
    params.m_deadline = m_deadline;

    // Following a sketch: stay near it, but go round whatever it runs into
    if( aCorridor )
    {
        params.m_corridor = aCorridor;
        params.m_corridorHalfWidth = aCorridorHalfWidth;
        params.m_centerCost = aCenterCost;
        params.m_followOrder = true;
    }

    BOX2I window( aFrom, VECTOR2I( 0, 0 ) );
    window.Merge( aTo );

    if( aCorridor )
        window.Merge( aCorridor->BBox() );

    const double span = ( aTo - aFrom ).EuclideanNorm();
    window.Inflate( std::max( { 5000000, int( span / 2 ), aCorridorHalfWidth } ) );

    if( m_options.m_boundary.GetWidth() > 0 && window.Intersects( m_options.m_boundary ) )
        window = window.Intersect( m_options.m_boundary );

    window.Merge( aFrom );
    window.Merge( aTo );
    params.m_window = window;

    // Same limit on the search size as for single routes
    const size_t cells = GRID_SEARCH::CellCount( window, params.m_pitch );

    if( cells > (size_t) m_options.m_maxCells )
        params.m_pitch = int( params.m_pitch * std::sqrt( double( cells ) / m_options.m_maxCells ) ) + 1;

    params.m_endRadius = 3 * params.m_pitch;
    params.m_keepGoing = [this]() { return keepGoing(); };

    GRID_SEARCH         search( params, m_stats );
    GRID_SEARCH::RESULT result;

    if( search.Run( result ) != GRID_SEARCH::STATUS::FOUND || result.m_runs.empty() )
        return SHAPE_LINE_CHAIN();

    // The lanes start where the bundle forms, clear of the fanouts at either end
    const SHAPE_LINE_CHAIN& path = result.m_runs.front();
    const VECTOR2D          startCenter( aFrom );
    const VECTOR2D          endCenter( aTo );
    const double            freeStart = aFreeFrom;
    const double            freeEnd = aFreeTo;
    SHAPE_LINE_CHAIN        spine;

    auto crossing =
            [&]( const SEG& aSeg, const VECTOR2D& aCenter, double aRadius )
            {
                double lo = 0.0, hi = 1.0;
                const bool aInside = ( VECTOR2D( aSeg.A ) - aCenter ).EuclideanNorm() <= aRadius;

                for( int iter = 0; iter < 40; iter++ )
                {
                    double   mid = ( lo + hi ) / 2;
                    VECTOR2D pt = VECTOR2D( aSeg.A ) + VECTOR2D( aSeg.B - aSeg.A ) * mid;
                    bool     inside = ( pt - aCenter ).EuclideanNorm() <= aRadius;

                    if( inside == aInside )
                        lo = mid;
                    else
                        hi = mid;
                }

                return KiROUND( VECTOR2D( aSeg.A ) + VECTOR2D( aSeg.B - aSeg.A ) * hi );
            };

    auto inside =
            [&]( const VECTOR2I& aPt, const VECTOR2D& aCenter, double aRadius )
            {
                return ( VECTOR2D( aPt ) - aCenter ).EuclideanNorm() <= aRadius;
            };

    for( int i = 0; i < path.SegmentCount(); i++ )
    {
        const SEG seg = path.CSegment( i );
        const bool aIn = inside( seg.A, startCenter, freeStart ) || inside( seg.A, endCenter, freeEnd );
        const bool bIn = inside( seg.B, startCenter, freeStart ) || inside( seg.B, endCenter, freeEnd );

        if( !aIn && spine.PointCount() == 0 )
            spine.Append( seg.A );

        if( aIn && !bIn )
            spine.Append( crossing( seg, inside( seg.A, startCenter, freeStart ) ? startCenter : endCenter,
                                    inside( seg.A, startCenter, freeStart ) ? freeStart : freeEnd ) );

        if( !aIn && bIn )
        {
            spine.Append( crossing( seg, inside( seg.B, startCenter, freeStart ) ? startCenter : endCenter,
                                    inside( seg.B, startCenter, freeStart ) ? freeStart : freeEnd ) );
            break;
        }

        if( !bIn )
            spine.Append( seg.B );
    }

    if( spine.PointCount() < 2 || spine.Length() < 2 * aPitch )
        spine = path;

    return spine;
}


void SKETCH_ROUTER::activateBundle( int aBundle )
{
    if( aBundle >= 0 && aBundle < (int) m_bundles.size() )
    {
        m_guide = m_bundles[aBundle].m_guide;
        m_lanePitch = m_bundles[aBundle].m_lanePitch;
    }
    else
    {
        m_guide.Clear();
        m_lanePitch = 0;
    }
}


int SKETCH_ROUTER::bundlePitch( const std::vector<SKETCH_CONNECTION>& aConnections,
                                const std::vector<int>& aMembers ) const
{
    ROUTER_IFACE* iface = Router()->GetInterface();
    int           pitch = 0;

    for( int member : aMembers )
    {
        const SKETCH_CONNECTION& conn = aConnections[member];
        SIZES_SETTINGS           sizes( Router()->Sizes() );

        if( conn.m_start && iface->ImportSizes( sizes, conn.m_start, conn.m_net, conn.m_startPos ) )
        {
            pitch = std::max( pitch, sizes.TrackWidth()
                                     + std::max( sizes.Clearance(), sizes.MinClearance() ) );
        }
    }

    return pitch;
}


void SKETCH_ROUTER::assignLanes( const std::vector<SKETCH_CONNECTION>& aConnections, BUNDLE& aBundle )
{
    // Lanes are spaced for the widest track in the bundle
    m_lanePitch = bundlePitch( aConnections, aBundle.m_members );

    // Snap the sketch to the routing angles so that the lanes can be followed exactly
    m_guide = octilinearGuide( aBundle.m_guide, m_orthogonal, std::max( 300000, m_lanePitch ) );

    aBundle.m_guide = m_guide;
    aBundle.m_lanePitch = m_lanePitch;

    if( m_guide.PointCount() < 2 || m_lanePitch <= 0 || aBundle.m_members.size() < 2 )
        return;

    // Order the connections across the bundle by where their ends sit beside the sketch, in the
    // sketch's own direction so that every connection is measured the same way
    VECTOR2D d0( m_guide.CPoint( 1 ) - m_guide.CPoint( 0 ) );
    VECTOR2D dN( m_guide.CLastPoint() - m_guide.CPoint( -2 ) );

    if( d0.EuclideanNorm() == 0 || dN.EuclideanNorm() == 0 )
        return;

    d0 = d0 / d0.EuclideanNorm();
    dN = dN / dN.EuclideanNorm();

    // Tracks converging on the start of the sketch keep the angular order of their pads around
    // it: a pad straight behind the start lands in the middle of the bundle, and pads further
    // round to either side land further out on that side.  At the end of the sketch the pads
    // straight ahead are in the middle.  Positive keys are on the side of positive cross
    // products, which is where offsetPolyline() moves positive offsets.
    const VECTOR2D g0( m_guide.CPoint( 0 ) );
    const VECTOR2D gN( m_guide.CLastPoint() );

    std::vector<double> key( aConnections.size() );
    std::vector<double> lateral( aConnections.size() );

    for( int i : aBundle.m_members )
    {
        const SKETCH_CONNECTION& conn = aConnections[i];
        const bool               rev = guideReversed( conn );
        const VECTOR2D           a = VECTOR2D( rev ? conn.m_endPos : conn.m_startPos ) - g0;
        const VECTOR2D           b = VECTOR2D( rev ? conn.m_startPos : conn.m_endPos ) - gN;

        double startKey = 0.0;
        double endKey = 0.0;

        if( a.EuclideanNorm() > 0 )
        {
            double phi = std::atan2( d0.Cross( a ), d0.Dot( a ) );
            startKey = phi > 0 ? M_PI - phi : -M_PI - phi;
        }

        if( b.EuclideanNorm() > 0 )
            endKey = std::atan2( dN.Cross( b ), dN.Dot( b ) );

        key[i] = startKey + endKey;
        lateral[i] = d0.Cross( a ) + dN.Cross( b );
    }

    std::vector<int> rank = aBundle.m_members;

    std::stable_sort( rank.begin(), rank.end(),
                      [&]( int aA, int aB )
                      {
                          if( std::abs( key[aA] - key[aB] ) > 1e-6 )
                              return key[aA] < key[aB];

                          return lateral[aA] < lateral[aB];
                      } );

    // offsetPolyline() moves toward (-dy, dx), which is the side with positive cross products
    const double center = ( rank.size() - 1 ) / 2.0;

    for( size_t r = 0; r < rank.size(); r++ )
        m_laneOffsets[rank[r]] = ( r - center ) * m_lanePitch;
}


bool SKETCH_ROUTER::guideReversed( const SKETCH_CONNECTION& aConn ) const
{
    const VECTOR2I& s = aConn.m_startPos;
    const VECTOR2I& t = aConn.m_endPos;

    return ( m_guide.CPoint( 0 ) - s ).EuclideanNorm() + ( m_guide.CLastPoint() - t ).EuclideanNorm()
           > ( m_guide.CPoint( 0 ) - t ).EuclideanNorm() + ( m_guide.CLastPoint() - s ).EuclideanNorm();
}


SHAPE_LINE_CHAIN SKETCH_ROUTER::orientedCorridor( const SKETCH_CONNECTION& aConn,
                                                  double aLaneOffset ) const
{
    SHAPE_LINE_CHAIN guide = m_guide;

    if( aLaneOffset != 0.0 )
        guide = offsetPolyline( guide, aLaneOffset );

    if( guideReversed( aConn ) )
        guide = guide.Reverse();

    SHAPE_LINE_CHAIN corridor;
    corridor.Append( aConn.m_startPos );
    corridor.Append( guide );
    corridor.Append( aConn.m_endPos );
    return corridor;
}


SHAPE_LINE_CHAIN SKETCH_ROUTER::laneCorridor( const SKETCH_CONNECTION& aConn, double aLaneOffset,
                                              const std::vector<SKETCH_EXIT>& aStartExits,
                                              const std::vector<SKETCH_EXIT>& aEndExits,
                                              int& aStartRoom, int& aEndRoom ) const
{
    aStartRoom = 0;
    aEndRoom = 0;

    const SHAPE_LINE_CHAIN lane = orientedCorridor( aConn, aLaneOffset );
    const int              n = lane.PointCount();

    if( n < 4 )
        return lane;

    // A fanout between a pad and its lane leaves the pad straight along one of its exits, then
    // turns onto the lane in 45 degree steps.  When every pad of a row does the same, the
    // fanouts nest instead of crossing.  aArriveDir is the direction the fanout has to be able
    // to turn onto where it meets the lane.
    auto fanout =
            [&]( const VECTOR2I& aPad, const std::vector<SKETCH_EXIT>& aExits,
                 const VECTOR2I& aLaneEnd, int aArriveDir, int* aPenalty = nullptr )
            {
                SHAPE_LINE_CHAIN best;
                double           bestScore = std::numeric_limits<double>::max();

                if( aPenalty )
                    *aPenalty = 0;

                if( aExits.empty() )
                {
                    for( int d = 0; d < 8; d++ )
                    {
                        if( m_orthogonal && ( d & 1 ) )
                            continue;

                        SHAPE_LINE_CHAIN path = directedPath( aPad, d, aLaneEnd, aArriveDir, m_orthogonal );

                        if( path.PointCount() >= 2
                                && ( best.PointCount() == 0 || path.Length() < best.Length() ) )
                        {
                            best = path;
                        }
                    }

                    return best;
                }

                for( const SKETCH_EXIT& exit : aExits )
                {
                    const int      steps = (int) std::ceil( exit.m_length / dirLength( exit.m_dir ) );
                    const VECTOR2I from = aPad + VECTOR2I( DX[exit.m_dir], DY[exit.m_dir] ) * steps;

                    SHAPE_LINE_CHAIN tail = directedPath( from, exit.m_dir, aLaneEnd, aArriveDir,
                                                          m_orthogonal );

                    if( tail.PointCount() < 2 )
                        continue;

                    SHAPE_LINE_CHAIN path;
                    path.Append( aPad );
                    path.Append( tail );

                    const double score = path.Length() + exit.m_penalty;

                    if( score < bestScore )
                    {
                        bestScore = score;
                        best = path;

                        if( aPenalty )
                            *aPenalty = exit.m_penalty;
                    }
                }

                return best;
            };

    SHAPE_LINE_CHAIN body;

    for( int i = 1; i < n - 1; i++ )
        body.Append( lane.CPoint( i ) );

    // Offsetting a sketch that starts or ends with a stretch shorter than the offset, on the
    // inside of a bend, leaves a stub running backwards at that end.  Trim it off.
    auto reverses =
            [&]( const SEG& aA, const SEG& aB )
            {
                const int a = nearestDirection( VECTOR2D( aA.B - aA.A ) );
                const int b = nearestDirection( VECTOR2D( aB.B - aB.A ) );

                return a >= 0 && b >= 0 && turnSteps( a, b ) >= 3;
            };

    while( body.PointCount() >= 3 && reverses( body.CSegment( 0 ), body.CSegment( 1 ) ) )
        body.Remove( 0 );

    while( body.PointCount() >= 3 && reverses( body.CSegment( -2 ), body.CSegment( -1 ) ) )
        body.Remove( body.PointCount() - 1 );

    // The lane as sketched, before it is extended
    const SHAPE_LINE_CHAIN sketched = body;

    // Run the lane on past its ends, back toward the pads, so that pads beside the start of the
    // bundle can meet their lane with a single turn off their straight exit
    if( body.PointCount() >= 2 )
    {
        auto extend =
                [&]( const VECTOR2I& aEnd, const VECTOR2I& aNext, const VECTOR2I& aPad )
                {
                    VECTOR2D dir( aEnd - aNext );

                    if( dir.EuclideanNorm() == 0 )
                        return aEnd;

                    // No further than the pad itself: joins behind a pad are useless
                    const double reach = VECTOR2D( aPad - aEnd ).Dot( dir.Resize( 1.0 ) );

                    if( reach <= 0 )
                        return aEnd;

                    return KiROUND( VECTOR2D( aEnd ) + dir.Resize( reach ) );
                };

        SHAPE_LINE_CHAIN extended;
        extended.Append( extend( body.CPoint( 0 ), body.CPoint( 1 ), lane.CPoint( 0 ) ) );
        extended.Append( body );
        extended.Append( extend( body.CLastPoint(), body.CPoint( -2 ), lane.CLastPoint() ) );
        body = simplifyExact( extended );
    }

    const double bodyLength = body.Length();

    // How far along the lane each of its vertices is
    std::vector<double> vertexAlong( 1, 0.0 );

    for( int i = 0; i < body.SegmentCount(); i++ )
        vertexAlong.push_back( vertexAlong.back() + body.CSegment( i ).Length() );

    // Candidate joins: the points along the lane, plus the closest point to each pad
    struct JOIN
    {
        VECTOR2I m_pt;
        int      m_segment;
        double   m_along;
    };

    std::vector<JOIN> joins;
    const double      step = std::max( 1000, m_lanePitch / 4 );
    double            total = 0.0;

    for( int i = 0; i < body.SegmentCount(); i++ )
    {
        const SEG    seg = body.CSegment( i );
        const double len = seg.Length();
        const int    samples = std::max( 1, int( len / step ) );

        for( int j = 0; j <= samples; j++ )
            joins.push_back( { seg.A + ( seg.B - seg.A ) * j / samples, i, total + len * j / samples } );

        for( const VECTOR2I& pad : { lane.CPoint( 0 ), lane.CLastPoint() } )
        {
            VECTOR2I p = seg.NearestPoint( pad );
            joins.push_back( { p, i, total + ( p - seg.A ).EuclideanNorm() } );
        }

        total += len;
    }

    // A pad joins the lane at the closest point it can reach without doubling back: the fanout
    // has to meet the lane at no more than 45 degrees (90 in orthogonal modes), so the route
    // runs on smoothly instead of looping back into the lane.
    // Prefer the simplest fanout, then the shortest.  Where a pad's straight exit runs into its
    // lane, the fanout needs only one turn; a row of pads fanned out like that makes a clean
    // staircase into the bundle.  A fanout that strays much further from the lane than the pad
    // itself is would cut across a bend and skip part of the lane, so those don't count.
    auto pickJoin =
            [&]( const VECTOR2I& aPad, const std::vector<SKETCH_EXIT>& aExits, bool aAtStart,
                 double aMinAlong )
            {
                std::vector<JOIN> candidates = joins;

                for( const SKETCH_EXIT& exit : aExits )
                {
                    const int      steps = (int) std::ceil( exit.m_length / dirLength( exit.m_dir ) );
                    const VECTOR2D from( aPad + VECTOR2I( DX[exit.m_dir], DY[exit.m_dir] ) * steps );
                    const VECTOR2D dir = VECTOR2D( DX[exit.m_dir], DY[exit.m_dir] ).Resize( 1.0 );
                    double         along = 0.0;

                    for( int i = 0; i < body.SegmentCount(); i++ )
                    {
                        const SEG      seg = body.CSegment( i );
                        const VECTOR2D ab( seg.B - seg.A );
                        const double   det = dir.x * -ab.y - dir.y * -ab.x;

                        if( std::abs( det ) > 1e-9 )
                        {
                            const VECTOR2D rhs = VECTOR2D( seg.A ) - from;
                            const double   t = ( rhs.x * -ab.y - rhs.y * -ab.x ) / det;
                            const double   u = ( dir.x * rhs.y - dir.y * rhs.x ) / det;

                            if( t >= 0 && u >= 0 && u <= 1 )
                                candidates.push_back( { KiROUND( from + dir * t ), i, along + u * seg.Length() } );
                        }

                        along += seg.Length();
                    }
                }

                // The part of the lane a fanout skips may turn one 45 degree corner at most.
                // Skipping more would cut across a bend or loop the sketch asked for, or run the
                // fanout alongside the bundle where it would cross the lanes beside it.
                auto skipTurn =
                        [&]( double aJoinAlong )
                        {
                            const double from = aAtStart ? 0.0 : aJoinAlong;
                            const double to = aAtStart ? aJoinAlong : bodyLength;
                            int          steps = 0;

                            for( int v = 1; v < body.PointCount() - 1; v++ )
                            {
                                if( vertexAlong[v] <= from + 1000.0 || vertexAlong[v] >= to - 1000.0 )
                                    continue;

                                const SEG in = body.CSegment( v - 1 );
                                const SEG out = body.CSegment( v );
                                const int a = nearestDirection( VECTOR2D( in.B - in.A ) );
                                const int b = nearestDirection( VECTOR2D( out.B - out.A ) );

                                if( a >= 0 && b >= 0 )
                                    steps += turnSteps( a, b );
                            }

                            return steps;
                        };

                int    best = -1;
                double bestScore = std::numeric_limits<double>::max();
                double bestFanout = std::numeric_limits<double>::max();
                int    nearestIdx = 0;
                double nearestDist = std::numeric_limits<double>::max();

                for( int i = 0; i < (int) candidates.size(); i++ )
                {
                    const double d = ( candidates[i].m_pt - aPad ).EuclideanNorm();

                    if( d < nearestDist )
                    {
                        nearestDist = d;
                        nearestIdx = i;
                    }
                }

                const double maxStray = nearestDist + 2.0 * m_lanePitch;

                auto stray =
                        [&]( const SHAPE_LINE_CHAIN& aPath )
                        {
                            double worst = 0.0;

                            for( int i = 0; i < aPath.SegmentCount(); i++ )
                            {
                                const SEG seg = aPath.CSegment( i );
                                const int samples = std::max( 1, int( seg.Length() / std::max( 1000, m_lanePitch ) ) );

                                for( int j = 0; j <= samples; j++ )
                                {
                                    const VECTOR2I pt = seg.A + ( seg.B - seg.A ) * j / samples;
                                    double         d = std::numeric_limits<double>::max();

                                    for( int k = 0; k < body.SegmentCount(); k++ )
                                        d = std::min( d, (double) body.CSegment( k ).Distance( pt ) );

                                    worst = std::max( worst, d );

                                    if( worst > maxStray )
                                        return worst;
                                }
                            }

                            return worst;
                        };

                for( int i = 0; i < (int) candidates.size(); i++ )
                {
                    const JOIN& join = candidates[i];

                    if( join.m_along <= aMinAlong )
                        continue;


                    const SEG seg = body.CSegment( join.m_segment );
                    const int laneDir = nearestDirection( VECTOR2D( seg.B - seg.A ) );

                    if( laneDir < 0 )
                        continue;

                    // At the end the route leaves the lane toward the pad, so the fanout, built
                    // from the pad, arrives heading against the lane
                    const int        arrive = aAtStart ? laneDir : ( laneDir + 4 ) % 8;
                    int              penalty = 0;
                    SHAPE_LINE_CHAIN path = fanout( aPad, aExits, join.m_pt, arrive, &penalty );

                    if( path.PointCount() < 2 )
                        continue;

                    path.Simplify();

                    // The whole route counts: the fanout, the lane still to follow and a little
                    // for each corner.  Running beside the lane is never shorter than running in
                    // it, so between equals the shorter fanout wins.
                    // Corners count wherever they are: joining the lane just before one of its
                    // corners leaves a jog, where joining just after it would run straight on
                    int laneCorners = 0;

                    for( size_t v = 1; v + 1 < vertexAlong.size(); v++ )
                    {
                        if( aAtStart ? vertexAlong[v] > join.m_along + 1000.0
                                     : vertexAlong[v] < join.m_along - 1000.0 )
                        {
                            laneCorners++;
                        }
                    }

                    const double fanoutLength = path.Length();
                    const double laneLength = aAtStart ? bodyLength - join.m_along : join.m_along;
                    const double score = fanoutLength + laneLength + penalty
                                         + 0.5 * m_lanePitch * ( path.SegmentCount() + laneCorners );

                    if( score > bestScore + 1000.0
                            || ( std::abs( score - bestScore ) <= 1000.0 && fanoutLength >= bestFanout ) )
                    {
                        continue;
                    }

                    if( skipTurn( join.m_along ) > 1 || stray( path ) > maxStray )
                        continue;

                    // A fanout that crosses its own lane before joining it has cut a corner off
                    // the bundle, and would cross the lanes beside it too
                    bool crosses = false;

                    for( int f = 0; f < path.SegmentCount() && !crosses; f++ )
                    {
                        for( int k = 0; k < sketched.SegmentCount() && !crosses; k++ )
                        {
                            OPT_VECTOR2I ip = path.CSegment( f ).Intersect( sketched.CSegment( k ) );

                            if( ip && ( *ip - join.m_pt ).EuclideanNorm() > m_lanePitch / 2 )
                                crosses = true;
                        }
                    }

                    if( crosses )
                        continue;

                    bestScore = std::min( bestScore, score );
                    bestFanout = fanoutLength;
                    best = i;
                }

                if( best < 0 )
                {
                    wxLogTrace( wxT( "PNS_SKETCH" ), wxT( "no fanout from (%d,%d), %d candidates, nearest %.0f, %d exits" ),
                                aPad.x, aPad.y, (int) candidates.size(), nearestDist, (int) aExits.size() );
                }

                return candidates[best >= 0 ? best : nearestIdx];
            };

    const JOIN startJoin = pickJoin( lane.CPoint( 0 ), aStartExits, true, -1.0 );
    const JOIN endJoin = pickJoin( lane.CLastPoint(), aEndExits, false, startJoin.m_along );

    int      startSeg = startJoin.m_segment;
    int      endSeg = endJoin.m_segment;
    double   startAlong = startJoin.m_along;
    double   endAlong = endJoin.m_along;
    VECTOR2I joinStart = startJoin.m_pt;
    VECTOR2I joinEnd = endJoin.m_pt;

    wxLogTrace( wxT( "PNS_SKETCH_LANES" ), wxT( "joins: start (%d,%d) seg %d along %.0f, end (%d,%d) seg %d along %.0f" ),
                joinStart.x, joinStart.y, startSeg, startAlong, joinEnd.x, joinEnd.y, endSeg, endAlong );

    // The pads have to join in order along the lane
    if( startAlong >= endAlong )
    {
        startSeg = 0;
        endSeg = body.SegmentCount() - 1;
        joinStart = body.CPoint( 0 );
        joinEnd = body.CLastPoint();
    }

    auto laneDirAt =
            [&]( int aSegment )
            {
                const SEG seg = body.CSegment( aSegment );
                return nearestDirection( VECTOR2D( seg.B - seg.A ) );
            };

    // Where no proper fanout exists, a straight line still keeps the corridor connected
    auto fanoutOrLine =
            [&]( const VECTOR2I& aPad, const std::vector<SKETCH_EXIT>& aExits, const VECTOR2I& aJoin,
                 int aArriveDir )
            {
                SHAPE_LINE_CHAIN path = aArriveDir >= 0 ? fanout( aPad, aExits, aJoin, aArriveDir )
                                                        : SHAPE_LINE_CHAIN();

                if( path.PointCount() < 2 )
                {
                    path.Clear();
                    path.Append( aPad );

                    // At least leave the pad straight before heading for the lane
                    const SKETCH_EXIT* best = nullptr;
                    double             bestDot = -2.0;
                    const VECTOR2D     toward = VECTOR2D( aJoin - aPad ).Resize( 1.0 );

                    for( const SKETCH_EXIT& exit : aExits )
                    {
                        double dot = VECTOR2D( DX[exit.m_dir], DY[exit.m_dir] ).Resize( 1.0 ).Dot( toward );

                        if( dot > bestDot )
                        {
                            bestDot = dot;
                            best = &exit;
                        }
                    }

                    if( best )
                    {
                        const int steps = (int) std::ceil( best->m_length / dirLength( best->m_dir ) );
                        path.Append( aPad + VECTOR2I( DX[best->m_dir], DY[best->m_dir] ) * steps );
                    }

                    path.Append( aJoin );
                }

                return path;
            };

    // The fanouts may stray from their line to keep clear of each other; only the lane itself
    // is held tight
    aStartRoom = int( ( joinStart - lane.CPoint( 0 ) ).EuclideanNorm() ) + m_lanePitch;
    aEndRoom = int( ( joinEnd - lane.CLastPoint() ).EuclideanNorm() ) + m_lanePitch;

    const int startLaneDir = laneDirAt( startSeg );
    const int endLaneDir = laneDirAt( endSeg );

    SHAPE_LINE_CHAIN out = fanoutOrLine( lane.CPoint( 0 ), aStartExits, joinStart, startLaneDir );

    for( int i = startSeg + 1; i <= endSeg; i++ )
        out.Append( body.CPoint( i ) );

    SHAPE_LINE_CHAIN tail = fanoutOrLine( lane.CLastPoint(), aEndExits, joinEnd,
                                          endLaneDir >= 0 ? ( endLaneDir + 4 ) % 8 : -1 ).Reverse();
    out.Append( tail );
    return out;
}


double SKETCH_ROUTER::bendInnerness( const SKETCH_CONNECTION& aConn ) const
{
    // corridor = start, sketch points..., end
    const SHAPE_LINE_CHAIN corridor = orientedCorridor( aConn );
    const int              n = corridor.PointCount();

    if( n < 4 )
        return 0.0;

    std::vector<VECTOR2D> dirs;

    for( int i = 1; i < n - 2; i++ )
    {
        VECTOR2D d( corridor.CPoint( i + 1 ) - corridor.CPoint( i ) );

        if( d.EuclideanNorm() > 0 )
            dirs.push_back( d / d.EuclideanNorm() );
    }

    if( dirs.empty() )
        return 0.0;

    double turn = 0.0;

    for( size_t i = 1; i < dirs.size(); i++ )
        turn += std::atan2( dirs[i - 1].Cross( dirs[i] ), dirs[i - 1].Dot( dirs[i] ) );

    // Nearly straight sketches have no inside
    if( std::abs( turn ) < M_PI / 6 )
        return 0.0;

    // How far the ends sit toward the side the sketch turns to
    const VECTOR2D startOffset( corridor.CPoint( 0 ) - corridor.CPoint( 1 ) );
    const VECTOR2D endOffset( corridor.CPoint( n - 1 ) - corridor.CPoint( n - 2 ) );
    const double   lateral = dirs.front().Cross( startOffset ) + dirs.back().Cross( endOffset );

    return turn > 0 ? lateral : -lateral;
}


bool SKETCH_ROUTER::routePass( NODE* aNode, const std::vector<SKETCH_CONNECTION>& aConnections,
                               const std::vector<int>& aOrder, std::vector<uint8_t>& aStatus,
                               double& aCost, int& aVias, long long& aLength )
{
    // In a bundle the pads sit closer to each other than the tracks will, so an early route can
    // easily run across the front of a pad whose turn is still to come.  Reserve every bundle
    // pad's straight exit with a stub of its own net until its connection is routed.
    std::vector<std::vector<SEGMENT*>> reserved( aConnections.size() );
    ROUTER_IFACE*                      iface = Router()->GetInterface();

    for( int idx : aOrder )
    {
        const SKETCH_CONNECTION& conn = aConnections[idx];
        SIZES_SETTINGS           sizes( Router()->Sizes() );

        if( m_bundleOf[idx] < 0 || !conn.m_start
                || !iface->ImportSizes( sizes, conn.m_start, conn.m_net, conn.m_startPos ) )
        {
            continue;
        }

        const int width = std::max( 1, sizes.TrackWidth() );

        auto reserve =
                [&]( const ITEM* aPad, const VECTOR2I& aPos, const VECTOR2I& aToward )
                {
                    if( !aPad || aPad->Layers().Start() != aPad->Layers().End() )
                        return;

                    const SKETCH_EXIT* best = nullptr;
                    double             bestDot = -2.0;
                    const VECTOR2D     toward = VECTOR2D( aToward - aPos ).Resize( 1.0 );
                    std::vector<SKETCH_EXIT> exits = PadExits( aPad, aPos, width, m_orthogonal, true );

                    for( const SKETCH_EXIT& exit : exits )
                    {
                        double dot = VECTOR2D( DX[exit.m_dir], DY[exit.m_dir] ).Resize( 1.0 ).Dot( toward )
                                     - double( exit.m_penalty ) / 1e6;

                        if( dot > bestDot )
                        {
                            bestDot = dot;
                            best = &exit;
                        }
                    }

                    if( !best )
                        return;

                    const int steps = (int) std::ceil( best->m_length / dirLength( best->m_dir ) );
                    const VECTOR2I tip = aPos + VECTOR2I( DX[best->m_dir], DY[best->m_dir] ) * steps;

                    std::unique_ptr<SEGMENT> stub = std::make_unique<SEGMENT>( SEG( aPos, tip ), conn.m_net );
                    stub->SetWidth( width );
                    stub->SetLayer( aPad->Layers().Start() );

                    reserved[idx].push_back( stub.get() );
                    aNode->Add( std::move( stub ), true );
                };

        reserve( conn.m_start, conn.m_startPos, conn.m_endPos );
        reserve( conn.m_end, conn.m_endPos, conn.m_startPos );
    }

    auto release =
            [&]( int aIdx )
            {
                for( SEGMENT* stub : reserved[aIdx] )
                    aNode->Remove( stub );

                reserved[aIdx].clear();
            };

    int  done = 0;
    bool completed = true;

    m_progressTotal = (int) aOrder.size();

    for( int idx : aOrder )
    {
        m_progressDone = done;

        if( m_progressCallback && !m_progressCallback( done, (int) aOrder.size() ) )
            m_stats.m_cancelled = true;

        if( cancelled() )
        {
            completed = false;
            break;
        }

        double cost = 0.0;
        bool   detoured = false;
        int    bundle = m_bundleOf[idx];

        release( idx );
        activateBundle( bundle );

        // Keep out of the lanes of the bundle members still to be routed
        m_guardPlus = false;
        m_guardMinus = false;

        if( bundle >= 0 )
        {
            for( int member : m_bundles[bundle].m_members )
            {
                if( member == idx || aStatus[member] != ROUTE_FAILED )
                    continue;

                m_guardPlus |= m_laneOffsets[member] > m_laneOffsets[idx];
                m_guardMinus |= m_laneOffsets[member] < m_laneOffsets[idx];
            }
        }

        if( routeConnection( aNode, aConnections[idx],
                             bundle >= 0 ? (int) m_bundles[bundle].m_members.size() : 1,
                             m_laneOffsets[idx], cost, aVias, aLength, detoured ) )
        {
            aStatus[idx] = detoured ? ROUTE_DETOURED : ROUTE_OK;
            aCost += cost;
        }

        done++;
    }

    for( size_t i = 0; i < reserved.size(); i++ )
        release( (int) i );

    if( m_progressCallback && completed )
        m_progressCallback( done, (int) aOrder.size() );

    return completed;
}


std::vector<SKETCH_EXIT> SKETCH_ROUTER::PadExits( const ITEM* aItem, const VECTOR2I& aAnchor,
                                                  int aWidth, bool aOrthogonal, bool aStrict )
{
    std::vector<SKETCH_EXIT> exits;

    if( !aItem || !aItem->OfKind( ITEM::SOLID_T ) || !aItem->Parent()
            || aItem->Parent()->Type() != PCB_PAD_T )
    {
        return exits;
    }

    const PAD*   pad = static_cast<const PAD*>( aItem->Parent() );
    PCB_LAYER_ID layer = pad->IsOnLayer( F_Cu ) ? F_Cu
                         : pad->IsOnLayer( B_Cu ) ? B_Cu
                                                  : pad->GetPrincipalLayer();

    const VECTOR2I size = pad->GetSize( layer );
    const double   hx = size.x / 2.0;
    const double   hy = size.y / 2.0;

    if( hx <= 0 || hy <= 0 )
        return exits;

    const PAD_SHAPE shape = pad->GetShape( layer );
    const bool      round = shape == PAD_SHAPE::CIRCLE
                            || ( shape == PAD_SHAPE::OVAL && size.x == size.y );
    const double    aspect = std::max( hx, hy ) / std::min( hx, hy );

    VECTOR2D ux( 1.0, 0.0 );
    VECTOR2D uy( 0.0, 1.0 );
    RotatePoint( ux, pad->GetOrientation() );
    RotatePoint( uy, pad->GetOrientation() );

    const VECTOR2D longAxis = hx >= hy ? ux : uy;
    const VECTOR2D local( aAnchor - pad->ShapePos( layer ) );

    // Direction from the middle of the footprint out to this pad
    VECTOR2D   outward( 0, 0 );
    FOOTPRINT* footprint = pad->GetParentFootprint();

    if( footprint && footprint->Pads().size() >= 2 )
    {
        VECTOR2D centroid( 0, 0 );

        for( PAD* other : footprint->Pads() )
            centroid += VECTOR2D( other->GetPosition() );

        centroid = centroid / (double) footprint->Pads().size();
        outward = VECTOR2D( pad->GetPosition() ) - centroid;

        if( outward.EuclideanNorm() < std::min( hx, hy ) )
            outward = VECTOR2D( 0, 0 );
    }

    for( int d = 0; d < 8; d++ )
    {
        if( aOrthogonal && ( d & 1 ) )
            continue;

        const VECTOR2D dir = VECTOR2D( DX[d], DY[d] ).Resize( 1.0 );
        bool           allowed = true;
        int            penalty = 0;

        if( aStrict )
        {
            // Square to an edge; the side of an elongated pad is allowed, but costs extra so
            // the route only uses it when leaving from the end would take it a long way round
            if( round )
                allowed = true;
            else
                allowed = std::abs( dir.Dot( ux ) ) > 0.92 || std::abs( dir.Dot( uy ) ) > 0.92;

            if( !round && aspect > 1.25 && std::abs( dir.Dot( longAxis ) ) <= 0.92 )
                penalty = 2000000;

            // Never back under the part
            if( outward.EuclideanNorm() > 0 && dir.Dot( outward.Resize( 1.0 ) ) < -0.5 )
                allowed = false;
        }

        if( !allowed )
            continue;

        // Distance from the anchor to the pad edge along the exit
        double edge;

        if( round )
        {
            edge = hx + local.EuclideanNorm();
        }
        else
        {
            const double px = local.Dot( ux );
            const double py = local.Dot( uy );
            const double dx = dir.Dot( ux );
            const double dy = dir.Dot( uy );
            edge = std::numeric_limits<double>::max();

            if( std::abs( dx ) > 1e-9 )
                edge = std::min( edge, ( ( dx > 0 ? hx : -hx ) - px ) / dx );

            if( std::abs( dy ) > 1e-9 )
                edge = std::min( edge, ( ( dy > 0 ? hy : -hy ) - py ) / dy );

            edge = std::max( 0.0, edge );
        }

        // Clear the edge by the track width so the corner after the exit keeps off the pad.  Half
        // that is enough to keep the track off it, and lets a route that comes in a hair off its
        // lane meet the exit without a jog.
        exits.push_back( { d, int( std::ceil( edge ) ) + aWidth, penalty, int( std::ceil( edge ) ) + aWidth / 2 } );
    }

    // A pad turned to an odd angle may have no axis along the grid; let it out any way then
    if( exits.empty() && aStrict )
        return PadExits( aItem, aAnchor, aWidth, aOrthogonal, false );

    return exits;
}


bool SKETCH_ROUTER::routeConnection( NODE* aNode, const SKETCH_CONNECTION& aConn, int aBundleSize,
                                     double aLaneOffset, double& aCost, int& aVias,
                                     long long& aLength, bool& aDetoured )
{
    ROUTER_IFACE* iface = Router()->GetInterface();

    aDetoured = false;

    if( !aConn.m_start || !aConn.m_end )
        return false;

    SIZES_SETTINGS sizes( Router()->Sizes() );

    if( !iface->ImportSizes( sizes, aConn.m_start, aConn.m_net, aConn.m_startPos ) )
        return false;

    sizes.SetViaType( VIATYPE::THROUGH );

    const int width = std::max( 1, sizes.TrackWidth() );
    const int clearance = std::max( sizes.Clearance(), sizes.MinClearance() );

    // A through via spans the whole board, as the line placer's do
    const PNS_LAYER_RANGE viaLayers( iface->GetPNSLayerFromBoardLayer( F_Cu ),
                                     iface->GetPNSLayerFromBoardLayer( B_Cu ) );

    VIA viaProto( VECTOR2I( 0, 0 ), viaLayers, sizes.ViaDiameter(), sizes.ViaDrill(), aConn.m_net,
                  VIATYPE::THROUGH );

    const int viaDiameter = viaProto.Diameter( viaProto.Layers().Start() );

    // Work out which layers the route may use and which ones it can leave and enter on
    std::vector<int> layers = m_layers;

    auto flashedLayers =
            [&]( ITEM* aItem )
            {
                std::vector<int> result;

                for( int l = aItem->Layers().Start(); l <= aItem->Layers().End(); l++ )
                {
                    if( !iface->IsPNSCopperLayer( l ) )
                        continue;

                    if( aItem->OfKind( ITEM::SOLID_T | ITEM::VIA_T )
                            && !iface->IsFlashedOnLayer( aItem, l ) )
                    {
                        continue;
                    }

                    result.push_back( l );
                }

                return result;
            };

    std::vector<int> startLayers = flashedLayers( aConn.m_start );
    std::vector<int> endLayers = flashedLayers( aConn.m_end );

    auto ensureReachable =
            [&]( const std::vector<int>& aItemLayers )
            {
                for( int l : aItemLayers )
                {
                    if( std::find( layers.begin(), layers.end(), l ) != layers.end() )
                        return;
                }

                // None of the routing layers touch the item; allow the ones it is on
                layers.insert( layers.end(), aItemLayers.begin(), aItemLayers.end() );
                std::sort( layers.begin(), layers.end() );
                layers.erase( std::unique( layers.begin(), layers.end() ), layers.end() );
            };

    ensureReachable( startLayers );
    ensureReachable( endLayers );

    if( startLayers.empty() || endLayers.empty() || layers.empty() )
        return false;

    std::vector<bool> startMask( layers.size(), false );
    std::vector<bool> endMask( layers.size(), false );
    int               preferred = -1;

    for( size_t k = 0; k < layers.size(); k++ )
    {
        startMask[k] = std::find( startLayers.begin(), startLayers.end(), layers[k] ) != startLayers.end();
        endMask[k] = std::find( endLayers.begin(), endLayers.end(), layers[k] ) != endLayers.end();

        if( layers[k] == m_options.m_preferredLayer )
            preferred = (int) k;
    }

    // Without a preferred layer, stay on the layer of a single layer pad at either end
    if( preferred < 0 )
    {
        for( const std::vector<int>* padLayers : { &startLayers, &endLayers } )
        {
            if( preferred < 0 && padLayers->size() == 1 )
            {
                auto it = std::find( layers.begin(), layers.end(), padLayers->front() );

                if( it != layers.end() )
                    preferred = (int) ( it - layers.begin() );
            }
        }
    }

    const VECTOR2I& s = aConn.m_startPos;
    const VECTOR2I& t = aConn.m_endPos;
    const bool      guided = m_guide.PointCount() >= 2;

    if( s == t )
        return false;

    // Ways out of the start pad and into the end pad; the relaxed ones allow any direction
    const std::vector<SKETCH_EXIT> strictStart = PadExits( aConn.m_start, s, width, m_orthogonal, true );
    const std::vector<SKETCH_EXIT> strictEnd = PadExits( aConn.m_end, t, width, m_orthogonal, true );
    const std::vector<SKETCH_EXIT> relaxedStart = PadExits( aConn.m_start, s, width, m_orthogonal, false );
    const std::vector<SKETCH_EXIT> relaxedEnd = PadExits( aConn.m_end, t, width, m_orthogonal, false );

    const int minSegment = ( width + clearance ) / 8;

    // Short connections often need nothing more than a straight exit and a direct trace
    if( !guided )
    {
        for( size_t k = 0; k < layers.size(); k++ )
        {
            if( !startMask[k] || !endMask[k] )
                continue;

            std::vector<VECTOR2I> starts;   // where each straight exit ends

            if( strictStart.empty() )
            {
                starts.push_back( s );
            }
            else
            {
                for( const SKETCH_EXIT& exit : strictStart )
                {
                    int steps = (int) std::ceil( exit.m_length / dirLength( exit.m_dir ) );
                    starts.push_back( s + VECTOR2I( DX[exit.m_dir], DY[exit.m_dir] ) * steps );
                }
            }

            SHAPE_LINE_CHAIN best;
            double           bestLength = std::numeric_limits<double>::max();
            double           bestScore = std::numeric_limits<double>::max();

            for( const VECTOR2I& from : starts )
            {
                for( const SHAPE_LINE_CHAIN& approach : approachPaths( from, t, strictEnd, m_orthogonal,
                                                                       minSegment ) )
                {
                    SHAPE_LINE_CHAIN path;
                    path.Append( s );

                    for( int i = 0; i < approach.PointCount(); i++ )
                        path.Append( approach.CPoint( i ) );

                    path = simplifyExact( path );

                    if( path.PointCount() < 2 || !isRouteShapeValid( path )
                            || !exitsRespected( path, &strictStart, &strictEnd ) )
                    {
                        continue;
                    }

                    // Exits off the side of a pad count against the route
                    auto penalty =
                            []( const std::vector<SKETCH_EXIT>& aExits, const SEG& aOut )
                            {
                                const int dir = directionOf( aOut.B - aOut.A );

                                for( const SKETCH_EXIT& exit : aExits )
                                {
                                    if( exit.m_dir == dir )
                                        return exit.m_penalty;
                                }

                                return 0;
                            };

                    const SEG    last = path.CSegment( -1 );
                    const double score = path.Length() + penalty( strictStart, path.CSegment( 0 ) )
                                         + penalty( strictEnd, SEG( last.B, last.A ) );

                    if( score >= bestScore )
                        continue;

                    LINE line;
                    line.SetShape( path );
                    line.SetWidth( width );
                    line.SetLayer( layers[k] );
                    line.SetNet( aConn.m_net );

                    m_stats.m_collisionChecks++;

                    if( aNode->CheckColliding( &line ) )
                        continue;

                    best = path;
                    bestScore = score;
                    bestLength = path.Length();
                }
            }

            if( best.PointCount() >= 2 )
            {
                std::vector<SHAPE_LINE_CHAIN> runs = { best };
                std::vector<int>              runLayers = { layers[k] };

                if( commitPath( aNode, aConn, width, clearance, runs, runLayers, {}, viaProto,
                                strictStart, strictEnd, sizes.GetHoleToHole(), aVias, aLength ) )
                {
                    aCost = bestLength;
                    return true;
                }
            }
        }
    }

    // Routing grid: two steps make one track pitch, so parallel tracks pack tightly.  Within a
    // bundle lane the grid is twice as fine, which keeps neighbouring tracks close together.
    const int pitch = std::max( 1000, ( width + clearance ) / 2 );
    const int finePitch = std::max( 1000, ( width + clearance ) / 4 );

    int endRadius = 0;
    {
        const SHAPE* shape = aConn.m_end->Shape( endLayers.front() );
        BOX2I        bbox = shape ? shape->BBox() : BOX2I( t, VECTOR2I( 0, 0 ) );
        VECTOR2I     farthestCorner(
                std::max( std::abs( bbox.GetLeft() - t.x ), std::abs( bbox.GetRight() - t.x ) ),
                std::max( std::abs( bbox.GetTop() - t.y ), std::abs( bbox.GetBottom() - t.y ) ) );

        endRadius = int( farthestCorner.EuclideanNorm() ) + 2 * ( width + clearance );
    }

    int startRadius = 0;
    {
        const SHAPE* shape = aConn.m_start->Shape( startLayers.front() );
        BOX2I        bbox = shape ? shape->BBox() : BOX2I( s, VECTOR2I( 0, 0 ) );
        VECTOR2I     farthestCorner(
                std::max( std::abs( bbox.GetLeft() - s.x ), std::abs( bbox.GetRight() - s.x ) ),
                std::max( std::abs( bbox.GetTop() - s.y ), std::abs( bbox.GetBottom() - s.y ) ) );

        startRadius = int( farthestCorner.EuclideanNorm() ) + 2 * ( width + clearance );
    }

    // The guide corridor holds the whole bundle, so it gets wider with more connections.  Each
    // connection also has its own lane in the bundle, a narrower corridor offset from the sketch.
    SHAPE_LINE_CHAIN corridor;
    int              corridorHalfWidth = 0;
    SHAPE_LINE_CHAIN lane;
    int              laneHalfWidth = 0;
    int              laneStartRoom = 0;
    int              laneEndRoom = 0;
    bool             plusIsRight = true;

    if( guided )
    {
        corridor = orientedCorridor( aConn );
        corridorHalfWidth = m_options.m_guideHalfWidth;

        if( corridorHalfWidth <= 0 )
        {
            corridorHalfWidth = std::max( 4 * ( width + clearance ),
                                          aBundleSize * ( width + clearance ) / 2
                                                  + 2 * ( width + clearance ) )
                                + viaDiameter + clearance;
        }

        if( m_lanePitch > 0 )
        {
            lane = laneCorridor( aConn, aLaneOffset, strictStart, strictEnd, laneStartRoom,
                                 laneEndRoom );
            m_stats.m_lanes.push_back( lane );

            if( wxLog::IsAllowedTraceMask( wxT( "PNS_SKETCH_LANES" ) ) )
            {
                wxString pts;

                for( int i = 0; i < lane.PointCount(); i++ )
                    pts += wxString::Format( wxT( " (%.3f,%.3f)" ), lane.CPoint( i ).x / 1e6, lane.CPoint( i ).y / 1e6 );

                wxLogTrace( wxT( "PNS_SKETCH_LANES" ), wxT( "lane:%s" ), pts );
            }
            laneHalfWidth = std::max( m_lanePitch * 3 / 4, 3 * finePitch );

            // Which side of this lane the lanes with larger offsets are on
            const SHAPE_LINE_CHAIN own = orientedCorridor( aConn, aLaneOffset );
            const SHAPE_LINE_CHAIN plus = orientedCorridor( aConn, aLaneOffset + m_lanePitch );

            if( own.SegmentCount() >= 1 && plus.PointCount() >= 2 )
            {
                const VECTOR2I probe = plus.PointAlong( int( plus.Length() / 2 ) );
                const SEG      seg = own.CSegment( std::max( 0, own.NearestSegment( probe ) ) );
                const VECTOR2L dir( seg.B - seg.A );
                const VECTOR2L rel( probe - seg.A );

                // y grows downward, so a positive cross product is on the right
                plusIsRight = dir.x * rel.y - dir.y * rel.x > 0;
            }
        }
    }

    BOX2I span( s, VECTOR2I( 0, 0 ) );
    span.Merge( t );

    if( guided )
    {
        span.Merge( corridor.BBox() );

        if( lane.PointCount() )
            span.Merge( lane.BBox() );
    }

    // A bundle keeps to one layer; it only changes layer close to the pads at either end
    const int viaZone = guided ? std::max( 3000000, endRadius + 4 * ( viaDiameter + clearance ) ) : 0;

    struct ATTEMPT
    {
        int                             m_margin;
        const SHAPE_LINE_CHAIN*         m_corridor;
        int                             m_halfWidth;
        bool                            m_fine;
        bool                            m_viaZone;
        const std::vector<SKETCH_EXIT>* m_startExits;
        const std::vector<SKETCH_EXIT>* m_endExits;
    };

    const int baseMargin = std::max( { 3 * ( viaDiameter + clearance ),
                                       int( std::max( span.GetWidth(), span.GetHeight() ) / 4 ),
                                       15 * pitch, endRadius + 4 * pitch } );
    const int wholeBoard = std::numeric_limits<int>::max() / 4;

    std::vector<ATTEMPT> attempts;

    if( guided )
    {
        if( lane.PointCount() )
            attempts.push_back( { baseMargin, &lane, laneHalfWidth, true, true, &strictStart, &strictEnd } );

        attempts.push_back( { baseMargin, &corridor, corridorHalfWidth, false, true, &strictStart, &strictEnd } );
        attempts.push_back( { baseMargin, &corridor, corridorHalfWidth, false, true, &relaxedStart, &relaxedEnd } );
        attempts.push_back( { baseMargin * 3, nullptr, 0, false, false, &strictStart, &strictEnd } );
        attempts.push_back( { wholeBoard, nullptr, 0, false, false, &relaxedStart, &relaxedEnd } );
    }
    else
    {
        attempts.push_back( { baseMargin, nullptr, 0, false, false, &strictStart, &strictEnd } );
        attempts.push_back( { baseMargin * 4, nullptr, 0, false, false, &strictStart, &strictEnd } );
        attempts.push_back( { baseMargin * 4, nullptr, 0, false, false, &relaxedStart, &relaxedEnd } );
        attempts.push_back( { wholeBoard, nullptr, 0, false, false, &relaxedStart, &relaxedEnd } );
    }

    std::vector<std::tuple<BOX2I, const SHAPE_LINE_CHAIN*, bool>> tried;

    // A free search that ran out of room without touching its window's edge has seen all it
    // can reach; a larger window won't find anything more with the same exits
    bool strictExhausted = false;
    bool relaxedExhausted = false;

    for( const ATTEMPT& attempt : attempts )
    {
        if( cancelled() )
            return false;

        BOX2I window = span;
        window.Inflate( std::min( attempt.m_margin, 2000000000 / 4 ) );

        if( m_options.m_boundary.GetWidth() > 0 )
        {
            BOX2I boundary = m_options.m_boundary;
            boundary.Inflate( pitch );

            // Parts left outside the outline would clip the window down to nothing
            if( window.Intersects( boundary ) )
            {
                window = window.Intersect( boundary );
                window.Merge( s );
                window.Merge( t );
            }
        }

        // Relaxed exits are the same as strict ones for anything but pads
        const bool strict = *attempt.m_startExits == strictStart && *attempt.m_endExits == strictEnd;
        auto       key = std::make_tuple( window, attempt.m_corridor, strict );

        if( std::find( tried.begin(), tried.end(), key ) != tried.end() )
            continue;

        if( !attempt.m_corridor && ( strict ? strictExhausted : relaxedExhausted ) )
            continue;

        tried.push_back( key );

        // Coarsen the grid if the window is too large to search at full resolution
        int    attemptPitch = attempt.m_fine ? finePitch : pitch;
        size_t cells = GRID_SEARCH::CellCount( window, attemptPitch ) * layers.size();

        if( cells > (size_t) m_options.m_maxCells )
            attemptPitch = int( attemptPitch * std::sqrt( double( cells ) / m_options.m_maxCells ) ) + 1;

        GRID_SEARCH::PARAMS params;
        params.m_node = aNode;
        params.m_net = aConn.m_net;
        params.m_width = width;
        params.m_pitch = attemptPitch;
        params.m_viaProto = &viaProto;
        params.m_layers = layers;
        params.m_startLayers = startMask;
        params.m_endLayers = endMask;
        params.m_startExits = *attempt.m_startExits;
        params.m_endExits = *attempt.m_endExits;
        params.m_start = s;
        params.m_end = t;
        params.m_endRadius = std::clamp( endRadius, 3 * attemptPitch, 25 * attemptPitch );
        params.m_minViaSpacing = viaDiameter + clearance;
        params.m_holeToHole = sizes.GetHoleToHole();
        params.m_viaZoneRadius = attempt.m_viaZone ? viaZone : 0;
        params.m_preferredLayer = preferred;
        params.m_offLayerCost = guided ? 0.5 : 0.15;
        params.m_window = window;
        params.m_corridor = attempt.m_corridor;
        params.m_corridorHalfWidth = attempt.m_halfWidth;
        params.m_centerCost = attempt.m_corridor == &lane ? 3.0 : 0.0;

        if( attempt.m_corridor == &lane )
        {
            params.m_corridorStartRoom = laneStartRoom;
            params.m_corridorEndRoom = laneEndRoom;
            params.m_guardRight = plusIsRight ? m_guardPlus : m_guardMinus;
            params.m_guardLeft = plusIsRight ? m_guardMinus : m_guardPlus;
        }
        params.m_orthogonal = m_orthogonal;
        // A route in its bundle lane stays on one layer, unless its pads aren't both on it
        params.m_allowVias = m_options.m_allowVias
                             && ( attempt.m_corridor != &lane || preferred < 0
                                  || !startMask[preferred] || !endMask[preferred] );
        params.m_viaCost = m_options.m_viaCost / attemptPitch;
        params.m_bendCost = m_options.m_bendCost;
        params.m_wrongWayCost = guided || preferred >= 0 ? 0.0 : m_options.m_wrongWayCost;
        // A greedy search would drift across its lane to save a few expansions and crowd the
        // lane beside it; lanes are narrow enough to search properly
        params.m_heuristicWeight = attempt.m_corridor == &lane ? 1.0 : m_options.m_heuristicWeight;
        params.m_maxExpansions = m_options.m_maxExpansions;
        params.m_deadline = m_deadline;
        params.m_keepGoing = [this]() { return keepGoing(); };

        // The endpoint allowance has to reach past the straight part of the exits
        for( const SKETCH_EXIT& exit : params.m_endExits )
            params.m_endRadius = std::max( params.m_endRadius, exit.m_length + 4 * attemptPitch );

        GRID_SEARCH     search( params, m_stats );
        const int64_t   attemptStart = nowMs();
        const long long expansionsBefore = m_stats.m_expansions;

        if( !search.EndReachable() )
        {
            wxLogTrace( wxT( "PNS_SKETCH" ), wxT( "  %s: end unreachable (window %d x %d, pitch %d)" ),
                        iface->GetNetName( aConn.m_net ), (int) window.GetWidth(),
                        (int) window.GetHeight(), attemptPitch );
            continue;
        }

        GRID_SEARCH::RESULT result;
        GRID_SEARCH::STATUS status = GRID_SEARCH::STATUS::BUDGET;

        // A congested end often can't be reached at all, and finding that out from the start
        // floods everything the start can reach.  A short search backwards from the end tells
        // quickly, and often simply finds the route.
        if( !attempt.m_corridor )
        {
            GRID_SEARCH::PARAMS back = params;
            std::swap( back.m_start, back.m_end );
            std::swap( back.m_startLayers, back.m_endLayers );
            std::swap( back.m_startExits, back.m_endExits );
            back.m_endRadius = std::clamp( startRadius, 3 * attemptPitch, 25 * attemptPitch );

            for( const SKETCH_EXIT& exit : back.m_endExits )
                back.m_endRadius = std::max( back.m_endRadius, exit.m_length + 4 * attemptPitch );

            back.m_maxExpansions = std::min( params.m_maxExpansions, 30000 );

            GRID_SEARCH         backSearch( back, m_stats );
            GRID_SEARCH::RESULT backResult;

            // The probe starts from grid lines through the end, so failing to find a way into
            // the start from them doesn't mean the forward search will fail too
            GRID_SEARCH::STATUS backStatus = backSearch.EndReachable() ? backSearch.Run( backResult )
                                                                       : GRID_SEARCH::STATUS::BUDGET;

            if( backStatus == GRID_SEARCH::STATUS::FOUND )
            {
                // Turn it around
                std::reverse( backResult.m_runs.begin(), backResult.m_runs.end() );
                std::reverse( backResult.m_runLayers.begin(), backResult.m_runLayers.end() );
                std::reverse( backResult.m_vias.begin(), backResult.m_vias.end() );

                for( SHAPE_LINE_CHAIN& run : backResult.m_runs )
                    run = run.Reverse();

                result = backResult;
                status = backStatus;
            }
            else if( backStatus == GRID_SEARCH::STATUS::UNREACHABLE && !backSearch.HitWindowEdge() )
            {
                status = backStatus;
                ( strict ? strictExhausted : relaxedExhausted ) = true;
            }
        }

        if( status == GRID_SEARCH::STATUS::BUDGET )
            status = search.Run( result );

        wxLogTrace( wxT( "PNS_SKETCH" ),
                    wxT( "  %s (%d,%d)->(%d,%d): %s, corridor %d, exits %s, window %d x %d, "
                         "pitch %d, %lld expansions, %lld ms" ),
                    iface->GetNetName( aConn.m_net ), s.x, s.y, t.x, t.y,
                    status == GRID_SEARCH::STATUS::FOUND         ? wxT( "found" )
                    : status == GRID_SEARCH::STATUS::UNREACHABLE ? wxT( "unreachable" )
                                                                 : wxT( "over budget" ),
                    attempt.m_corridor == &lane ? 2 : attempt.m_corridor ? 1 : 0,
                    strict ? wxT( "strict" ) : wxT( "relaxed" ),
                    (int) window.GetWidth(), (int) window.GetHeight(), attemptPitch,
                    m_stats.m_expansions - expansionsBefore, (long long) ( nowMs() - attemptStart ) );

        if( status == GRID_SEARCH::STATUS::UNREACHABLE && !attempt.m_corridor && !search.HitWindowEdge() )
            ( strict ? strictExhausted : relaxedExhausted ) = true;

        if( status != GRID_SEARCH::STATUS::FOUND )
            continue;

        // Keep the cleanups inside the corridor the route was found in.  A lane's fanout is part
        // of its corridor, so the cleanups don't get the extra room around the pads the search
        // had: where a fanout runs a long way, that would let them cut across the sketch.
        m_corridor = attempt.m_corridor;
        m_corridorHalfWidth = attempt.m_halfWidth;
        m_corridorStartRoom = std::max( attempt.m_halfWidth, params.m_endRadius + 2 * attemptPitch );
        m_corridorEndRoom = m_corridorStartRoom;

        bool committed = commitPath( aNode, aConn, width, clearance, result.m_runs,
                                     result.m_runLayers, result.m_vias, viaProto,
                                     *attempt.m_startExits, *attempt.m_endExits,
                                     sizes.GetHoleToHole(), aVias, aLength );
        m_corridor = nullptr;

        if( committed )
        {
            aCost = result.m_cost * attemptPitch;
            aDetoured = guided && !attempt.m_corridor;
            return true;
        }
    }

    return false;
}


bool SKETCH_ROUTER::isRouteShapeValid( const SHAPE_LINE_CHAIN& aChain ) const
{
    if( aChain.PointCount() < 2 )
        return false;

    int prevDir = -1;

    for( int i = 0; i < aChain.SegmentCount(); i++ )
    {
        if( aChain.IsArcSegment( i ) )
        {
            prevDir = -1;
            continue;
        }

        const SEG seg = aChain.CSegment( i );

        if( seg.A == seg.B )
            continue;

        const int dir = directionOf( seg.B - seg.A );

        if( dir < 0 || ( m_orthogonal && ( dir & 1 ) ) )
            return false;

        if( prevDir >= 0 && !turnAllowed( turnSteps( prevDir, dir ), m_orthogonal ) )
            return false;

        prevDir = dir;
    }

    return true;
}


bool SKETCH_ROUTER::insideCorridor( const SHAPE_LINE_CHAIN& aChain ) const
{
    if( !m_corridor )
        return true;

    const SEG::ecoord limit = SEG::Square( m_corridorHalfWidth );
    const SEG::ecoord startLimit = SEG::Square( m_corridorStartRoom );
    const SEG::ecoord endLimit = SEG::Square( m_corridorEndRoom );
    const int         step = std::max( 1000, m_corridorHalfWidth / 2 );

    auto inside =
            [&]( const VECTOR2I& aPt )
            {
                if( ( aPt - m_corridor->CPoint( 0 ) ).SquaredEuclideanNorm() <= startLimit
                        || ( aPt - m_corridor->CLastPoint() ).SquaredEuclideanNorm() <= endLimit )
                {
                    return true;
                }

                for( int i = 0; i < m_corridor->SegmentCount(); i++ )
                {
                    if( m_corridor->CSegment( i ).SquaredDistance( aPt ) <= limit )
                        return true;
                }

                return false;
            };

    for( int i = 0; i < aChain.SegmentCount(); i++ )
    {
        const SEG    seg = aChain.CSegment( i );
        const double len = seg.Length();
        const int    samples = std::max( 1, int( len / step ) );

        for( int j = 0; j <= samples; j++ )
        {
            if( !inside( seg.A + ( seg.B - seg.A ) * j / samples ) )
                return false;
        }
    }

    return true;
}


SHAPE_LINE_CHAIN SKETCH_ROUTER::optimizeRun( NODE* aNode, const SHAPE_LINE_CHAIN& aRaw, int aLayer,
                                             int aWidth, NET_HANDLE aNet,
                                             const std::vector<SKETCH_EXIT>* aStartExits,
                                             const std::vector<SKETCH_EXIT>* aEndExits )
{
    if( !m_options.m_optimize || aRaw.SegmentCount() < 2 )
        return aRaw;

    // Full optimization shortcuts whole stretches of the route.  When that pulls a route out of
    // its corridor or breaks a pad exit, fall back to local cleanups only.  The optimizer's own
    // pad breakouts would override the exits, so they are left out.
    const int local = OPTIMIZER::MERGE_OBTUSE | OPTIMIZER::MERGE_COLINEAR;

    for( int effort : { local | OPTIMIZER::MERGE_SEGMENTS, local } )
    {
        LINE line;
        line.SetShape( aRaw );
        line.SetWidth( aWidth );
        line.SetLayer( aLayer );
        line.SetNet( aNet );

        {
            MITERED_CORNERS_GUARD guard( Router(), m_orthogonal );
            OPTIMIZER::Optimize( &line, effort, aNode );
        }

        if( line.CLine().ArcCount() > 0 )
            continue;

        SHAPE_LINE_CHAIN result = simplifyExact( line.CLine() );

        if( result.PointCount() < 2 || result.CPoint( 0 ) != aRaw.CPoint( 0 )
                || result.CLastPoint() != aRaw.CLastPoint() || !isRouteShapeValid( result )
                || !exitsRespected( result, aStartExits, aEndExits ) || !insideCorridor( result ) )
        {
            continue;
        }

        LINE check( line, result );
        m_stats.m_collisionChecks++;

        if( aNode->CheckColliding( &check ) )
            continue;

        return result;
    }

    return aRaw;
}


SHAPE_LINE_CHAIN SKETCH_ROUTER::roundCorners( NODE* aNode, const SHAPE_LINE_CHAIN& aChain, int aLayer,
                                              int aWidth, int aClearance, NET_HANDLE aNet,
                                              int aStartStraight, int aEndStraight )
{
    if( aChain.PointCount() < 3 )
        return aChain;

    const double target = std::max( 2.0 * aWidth, 1.5 * ( aWidth + aClearance ) );
    const int    last = aChain.PointCount() - 1;

    for( double scale : { 1.0, 0.5, 0.25 } )
    {
        SHAPE_LINE_CHAIN out;
        out.Append( aChain.CPoint( 0 ) );

        for( int i = 1; i < last; i++ )
        {
            const VECTOR2D prev( aChain.CPoint( i - 1 ) );
            const VECTOR2D v( aChain.CPoint( i ) );
            const VECTOR2D next( aChain.CPoint( i + 1 ) );
            const double   lenIn = ( v - prev ).EuclideanNorm();
            const double   lenOut = ( next - v ).EuclideanNorm();

            if( lenIn <= 0 || lenOut <= 0 )
                continue;

            const VECTOR2D uIn = ( v - prev ) / lenIn;
            const VECTOR2D uOut = ( next - v ) / lenOut;
            const double   cross = uIn.x * uOut.y - uIn.y * uOut.x;
            const double   dot = uIn.x * uOut.x + uIn.y * uOut.y;
            const double   theta = std::atan2( std::abs( cross ), dot );

            // Leave straight and reversing corners alone
            if( theta < 0.01 || theta > M_PI / 2 + 0.01 )
            {
                out.Append( aChain.CPoint( i ) );
                continue;
            }

            // Pad exits have to stay straight up to their required length
            const double roomIn = i == 1 ? lenIn - aStartStraight : lenIn / 2.0;
            const double roomOut = i == last - 1 ? lenOut - aEndStraight : lenOut / 2.0;
            const double tangent = std::min( { target * scale, roomIn, roomOut } );
            const double radius = tangent / std::tan( theta / 2.0 );

            if( tangent < 2.0 * SHAPE_ARC::MIN_PRECISION_IU || radius < aWidth / 2.0 )
            {
                out.Append( aChain.CPoint( i ) );
                continue;
            }

            const VECTOR2D p1 = v - uIn * tangent;
            const VECTOR2D p2 = v + uOut * tangent;
            const VECTOR2D normal = cross > 0 ? VECTOR2D( -uIn.y, uIn.x ) : VECTOR2D( uIn.y, -uIn.x );
            const VECTOR2D center = p1 + normal * radius;
            const VECTOR2D chordMid = ( p1 + p2 ) / 2.0;
            const VECTOR2D mid = center + ( chordMid - center ).Resize( radius );

            SHAPE_ARC arc( KiROUND( p1 ), KiROUND( mid ), KiROUND( p2 ), 0 );
            out.Append( arc );
        }

        out.Append( aChain.CLastPoint() );

        LINE line;
        line.SetShape( out );
        line.SetWidth( aWidth );
        line.SetLayer( aLayer );
        line.SetNet( aNet );

        m_stats.m_collisionChecks++;

        if( insideCorridor( out ) && !aNode->CheckColliding( &line ) )
            return out;
    }

    return aChain;
}


bool SKETCH_ROUTER::commitPath( NODE* aNode, const SKETCH_CONNECTION& aConn, int aWidth,
                                int aClearance, const std::vector<SHAPE_LINE_CHAIN>& aRuns,
                                const std::vector<int>& aRunLayers,
                                const std::vector<VECTOR2I>& aVias, const VIA& aViaProto,
                                const std::vector<SKETCH_EXIT>& aStartExits,
                                const std::vector<SKETCH_EXIT>& aEndExits, int aHoleToHole,
                                int& aViaCount, long long& aLength )
{
    std::vector<VIA*> addedVias;

    for( const VECTOR2I& pos : aVias )
    {
        std::unique_ptr<VIA> via = std::make_unique<VIA>( aViaProto );
        via->SetPos( pos );
        via->SetNet( aConn.m_net );

        addedVias.push_back( via.get() );
        aNode->Add( std::move( via ) );
    }

    // The search checked each via against the world, but not against the other vias of this route
    bool ok = true;

    for( VIA* via : addedVias )
    {
        m_stats.m_collisionChecks++;

        if( aNode->CheckColliding( via ) || !viaHolesClear( aNode, *via, aHoleToHole ) )
            ok = false;
    }

    const bool rounded = m_cornerMode == DIRECTION_45::ROUNDED_45
                         || m_cornerMode == DIRECTION_45::ROUNDED_90;

    auto straightLength =
            []( const std::vector<SKETCH_EXIT>* aExits, const SHAPE_LINE_CHAIN& aChain, bool aAtStart )
            {
                if( !aExits || aExits->empty() || aChain.SegmentCount() < 1 )
                    return 0;

                const SEG seg = aAtStart ? aChain.CSegment( 0 ) : aChain.CSegment( -1 );
                const int dir = aAtStart ? directionOf( seg.B - seg.A ) : directionOf( seg.A - seg.B );

                for( const SKETCH_EXIT& exit : *aExits )
                {
                    if( exit.m_dir == dir )
                        return exit.MinLength();
                }

                return 0;
            };

    std::vector<LINE> lines;
    long long         length = 0;

    for( size_t i = 0; i < aRuns.size() && ok; i++ )
    {
        SHAPE_LINE_CHAIN raw = simplifyExact( aRuns[i] );

        if( raw.PointCount() < 2 || raw.Length() == 0 )
            continue;

        // Only the ends of the route are pads; the other run ends are vias
        const std::vector<SKETCH_EXIT>* startExits = raw.CPoint( 0 ) == aConn.m_startPos ? &aStartExits
                                                                                         : nullptr;
        const std::vector<SKETCH_EXIT>* endExits = raw.CLastPoint() == aConn.m_endPos ? &aEndExits
                                                                                      : nullptr;

        SHAPE_LINE_CHAIN shape = optimizeRun( aNode, raw, aRunLayers[i], aWidth, aConn.m_net,
                                              startExits, endExits );

        if( rounded )
        {
            shape = roundCorners( aNode, shape, aRunLayers[i], aWidth, aClearance, aConn.m_net,
                                  straightLength( startExits, shape, true ),
                                  straightLength( endExits, shape, false ) );
        }

        LINE line;
        line.SetShape( shape );
        line.SetWidth( aWidth );
        line.SetLayer( aRunLayers[i] );
        line.SetNet( aConn.m_net );

        m_stats.m_collisionChecks++;

        if( aNode->CheckColliding( &line ) )
        {
            ok = false;
            break;
        }

        length += shape.Length();
        lines.push_back( line );
    }

    if( !ok )
    {
        for( VIA* via : addedVias )
            aNode->Remove( via );

        return false;
    }

    for( LINE& line : lines )
        aNode->Add( line );

    aViaCount += (int) aVias.size();
    aLength += length;
    return true;
}


std::vector<int> SKETCH_ROUTER::RoutableLayers( BOARD* aBoard, ROUTER_IFACE* aIface )
{
    std::vector<int> layers;

    for( PCB_LAYER_ID layer : LSET::AllCuMask( aBoard->GetCopperLayerCount() ).CuStack() )
    {
        if( !aBoard->IsLayerEnabled( layer ) || aBoard->GetLayerType( layer ) == LT_POWER )
            continue;

        layers.push_back( aIface->GetPNSLayerFromBoardLayer( layer ) );
    }

    if( layers.empty() )
    {
        for( int i = 0; i < aBoard->GetCopperLayerCount(); i++ )
            layers.push_back( i );
    }

    std::sort( layers.begin(), layers.end() );
    return layers;
}


std::vector<SKETCH_CONNECTION>
SKETCH_ROUTER::CollectConnections( BOARD* aBoard, NODE* aWorld,
                                   const std::vector<BOARD_CONNECTED_ITEM*>& aItems )
{
    std::set<const BOARD_ITEM*> selected;
    std::set<int>               nets;

    for( BOARD_CONNECTED_ITEM* item : aItems )
    {
        if( !item )
            continue;

        selected.insert( item );

        if( item->GetNetCode() > 0 )
            nets.insert( item->GetNetCode() );
    }

    std::shared_ptr<CONNECTIVITY_DATA> connectivity = aBoard->GetConnectivity();
    std::vector<SKETCH_CONNECTION>     between;
    std::vector<SKETCH_CONNECTION>     touching;

    auto itemSize =
            []( ITEM* aItem )
            {
                const SHAPE* shape = aItem->Shape( aItem->Layers().Start() );
                return shape ? (double) shape->BBox().GetArea() : 0.0;
            };

    for( int netCode : nets )
    {
        RN_NET* net = connectivity->GetRatsnestForNet( netCode );

        if( !net )
            continue;

        for( const CN_EDGE& edge : net->GetEdges() )
        {
            std::shared_ptr<const CN_ANCHOR> source = edge.GetSourceNode();
            std::shared_ptr<const CN_ANCHOR> target = edge.GetTargetNode();

            if( !source || source->Dirty() || !target || target->Dirty() )
                continue;

            if( !source->Valid() || !target->Valid() )
                continue;

            const bool hasSource = selected.count( source->Parent() ) > 0;
            const bool hasTarget = selected.count( target->Parent() ) > 0;

            if( !hasSource && !hasTarget )
                continue;

            ITEM* sourceItem = aWorld->FindItemByParent( source->Parent() );
            ITEM* targetItem = aWorld->FindItemByParent( target->Parent() );

            if( !sourceItem || !targetItem )
                continue;

            SKETCH_CONNECTION conn;
            conn.m_start = sourceItem;
            conn.m_startPos = source->Pos();
            conn.m_end = targetItem;
            conn.m_endPos = target->Pos();
            conn.m_net = source->Parent()->GetNet();

            // Start from the selected end, and between two selected ends from the smaller item.
            // The route leaves its start cleanly on the grid, which matters most at fine pitch.
            bool swap = hasSource && hasTarget ? itemSize( targetItem ) < itemSize( sourceItem )
                                               : !hasSource;

            if( swap )
            {
                std::swap( conn.m_start, conn.m_end );
                std::swap( conn.m_startPos, conn.m_endPos );
            }

            if( hasSource && hasTarget )
                between.push_back( conn );
            else
                touching.push_back( conn );
        }
    }

    return between.empty() ? touching : between;
}

}
