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
#include <cmath>
#include <cstdio>
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
            add( aFill.COutline( ii ) );

            for( int jj = 0; jj < aFill.HoleCount( ii ); ++jj )
                add( aFill.CHole( ii, jj ) );
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
    const std::vector<VECTOR2I>& Vertices() const { return m_vertices; }

    /// Whether the edge from @a aA to @a aB runs along one ring: both ends on it, and every ring
    /// point between them (one way round) within the tolerance of the edge.  So the strip between
    /// a boundary edge and the outline is never wider than the tolerance.
    bool OnBoundary( const VECTOR2I& aA, const VECTOR2I& aB ) const
    {
        std::vector<std::pair<int, double>> pa = locate( aA );
        std::vector<std::pair<int, double>> pb = locate( aB );
        SEG                                 edge( aA, aB );

        for( const auto& [ringA, sa] : pa )
        {
            for( const auto& [ringB, sb] : pb )
            {
                if( ringA != ringB )
                    continue;

                for( bool forward : { true, false } )
                {
                    if( skippedWithin( ringA, forward ? sa : sb, forward ? sb : sa, edge ) )
                        return true;
                }
            }
        }

        return false;
    }

private:
    struct SEGMENT
    {
        VECTOR2I a, b;
        int      ring;
        double   start;     // ring position of a
    };

    void add( const SHAPE_LINE_CHAIN& aChain )
    {
        int    n = aChain.PointCount();
        int    ring = (int) m_ringLength.size();
        double pos = 0.0;
        double area = 0.0;

        m_ringPoints.emplace_back();
        m_ringPositions.emplace_back();

        for( int ii = 0; ii < n; ++ii )
        {
            const VECTOR2I& a = aChain.CPoint( ii );
            const VECTOR2I& b = aChain.CPoint( ( ii + 1 ) % n );

            m_vertices.push_back( a );
            m_ringPoints.back().push_back( a );
            m_ringPositions.back().push_back( pos );
            m_segments.push_back( { a, b, ring, pos } );
            pos += dist( a, b );
            area += (double) a.x * b.y - (double) b.x * a.y;
        }

        m_ringLength.push_back( std::max( pos, 1.0 ) );
        m_perimeter += pos;
        // Outlines are counter-clockwise and holes clockwise (or the reverse): the signed sum
        // gives the filled area either way
        m_signedArea += area / 2.0;
        m_area = std::abs( m_signedArea );
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

    std::vector<SEGMENT>               m_segments;
    std::vector<std::vector<VECTOR2I>> m_ringPoints;
    std::vector<std::vector<double>>   m_ringPositions;
    std::vector<double>                m_ringLength;
    std::vector<VECTOR2I>         m_vertices;
    std::vector<std::vector<int>> m_grid;
    BOX2I                         m_extent;
    int                           m_cells = 1;
    double                        m_cw = 1.0;
    double                        m_ch = 1.0;
    double                        m_perimeter = 0.0;
    double                        m_signedArea = 0.0;
    double                        m_area = 0.0;
};


struct TRIANGLE
{
    VECTOR2I a, b, c;
};


bool vertexInsideTriangles( const std::vector<TRIANGLE>& aTriangles, const RINGS& aRings,
                            const BOX2I& aExtent, wxString* aWhy )
{
    int    cells = std::max( 1, (int) std::sqrt( (double) aTriangles.size() ) );
    double cw = std::max( 1.0, (double) aExtent.GetWidth() / cells );
    double ch = std::max( 1.0, (double) aExtent.GetHeight() / cells );
    std::vector<std::vector<int>> grid( (size_t) cells * cells );

    auto cellOf =
            [&]( int x, int y, int& gx, int& gy )
            {
                gx = std::clamp( (int) ( ( x - aExtent.GetX() ) / cw ), 0, cells - 1 );
                gy = std::clamp( (int) ( ( y - aExtent.GetY() ) / ch ), 0, cells - 1 );
            };

    for( size_t ii = 0; ii < aTriangles.size(); ++ii )
    {
        const TRIANGLE& t = aTriangles[ii];
        int x0, y0, x1, y1;
        cellOf( std::min( { t.a.x, t.b.x, t.c.x } ), std::min( { t.a.y, t.b.y, t.c.y } ), x0, y0 );
        cellOf( std::max( { t.a.x, t.b.x, t.c.x } ), std::max( { t.a.y, t.b.y, t.c.y } ), x1, y1 );

        for( int gx = x0; gx <= x1; ++gx )
        {
            for( int gy = y0; gy <= y1; ++gy )
                grid[(size_t) gy * cells + gx].push_back( (int) ii );
        }
    }

    for( const VECTOR2I& v : aRings.Vertices() )
    {
        int gx, gy;
        cellOf( v.x, v.y, gx, gy );

        for( int idx : grid[(size_t) gy * cells + gx] )
        {
            const TRIANGLE& t = aTriangles[idx];

            // Triangles are counter-clockwise here: inside means left of every edge
            double d1 = cross( t.a, t.b, v ) / std::max( 1.0, dist( t.a, t.b ) );
            double d2 = cross( t.b, t.c, v ) / std::max( 1.0, dist( t.b, t.c ) );
            double d3 = cross( t.c, t.a, v ) / std::max( 1.0, dist( t.c, t.a ) );

            if( std::min( { d1, d2, d3 } ) > TOLERANCE_NM )
            {
                if( aWhy )
                    *aWhy = wxString::Format( wxS( "outline point (%d, %d) inside a triangle" ), v.x, v.y );

                return true;
            }
        }
    }

    return false;
}


struct RESULT
{
    const SHAPE_POLY_SET* fill;
    bool                  trusted;
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
    RINGS rings( aFill );

    // A fill thinner than the triangulator resolves (a degenerate sliver) has nothing to cover
    if( rings.Area() <= rings.Perimeter() * TOLERANCE_NM / 2.0 + 1e6 )
        return true;

    std::vector<TRIANGLE> triangles;
    std::unordered_map<std::pair<VECTOR2I, VECTOR2I>, int, EDGE_HASH> edges;
    double triArea = 0.0;

    for( unsigned ii = 0; ii < aFill.TriangulatedPolyCount(); ++ii )
    {
        const SHAPE_POLY_SET::TRIANGULATED_POLYGON* tp = aFill.TriangulatedPolygon( ii );
        int                                         turn = 0;

        for( size_t jj = 0; jj < tp->GetTriangleCount(); ++jj )
        {
            TRIANGLE t;
            tp->GetTriangle( (int) jj, t.a, t.b, t.c );

            double area2 = cross( t.a, t.b, t.c );

            if( area2 == 0.0 )
                continue;

            // A triangle turning the other way from its neighbours is folded over them
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
                                              t.a.x, t.a.y, t.b.x, t.b.y, t.c.x, t.c.y );

                return false;
            }

            // Counter-clockwise from here on, for the edge pairing and inside tests
            if( area2 < 0.0 )
                std::swap( t.b, t.c );

            triArea += std::abs( area2 ) / 2.0;
            triangles.push_back( t );
            edges[{ t.a, t.b }]++;
            edges[{ t.b, t.c }]++;
            edges[{ t.c, t.a }]++;
        }
    }

    // The triangles must cover the fill's area, give or take what the simplification drops
    if( std::abs( triArea - rings.Area() ) > rings.Perimeter() * TOLERANCE_NM / 2.0 + 1e6 )
    {
        if( aWhy )
            *aWhy = wxString::Format( wxS( "triangles cover %.6f mm2 of a %.6f mm2 fill" ),
                                      triArea / 1e12, rings.Area() / 1e12 );

        return false;
    }

    // In a tiling, an edge inside the fill is shared by two triangles in opposite directions and
    // any other edge runs along the outline.  A gap or an overlap leaves an edge that is neither.
    for( const auto& [edge, count] : edges )
    {
        auto reverse = edges.find( { edge.second, edge.first } );
        int  reverseCount = reverse == edges.end() ? 0 : reverse->second;

        if( count == reverseCount )
            continue;

        if( count == 1 && reverseCount == 0 && rings.OnBoundary( edge.first, edge.second ) )
            continue;

        if( reverseCount > count )
            continue;   // reported from the other direction

        if( aWhy )
        {
            *aWhy = wxString::Format( wxS( "triangle edge (%d, %d)-(%d, %d) is neither shared nor on "
                                           "the outline" ),
                                      edge.first.x, edge.first.y, edge.second.x, edge.second.y );
        }

        return false;
    }

    if( vertexInsideTriangles( triangles, rings, aFill.BBox(), aWhy ) )
        return false;

    return true;
}


void DRC_ZONE_TRIANGULATION::Check( const ZONE* aZone, PCB_LAYER_ID aLayer )
{
    std::shared_ptr<SHAPE_POLY_SET> fill = aZone->GetFilledPolysList( aLayer );

    if( !fill )
        return;

    wxString why;
    bool     trusted = fill->OutlineCount() == 0 || Validate( *fill, &why );

    {
        std::unique_lock lock( registryMutex() );
        registry()[{ aZone, aLayer }] = { fill.get(), trusted };
    }

    if( trusted )
        return;

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
                                          "the fill outline instead." ),
                                     aZone->m_Uuid.AsString(), aZone->GetNetname(), LayerName( aLayer ),
                                     boardName, why );

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
