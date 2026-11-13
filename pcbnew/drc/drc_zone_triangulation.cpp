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


/// The fill's outlines and holes, with each point's position along its ring, for asking whether
/// a triangle edge runs along the boundary.
class RINGS
{
public:
    explicit RINGS( const SHAPE_POLY_SET& aFill )
    {
        for( int ii = 0; ii < aFill.OutlineCount(); ++ii )
        {
            add( aFill.COutline( ii ), false );

            for( int jj = 0; jj < aFill.HoleCount( ii ); ++jj )
                add( aFill.CHole( ii, jj ), true );
        }

        m_extent = aFill.BBox();
        m_cells = std::max( 1, (int) std::sqrt( (double) m_segments.size() / 4.0 ) );
        m_cw = std::max( 1.0, (double) m_extent.GetWidth() / m_cells );
        m_ch = std::max( 1.0, (double) m_extent.GetHeight() / m_cells );
        m_grid.resize( (size_t) m_cells * m_cells );

        for( size_t ii = 0; ii < m_segments.size(); ++ii )
        {
            const SEGMENT& s = m_segments[ii];
            int x0, y0, x1, y1;
            cell( std::min( s.a.x, s.b.x ), std::min( s.a.y, s.b.y ), x0, y0 );
            cell( std::max( s.a.x, s.b.x ), std::max( s.a.y, s.b.y ), x1, y1 );

            for( int gx = x0; gx <= x1; ++gx )
            {
                for( int gy = y0; gy <= y1; ++gy )
                    m_grid[(size_t) gy * m_cells + gx].push_back( (int) ii );
            }
        }
    }

    double Area() const { return m_area; }
    double Perimeter() const { return m_perimeter; }
    int    RingCount() const { return (int) m_ringLength.size(); }

    /// A piece of one ring, from ring position @a from forwards to @a to (wrapping round).
    struct ARC
    {
        int    ring;
        double from, to;
    };

    /**
     * Whether the triangle edge from @a aA to @a aB, with its triangle on its left, runs along
     * one ring the way the ring goes with the fill on its left: both ends on the ring, and every
     * ring point between them that way within the tolerance of the edge.  If so, @a aArc gets
     * the piece of ring the edge stands for (the shortest, where an end lies on the ring twice).
     */
    bool OnBoundary( const VECTOR2I& aA, const VECTOR2I& aB, ARC& aArc ) const
    {
        std::vector<std::pair<int, double>> pa = locate( aA );
        std::vector<std::pair<int, double>> pb = locate( aB );
        SEG                                 edge( aA, aB );
        double                              best = -1.0;

        for( const auto& [ringA, sa] : pa )
        {
            for( const auto& [ringB, sb] : pb )
            {
                if( ringA != ringB )
                    continue;

                // Walking forwards along a ring with the fill on its left goes from aA to aB
                double from = m_fillOnLeft[ringA] ? sa : sb;
                double to = m_fillOnLeft[ringA] ? sb : sa;
                double len = m_ringLength[ringA];
                double span = std::fmod( to - from + len, len );

                if( ( best < 0.0 || span < best ) && skippedWithin( ringA, from, to, edge ) )
                {
                    best = span;
                    aArc = { ringA, from, to };
                }
            }
        }

        return best >= 0.0;
    }

    /**
     * Whether @a aArcs cover every ring exactly once, end to end.  A ring no wider than the
     * tolerance may be left out: the triangulator drops it, and it is below DRC's resolution.
     * @param aWhy receives the first ring that is not covered so, if any.
     */
    bool Tiled( std::vector<ARC>& aArcs, wxString* aWhy ) const
    {
        std::sort( aArcs.begin(), aArcs.end(),
                   []( const ARC& a, const ARC& b )
                   {
                       return std::tie( a.ring, a.from ) < std::tie( b.ring, b.from );
                   } );

        size_t next = 0;

        for( int ring = 0; ring < RingCount(); ++ring )
        {
            size_t first = next;

            while( next < aArcs.size() && aArcs[next].ring == ring )
                ++next;

            double len = m_ringLength[ring];
            bool   ok = true;

            if( first == next )
            {
                ok = m_ringExtent[ring] <= TOLERANCE_NM;
            }
            else
            {
                double covered = 0.0;

                for( size_t ii = first; ii < next; ++ii )
                {
                    const ARC& arc = aArcs[ii];
                    const ARC& following = aArcs[ii + 1 < next ? ii + 1 : first];
                    double     gap = std::fmod( following.from - arc.to + 2.0 * len, len );

                    covered += std::fmod( arc.to - arc.from + len, len );

                    if( std::min( gap, len - gap ) > 0.5 )
                        ok = false;
                }

                if( std::abs( covered - len ) > 0.5 * ( next - first ) + 0.5 )
                    ok = false;
            }

            if( !ok )
            {
                if( aWhy )
                {
                    *aWhy = wxString::Format( wxS( "the triangles' boundary edges do not run once round "
                                                   "the ring through (%d, %d)" ),
                                              m_ringPoints[ring][0].x, m_ringPoints[ring][0].y );
                }

                return false;
            }
        }

        return true;
    }

private:
    struct SEGMENT
    {
        VECTOR2I a, b;
        int      ring;
        double   start;     // ring position of a
    };

    void add( const SHAPE_LINE_CHAIN& aChain, bool aHole )
    {
        int    n = aChain.PointCount();
        int    ring = (int) m_ringLength.size();
        double pos = 0.0;
        double area = 0.0;
        BOX2I  extent = aChain.BBox();

        m_ringPoints.emplace_back();
        m_ringPositions.emplace_back();

        for( int ii = 0; ii < n; ++ii )
        {
            const VECTOR2I& a = aChain.CPoint( ii );
            const VECTOR2I& b = aChain.CPoint( ( ii + 1 ) % n );

            m_vertexPositions.emplace( a, std::make_pair( ring, pos ) );
            m_ringPoints.back().push_back( a );
            m_ringPositions.back().push_back( pos );
            m_segments.push_back( { a, b, ring, pos } );
            pos += dist( a, b );
            area += (double) a.x * b.y - (double) b.x * a.y;
        }

        m_ringLength.push_back( std::max( pos, 1.0 ) );
        m_ringExtent.push_back( std::hypot( (double) extent.GetWidth(), (double) extent.GetHeight() ) );
        m_perimeter += pos;

        // A positive shoelace sum turns the same way as a triangle with a positive cross
        // product, so its inside is on the left; the fill is outside a hole
        m_fillOnLeft.push_back( aHole ? area < 0.0 : area > 0.0 );
        m_area += ( aHole ? -1.0 : 1.0 ) * std::abs( area ) / 2.0;
    }

    /// Whether the ring points strictly after position @a aFrom and before @a aTo (walking
    /// forwards, wrapping round) all lie within the tolerance of @a aEdge.
    bool skippedWithin( int aRing, double aFrom, double aTo, const SEG& aEdge ) const
    {
        const std::vector<VECTOR2I>& pts = m_ringPoints[aRing];
        const std::vector<double>&   pos = m_ringPositions[aRing];
        double                       len = m_ringLength[aRing];
        double                       span = std::fmod( aTo - aFrom + len, len );
        size_t                       n = pts.size();

        // The first ring point after aFrom
        size_t start = std::upper_bound( pos.begin(), pos.end(), aFrom + 0.5 ) - pos.begin();
        double limit = (double) TOLERANCE_NM * TOLERANCE_NM;

        for( size_t step = 0; step < n; ++step )
        {
            size_t idx = ( start + step ) % n;
            double along = std::fmod( pos[idx] - aFrom + len, len );

            if( along >= span - 0.5 )
                return true;

            if( (double) aEdge.SquaredDistance( pts[idx] ) > limit )
                return false;
        }

        return true;
    }

    void cell( int aX, int aY, int& aCX, int& aCY ) const
    {
        aCX = std::clamp( (int) ( ( aX - m_extent.GetX() ) / m_cw ), 0, m_cells - 1 );
        aCY = std::clamp( (int) ( ( aY - m_extent.GetY() ) / m_ch ), 0, m_cells - 1 );
    }

    /// Positions along the rings of every place @a aPt lies on (within a nanometre).
    std::vector<std::pair<int, double>> locate( const VECTOR2I& aPt ) const
    {
        std::vector<std::pair<int, double>> found;

        if( auto range = m_vertexPositions.equal_range( aPt ); range.first != range.second )
        {
            for( auto it = range.first; it != range.second; ++it )
                found.push_back( it->second );

            return found;
        }

        int cx, cy;
        cell( aPt.x, aPt.y, cx, cy );

        for( int idx : m_grid[(size_t) cy * m_cells + cx] )
        {
            const SEGMENT& s = m_segments[idx];
            SEG            seg( s.a, s.b );

            if( seg.SquaredDistance( aPt ) <= 1 )
            {
                double t = std::min( dist( s.a, aPt ), dist( s.a, s.b ) );
                found.emplace_back( s.ring, s.start + t );
            }
        }

        return found;
    }

    std::unordered_multimap<VECTOR2I, std::pair<int, double>, POINT_HASH> m_vertexPositions;
    std::vector<SEGMENT>               m_segments;
    std::vector<std::vector<VECTOR2I>> m_ringPoints;
    std::vector<std::vector<double>>   m_ringPositions;
    std::vector<double>                m_ringLength;
    std::vector<double>                m_ringExtent;
    std::vector<bool>                  m_fillOnLeft;
    std::vector<std::vector<int>> m_grid;
    BOX2I                         m_extent;
    int                           m_cells = 1;
    double                        m_cw = 1.0;
    double                        m_ch = 1.0;
    double                        m_perimeter = 0.0;
    double                        m_area = 0.0;
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
    RINGS rings( aFill );

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

    // ... and those pieces must go once round every ring
    return rings.Tiled( arcs, aWhy );
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
