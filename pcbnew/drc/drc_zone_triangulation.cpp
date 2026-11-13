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
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <drc/drc_zone_triangulation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <tuple>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <board.h>
#include <geometry/shape_poly_set.h>
#include <wx/log.h>
#include <zone.h>


namespace
{
// The triangulator drops outline points closer than 50 nm to their neighbour; what it leaves out
// is thinner than this, and below anything DRC resolves.
constexpr double TOLERANCE_NM = 100.0;

struct POINT_HASH
{
    size_t operator()( const VECTOR2I& aPt ) const
    {
        return std::hash<long long>()( ( (long long) aPt.x << 32 ) ^ (unsigned int) aPt.y );
    }
};

struct EDGE_HASH
{
    size_t operator()( const std::pair<VECTOR2I, VECTOR2I>& aEdge ) const
    {
        return POINT_HASH()( aEdge.first ) * 31 + POINT_HASH()( aEdge.second );
    }
};

double cross( const VECTOR2I& aO, const VECTOR2I& aA, const VECTOR2I& aB )
{
    return (double) ( aA.x - aO.x ) * ( aB.y - aO.y ) - (double) ( aA.y - aO.y ) * ( aB.x - aO.x );
}

double dist( const VECTOR2I& aA, const VECTOR2I& aB )
{
    return std::hypot( (double) ( aB.x - aA.x ), (double) ( aB.y - aA.y ) );
}


/// The fill's outlines and holes, for asking whether a triangle edge runs along the boundary
/// and whether the boundary edges add up to the outline.
class RINGS
{
public:
    RINGS( const SHAPE_POLY_SET& aFill, const std::vector<std::vector<DRC_ZONE_TRIANGULATION::TRIANGLE>>& aTriangles )
    {
        std::vector<std::vector<VECTOR2I>> rings;
        std::vector<bool>                  holes;

        auto take =
                [&]( const SHAPE_LINE_CHAIN& aChain, bool aHole )
                {
                    std::vector<VECTOR2I>& pts = rings.emplace_back();
                    pts.reserve( aChain.PointCount() );

                    for( int ii = 0; ii < aChain.PointCount(); ++ii )
                        pts.push_back( aChain.CPoint( ii ) );

                    holes.push_back( aHole );
                };

        for( int ii = 0; ii < aFill.OutlineCount(); ++ii )
        {
            take( aFill.COutline( ii ), false );

            for( int jj = 0; jj < aFill.HoleCount( ii ); ++jj )
                take( aFill.CHole( ii, jj ), true );
        }

        insertSplits( rings, aTriangles );

        for( size_t ii = 0; ii < rings.size(); ++ii )
            add( rings[ii], holes[ii] );
    }

    double Area() const { return m_area; }
    double Perimeter() const { return m_perimeter; }

    /// The ring points from @a from forwards to @a to (wrapping round) on one ring.
    struct ARC
    {
        int ring;
        int from, to;
    };

    /**
     * Whether the triangle edge from @a aA to @a aB, with its triangle on its left, runs along
     * one ring the way the ring goes with the fill on its left: both ends are points of the
     * ring, and every ring point between them that way lies within the tolerance of the edge.
     * If so, @a aArc gets the piece of ring the edge stands for (the shortest, where an end is
     * on the ring more than once).
     */
    bool OnBoundary( const VECTOR2I& aA, const VECTOR2I& aB, ARC& aArc ) const
    {
        auto   rangeA = m_vertexIndex.equal_range( aA );
        auto   rangeB = m_vertexIndex.equal_range( aB );
        SEG    edge( aA, aB );
        double best = -1.0;

        for( auto ia = rangeA.first; ia != rangeA.second; ++ia )
        {
            for( auto ib = rangeB.first; ib != rangeB.second; ++ib )
            {
                const auto& [ringA, idxA] = ia->second;
                const auto& [ringB, idxB] = ib->second;

                if( ringA != ringB )
                    continue;

                // Walking forwards along a ring with the fill on its left goes from aA to aB
                int    from = m_fillOnLeft[ringA] ? idxA : idxB;
                int    to = m_fillOnLeft[ringA] ? idxB : idxA;
                double len = m_ringLength[ringA];
                double span = std::fmod( m_positions[ringA][to] - m_positions[ringA][from] + len, len );

                if( from != to && ( best < 0.0 || span < best ) && skippedWithin( ringA, from, to, edge ) )
                {
                    best = span;
                    aArc = { ringA, from, to };
                }
            }
        }

        return best >= 0.0;
    }

    /**
     * Whether the pieces of ring @a aArcs stand for add up to the fill's outlines and holes:
     * every ring segment, taken the way that has the fill on its left, is covered as many times
     * as it occurs.  Segments occurring both ways (the zero-width bridges joining a hole to its
     * outline in a fractured fill) add up to nothing, so they may be covered both ways or not
     * at all.  A ring no wider than the tolerance may be left out: it is below DRC's resolution.
     * @param aWhy receives where they do not add up, if they do not.
     */
    bool AddUpToOutline( const std::vector<ARC>& aArcs, wxString* aWhy ) const
    {
        std::vector<std::vector<int>> cover( m_points.size() );
        size_t                        walked = 0;
        size_t                        limit = 4 * m_segmentCount + 16;

        for( size_t ring = 0; ring < m_points.size(); ++ring )
            cover[ring].assign( m_points[ring].size(), 0 );

        for( const ARC& arc : aArcs )
        {
            int n = (int) m_points[arc.ring].size();

            for( int kk = arc.from; kk != arc.to; kk = ( kk + 1 ) % n )
            {
                cover[arc.ring][kk]++;

                // Arcs that cover the outline many times over cannot add up to it
                if( ++walked > limit )
                    return fail( m_points[arc.ring][kk], aWhy );
            }
        }

        // For each segment, the times it is covered less the times it occurs, both counted the
        // same way along it; only segments not covered exactly once can leave anything over
        std::unordered_map<std::pair<VECTOR2I, VECTOR2I>, int, EDGE_HASH> excess;

        for( size_t ring = 0; ring < m_points.size(); ++ring )
        {
            if( m_ringExtent[ring] <= TOLERANCE_NM )
                continue;

            const std::vector<VECTOR2I>& pts = m_points[ring];
            int                          n = (int) pts.size();

            for( int kk = 0; kk < n; ++kk )
            {
                const VECTOR2I& p = pts[kk];
                const VECTOR2I& q = pts[( kk + 1 ) % n];

                if( p == q || cover[ring][kk] == 1 )
                    continue;

                bool ordered = std::tie( p.x, p.y ) < std::tie( q.x, q.y );
                int  sign = ( m_fillOnLeft[ring] ? 1 : -1 ) * ( ordered ? 1 : -1 );
                auto key = ordered ? std::make_pair( p, q ) : std::make_pair( q, p );

                excess[key] += sign * ( cover[ring][kk] - 1 );
            }
        }

        for( const auto& [segment, count] : excess )
        {
            if( count != 0 )
                return fail( segment.first, aWhy );
        }

        return true;
    }

private:
    static bool fail( const VECTOR2I& aAt, wxString* aWhy )
    {
        if( aWhy )
        {
            *aWhy = wxString::Format( wxS( "the triangles' boundary edges do not add up to the outline "
                                           "at (%d, %d)" ),
                                      aAt.x, aAt.y );
        }

        return false;
    }

    /// Make each triangle vertex that lies on a ring segment but is not a ring point (the
    /// triangulator splits large polygons, adding points along their outline) a ring point, so
    /// that every boundary edge runs between ring points.
    static void insertSplits( std::vector<std::vector<VECTOR2I>>& aRings,
                              const std::vector<std::vector<DRC_ZONE_TRIANGULATION::TRIANGLE>>& aTriangles )
    {
        std::unordered_set<VECTOR2I, POINT_HASH> ringPoints;
        std::unordered_set<VECTOR2I, POINT_HASH> extra;
        size_t                                   segments = 0;

        for( const std::vector<VECTOR2I>& pts : aRings )
        {
            ringPoints.insert( pts.begin(), pts.end() );
            segments += pts.size();
        }

        for( const auto& polygon : aTriangles )
        {
            for( const auto& t : polygon )
            {
                for( const VECTOR2I& pt : t )
                {
                    if( !ringPoints.count( pt ) )
                        extra.insert( pt );
                }
            }
        }

        if( extra.empty() || segments == 0 )
            return;

        // A grid of the ring segments, to find those an extra point lies on
        BOX2I extent;

        for( const std::vector<VECTOR2I>& pts : aRings )
        {
            for( const VECTOR2I& pt : pts )
                extent.Merge( pt );
        }

        int    cells = std::max( 1, (int) std::sqrt( (double) segments / 4.0 ) );
        double cw = std::max( 1.0, (double) extent.GetWidth() / cells );
        double ch = std::max( 1.0, (double) extent.GetHeight() / cells );

        auto cell =
                [&]( long long aX, long long aY, int& aCX, int& aCY )
                {
                    aCX = std::clamp( (int) ( ( aX - extent.GetX() ) / cw ), 0, cells - 1 );
                    aCY = std::clamp( (int) ( ( aY - extent.GetY() ) / ch ), 0, cells - 1 );
                };

        std::vector<std::vector<std::pair<int, int>>> grid( (size_t) cells * cells );

        for( int ring = 0; ring < (int) aRings.size(); ++ring )
        {
            const std::vector<VECTOR2I>& pts = aRings[ring];
            int                          n = (int) pts.size();

            for( int kk = 0; kk < n; ++kk )
            {
                const VECTOR2I& a = pts[kk];
                const VECTOR2I& b = pts[( kk + 1 ) % n];
                int             x0, y0, x1, y1;
                cell( std::min( a.x, b.x ) - 1LL, std::min( a.y, b.y ) - 1LL, x0, y0 );
                cell( std::max( a.x, b.x ) + 1LL, std::max( a.y, b.y ) + 1LL, x1, y1 );

                for( int gx = x0; gx <= x1; ++gx )
                {
                    for( int gy = y0; gy <= y1; ++gy )
                        grid[(size_t) gy * cells + gx].emplace_back( ring, kk );
                }
            }
        }

        // Points to insert in each segment, with their distance from its start
        std::map<std::pair<int, int>, std::vector<std::pair<double, VECTOR2I>>> inserts;

        for( const VECTOR2I& pt : extra )
        {
            int cx, cy;
            cell( pt.x, pt.y, cx, cy );

            for( const auto& [ring, kk] : grid[(size_t) cy * cells + cx] )
            {
                const std::vector<VECTOR2I>& pts = aRings[ring];
                const VECTOR2I&              a = pts[kk];
                const VECTOR2I&              b = pts[( kk + 1 ) % pts.size()];

                if( SEG( a, b ).SquaredDistance( pt ) <= 1 )
                    inserts[{ ring, kk }].emplace_back( dist( a, pt ), pt );
            }
        }

        for( auto it = inserts.rbegin(); it != inserts.rend(); ++it )
        {
            auto& [where, points] = *it;
            std::vector<VECTOR2I>& pts = aRings[where.first];

            std::sort( points.begin(), points.end(),
                       []( const auto& a, const auto& b )
                       {
                           return std::tie( a.first, a.second.x, a.second.y )
                                  < std::tie( b.first, b.second.x, b.second.y );
                       } );

            std::vector<VECTOR2I> add;

            for( const auto& [d, pt] : points )
                add.push_back( pt );

            // Segments are visited from the last, so earlier indices stay valid
            pts.insert( pts.begin() + where.second + 1, add.begin(), add.end() );
        }
    }

    void add( const std::vector<VECTOR2I>& aPoints, bool aHole )
    {
        int    n = (int) aPoints.size();
        int    ring = (int) m_points.size();
        double pos = 0.0;
        double area = 0.0;
        BOX2I  extent;

        m_points.push_back( aPoints );
        m_positions.emplace_back();

        for( int ii = 0; ii < n; ++ii )
        {
            const VECTOR2I& a = aPoints[ii];
            const VECTOR2I& b = aPoints[( ii + 1 ) % n];

            extent.Merge( a );
            m_vertexIndex.emplace( a, std::make_pair( ring, ii ) );
            m_positions.back().push_back( pos );
            pos += dist( a, b );
            area += (double) a.x * b.y - (double) b.x * a.y;
        }

        m_segmentCount += n;
        m_ringLength.push_back( std::max( pos, 1.0 ) );
        m_ringExtent.push_back( std::hypot( (double) extent.GetWidth(), (double) extent.GetHeight() ) );
        m_perimeter += pos;

        // A positive shoelace sum turns the same way as a triangle with a positive cross
        // product, so its inside is on the left; the fill is outside a hole
        m_fillOnLeft.push_back( aHole ? area < 0.0 : area > 0.0 );
        m_area += ( aHole ? -1.0 : 1.0 ) * std::abs( area ) / 2.0;
    }

    /// Whether the ring points strictly between indices @a aFrom and @a aTo (walking forwards,
    /// wrapping round) all lie within the tolerance of @a aEdge.
    bool skippedWithin( int aRing, int aFrom, int aTo, const SEG& aEdge ) const
    {
        const std::vector<VECTOR2I>& pts = m_points[aRing];
        int                          n = (int) pts.size();
        double                       limit = (double) TOLERANCE_NM * TOLERANCE_NM;

        for( int idx = ( aFrom + 1 ) % n; idx != aTo; idx = ( idx + 1 ) % n )
        {
            if( (double) aEdge.SquaredDistance( pts[idx] ) > limit )
                return false;
        }

        return true;
    }

    std::unordered_multimap<VECTOR2I, std::pair<int, int>, POINT_HASH> m_vertexIndex;
    std::vector<std::vector<VECTOR2I>> m_points;
    std::vector<std::vector<double>>   m_positions;
    std::vector<double>                m_ringLength;
    std::vector<double>                m_ringExtent;
    std::vector<bool>                  m_fillOnLeft;
    size_t                             m_segmentCount = 0;
    double                             m_perimeter = 0.0;
    double                             m_area = 0.0;
};


struct RESULT
{
    const SHAPE_POLY_SET*           fill;
    bool                            trusted;
    std::shared_ptr<SHAPE_POLY_SET> replacement;  // a validated re-triangulation, if one was found
};

std::shared_mutex&                                             registryMutex()
{
    static std::shared_mutex mutex;
    return mutex;
}

std::map<std::pair<const ZONE*, PCB_LAYER_ID>, RESULT>& registry()
{
    static std::map<std::pair<const ZONE*, PCB_LAYER_ID>, RESULT> results;
    return results;
}

std::set<std::pair<wxString, wxString>>& logged()
{
    static std::set<std::pair<wxString, wxString>> once;
    return once;
}

} // namespace


bool DRC_ZONE_TRIANGULATION::Validate( const SHAPE_POLY_SET& aFill, wxString* aWhy )
{
    std::vector<std::vector<TRIANGLE>> triangles( aFill.TriangulatedPolyCount() );

    for( unsigned ii = 0; ii < aFill.TriangulatedPolyCount(); ++ii )
    {
        const SHAPE_POLY_SET::TRIANGULATED_POLYGON* tp = aFill.TriangulatedPolygon( ii );

        triangles[ii].resize( tp->GetTriangleCount() );

        for( size_t jj = 0; jj < tp->GetTriangleCount(); ++jj )
        {
            TRIANGLE& t = triangles[ii][jj];
            tp->GetTriangle( (int) jj, t[0], t[1], t[2] );
        }
    }

    return Validate( aFill, triangles, aWhy );
}


bool DRC_ZONE_TRIANGULATION::Validate( const SHAPE_POLY_SET& aFill,
                                       const std::vector<std::vector<TRIANGLE>>& aTriangles,
                                       wxString* aWhy )
{
    RINGS rings( aFill, aTriangles );

    // A fill thinner than the triangulator resolves (a degenerate sliver) has nothing to cover
    if( rings.Area() <= rings.Perimeter() * TOLERANCE_NM / 2.0 + 1e6 )
        return true;

    // Directed triangle edges as (from x, from y, to x, to y), sorted below for pairing
    std::vector<std::array<int, 4>> edges;
    double triArea = 0.0;

    for( const std::vector<TRIANGLE>& polygon : aTriangles )
    {
        int turn = 0;

        for( TRIANGLE t : polygon )
        {
            double area2 = cross( t[0], t[1], t[2] );

            // A zero-area triangle covers nothing and its edges add up to nothing
            if( area2 == 0.0 )
                continue;

            // Not needed for the guarantee (below), but a triangulator that turns one triangle
            // of a polygon the other way from the rest has gone wrong
            int sign = area2 > 0.0 ? 1 : -1;

            if( turn == 0 )
            {
                turn = sign;
            }
            else if( sign != turn )
            {
                if( aWhy )
                    *aWhy = wxString::Format( wxS( "triangle (%d, %d) (%d, %d) (%d, %d) is folded over "
                                                   "its neighbours" ),
                                              t[0].x, t[0].y, t[1].x, t[1].y, t[2].x, t[2].y );

                return false;
            }

            // Counter-clockwise from here on: each triangle is on the left of its edges
            if( area2 < 0.0 )
                std::swap( t[1], t[2] );

            triArea += std::abs( area2 ) / 2.0;

            for( int kk = 0; kk < 3; ++kk )
            {
                const VECTOR2I& from = t[kk];
                const VECTOR2I& to = t[( kk + 1 ) % 3];
                edges.push_back( { from.x, from.y, to.x, to.y } );
            }
        }
    }

    // Also not needed for the guarantee; a cheap first test that catches most failures
    if( std::abs( triArea - rings.Area() ) > rings.Perimeter() * TOLERANCE_NM / 2.0 + 1e6 )
    {
        if( aWhy )
            *aWhy = wxString::Format( wxS( "triangles cover %.6f mm2 of a %.6f mm2 fill" ),
                                      triArea / 1e12, rings.Area() / 1e12 );

        return false;
    }

    // Edges a triangle shares, in the opposite direction, with another cancel out; every other
    // edge must run along the outline (RINGS::OnBoundary) and stands for the piece of outline
    // it runs along
    std::sort( edges.begin(), edges.end() );

    std::vector<RINGS::ARC> arcs;

    for( size_t ii = 0; ii < edges.size(); )
    {
        size_t                    jj = ii;
        const std::array<int, 4>& edge = edges[ii];

        while( jj < edges.size() && edges[jj] == edge )
            ++jj;

        int                count = (int) ( jj - ii );
        std::array<int, 4> reverseKey = { edge[2], edge[3], edge[0], edge[1] };
        auto               range = std::equal_range( edges.begin(), edges.end(), reverseKey );
        int                reverseCount = (int) ( range.second - range.first );
        VECTOR2I           from( edge[0], edge[1] );
        VECTOR2I           to( edge[2], edge[3] );
        RINGS::ARC         arc;

        ii = jj;

        if( count == reverseCount )
            continue;

        if( count == 1 && reverseCount == 0 && rings.OnBoundary( from, to, arc ) )
        {
            arcs.push_back( arc );
            continue;
        }

        if( reverseCount > count )
            continue;   // reported from the other direction

        if( aWhy )
        {
            *aWhy = wxString::Format( wxS( "triangle edge (%d, %d)-(%d, %d) is neither shared nor on "
                                           "the outline the right way round" ),
                                      from.x, from.y, to.x, to.y );
        }

        return false;
    }

    // ... and those pieces must add up to the outline
    return rings.AddUpToOutline( arcs, aWhy );
}


void DRC_ZONE_TRIANGULATION::Check( const ZONE* aZone, PCB_LAYER_ID aLayer )
{
    std::shared_ptr<SHAPE_POLY_SET> fill = aZone->GetFilledPolysList( aLayer );

    if( !fill )
        return;

    wxString why;
    bool     trusted = fill->OutlineCount() == 0 || Validate( *fill, &why );

    // The same copper, unioned (which drops zero-width bridges and redundant points) and
    // triangulated afresh, usually triangulates correctly; that keeps the zone indexed by
    // triangles instead of falling back to its outline
    std::shared_ptr<SHAPE_POLY_SET> replacement;

    if( !trusted )
    {
        replacement = std::make_shared<SHAPE_POLY_SET>( *fill );
        replacement->ClearArcs();
        replacement->Simplify();
        replacement->CacheTriangulation( false );

        if( !Validate( *replacement ) )
            replacement.reset();
    }

    {
        std::unique_lock lock( registryMutex() );
        registry()[{ aZone, aLayer }] = { fill.get(), trusted, replacement };
    }

    if( trusted )
        return;

    if( const char* dir = std::getenv( "KICAD_DRC_DUMP_TRIANGULATION" ) )
    {
        wxString path = wxString::Format( wxS( "%s/%s-%s.tri" ), wxString::FromUTF8( dir ),
                                          aZone->m_Uuid.AsString(), LayerName( aLayer ) );
        Dump( *fill, path );
    }

    const BOARD* board = aZone->GetBoard();
    wxString     boardName = board ? board->GetFileName() : wxString();
    wxString     zoneId = aZone->m_Uuid.AsString() + wxS( "/" ) + LayerName( aLayer );

    {
        std::unique_lock lock( registryMutex() );

        if( !logged().insert( { boardName, zoneId } ).second )
            return;
    }

    wxString msg = wxString::Format( wxS( "DRC: the fill triangulation of zone %s (%s) on %s in %s "
                                          "failed validation (%s); its clearances are checked against "
                                          "%s instead." ),
                                     aZone->m_Uuid.AsString(), aZone->GetNetname(), LayerName( aLayer ),
                                     boardName, why,
                                     replacement ? wxS( "a re-triangulation of the fill" )
                                                 : wxS( "the fill outline" ) );

    // Once per zone and board, on stderr (kicad-cli, the API server) and as a trace; not as a
    // log dialog, since this runs on DRC worker threads
    std::fprintf( stderr, "%s\n", msg.ToStdString().c_str() );
    wxLogTrace( wxS( "KICAD_DRC_ZONE_GUARD" ), msg );
}


bool DRC_ZONE_TRIANGULATION::Trusted( const ZONE* aZone, PCB_LAYER_ID aLayer )
{
    std::shared_ptr<SHAPE_POLY_SET> fill = aZone->GetFilledPolysList( aLayer );
    std::shared_lock                lock( registryMutex() );
    auto                            it = registry().find( { aZone, aLayer } );

    // Unchecked (outside a DRC run), or the fill was replaced since it was checked
    if( it == registry().end() || it->second.fill != fill.get() )
        return true;

    return it->second.trusted;
}


std::shared_ptr<SHAPE_POLY_SET> DRC_ZONE_TRIANGULATION::Replacement( const ZONE* aZone, PCB_LAYER_ID aLayer )
{
    std::shared_ptr<SHAPE_POLY_SET> fill = aZone->GetFilledPolysList( aLayer );
    std::shared_lock                lock( registryMutex() );
    auto                            it = registry().find( { aZone, aLayer } );

    if( it == registry().end() || it->second.fill != fill.get() )
        return nullptr;

    return it->second.replacement;
}


bool DRC_ZONE_TRIANGULATION::Dump( const SHAPE_POLY_SET& aFill, const wxString& aPath )
{
    std::ofstream out( aPath.fn_str() );

    if( !out )
        return false;

    out << "# zone fill triangulation: rings (outline, then its holes), then triangles\n";

    auto ring =
            [&]( const char* aKind, const SHAPE_LINE_CHAIN& aChain )
            {
                out << "ring " << aKind << " " << aChain.PointCount() << "\n";

                for( int ii = 0; ii < aChain.PointCount(); ++ii )
                    out << aChain.CPoint( ii ).x << " " << aChain.CPoint( ii ).y << "\n";
            };

    for( int ii = 0; ii < aFill.OutlineCount(); ++ii )
    {
        ring( "outline", aFill.COutline( ii ) );

        for( int jj = 0; jj < aFill.HoleCount( ii ); ++jj )
            ring( "hole", aFill.CHole( ii, jj ) );
    }

    for( unsigned ii = 0; ii < aFill.TriangulatedPolyCount(); ++ii )
    {
        const SHAPE_POLY_SET::TRIANGULATED_POLYGON* tp = aFill.TriangulatedPolygon( ii );

        out << "polygon " << tp->GetTriangleCount() << "\n";

        for( size_t jj = 0; jj < tp->GetTriangleCount(); ++jj )
        {
            VECTOR2I a, b, c;
            tp->GetTriangle( (int) jj, a, b, c );
            out << a.x << " " << a.y << " " << b.x << " " << b.y << " " << c.x << " " << c.y << "\n";
        }
    }

    return (bool) out;
}


bool DRC_ZONE_TRIANGULATION::Load( const wxString& aPath, SHAPE_POLY_SET& aFill,
                                   std::vector<std::vector<TRIANGLE>>& aTriangles )
{
    std::ifstream in( aPath.fn_str() );
    std::string   line;

    if( !in )
        return false;

    aFill.RemoveAllContours();
    aTriangles.clear();

    while( std::getline( in, line ) )
    {
        std::istringstream words( line );
        std::string        kind;
        words >> kind;

        if( kind == "ring" )
        {
            std::string sub;
            int         count = 0;
            words >> sub >> count;

            SHAPE_LINE_CHAIN chain;

            for( int ii = 0; ii < count && std::getline( in, line ); ++ii )
            {
                std::istringstream xy( line );
                int                x, y;
                xy >> x >> y;
                chain.Append( x, y );
            }

            chain.SetClosed( true );

            if( sub == "outline" )
                aFill.AddOutline( chain );
            else if( aFill.OutlineCount() > 0 )
                aFill.AddHole( chain );
            else
                return false;
        }
        else if( kind == "polygon" )
        {
            size_t count = 0;
            words >> count;

            std::vector<TRIANGLE>& polygon = aTriangles.emplace_back();

            for( size_t ii = 0; ii < count && std::getline( in, line ); ++ii )
            {
                std::istringstream v( line );
                TRIANGLE           t;
                v >> t[0].x >> t[0].y >> t[1].x >> t[1].y >> t[2].x >> t[2].y;
                polygon.push_back( t );
            }
        }
    }

    return aFill.OutlineCount() > 0;
}
