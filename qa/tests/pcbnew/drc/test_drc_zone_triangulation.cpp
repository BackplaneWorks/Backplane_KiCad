/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * The DRC zone guard (DRC_ZONE_TRIANGULATION::Validate) on triangulations given to it directly,
 * so these hold whatever the triangulator of the day produces.
 */

#include <filesystem>

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>

#include <drc/drc_zone_triangulation.h>
#include <geometry/shape_poly_set.h>


namespace
{
using DRC_ZONE_TRIANGULATION::TRIANGLE;

SHAPE_LINE_CHAIN ring( std::initializer_list<VECTOR2I> aPoints )
{
    SHAPE_LINE_CHAIN chain;

    for( const VECTOR2I& pt : aPoints )
        chain.Append( pt );

    chain.SetClosed( true );
    return chain;
}

std::vector<TRIANGLE> cachedTriangles( SHAPE_POLY_SET& aPoly )
{
    std::vector<TRIANGLE> triangles;
    aPoly.CacheTriangulation( false );

    for( unsigned ii = 0; ii < aPoly.TriangulatedPolyCount(); ++ii )
    {
        const SHAPE_POLY_SET::TRIANGULATED_POLYGON* tp = aPoly.TriangulatedPolygon( ii );

        for( size_t jj = 0; jj < tp->GetTriangleCount(); ++jj )
        {
            TRIANGLE& t = triangles.emplace_back();
            tp->GetTriangle( (int) jj, t[0], t[1], t[2] );
        }
    }

    return triangles;
}

constexpr int MM = 1000000;
} // namespace


BOOST_AUTO_TEST_SUITE( DRCZoneTriangulation )


/**
 * The GND fill of demos/complex_hierarchy on B.Cu, as the triangulator left it before the guard
 * re-triangulated it (saved with KICAD_DRC_DUMP_TRIANGULATION).  Its triangles miss copper:
 * DRC on it measured a foreign track inside the fill at (100.5, 100.9) mm as 0.1174 mm away
 * and, under 0.1 mm rules, passed the short.  The guard must reject it on its own, without
 * relying on what DRC then reports.
 */
BOOST_AUTO_TEST_CASE( ComplexHierarchyGndFillFails )
{
    std::filesystem::path path = std::filesystem::path( KI_TEST::GetPcbnewTestDataDir() )
                                 / "drc_zone_triangulation" / "complex_hierarchy_gnd_bcu.tri";

    SHAPE_POLY_SET                     fill;
    std::vector<std::vector<TRIANGLE>> triangles;

    BOOST_REQUIRE( DRC_ZONE_TRIANGULATION::Load( path.string(), fill, triangles ) );
    BOOST_REQUIRE( !triangles.empty() );

    wxString why;
    BOOST_CHECK( !DRC_ZONE_TRIANGULATION::Validate( fill, triangles, &why ) );
    BOOST_TEST_MESSAGE( "rejected: " << why.ToStdString() );

    // The deepest point of the copper the saved triangles miss (0.1 mm inside it) lies in the
    // fill but in none of them
    VECTOR2I probe( 101598447, 100841940 );
    BOOST_CHECK( fill.Contains( probe ) );

    bool covered = false;

    for( const std::vector<TRIANGLE>& polygon : triangles )
    {
        for( const TRIANGLE& t : polygon )
        {
            SHAPE_LINE_CHAIN tri = ring( { t[0], t[1], t[2] } );
            covered = covered || tri.PointInside( probe, 0, true );
        }
    }

    BOOST_CHECK( !covered );
}


/// The same fill, triangulated afresh, is a tiling the guard accepts (so the check above fails
/// for what the saved triangles do, not for anything about the fill).
BOOST_AUTO_TEST_CASE( ComplexHierarchyGndFillRetriangulatedPasses )
{
    std::filesystem::path path = std::filesystem::path( KI_TEST::GetPcbnewTestDataDir() )
                                 / "drc_zone_triangulation" / "complex_hierarchy_gnd_bcu.tri";

    SHAPE_POLY_SET                     fill;
    std::vector<std::vector<TRIANGLE>> saved;

    BOOST_REQUIRE( DRC_ZONE_TRIANGULATION::Load( path.string(), fill, saved ) );

    SHAPE_POLY_SET fresh( fill );
    fresh.Simplify();

    wxString why;
    BOOST_CHECK_MESSAGE( DRC_ZONE_TRIANGULATION::Validate( fresh, { cachedTriangles( fresh ) }, &why ),
                         why.ToStdString() );
}


/// A plain square with a hole, triangulated by the triangulator, passes.
BOOST_AUTO_TEST_CASE( SquareWithHolePasses )
{
    SHAPE_POLY_SET fill;
    fill.AddOutline( ring( { { 0, 0 }, { 10 * MM, 0 }, { 10 * MM, 10 * MM }, { 0, 10 * MM } } ) );
    fill.AddHole( ring( { { 2 * MM, 2 * MM }, { 2 * MM, 4 * MM }, { 4 * MM, 2 * MM } } ) );

    wxString why;
    BOOST_CHECK_MESSAGE( DRC_ZONE_TRIANGULATION::Validate( fill, { cachedTriangles( fill ) }, &why ),
                         why.ToStdString() );
}


/**
 * Copper missed where the area adds up: a whole island of the fill has no triangles, and a
 * triangle over a hole of the same area (2 mm2) makes up the total.  Every triangle edge is
 * shared or on the outline, so edge pairing and the area test alone pass it; the guard must
 * not, since the island is missed copper.
 */
BOOST_AUTO_TEST_CASE( MissingIslandOffsetByCoveredHoleFails )
{
    SHAPE_LINE_CHAIN square = ring( { { 0, 0 }, { 10 * MM, 0 }, { 10 * MM, 10 * MM }, { 0, 10 * MM } } );
    SHAPE_LINE_CHAIN hole = ring( { { 2 * MM, 2 * MM }, { 2 * MM, 4 * MM }, { 4 * MM, 2 * MM } } );
    SHAPE_LINE_CHAIN island = ring( { { 20 * MM, 0 }, { 21 * MM, 0 }, { 21 * MM, 2 * MM }, { 20 * MM, 2 * MM } } );

    SHAPE_POLY_SET fill;
    fill.AddOutline( square );
    fill.AddHole( hole );
    fill.AddOutline( island );

    SHAPE_POLY_SET annulus;
    annulus.AddOutline( square );
    annulus.AddHole( hole );

    std::vector<TRIANGLE> triangles = cachedTriangles( annulus );
    triangles.push_back( { VECTOR2I( 2 * MM, 2 * MM ), VECTOR2I( 4 * MM, 2 * MM ), VECTOR2I( 2 * MM, 4 * MM ) } );

    BOOST_REQUIRE_CLOSE( fill.Area(), 98.0 * MM * MM + 2.0 * MM * MM, 1e-9 );

    wxString why;
    BOOST_CHECK( !DRC_ZONE_TRIANGULATION::Validate( fill, { triangles }, &why ) );
    BOOST_TEST_MESSAGE( "rejected: " << why.ToStdString() );
}


/// A triangle missing from the middle of a tiling leaves edges that are neither shared nor on
/// the outline.
BOOST_AUTO_TEST_CASE( MissingTriangleFails )
{
    SHAPE_POLY_SET fill;
    fill.AddOutline( ring( { { 0, 0 }, { 10 * MM, 0 }, { 10 * MM, 10 * MM }, { 0, 10 * MM } } ) );
    fill.AddHole( ring( { { 2 * MM, 2 * MM }, { 2 * MM, 4 * MM }, { 4 * MM, 2 * MM } } ) );

    std::vector<TRIANGLE> triangles = cachedTriangles( fill );
    BOOST_REQUIRE( triangles.size() > 3 );

    // Its neighbours' edges facing it lose their partner, whichever triangle it is
    triangles.erase( triangles.begin() + triangles.size() / 2 );

    BOOST_CHECK( !DRC_ZONE_TRIANGULATION::Validate( fill, { triangles } ) );
}


BOOST_AUTO_TEST_SUITE_END()
