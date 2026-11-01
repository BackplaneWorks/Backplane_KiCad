/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2015-2016 Mario Luzeiro <mrluzeiro@ua.pt>
 * Copyright (C) 2023 CERN
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

#include "render_3d_opengl.h"
#include "../../3d_canvas/eda_3d_canvas.h"
#include "../3d_placeholder_utils.h"
#include <3d_viewer_id.h>
#include <board.h>
#include <footprint.h>
#include <pcb_3d_body.h>
#include <pcb_track.h>
#include "convert_basic_shapes_to_polygon.h"
#include <lset.h>
#include <core/profile.h>        // To use GetRunningMicroSecs or another profiling utility
#include <geometry/shape_poly_set.h>
#include <thread_pool.h>
#include <board_structure.h>


void RENDER_3D_OPENGL::addObjectTriangles( const FILLED_CIRCLE_2D*                aCircle,
                                           std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer, float aZtop, float aZbot )
{
    const SFVEC2F& center = aCircle->GetCenter();
    const float radius = aCircle->GetRadius() * 2.0f; // Double because the render triangle

    // This is a small adjustment to the circle texture
    const float texture_factor = ( 8.0f / (float) SIZE_OF_CIRCLE_TEXTURE ) + 1.0f;
    const float f = ( sqrtf( 2.0f ) / 2.0f ) * radius * texture_factor;

    // Top and Bot segments ends are just triangle semi-circles, so need to add it in duplicated.
    aDstLayer->m_layer_top_segment_ends->AddTriangle( SFVEC3F( center.x + f, center.y, aZtop ),
                                                      SFVEC3F( center.x - f, center.y, aZtop ),
                                                      SFVEC3F( center.x, center.y - f, aZtop ) );

    aDstLayer->m_layer_top_segment_ends->AddTriangle( SFVEC3F( center.x - f, center.y, aZtop ),
                                                      SFVEC3F( center.x + f, center.y, aZtop ),
                                                      SFVEC3F( center.x, center.y + f, aZtop ) );

    aDstLayer->m_layer_bot_segment_ends->AddTriangle( SFVEC3F( center.x - f, center.y, aZbot ),
                                                      SFVEC3F( center.x + f, center.y, aZbot ),
                                                      SFVEC3F( center.x, center.y - f, aZbot ) );

    aDstLayer->m_layer_bot_segment_ends->AddTriangle( SFVEC3F( center.x + f, center.y, aZbot ),
                                                      SFVEC3F( center.x - f, center.y, aZbot ),
                                                      SFVEC3F( center.x, center.y + f, aZbot ) );
}


void RENDER_3D_OPENGL::addObjectTriangles( const POLYGON_4PT_2D*                  aPoly,
                                           std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer, float aZtop, float aZbot )
{
    const SFVEC2F& v0 = aPoly->GetV0();
    const SFVEC2F& v1 = aPoly->GetV1();
    const SFVEC2F& v2 = aPoly->GetV2();
    const SFVEC2F& v3 = aPoly->GetV3();

    addTopAndBottomTriangles( aDstLayer, v0, v2, v1, aZtop, aZbot );
    addTopAndBottomTriangles( aDstLayer, v2, v0, v3, aZtop, aZbot );
}


void RENDER_3D_OPENGL::generateRing( const SFVEC2F& aCenter, float aInnerRadius,
                                     float aOuterRadius, unsigned int aNr_sides_per_circle,
                                     std::vector< SFVEC2F >& aInnerContourResult,
                                     std::vector< SFVEC2F >& aOuterContourResult,
                                     bool aInvertOrder )
{
    aInnerContourResult.clear();
    aInnerContourResult.reserve( aNr_sides_per_circle + 2 );

    aOuterContourResult.clear();
    aOuterContourResult.reserve( aNr_sides_per_circle + 2 );

    const int delta = 3600 / aNr_sides_per_circle;

    for( int ii = 0; ii < 3600; ii += delta )
    {
        float angle = (float)( aInvertOrder ? ( 3600 - ii ) : ii )
                               * 2.0f * glm::pi<float>() / 3600.0f;
        const SFVEC2F rotatedDir = SFVEC2F( cos( angle ), sin( angle ) );

        aInnerContourResult.emplace_back( aCenter.x + rotatedDir.x * aInnerRadius,
                                          aCenter.y + rotatedDir.y * aInnerRadius );

        aOuterContourResult.emplace_back( aCenter.x + rotatedDir.x * aOuterRadius,
                                          aCenter.y + rotatedDir.y * aOuterRadius );
    }

    aInnerContourResult.push_back( aInnerContourResult[0] );
    aOuterContourResult.push_back( aOuterContourResult[0] );

    wxASSERT( aInnerContourResult.size() == aOuterContourResult.size() );
}


void RENDER_3D_OPENGL::addObjectTriangles( const RING_2D* aRing, std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer,
                                           float aZtop, float aZbot )
{
    const SFVEC2F& center = aRing->GetCenter();
    const float inner = aRing->GetInnerRadius();
    const float outer = aRing->GetOuterRadius();

    std::vector< SFVEC2F > innerContour;
    std::vector< SFVEC2F > outerContour;

    generateRing( center, inner, outer, m_boardAdapter.GetCircleSegmentCount( outer * 2.0f ),
                  innerContour, outerContour, false );

    // This will add the top and bot quads that will form the approximated ring
    for( unsigned int i = 0; i < ( innerContour.size() - 1 ); ++i )
    {
        const SFVEC2F& vi0 = innerContour[i + 0];
        const SFVEC2F& vi1 = innerContour[i + 1];
        const SFVEC2F& vo0 = outerContour[i + 0];
        const SFVEC2F& vo1 = outerContour[i + 1];

        aDstLayer->m_layer_top_triangles->AddQuad( SFVEC3F( vi1.x, vi1.y, aZtop ),
                                                   SFVEC3F( vi0.x, vi0.y, aZtop ),
                                                   SFVEC3F( vo0.x, vo0.y, aZtop ),
                                                   SFVEC3F( vo1.x, vo1.y, aZtop ) );

        aDstLayer->m_layer_bot_triangles->AddQuad( SFVEC3F( vi1.x, vi1.y, aZbot ),
                                                   SFVEC3F( vo1.x, vo1.y, aZbot ),
                                                   SFVEC3F( vo0.x, vo0.y, aZbot ),
                                                   SFVEC3F( vi0.x, vi0.y, aZbot ) );
    }
}


void RENDER_3D_OPENGL::addObjectTriangles( const TRIANGLE_2D* aTri, std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer,
                                           float aZtop, float aZbot )
{
    const SFVEC2F& v1 = aTri->GetP1();
    const SFVEC2F& v2 = aTri->GetP2();
    const SFVEC2F& v3 = aTri->GetP3();

    addTopAndBottomTriangles( aDstLayer, v1, v2, v3, aZtop, aZbot );
}


void RENDER_3D_OPENGL::addObjectTriangles( const ROUND_SEGMENT_2D*                aSeg,
                                           std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer, float aZtop, float aZbot )
{
    const SFVEC2F& leftStart   = aSeg->GetLeftStar();
    const SFVEC2F& leftEnd     = aSeg->GetLeftEnd();
    const SFVEC2F& leftDir     = aSeg->GetLeftDir();

    const SFVEC2F& rightStart  = aSeg->GetRightStar();
    const SFVEC2F& rightEnd    = aSeg->GetRightEnd();
    const SFVEC2F& rightDir    = aSeg->GetRightDir();
    const float    radius      = aSeg->GetRadius();

    const SFVEC2F& start       = aSeg->GetStart();
    const SFVEC2F& end         = aSeg->GetEnd();

    const float texture_factor  = ( 12.0f / (float) SIZE_OF_CIRCLE_TEXTURE ) + 1.0f;
    const float texture_factorF = ( 6.0f / (float) SIZE_OF_CIRCLE_TEXTURE ) + 1.0f;

    const float radius_of_the_square   = sqrtf( aSeg->GetRadiusSquared() * 2.0f );
    const float radius_triangle_factor = ( radius_of_the_square - radius ) / radius;

    const SFVEC2F factorS = SFVEC2F( -rightDir.y * radius * radius_triangle_factor,
                                      rightDir.x * radius * radius_triangle_factor );

    const SFVEC2F factorE = SFVEC2F( -leftDir.y  * radius * radius_triangle_factor,
                                      leftDir.x  * radius * radius_triangle_factor );

    // Top end segment triangles (semi-circles)
    aDstLayer->m_layer_top_segment_ends->AddTriangle(
                SFVEC3F( rightEnd.x  + texture_factor * factorS.x,
                         rightEnd.y  + texture_factor * factorS.y,
                         aZtop ),
                SFVEC3F( leftStart.x + texture_factor * factorE.x,
                         leftStart.y + texture_factor * factorE.y,
                         aZtop ),
                SFVEC3F( start.x - texture_factorF * leftDir.x * radius * sqrtf( 2.0f ),
                         start.y - texture_factorF * leftDir.y * radius * sqrtf( 2.0f ),
                         aZtop ) );

    aDstLayer->m_layer_top_segment_ends->AddTriangle(
                SFVEC3F( leftEnd.x    + texture_factor * factorE.x,
                         leftEnd.y    + texture_factor * factorE.y, aZtop ),
                SFVEC3F( rightStart.x + texture_factor * factorS.x,
                         rightStart.y + texture_factor * factorS.y, aZtop ),
                SFVEC3F( end.x - texture_factorF * rightDir.x * radius * sqrtf( 2.0f ),
                         end.y - texture_factorF * rightDir.y * radius * sqrtf( 2.0f ),
                         aZtop ) );

    // Bot end segment triangles (semi-circles)
    aDstLayer->m_layer_bot_segment_ends->AddTriangle(
                SFVEC3F( leftStart.x + texture_factor * factorE.x,
                         leftStart.y + texture_factor * factorE.y,
                         aZbot ),
                SFVEC3F( rightEnd.x  + texture_factor * factorS.x,
                         rightEnd.y  + texture_factor * factorS.y,
                         aZbot ),
                SFVEC3F( start.x - texture_factorF * leftDir.x * radius * sqrtf( 2.0f ),
                         start.y - texture_factorF * leftDir.y * radius * sqrtf( 2.0f ),
                         aZbot ) );

    aDstLayer->m_layer_bot_segment_ends->AddTriangle(
                SFVEC3F( rightStart.x + texture_factor * factorS.x,
                         rightStart.y + texture_factor * factorS.y, aZbot ),
                SFVEC3F( leftEnd.x    + texture_factor * factorE.x,
                         leftEnd.y    + texture_factor * factorE.y, aZbot ),
                SFVEC3F( end.x - texture_factorF * rightDir.x * radius * sqrtf( 2.0f ),
                         end.y - texture_factorF * rightDir.y * radius * sqrtf( 2.0f ),
                         aZbot ) );

    // Segment top and bot planes
    aDstLayer->m_layer_top_triangles->AddQuad(
                SFVEC3F( rightEnd.x,   rightEnd.y,   aZtop ),
                SFVEC3F( rightStart.x, rightStart.y, aZtop ),
                SFVEC3F( leftEnd.x,    leftEnd.y,    aZtop ),
                SFVEC3F( leftStart.x,  leftStart.y,  aZtop ) );

    aDstLayer->m_layer_bot_triangles->AddQuad(
                SFVEC3F( rightEnd.x,   rightEnd.y,   aZbot ),
                SFVEC3F( leftStart.x,  leftStart.y,  aZbot ),
                SFVEC3F( leftEnd.x,    leftEnd.y,    aZbot ),
                SFVEC3F( rightStart.x, rightStart.y, aZbot ) );
}


std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED> RENDER_3D_OPENGL::generateHoles( const LIST_OBJECT2D&  aListHolesObject2d,
                                                                              const SHAPE_POLY_SET& aPoly, float aZtop,
                                                                              float aZbot, bool aInvertFaces,
                                                                              const BVH_CONTAINER_2D* aThroughHoles )
{
    if( aListHolesObject2d.size() == 0 )
        return nullptr;

    auto layerTriangles = std::make_shared<TRIANGLE_DISPLAY_LIST>( aListHolesObject2d.size() * 2 );

    // Convert the list of objects(filled circles) to triangle layer structure
    for( const OBJECT_2D* object2d : aListHolesObject2d )
    {
        switch( object2d->GetObjectType() )
        {
        case OBJECT_2D_TYPE::FILLED_CIRCLE:
            addObjectTriangles( static_cast<const FILLED_CIRCLE_2D*>( object2d ), layerTriangles, aZtop, aZbot );
            break;

        case OBJECT_2D_TYPE::ROUNDSEG:
            addObjectTriangles( static_cast<const ROUND_SEGMENT_2D*>( object2d ), layerTriangles, aZtop, aZbot );
            break;

        default:
            wxFAIL_MSG( wxT( "RENDER_3D_OPENGL::generateHoles: Object type not implemented" ) );
            break;
        }
    }

    // Note: he can have a aListHolesObject2d with holes but without contours
    // eg: when there are only NPTH on the list and the contours were not added
    if( aPoly.OutlineCount() > 0 )
    {
        layerTriangles->AddToMiddleContours( aPoly, aZbot, aZtop, m_boardAdapter.BiuTo3dUnits(), aInvertFaces,
                                             aThroughHoles );
    }

    return std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( layerTriangles, m_circleTexture, aZbot, aZtop );
}


std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED>
RENDER_3D_OPENGL::generateLayerList( const BVH_CONTAINER_2D* aContainer, const SHAPE_POLY_SET* aPolyList,
                                     PCB_LAYER_ID aLayer, const BVH_CONTAINER_2D* aThroughHoles )
{
    if( aContainer == nullptr )
        return nullptr;

    const LIST_OBJECT2D& listObject2d = aContainer->GetList();

    if( listObject2d.size() == 0 )
        return nullptr;

    float zBot = 0.0f;
    float zTop = 0.0f;

    getLayerZPos( aLayer, zTop, zBot );

    // Calculate an estimation for the nr of triangles based on the nr of objects
    unsigned int nrTrianglesEstimation = listObject2d.size() * 8;

    auto layerTriangles = std::make_shared<TRIANGLE_DISPLAY_LIST>( nrTrianglesEstimation );

    // store in a list so it will be latter deleted
    appendRenderTriangleList( layerTriangles );

    // Load the 2D (X,Y axis) component of shapes
    for( const OBJECT_2D* object2d : listObject2d )
    {
        switch( object2d->GetObjectType() )
        {
        case OBJECT_2D_TYPE::FILLED_CIRCLE:
            addObjectTriangles( static_cast<const FILLED_CIRCLE_2D*>( object2d ), layerTriangles, zTop, zBot );
            break;

        case OBJECT_2D_TYPE::POLYGON4PT:
            addObjectTriangles( static_cast<const POLYGON_4PT_2D*>( object2d ), layerTriangles, zTop, zBot );
            break;

        case OBJECT_2D_TYPE::RING:
            addObjectTriangles( static_cast<const RING_2D*>( object2d ), layerTriangles, zTop, zBot );
            break;

        case OBJECT_2D_TYPE::TRIANGLE:
            addObjectTriangles( static_cast<const TRIANGLE_2D*>( object2d ), layerTriangles, zTop, zBot );
            break;

        case OBJECT_2D_TYPE::ROUNDSEG:
            addObjectTriangles( static_cast<const ROUND_SEGMENT_2D*>( object2d ), layerTriangles, zTop, zBot );
            break;

        default:
            wxFAIL_MSG( wxT( "RENDER_3D_OPENGL: Object type is not implemented" ) );
            break;
        }
    }

    if( aPolyList && aPolyList->OutlineCount() > 0 )
    {
        layerTriangles->AddToMiddleContours( *aPolyList, zBot, zTop, m_boardAdapter.BiuTo3dUnits(), false,
                                             aThroughHoles );
    }

    // Create display list
    return std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( layerTriangles, m_circleTexture, zBot, zTop );
}


std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED> RENDER_3D_OPENGL::generateEmptyLayerList( PCB_LAYER_ID aLayer )
{
    float layer_z_bot = 0.0f;
    float layer_z_top = 0.0f;

    getLayerZPos( aLayer, layer_z_top, layer_z_bot );

    auto layerTriangles = std::make_shared<TRIANGLE_DISPLAY_LIST>( 1 );

    // store in a list so it will be latter deleted
    appendRenderTriangleList( layerTriangles );

    return std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( layerTriangles, m_circleTexture, layer_z_bot, layer_z_top );
}


std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED> RENDER_3D_OPENGL::createBoard( const SHAPE_POLY_SET& aBoardPoly,
                                                   const BVH_CONTAINER_2D* aThroughHoles,
                                                   bool aTransparent,
                                                   const BOARD_STRUCTURE_BEND* aBend,
                                                   float aZtop, float aZbot,
                                                   double aFoldThicknessMm )
{
    std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED> dispLists;
    CONTAINER_2D                                 boardContainer;

    ConvertPolygonToTriangles( aBoardPoly, boardContainer, m_boardAdapter.BiuTo3dUnits(),
                               (const BOARD_ITEM &)*m_boardAdapter.GetBoard() );

    const LIST_OBJECT2D& listBoardObject2d = boardContainer.GetList();

    if( listBoardObject2d.size() > 0 )
    {
        auto layerTriangles = std::make_shared<TRIANGLE_DISPLAY_LIST>(
                listBoardObject2d.size() * ( aBend ? 512 : 1 ) );

        // A folded list is emitted in physical 3D units.  Display-list Z
        // scaling is deliberately bypassed for these lists; applying the
        // usual normalized 0..1 transform after folding would bend the
        // already transformed Z coordinates a second time.
        const float layer_z_top = aBend ? aZtop : 1.0f;
        const float layer_z_bot = aBend ? aZbot : 0.0f;

        const double unitsPerMm = m_boardAdapter.BiuTo3dUnits() * pcbIUScale.IU_PER_MM;
        auto mapFolded = [&]( const SFVEC2F& aPoint, float aZ )
        {
            VECTOR3D flat( aPoint.x / unitsPerMm, -aPoint.y / unitsPerMm,
                           aZ / unitsPerMm );
            VECTOR3D folded = m_boardAdapter.GetFoldedPoint( flat, aFoldThicknessMm );
            return SFVEC3F( folded.x * unitsPerMm, -folded.y * unitsPerMm,
                            folded.z * unitsPerMm );
        };

        auto emitFoldedTriangle = [&]( const SFVEC2F& a, const SFVEC2F& b,
                                       const SFVEC2F& c )
        {
            const SFVEC3F b1 = mapFolded( a, layer_z_bot );
            const SFVEC3F b2 = mapFolded( b, layer_z_bot );
            const SFVEC3F b3 = mapFolded( c, layer_z_bot );
            const SFVEC3F t1 = mapFolded( a, layer_z_top );
            const SFVEC3F t2 = mapFolded( b, layer_z_top );
            const SFVEC3F t3 = mapFolded( c, layer_z_top );

            // Folded faces have arbitrary normals; the ordinary top/bottom
            // buffers assume a constant +/-Z normal and cannot shade them.
            auto addFace = [&]( const SFVEC3F& p, const SFVEC3F& q, const SFVEC3F& r )
            {
                SFVEC3F normal = glm::cross( q - p, r - p );
                const float length = glm::length( normal );
                if( length <= 1e-12f )
                    return;
                normal /= length;
                layerTriangles->m_layer_middle_contours_quads->AddTriangle( p, q, r );
                layerTriangles->m_layer_middle_contours_quads->AddNormal( normal, normal, normal );
            };
            addFace( b1, b2, b3 );
            addFace( t3, t2, t1 );
        };

        // Convert the list of objects(triangles) to triangle layer structure
        for( const OBJECT_2D* itemOnLayer : listBoardObject2d )
        {
            const OBJECT_2D* object2d_A = itemOnLayer;

            wxASSERT( object2d_A->GetObjectType() == OBJECT_2D_TYPE::TRIANGLE );

            const TRIANGLE_2D* tri = static_cast<const TRIANGLE_2D*>( object2d_A );

            const SFVEC2F& v1 = tri->GetP1();
            const SFVEC2F& v2 = tri->GetP2();
            const SFVEC2F& v3 = tri->GetP3();

            if( !aBend )
                addTopAndBottomTriangles( layerTriangles, v1, v2, v3, layer_z_top, layer_z_bot );
            else
            {
                // Subdivide by developed distance, not total bend angle:
                // a triangle can be much longer than the curved strip.
                const double longestMm = std::max( { glm::length( v2 - v1 ),
                        glm::length( v3 - v2 ), glm::length( v1 - v3 ) } ) / unitsPerMm;
                const int subdivisions = std::max( 1,
                        static_cast<int>( std::ceil( longestMm / 0.25 ) ) );
                for( int row = 0; row < subdivisions; ++row )
                    for( int col = 0; col < subdivisions - row; ++col )
                    {
                        const float u0 = static_cast<float>( row ) / subdivisions;
                        const float u1 = static_cast<float>( row + 1 ) / subdivisions;
                        const float v0 = static_cast<float>( col ) / subdivisions;
                        const float v1f = static_cast<float>( col + 1 ) / subdivisions;
                        const SFVEC2F p0 = v1 + ( v2 - v1 ) * u0 + ( v3 - v1 ) * v0;
                        const SFVEC2F p1 = v1 + ( v2 - v1 ) * u1 + ( v3 - v1 ) * v0;
                        const SFVEC2F p2 = v1 + ( v2 - v1 ) * u0 + ( v3 - v1 ) * v1f;
                        emitFoldedTriangle( p0, p1, p2 );

                        if( col < subdivisions - row - 1 )
                        {
                            const SFVEC2F p3 = v1 + ( v2 - v1 ) * u1 + ( v3 - v1 ) * v1f;
                            emitFoldedTriangle( p1, p3, p2 );
                        }
                    }
            }

        }

        if( aBend )
        {
            // The triangulator's edges include internal diagonals.  Close the
            // physical solid only along the source polygon perimeter so the
            // folded display has no internal walls or seams.
            auto addFoldedContour = [&]( const SHAPE_LINE_CHAIN& aContour )
            {
                for( int i = 0; i < aContour.PointCount(); ++i )
                {
                    const VECTOR2I& p0 = aContour.CPoint( i );
                    const VECTOR2I& p1 = aContour.CPoint( ( i + 1 ) % aContour.PointCount() );
                    if( p0 == p1 )
                        continue;

                    const SFVEC2F q0( p0.x * m_boardAdapter.BiuTo3dUnits(),
                                      -p0.y * m_boardAdapter.BiuTo3dUnits() );
                    const SFVEC2F q1( p1.x * m_boardAdapter.BiuTo3dUnits(),
                                      -p1.y * m_boardAdapter.BiuTo3dUnits() );
                    const int segments = std::max( 1, static_cast<int>( std::ceil(
                            glm::length( q1 - q0 ) / unitsPerMm / 0.25 ) ) );
                    for( int segment = 0; segment < segments; ++segment )
                    {
                        const SFVEC2F start = q0 + ( q1 - q0 ) * ( float( segment ) / segments );
                        const SFVEC2F end = q0 + ( q1 - q0 ) * ( float( segment + 1 ) / segments );
                        const SFVEC3F b0 = mapFolded( start, layer_z_bot );
                        const SFVEC3F b1 = mapFolded( end, layer_z_bot );
                        const SFVEC3F t0 = mapFolded( start, layer_z_top );
                        const SFVEC3F t1 = mapFolded( end, layer_z_top );
                        SFVEC3F normal = glm::cross( b1 - b0, t0 - b0 );
                        const float length = glm::length( normal );
                        if( length <= 1e-12f )
                            continue;
                        normal /= length;
                        layerTriangles->m_layer_middle_contours_quads->AddQuad( b0, b1, t1, t0 );
                        layerTriangles->m_layer_middle_contours_quads->AddNormal(
                                normal, normal, normal, normal );
                    }
                }
            };

            for( int i = 0; i < aBoardPoly.OutlineCount(); ++i )
            {
                addFoldedContour( aBoardPoly.COutline( i ) );
                for( int h = 0; h < aBoardPoly.HoleCount( i ); ++h )
                    addFoldedContour( aBoardPoly.CHole( i, h ) );
            }
        }

        if( aBoardPoly.OutlineCount() > 0 && !aBend )
        {
            layerTriangles->AddToMiddleContours( aBoardPoly, layer_z_bot, layer_z_top,
                                                 m_boardAdapter.BiuTo3dUnits(), false,
                                                 aThroughHoles );

        }

        if( aBoardPoly.OutlineCount() > 0 )
            dispLists = std::make_shared<OPENGL_RENDER_LIST_DEFERRED>(
                    layerTriangles, m_circleTexture, aBend ? layer_z_bot : layer_z_top,
                    layer_z_top, aTransparent );
    }

    return dispLists;
}


void RENDER_3D_OPENGL::backfillPostMachine()
{
    if( !m_boardAdapter.GetBoard() )
        return;

    const int copperLayerCount = m_boardAdapter.GetBoard()->GetCopperLayerCount();
    const float unitScale = m_boardAdapter.BiuTo3dUnits();
    const int platingThickness = m_boardAdapter.GetHolePlatingThickness();
    const float boardBodyThickness = m_boardAdapter.GetBoardBodyThickness();

    // We use the same unit z range as the board (0 to 1) and apply scaling when rendering
    const float boardZTop = 1.0f;  // Top of board body
    const float boardZBot = 0.0f;  // Bottom of board body

    // Helper to convert layer Z position to normalized 0-1 range
    auto normalizeZ = [&]( float absZ ) -> float
    {
        float boardTop = m_boardAdapter.GetLayerBottomZPos( F_Cu );
        float boardBot = m_boardAdapter.GetLayerBottomZPos( B_Cu );
        float boardThick = boardTop - boardBot;

        if( boardThick <= 0 )
            return 0.5f;

        // Map absolute Z to 0-1 range where 0 = B_Cu and 1 = F_Cu
        return ( absZ - boardBot ) / boardThick;
    };

    // We'll accumulate all plug geometry into a single triangle list
    std::shared_ptr<TRIANGLE_DISPLAY_LIST> plugTriangles = std::make_shared<TRIANGLE_DISPLAY_LIST>( 1024 );

    // Process vias for backdrill and post-machining plugs
    for( const PCB_TRACK* track : m_boardAdapter.GetBoard()->Tracks() )
    {
        if( track->Type() != PCB_VIA_T )
            continue;

        const PCB_VIA* via = static_cast<const PCB_VIA*>( track );

        const float holeDiameter = via->GetDrillValue() * unitScale;
        const float holeInnerRadius = holeDiameter / 2.0f;
        const float holeOuterRadius = holeInnerRadius + platingThickness * unitScale;
        const SFVEC2F center( via->GetStart().x * unitScale, -via->GetStart().y * unitScale );
        const int nrSegments = m_boardAdapter.GetCircleSegmentCount( via->GetDrillValue() );

        PCB_LAYER_ID topLayer, bottomLayer;
        via->LayerPair( &topLayer, &bottomLayer );

        float viaZTop, viaZBot, dummy;
        getLayerZPos( topLayer, viaZTop, dummy );
        getLayerZPos( bottomLayer, dummy, viaZBot );

        // Handle backdrill plugs
        const float secondaryDrillRadius = via->GetSecondaryDrillSize().value_or( 0 ) * 0.5f * unitScale;
        const float tertiaryDrillRadius = via->GetTertiaryDrillSize().value_or( 0 ) * 0.5f * unitScale;

        if( secondaryDrillRadius > holeOuterRadius || tertiaryDrillRadius > holeOuterRadius )
        {
            PCB_LAYER_ID plug_start_layer = F_Cu;
            PCB_LAYER_ID plug_end_layer = B_Cu;

            // Case 1: secondary drill exists, so we need to adjust the plug_end_layer
            if( secondaryDrillRadius > holeOuterRadius )
            {
                plug_end_layer = via->GetSecondaryDrillEndLayer();
            }
            // Case 2: tertiary drill exists, so we need to adjust the plug_start_layer
            if( tertiaryDrillRadius > holeOuterRadius )
            {
                plug_start_layer = via->GetTertiaryDrillStartLayer();
            }

            // Calculate where the backdrill ends and plug should start
            float plugZTop, plugZBot, temp;
            getLayerZPos( plug_end_layer, temp, plugZBot );
            getLayerZPos( plug_start_layer, plugZTop, temp );

            // Create a ring from holeOuterRadius to backdrillRadius
            generateCylinder( center, holeOuterRadius, std::max( secondaryDrillRadius, tertiaryDrillRadius ),
                                plugZTop, plugZBot, nrSegments, plugTriangles );
        }

        // Handle front post-machining plugs
        const auto frontMode = via->GetFrontPostMachining();

        if( frontMode.has_value()
            && frontMode.value() != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED
            && frontMode.value() != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
        {
            const float frontRadius = via->GetFrontPostMachiningSize() * 0.5f * unitScale;
            const float frontDepth = via->GetFrontPostMachiningDepth() * unitScale;

            if( frontRadius > holeOuterRadius && frontDepth > 0 )
            {
                // Plug goes from bottom of post-machining to bottom of via
                float pmBottomZ = normalizeZ( viaZTop - frontDepth );
                float plugZBot = normalizeZ( viaZBot );

                if( pmBottomZ > plugZBot )
                {
                    if( frontMode.value() == PAD_DRILL_POST_MACHINING_MODE::COUNTERSINK )
                    {
                        EDA_ANGLE angle( via->GetFrontPostMachiningAngle(), TENTHS_OF_A_DEGREE_T );
                        generateInvCone( center, holeOuterRadius, frontRadius,
                                         pmBottomZ, plugZBot, nrSegments, plugTriangles, angle );
                    }
                    else
                    {
                        generateCylinder( center, holeOuterRadius, frontRadius,
                                          pmBottomZ, plugZBot, nrSegments, plugTriangles );
                    }
                }
            }
        }

        // Handle back post-machining plugs
        const auto backMode = via->GetBackPostMachining();

        if( backMode.has_value()
            && backMode.value() != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED
            && backMode.value() != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
        {
            const float backRadius = via->GetBackPostMachiningSize() * 0.5f * unitScale;
            const float backDepth = via->GetBackPostMachiningDepth() * unitScale;

            if( backRadius > holeOuterRadius && backDepth > 0 )
            {
                // Plug goes from top of via to top of post-machining
                float plugZTop = normalizeZ( viaZTop );
                float pmTopZ = normalizeZ( viaZBot + backDepth );

                if( plugZTop > pmTopZ )
                {
                    if( backMode.value() == PAD_DRILL_POST_MACHINING_MODE::COUNTERSINK )
                    {
                        EDA_ANGLE angle( via->GetBackPostMachiningAngle(), TENTHS_OF_A_DEGREE_T );
                        generateInvCone( center, holeOuterRadius, backRadius,
                                         plugZTop, pmTopZ, nrSegments, plugTriangles, angle );
                    }
                    else
                    {
                        generateCylinder( center, holeOuterRadius, backRadius,
                                          plugZTop, pmTopZ, nrSegments, plugTriangles );
                    }
                }
            }
        }
    }

    // Process pads for post-machining plugs
    for( const FOOTPRINT* footprint : m_boardAdapter.GetBoard()->Footprints() )
    {
        for( const PAD* pad : footprint->Pads() )
        {
            if( !pad->HasHole() )
                continue;

            if( pad->GetDrillShape() != PAD_DRILL_SHAPE::CIRCLE )
                continue;

            const SFVEC2F padCenter( pad->GetPosition().x * unitScale,
                                     -pad->GetPosition().y * unitScale );
            const float holeInnerRadius = pad->GetDrillSize().x * 0.5f * unitScale;
            const float holeOuterRadius = holeInnerRadius + platingThickness * unitScale;
            const int nrSegments = m_boardAdapter.GetCircleSegmentCount( pad->GetDrillSize().x );

            float padZTop, padZBot, padDummy;
            getLayerZPos( F_Cu, padZTop, padDummy );
            getLayerZPos( B_Cu, padDummy, padZBot );

            // Handle front post-machining plugs for pads
            const auto frontMode = pad->GetFrontPostMachining();

            if( frontMode.has_value()
                && frontMode.value() != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED
                && frontMode.value() != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
            {
                const float frontRadius = pad->GetFrontPostMachiningSize() * 0.5f * unitScale;
                const float frontDepth = pad->GetFrontPostMachiningDepth() * unitScale;

                if( frontRadius > holeOuterRadius && frontDepth > 0 )
                {
                    float pmBottomZ = normalizeZ( padZTop - frontDepth );
                    float plugZBot = normalizeZ( padZBot );

                    if( pmBottomZ > plugZBot )
                    {
                        if( frontMode.value() == PAD_DRILL_POST_MACHINING_MODE::COUNTERSINK )
                        {
                            EDA_ANGLE angle( pad->GetFrontPostMachiningAngle(), TENTHS_OF_A_DEGREE_T );
                            generateInvCone( padCenter, holeOuterRadius, frontRadius,
                                             pmBottomZ, plugZBot, nrSegments, plugTriangles, angle );
                        }
                        else
                        {
                            generateCylinder( padCenter, holeOuterRadius, frontRadius,
                                              pmBottomZ, plugZBot, nrSegments, plugTriangles );
                        }
                    }
                }
            }

            // Handle back post-machining plugs for pads
            const auto backMode = pad->GetBackPostMachining();

            if( backMode.has_value()
                && backMode.value() != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED
                && backMode.value() != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
            {
                const float backRadius = pad->GetBackPostMachiningSize() * 0.5f * unitScale;
                const float backDepth = pad->GetBackPostMachiningDepth() * unitScale;

                if( backRadius > holeOuterRadius && backDepth > 0 )
                {
                    float plugZTop = normalizeZ( padZTop );
                    float pmTopZ = normalizeZ( padZBot + backDepth );

                    if( plugZTop > pmTopZ )
                    {
                        if( backMode.value() == PAD_DRILL_POST_MACHINING_MODE::COUNTERSINK )
                        {
                            EDA_ANGLE angle( pad->GetBackPostMachiningAngle(), TENTHS_OF_A_DEGREE_T );
                            generateInvCone( padCenter, holeOuterRadius, backRadius,
                                             plugZTop, pmTopZ, nrSegments, plugTriangles, angle );
                        }
                        else
                        {
                            generateCylinder( padCenter, holeOuterRadius, backRadius,
                                              plugZTop, pmTopZ, nrSegments, plugTriangles );
                        }
                    }
                }
            }
        }
    }

    // If we have any plug geometry, create a render list for it
    if( plugTriangles->m_layer_top_triangles->GetVertexSize() > 0
        || plugTriangles->m_layer_bot_triangles->GetVertexSize() > 0
        || plugTriangles->m_layer_middle_contours_quads->GetVertexSize() > 0 )
    {
        // Store the triangles for later cleanup
        appendRenderTriangleList( plugTriangles );

        // Create a render list for the plugs using the same Z range as the board
        // This will be scaled and drawn alongside m_boardWithHoles in renderBoardBody()
        assignRenderPtr( m_postMachinePlugs,
                         std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( plugTriangles, m_circleTexture, boardZTop, boardZTop ) );
    }
}


void RENDER_3D_OPENGL::renderExtrudedBodies()
{
    if( !m_boardAdapter.GetBoard() )
        return;

    const float biuTo3d = m_boardAdapter.BiuTo3dUnits();

    for( FOOTPRINT* fp : m_boardAdapter.GetBoard()->Footprints() )
    {
        if( !fp->HasBody() )
            continue;

        const EXTRUDED_3D_BODY* body = fp->GetBody()->GetExtruded();

        if( !body->m_show || !m_boardAdapter.IsFootprintShown( fp ) )
            continue;

        SHAPE_POLY_SET outline;

        if( !GetExtrusionOutline( fp, outline ) )
            continue;

        if( outline.OutlineCount() == 0 )
            continue;

        outline.Simplify();

        VECTOR2I fpPos = fp->GetPosition();
        ApplyExtrusionTransform( outline, body, fpPos );

        bool  isBack = fp->IsFlipped();
        float boardSurfaceZ = m_boardAdapter.GetFootprintZPos( isBack );
        float standoff3d = body->m_standoff * biuTo3d;
        float bodyThickness = ( body->m_height - body->m_standoff ) * biuTo3d * body->m_scale.z;
        float zOffset3d = pcbIUScale.mmToIU( body->m_offset.z ) * biuTo3d;

        float zBot, zTop;

        if( !isBack )
        {
            zBot = boardSurfaceZ + standoff3d + zOffset3d;
            zTop = zBot + bodyThickness;
        }
        else
        {
            zTop = boardSurfaceZ - standoff3d - zOffset3d;
            zBot = zTop - bodyThickness;
        }

        CONTAINER_2D triContainer;
        ConvertPolygonToTriangles( outline, triContainer, biuTo3d, *fp );

        const LIST_OBJECT2D& triList = triContainer.GetList();

        if( triList.empty() )
            continue;

        std::shared_ptr<TRIANGLE_DISPLAY_LIST> layerTri = std::make_shared<TRIANGLE_DISPLAY_LIST>( triList.size() );

        for( const OBJECT_2D* obj : triList )
        {
            const TRIANGLE_2D* tri = static_cast<const TRIANGLE_2D*>( obj );
            addTopAndBottomTriangles( layerTri, tri->GetP1(), tri->GetP2(), tri->GetP3(), zTop, zBot );
        }

        layerTri->AddToMiddleContours( outline, zBot, zTop, biuTo3d, false );

        std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED> renderList =
                std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( layerTri, m_circleTexture, zTop, zBot );

        appendRenderTriangleList( layerTri );
        assignRenderMap( m_extrudedBodyLists, fp, renderList );

        // Create pin extrusions for pad holes
        // The pins move with the body, so the Z offset shifts them whole
        if( standoff3d > 0.0f )
        {
            SHAPE_POLY_SET pinPoly;
            SHAPE_POLY_SET pegPoly;

            if( GetExtrusionPinOutlines( fp, pinPoly, pegPoly ) )
            {
                float oppositeSurfaceZ = m_boardAdapter.GetFootprintZPos( !isBack );
                float protrusion = 1.0f * pcbIUScale.IU_PER_MM * biuTo3d;
                float pinZBot, pinZTop;

                if( !isBack )
                {
                    pinZBot = oppositeSurfaceZ - protrusion + zOffset3d;
                    pinZTop = boardSurfaceZ + standoff3d + zOffset3d;
                }
                else
                {
                    pinZTop = oppositeSurfaceZ + protrusion - zOffset3d;
                    pinZBot = boardSurfaceZ - standoff3d - zOffset3d;
                }

                auto buildPinList = [&]( SHAPE_POLY_SET& aPoly, auto& aDstMap )
                {
                    if( aPoly.OutlineCount() == 0 )
                        return;

                    ApplyExtrusionTransform( aPoly, body, fpPos );

                    CONTAINER_2D pinTriContainer;
                    ConvertPolygonToTriangles( aPoly, pinTriContainer, biuTo3d, *fp );

                    const LIST_OBJECT2D& pinTriList = pinTriContainer.GetList();

                    if( pinTriList.empty() )
                        return;

                    auto pinLayerTri = std::make_shared<TRIANGLE_DISPLAY_LIST>( pinTriList.size() );

                    for( const OBJECT_2D* obj : pinTriList )
                    {
                        const TRIANGLE_2D* tri = static_cast<const TRIANGLE_2D*>( obj );
                        addTopAndBottomTriangles( pinLayerTri, tri->GetP1(), tri->GetP2(), tri->GetP3(), pinZTop,
                                                  pinZBot );
                    }

                    pinLayerTri->AddToMiddleContours( aPoly, pinZBot, pinZTop, biuTo3d, false );

                    std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED> pinRenderList =
                            std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( pinLayerTri, m_circleTexture, pinZTop,
                                                                           pinZBot );

                    appendRenderTriangleList( pinLayerTri );
                    assignRenderMap( aDstMap, fp, pinRenderList );
                };

                buildPinList( pinPoly, m_extrudedPadLists );
                buildPinList( pegPoly, m_extrudedPegLists );
            }
        }
    }
}


void RENDER_3D_OPENGL::reload()
{
    m_reloadRequested = false;

    // Ensure previous reload worker is finished before freeing/rebuilding render data.
    StopBgWorker();

    // Drop hover BVH before layers are destroyed; LAYER_ITEM points into those OBJECT_2Ds.
    if( m_canvas )
        m_canvas->InvalidateRaytracingHitTesting();

    freeAllLists();

    if( !m_boardAdapter.m_IsPreviewer )
        createPlaceholderModel();

    OBJECT_2D_STATS::Instance().ResetStats();

    m_boardAdapter.InitSettings( m_activityReporter, m_warningReporter );

    SFVEC3F camera_pos = m_boardAdapter.GetBoardCenter();
    m_camera.SetBoardLookAtPos( camera_pos );

    // Create basic Board without holes
    assignRenderPtr( m_board, createBoard( m_boardAdapter.GetBoardPoly(), nullptr ) );

    startBgWorker();
}


void RENDER_3D_OPENGL::sendRefreshView()
{
    if( m_canvas )
        wxQueueEvent( m_canvas, new wxCommandEvent( wxEVT_REFRESH_CUSTOM_COMMAND, ID_CUSTOM_EVENT_1 ) );
}


void RENDER_3D_OPENGL::bgWorker( std::stop_token aStop )
{
    int64_t stats_startReloadTime = GetRunningMicroSecs();

    if( aStop.stop_requested() )
        return;

    // Ensure hover BVH is gone before destroyLayers() inside CreateLayers.
    if( m_canvas )
        m_canvas->InvalidateRaytracingHitTesting();

    m_boardAdapter.CreateLayers( m_activityReporter, aStop );

    if( aStop.stop_requested() )
        return;

    sendRefreshView();

    if( m_activityReporter )
        m_activityReporter->Report( _( "Load OpenGL: board" ) );

    // Create Board with TH
    assignRenderPtr( m_board, createBoard( m_boardAdapter.GetBoardPoly(), &m_boardAdapter.GetTH_IDs() ) );

    if( aStop.stop_requested() )
        return;

    sendRefreshView();

    // Structural regions get independent render lists so local stack and
    // stiffener thicknesses are represented by their own Z extents.
    std::vector<STRUCTURED_BODY> structuredBodies;
    std::vector<STRUCTURED_ARTWORK> structuredArtwork;
    const BOARD_STRUCTURE& structure = m_boardAdapter.GetBoard()->GetBoardStructure();
    const bool foldedView = m_boardAdapter.GetConstructionView()
                         && m_boardAdapter.GetFoldedView()
                         && m_boardAdapter.m_Cfg->m_Render.engine == RENDER_ENGINE::OPENGL;
    const BOARD_STRUCTURE_BEND* globalBend = foldedView && !structure.bends.empty()
                                           ? &structure.bends.front() : nullptr;
    double foldThicknessMm = m_boardAdapter.GetBoardBodyThickness()
                           / m_boardAdapter.BiuTo3dUnits() / pcbIUScale.IU_PER_MM;
    if( globalBend )
        for( const BOARD_STRUCTURE_REGION& parent : structure.regions )
            if( parent.id == globalBend->region_id )
                if( const BOARD_STRUCTURE_STACK* stack = structure.StackForRegion( parent ) )
                {
                    double candidate = 0.0;
                    for( const auto& material : stack->materials )
                        candidate += material.thickness_nm * 1e-6;
                    if( candidate > 0.0 )
                        foldThicknessMm = candidate;
                }

    if( !structure.regions.empty() )
    {
        auto subtractConstructionVoids = [&]( SHAPE_POLY_SET& aPoly )
        {
            // Depth-specific counterbores, countersinks, and backdrills are
            // intentionally omitted here.  Their enlarged openings cannot be
            // subtracted from a full-height construction solid without also
            // removing material below/above the specified machining depth.
            const SHAPE_POLY_SET* voids[] = {
                &m_boardAdapter.GetTH_ODPolys(), &m_boardAdapter.GetNPTH_ODPolys(),
            };
            for( const SHAPE_POLY_SET* cutout : voids )
                if( cutout->OutlineCount() > 0 )
                    aPoly.BooleanSubtract( *cutout );
        };

        for( const BOARD_STRUCTURE_REGION& region : structure.regions )
        {
            SHAPE_POLY_SET poly;
            if( region.shape.outline.size() < 3 )
                continue;
            SHAPE_LINE_CHAIN outline;
            for( const VECTOR2I& p : region.shape.outline ) outline.Append( p );
            outline.SetClosed( true );
            poly.AddOutline( outline );
            for( const auto& points : region.shape.holes )
            {
                SHAPE_LINE_CHAIN hole;
                for( const VECTOR2I& p : points ) hole.Append( p );
                hole.SetClosed( true );
                poly.AddHole( hole );
            }
            subtractConstructionVoids( poly );
            double thicknessMm = m_boardAdapter.GetBoardBodyThickness() / m_boardAdapter.BiuTo3dUnits()
                               / pcbIUScale.IU_PER_MM;
            double zBotMm = -thicknessMm * 0.5;
            double zTopMm = thicknessMm * 0.5;
            for( const BOARD_STRUCTURE_STACK& stack : structure.stacks )
                if( stack.id == region.stack_id )
                {
                    thicknessMm = 0.0;
                    for( const BOARD_STRUCTURE_MATERIAL& material : stack.materials )
                        thicknessMm += material.thickness_nm * 1e-6;

                    bool haveZ = false;
                    for( const auto& layer : stack.layers )
                    {
                        double layerThickness = 0.0;
                        for( const auto& material : stack.materials )
                            if( material.id == layer.material_id )
                            {
                                layerThickness = material.thickness_nm * 1e-6;
                                break;
                            }
                        const double bottom = layer.z_offset_nm * 1e-6;
                        const double top = bottom + layerThickness;
                        if( !haveZ )
                        {
                            zBotMm = bottom;
                            zTopMm = top;
                            haveZ = true;
                        }
                        else
                        {
                            zBotMm = std::min( zBotMm, bottom );
                            zTopMm = std::max( zTopMm, top );
                        }
                    }
                }
            const bool physical = globalBend != nullptr;
            const float zBot = static_cast<float>( zBotMm * pcbIUScale.IU_PER_MM
                                                  * m_boardAdapter.BiuTo3dUnits() );
            const float zTop = static_cast<float>( zTopMm * pcbIUScale.IU_PER_MM
                                                  * m_boardAdapter.BiuTo3dUnits() );
            if( auto list = createBoard( poly, nullptr, false, globalBend,
                                                         zTop, zBot, foldThicknessMm ) )
                structuredBodies.push_back( { list, zBot, zTop, physical,
                                                STRUCTURED_BODY::KIND::SUBSTRATE } );
        }
        for( const BOARD_STRUCTURE_STIFFENER& stiffener : structure.stiffeners )
        {
            SHAPE_POLY_SET poly;
            if( stiffener.shape.outline.size() < 3 ) continue;
            SHAPE_LINE_CHAIN outline;
            for( const VECTOR2I& p : stiffener.shape.outline ) outline.Append( p );
            outline.SetClosed( true );
            poly.AddOutline( outline );
            for( const auto& points : stiffener.shape.holes )
            {
                SHAPE_LINE_CHAIN hole;
                for( const VECTOR2I& p : points ) hole.Append( p );
                hole.SetClosed( true );
                poly.AddHole( hole );
            }
            subtractConstructionVoids( poly );
            const BOARD_STRUCTURE_REGION* parent = nullptr;
            for( const BOARD_STRUCTURE_REGION& candidate : structure.regions )
                if( candidate.id == stiffener.region_id )
                {
                    parent = &candidate;
                    break;
                }

            double parentThicknessMm = m_boardAdapter.GetBoardBodyThickness()
                                      / m_boardAdapter.BiuTo3dUnits() / pcbIUScale.IU_PER_MM;
            double parentBotMm = -parentThicknessMm * 0.5;
            double parentTopMm = parentThicknessMm * 0.5;
            if( parent )
                if( const BOARD_STRUCTURE_STACK* stack = structure.StackForRegion( *parent ) )
                {
                    parentThicknessMm = 0.0;
                    for( const auto& material : stack->materials )
                        parentThicknessMm += material.thickness_nm * 1e-6;
                    bool haveZ = false;
                    for( const auto& layer : stack->layers )
                    {
                        double layerThickness = 0.0;
                        for( const auto& material : stack->materials )
                            if( material.id == layer.material_id )
                            {
                                layerThickness = material.thickness_nm * 1e-6;
                                break;
                            }
                        const double bottom = layer.z_offset_nm * 1e-6;
                        const double top = bottom + layerThickness;
                        if( !haveZ )
                        {
                            parentBotMm = bottom;
                            parentTopMm = top;
                            haveZ = true;
                        }
                        else
                        {
                            parentBotMm = std::min( parentBotMm, bottom );
                            parentTopMm = std::max( parentTopMm, top );
                        }
                    }
                }

            const double stiffenerMm = ( stiffener.thickness_nm > 0
                                       ? stiffener.thickness_nm : stiffener.max_thickness_nm ) * 1e-6;
            const double adhesiveMm = stiffener.adhesive_thickness_nm * 1e-6;
            const bool physical = globalBend != nullptr;
            const float bot = static_cast<float>( ( stiffener.top ? parentTopMm + adhesiveMm
                                                           : parentBotMm - adhesiveMm - stiffenerMm )
                                          * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits() );
            const float top = static_cast<float>( ( stiffener.top ? parentTopMm + adhesiveMm + stiffenerMm
                                                           : parentBotMm - adhesiveMm )
                                          * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits() );
            if( auto list = createBoard( poly, nullptr, false, globalBend,
                                                         top, bot, foldThicknessMm ) )
                structuredBodies.push_back( { list, bot, top, physical,
                                                STRUCTURED_BODY::KIND::STIFFENER } );
            if( adhesiveMm > 0.0 )
            {
                const double parentFace = stiffener.top ? parentTopMm : parentBotMm;
                const double adhesiveBot = stiffener.top ? parentFace : parentFace - adhesiveMm;
                const double adhesiveTop = stiffener.top ? parentFace + adhesiveMm : parentFace;
                const float ab = adhesiveBot * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits();
                const float at = adhesiveTop * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits();
                if( auto list = createBoard( poly, nullptr, false, globalBend,
                                                             at, ab, foldThicknessMm ) )
                    structuredBodies.push_back( { list, ab, at, physical,
                                                    STRUCTURED_BODY::KIND::ADHESIVE } );
            }
        }

        // Coverlays and their adhesive are construction solids as well.  Keep
        // them in the same physical coordinate path so a folded preview does
        // not leave a flat mask floating over the flex.
        for( const BOARD_STRUCTURE_COVERLAY& coverlay : structure.coverlays )
        {
            SHAPE_POLY_SET poly;
            if( coverlay.coverage.outline.size() < 3 )
                continue;
            SHAPE_LINE_CHAIN outline;
            for( const VECTOR2I& p : coverlay.coverage.outline ) outline.Append( p );
            outline.SetClosed( true );
            poly.AddOutline( outline );
            for( const auto& points : coverlay.coverage.holes )
            {
                SHAPE_LINE_CHAIN hole;
                for( const VECTOR2I& p : points ) hole.Append( p );
                hole.SetClosed( true );
                poly.AddHole( hole );
            }
            for( const auto& opening : coverlay.openings )
            {
                if( opening.outline.size() < 3 )
                    continue;
                const int outlineIndex = poly.NewHole( 0 );
                for( const VECTOR2I& p : opening.outline ) poly.Append( p, 0, outlineIndex );
                for( const auto& points : opening.holes )
                {
                    const int holeIndex = poly.NewHole( 0 );
                    for( const VECTOR2I& p : points ) poly.Append( p, 0, holeIndex );
                }
            }
            subtractConstructionVoids( poly );

            const BOARD_STRUCTURE_REGION* parent = nullptr;
            for( const BOARD_STRUCTURE_REGION& candidate : structure.regions )
                if( candidate.id == coverlay.region_id )
                {
                    parent = &candidate;
                    break;
                }
            double parentThicknessMm = m_boardAdapter.GetBoardBodyThickness()
                                      / m_boardAdapter.BiuTo3dUnits() / pcbIUScale.IU_PER_MM;
            double parentBotMm = -parentThicknessMm * 0.5;
            double parentTopMm = parentThicknessMm * 0.5;
            if( parent )
                if( const BOARD_STRUCTURE_STACK* stack = structure.StackForRegion( *parent ) )
                {
                    parentThicknessMm = 0.0;
                    for( const auto& material : stack->materials )
                        parentThicknessMm += material.thickness_nm * 1e-6;
                    bool haveZ = false;
                    for( const auto& layer : stack->layers )
                    {
                        double layerThickness = 0.0;
                        for( const auto& material : stack->materials )
                            if( material.id == layer.material_id )
                            {
                                layerThickness = material.thickness_nm * 1e-6;
                                break;
                            }
                        const double bottom = layer.z_offset_nm * 1e-6;
                        const double top = bottom + layerThickness;
                        if( !haveZ )
                        {
                            parentBotMm = bottom;
                            parentTopMm = top;
                            haveZ = true;
                        }
                        else
                        {
                            parentBotMm = std::min( parentBotMm, bottom );
                            parentTopMm = std::max( parentTopMm, top );
                        }
                    }
                }
            const double coverMm = coverlay.thickness_nm * 1e-6;
            const double adhesiveMm = coverlay.adhesive_thickness_nm * 1e-6;
            const bool physical = globalBend != nullptr;
            const float bot = static_cast<float>( ( coverlay.top ? parentTopMm + adhesiveMm
                                                          : parentBotMm - adhesiveMm - coverMm )
                                          * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits() );
            const float top = static_cast<float>( ( coverlay.top ? parentTopMm + adhesiveMm + coverMm
                                                          : parentBotMm - adhesiveMm )
                                          * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits() );
            if( auto list = createBoard( poly, nullptr, false, globalBend,
                                                         top, bot, foldThicknessMm ) )
                structuredBodies.push_back( { list, bot, top, physical,
                                                STRUCTURED_BODY::KIND::COVERLAY } );
            if( adhesiveMm > 0.0 )
            {
                const double parentFace = coverlay.top ? parentTopMm : parentBotMm;
                const double adhesiveBot = coverlay.top ? parentFace : parentFace - adhesiveMm;
                const double adhesiveTop = coverlay.top ? parentFace + adhesiveMm : parentFace;
                const float ab = adhesiveBot * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits();
                const float at = adhesiveTop * pcbIUScale.IU_PER_MM * m_boardAdapter.BiuTo3dUnits();
                if( auto list = createBoard( poly, nullptr, false, globalBend,
                                                             at, ab, foldThicknessMm ) )
                    structuredBodies.push_back( { list, ab, at, physical,
                                                    STRUCTURED_BODY::KIND::ADHESIVE } );
            }
        }

        // Native artwork is built from the same polygon map as the ordinary
        // flat renderer, but split at structural region boundaries.  This is
        // important for flex: transforming one large flat list leaves copper
        // and silk spanning the hinge as a planar overlay.
        if( m_boardAdapter.GetConstructionView() )
        {
            const MAP_POLY& artworkMap = m_boardAdapter.GetPolyMap();
            const double sceneUnitsPerMm = pcbIUScale.IU_PER_MM
                                         * m_boardAdapter.BiuTo3dUnits();

            auto regionZ = [&]( const BOARD_STRUCTURE_REGION& aRegion,
                                PCB_LAYER_ID aLayer ) -> std::pair<float, float>
            {
                const BOARD_STRUCTURE_STACK* stack = structure.StackForRegion( aRegion );
                double bodyBot = -foldThicknessMm * 0.5;
                double bodyTop = foldThicknessMm * 0.5;
                if( stack )
                {
                    bool haveBody = false;
                    for( const auto& instance : stack->layers )
                    {
                        double materialThickness = 0.0;
                        for( const auto& material : stack->materials )
                            if( material.id == instance.material_id )
                            {
                                materialThickness = material.thickness_nm * 1e-6;
                                break;
                            }
                        const double bot = instance.z_offset_nm * 1e-6;
                        const double top = bot + materialThickness;
                        if( !haveBody )
                        {
                            bodyBot = bot;
                            bodyTop = top;
                            haveBody = true;
                        }
                        else
                        {
                            bodyBot = std::min( bodyBot, bot );
                            bodyTop = std::max( bodyTop, top );
                        }
                        if( instance.global_copper_layer == static_cast<int>( aLayer ) )
                            return { static_cast<float>( bot * sceneUnitsPerMm ),
                                     static_cast<float>( top * sceneUnitsPerMm ) };
                    }
                }

                // Technical layers sit on the corresponding regional face.
                // Keep a small real thickness so mask and silk remain visible
                // after the fold and retain their normal depth ordering.
                const bool topSide = aLayer == F_Cu || aLayer == F_Mask || aLayer == F_SilkS
                                  || aLayer == F_Paste;
                const double thickness = m_boardAdapter.GetNonCopperLayerThickness() / sceneUnitsPerMm;
                const double lift = ( aLayer == F_SilkS || aLayer == B_SilkS ) ? thickness : 0.0;
                const double face = topSide ? bodyTop + lift : bodyBot - lift;
                if( topSide )
                    return { static_cast<float>( face * sceneUnitsPerMm ),
                             static_cast<float>( ( face + thickness ) * sceneUnitsPerMm ) };
                return { static_cast<float>( ( face - thickness ) * sceneUnitsPerMm ),
                         static_cast<float>( face * sceneUnitsPerMm ) };
            };

            const std::bitset<LAYER_3D_END> artworkVisibility = m_boardAdapter.GetVisibleLayers();
            for( const auto& [layer, polySource] : artworkMap )
            {
                if( !m_boardAdapter.Is3dLayerEnabled( layer, artworkVisibility )
                    || !polySource )
                    continue;

                for( const BOARD_STRUCTURE_REGION& region : structure.regions )
                {
                    if( region.shape.outline.size() < 3 )
                        continue;
                    if( IsCopperLayer( layer ) )
                    {
                        const auto* stack = structure.StackForRegion( region );
                        if( !stack || std::none_of( stack->layers.begin(), stack->layers.end(),
                                [&]( const auto& instance ) { return instance.global_copper_layer == int( layer ); } ) )
                            continue;
                    }
                    if( layer == F_Mask || layer == B_Mask )
                    {
                        const int requiredCopper = layer == F_Mask ? int( F_Cu ) : int( B_Cu );
                        const auto* stack = structure.StackForRegion( region );
                        if( !stack || std::none_of( stack->layers.begin(), stack->layers.end(),
                                [&]( const auto& instance )
                                { return instance.global_copper_layer == requiredCopper; } ) )
                            continue;
                    }
                    SHAPE_LINE_CHAIN boundary;
                    for( const VECTOR2I& point : region.shape.outline )
                        boundary.Append( point );
                    boundary.SetClosed( true );
                    SHAPE_POLY_SET regionPoly;
                    regionPoly.AddOutline( boundary );
                    for( const auto& points : region.shape.holes )
                    {
                        SHAPE_LINE_CHAIN hole;
                        for( const VECTOR2I& point : points )
                            hole.Append( point );
                        hole.SetClosed( true );
                        regionPoly.AddHole( hole );
                    }
                    // KiCad's mask polygon map contains the openings.  The
                    // rendered mask is the sheet surrounding those openings,
                    // so start with the region and subtract the mask map.
                    SHAPE_POLY_SET clipped = ( layer == F_Mask || layer == B_Mask )
                                            ? regionPoly.CloneDropTriangulation()
                                            : polySource->CloneDropTriangulation();
                    if( layer == F_Mask || layer == B_Mask )
                        clipped.BooleanSubtract( *polySource );
                    else
                    {
                        // Exposed plated pads are removed from the ordinary
                        // copper map when plated-copper differentiation is on.
                        // Reunite them before clipping to avoid missing SMD pads.
                        const SHAPE_POLY_SET* plated = layer == F_Cu
                                ? m_boardAdapter.GetFrontPlatedCopperPolys()
                                : layer == B_Cu ? m_boardAdapter.GetBackPlatedCopperPolys() : nullptr;
                        if( plated && plated->OutlineCount() ) clipped.BooleanAdd( *plated );
                        clipped.BooleanIntersection( regionPoly );
                        if( ( layer == F_SilkS || layer == B_SilkS )
                            && m_boardAdapter.m_Cfg->m_Render.subtract_mask_from_silk )
                        {
                            const auto mask = artworkMap.find( layer == F_SilkS ? F_Mask : B_Mask );
                            if( mask != artworkMap.end() && mask->second ) clipped.BooleanSubtract( *mask->second );
                        }
                    }
                    const auto holes = m_boardAdapter.GetHoleIdPolysMap().find( layer );
                    if( holes != m_boardAdapter.GetHoleIdPolysMap().end() && holes->second )
                        clipped.BooleanSubtract( *holes->second );
                    clipped.BooleanSubtract( m_boardAdapter.GetNPTH_ODPolys() );
                    if( clipped.OutlineCount() == 0 )
                        continue;

                    const auto [zBot, zTop] = regionZ( region, layer );
                    if( auto list = createBoard( clipped, nullptr, false, globalBend,
                                                                  zTop, zBot, foldThicknessMm ) )
                    {
                        structuredArtwork.push_back( { layer, list,
                            layer == F_Mask || layer == B_Mask || layer == F_Paste
                                                        || layer == B_Paste,
                                                        zBot, zTop, globalBend != nullptr } );
                    }
                }
            }
        }
    }

    {
        std::lock_guard lock( m_renderMutex );
        for( auto& entry : m_structuredBodies )
            retireRenderPtr( entry.list );
        for( auto& entry : m_structuredArtwork )
            retireRenderPtr( entry.list );
        m_structuredBodies = std::move( structuredBodies );
        m_structuredArtwork = std::move( structuredArtwork );
    }

    m_antiBoardPolys.RemoveAllContours();
    m_antiBoardPolys.NewOutline();
    m_antiBoardPolys.Append( VECTOR2I( -INT_MAX/2, -INT_MAX/2 ) );
    m_antiBoardPolys.Append( VECTOR2I(  INT_MAX/2, -INT_MAX/2 ) );
    m_antiBoardPolys.Append( VECTOR2I(  INT_MAX/2,  INT_MAX/2 ) );
    m_antiBoardPolys.Append( VECTOR2I( -INT_MAX/2,  INT_MAX/2 ) );
    m_antiBoardPolys.Outline( 0 ).SetClosed( true );

    m_antiBoardPolys.BooleanSubtract( m_boardAdapter.GetBoardPoly() );

    assignRenderPtr( m_antiBoard, createBoard( m_antiBoardPolys, nullptr, true ) );

    if( aStop.stop_requested() )
        return;

    SHAPE_POLY_SET board_poly_with_holes = m_boardAdapter.GetBoardPoly().CloneDropTriangulation();
    board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetTH_ODPolys() );
    board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetNPTH_ODPolys() );

    // Also subtract counterbore, countersink, and backdrill polygons from the board
    if( m_boardAdapter.GetFrontCounterborePolys().OutlineCount() > 0 )
        board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetFrontCounterborePolys() );

    if( m_boardAdapter.GetBackCounterborePolys().OutlineCount() > 0 )
        board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetBackCounterborePolys() );

    if( m_boardAdapter.GetFrontCountersinkPolys().OutlineCount() > 0 )
        board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetFrontCountersinkPolys() );

    if( m_boardAdapter.GetBackCountersinkPolys().OutlineCount() > 0 )
        board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetBackCountersinkPolys() );

    if( m_boardAdapter.GetSecondarydrillPolys().OutlineCount() > 0 )
        board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetSecondarydrillPolys() );

    if( m_boardAdapter.GetTertiarydrillPolys().OutlineCount() > 0 )
        board_poly_with_holes.BooleanSubtract( m_boardAdapter.GetTertiarydrillPolys() );

    if( aStop.stop_requested() )
        return;

    assignRenderPtr( m_boardWithHoles, createBoard( board_poly_with_holes, &m_boardAdapter.GetTH_IDs() ) );

    // Create plugs for backdrilled and post-machined areas
    backfillPostMachine();

    if( aStop.stop_requested() )
        return;

    sendRefreshView();

    // Create Through Holes and vias
    if( m_activityReporter )
        m_activityReporter->Report( _( "Load OpenGL: holes and vias" ) );

    SHAPE_POLY_SET outerPolyTHT = m_boardAdapter.GetTH_ODPolys().CloneDropTriangulation();

    // Include NPTH polygons so their barrel walls are also generated
    if( m_boardAdapter.GetNPTH_ODPolys().OutlineCount() > 0 )
        outerPolyTHT.BooleanAdd( m_boardAdapter.GetNPTH_ODPolys() );

    outerPolyTHT.BooleanIntersection( m_boardAdapter.GetBoardPoly() );

    assignRenderPtr( m_outerThroughHoles, generateHoles( m_boardAdapter.GetTH_ODs().GetList(), outerPolyTHT, 1.0f, 0.0f,
                                                         false, &m_boardAdapter.GetTH_IDs() ) );

    assignRenderPtr( m_outerViaThroughHoles, generateHoles( m_boardAdapter.GetViaTH_ODs().GetList(),
                                                             m_boardAdapter.GetViaTH_ODPolys(), 1.0f, 0.0f, false ) );

    if( m_boardAdapter.m_Cfg->m_Render.clip_silk_on_via_annuli )
    {
        assignRenderPtr( m_outerThroughHoleRings, generateHoles( m_boardAdapter.GetViaAnnuli().GetList(),
                                                                  m_boardAdapter.GetViaAnnuliPolys(), 1.0f, 0.0f, false ) );
    }

    const MAP_POLY& innerMapHoles = m_boardAdapter.GetHoleIdPolysMap();
    const MAP_POLY& outerMapHoles = m_boardAdapter.GetHoleOdPolysMap();

    wxASSERT( innerMapHoles.size() == outerMapHoles.size() );

    const MAP_CONTAINER_2D_BASE& map_holes = m_boardAdapter.GetLayerHoleMap();

    if( outerMapHoles.size() > 0 )
    {
        float layer_z_bot = 0.0f;
        float layer_z_top = 0.0f;

        for( const auto& [ layer, poly ] : outerMapHoles )
        {
            getLayerZPos( layer, layer_z_top, layer_z_bot );

            assignRenderMap( m_outerLayerHoles, layer,
                             generateHoles( map_holes.at( layer )->GetList(), *poly, layer_z_top, layer_z_bot, false ) );
        }

        for( const auto& [ layer, poly ] : innerMapHoles )
        {
            getLayerZPos( layer, layer_z_top, layer_z_bot );

            assignRenderMap( m_innerLayerHoles, layer,
                             generateHoles( map_holes.at( layer )->GetList(), *poly, layer_z_top, layer_z_bot, false ) );
        }
    }

    // Generate vertical cylinders of vias and pads (copper)
    generateViasAndPads();

    if( aStop.stop_requested() )
        return;

    sendRefreshView();

    // Add layers maps
    if( m_activityReporter )
        m_activityReporter->Report( _( "Load OpenGL: layers" ) );

    std::bitset<LAYER_3D_END> visibilityFlags = m_boardAdapter.GetVisibleLayers();
    const MAP_POLY&           map_poly = m_boardAdapter.GetPolyMap();
    wxString                  msg;

    for( const auto& [ layer, container2d ] : m_boardAdapter.GetLayerMap() )
    {
        if( !m_boardAdapter.Is3dLayerEnabled( layer, visibilityFlags ) )
            continue;

        if( m_activityReporter )
        {
            msg = m_boardAdapter.GetBoard()->GetLayerName( layer );
            m_activityReporter->Report( wxString::Format( _( "Load OpenGL layer %s" ), msg ) );
        }

        SHAPE_POLY_SET polyListSubtracted;
        SHAPE_POLY_SET* polyList = nullptr;

        // Load the vertical (Z axis) component of shapes

        if( m_boardAdapter.m_Cfg->m_Render.opengl_copper_thickness )
        {
            if( map_poly.contains( layer ) )
            {
                polyListSubtracted = *map_poly.at( layer );

                if( LSET::PhysicalLayersMask().test( layer ) )
                {
                    polyListSubtracted.BooleanIntersection( m_boardAdapter.GetBoardPoly() );
                }

                if( layer != B_Mask && layer != F_Mask )
                {
                    polyListSubtracted.BooleanSubtract( m_boardAdapter.GetTH_ODPolys() );
                    polyListSubtracted.BooleanSubtract( m_boardAdapter.GetNPTH_ODPolys() );

                    // Subtract counterbore/countersink cutouts from copper layers
                    if( layer == F_Cu )
                    {
                        polyListSubtracted.BooleanSubtract( m_boardAdapter.GetFrontCounterborePolys() );
                        polyListSubtracted.BooleanSubtract( m_boardAdapter.GetFrontCountersinkPolys() );
                    }
                    else if( layer == B_Cu )
                    {
                        polyListSubtracted.BooleanSubtract( m_boardAdapter.GetBackCounterborePolys() );
                        polyListSubtracted.BooleanSubtract( m_boardAdapter.GetBackCountersinkPolys() );
                    }
                }

                if( m_boardAdapter.m_Cfg->m_Render.subtract_mask_from_silk )
                {
                    if( layer == B_SilkS && map_poly.contains( B_Mask ) )
                    {
                        polyListSubtracted.BooleanSubtract( *map_poly.at( B_Mask ) );
                    }
                    else if( layer == F_SilkS && map_poly.contains( F_Mask ) )
                    {
                        polyListSubtracted.BooleanSubtract( *map_poly.at( F_Mask ) );
                    }
                }

                polyList = &polyListSubtracted;
            }
        }

        if( aStop.stop_requested() )
            return;

        std::shared_ptr<OPENGL_RENDER_LIST_DEFERRED> oglList =
                generateLayerList( container2d, polyList, layer, &m_boardAdapter.GetTH_IDs() );

        if( oglList != nullptr )
            assignRenderMap( m_layers, layer, oglList );

        if( aStop.stop_requested() )
            return;

        sendRefreshView();
    }

    if( m_boardAdapter.m_Cfg->m_Render.DifferentiatePlatedCopper() )
    {
        const SHAPE_POLY_SET* frontPlatedCopperPolys = m_boardAdapter.GetFrontPlatedCopperPolys();
        const SHAPE_POLY_SET* backPlatedCopperPolys = m_boardAdapter.GetBackPlatedCopperPolys();

        if( frontPlatedCopperPolys )
        {
            SHAPE_POLY_SET poly = frontPlatedCopperPolys->CloneDropTriangulation();
            poly.BooleanIntersection( m_boardAdapter.GetBoardPoly() );
            poly.BooleanSubtract( m_boardAdapter.GetTH_ODPolys() );
            poly.BooleanSubtract( m_boardAdapter.GetNPTH_ODPolys() );
            poly.BooleanSubtract( m_boardAdapter.GetFrontCounterborePolys() );
            poly.BooleanSubtract( m_boardAdapter.GetFrontCountersinkPolys() );
            poly.BooleanSubtract( m_boardAdapter.GetSecondarydrillPolys() );

            assignRenderPtr( m_platedPadsFront, generateLayerList( m_boardAdapter.GetPlatedPadsFront(), &poly, F_Cu ) );

            // An entry for F_Cu must exist in m_layers or we'll never look at m_platedPadsFront
            if( m_layers.count( F_Cu ) == 0 )
                assignRenderMap( m_layers, F_Cu, generateEmptyLayerList( F_Cu ) );
        }

        if( backPlatedCopperPolys )
        {
            SHAPE_POLY_SET poly = backPlatedCopperPolys->CloneDropTriangulation();
            poly.BooleanIntersection( m_boardAdapter.GetBoardPoly() );
            poly.BooleanSubtract( m_boardAdapter.GetTH_ODPolys() );
            poly.BooleanSubtract( m_boardAdapter.GetNPTH_ODPolys() );
            poly.BooleanSubtract( m_boardAdapter.GetBackCounterborePolys() );
            poly.BooleanSubtract( m_boardAdapter.GetBackCountersinkPolys() );
            poly.BooleanSubtract( m_boardAdapter.GetTertiarydrillPolys() );

            assignRenderPtr( m_platedPadsBack, generateLayerList( m_boardAdapter.GetPlatedPadsBack(), &poly, B_Cu ) );

            // An entry for B_Cu must exist in m_layers or we'll never look at m_platedPadsBack
            if( m_layers.count( B_Cu ) == 0 )
                assignRenderMap( m_layers, B_Cu, generateEmptyLayerList( B_Cu ) );
        }
    }

    if( m_boardAdapter.m_Cfg->m_Render.show_off_board_silk )
    {
        if( const BVH_CONTAINER_2D* padsFront = m_boardAdapter.GetOffboardPadsFront() )
            assignRenderPtr( m_offboardPadsFront, generateLayerList( padsFront, nullptr, F_Cu ) );

        if( const BVH_CONTAINER_2D* padsBack = m_boardAdapter.GetOffboardPadsBack() )
            assignRenderPtr( m_offboardPadsBack, generateLayerList( padsBack, nullptr, B_Cu ) );
    }

    if( aStop.stop_requested() )
        return;

    sendRefreshView();

    // Load 3D models
    if( m_activityReporter )
        m_activityReporter->Report( _( "Loading 3D models..." ) );

    load3dModels( aStop );

    renderExtrudedBodies();

    if( aStop.stop_requested() )
        return;

    sendRefreshView();

    // OpenGL draws the board on the GPU, but hover/picking still uses the auxiliary
    // raytracing renderer (IntersectBoardItem). That path keeps its own BVH
    // (m_accelerator), which is not updated by the OpenGL scene build above.
    // Rebuild it here on the same worker thread once board layers are ready.
    if( !aStop.stop_requested() && m_canvas )
    {
        m_hitTestDirty = true;
        runHitTestRebuild( aStop );
    }

    if( aStop.stop_requested() )
        return;

    if( m_activityReporter )
    {
        // Calculation time in seconds
        double calculation_time = (double)( GetRunningMicroSecs() - stats_startReloadTime) / 1e6;

        m_activityReporter->Report( wxString::Format( _( "Load completed in %.3f s" ), calculation_time ) );
    }
}


void RENDER_3D_OPENGL::startBgWorker()
{
    wxLogTrace( m_logTrace, wxT( "RENDER_3D_OPENGL::startBgWorker" ) );

    // Claim the flag before the thread runs so a rebuild cannot displace the load.
    m_bgWorkerBusy = true;

    m_bgWorkerThread = std::jthread(
            [this]( std::stop_token aStopToken )
            {
                // Avoid lag in main (UI) thread
                BS::this_thread::set_os_thread_priority( BS::os_thread_priority::below_normal );

                bgWorker( aStopToken );

                m_bgWorkerBusy = false;
            } );
}


void RENDER_3D_OPENGL::RebuildHitTestAsync()
{
    m_hitTestDirty = true;

    // A running worker picks the request up before it releases the flag.
    if( m_bgWorkerBusy.exchange( true ) )
        return;

    m_bgWorkerThread = std::jthread(
            [this]( std::stop_token aStop )
            {
                BS::this_thread::set_os_thread_priority( BS::os_thread_priority::below_normal );

                runHitTestRebuild( aStop );

                m_bgWorkerBusy = false;
            } );
}


void RENDER_3D_OPENGL::runHitTestRebuild( std::stop_token aStop )
{
    for( ;; )
    {
        while( m_hitTestDirty.exchange( false ) )
        {
            if( aStop.stop_requested() || !m_canvas )
                return;

            m_canvas->ReloadRaytracingForHitTesting( aStop );
        }

        m_bgWorkerBusy = false;

        // Pick up a request that raced the release above.
        if( !m_hitTestDirty || m_bgWorkerBusy.exchange( true ) )
            return;
    }
}


void RENDER_3D_OPENGL::StopBgWorker()
{
    wxLogTrace( m_logTrace, wxT( "RENDER_3D_OPENGL::StopBgWorker" ) );

    if( m_bgWorkerThread.joinable() )
    {
        if( m_activityReporter )
            m_activityReporter->Report( _( "Stopping background load..." ) );

        m_bgWorkerThread.request_stop();
        m_bgWorkerThread.join();
    }

    m_bgWorkerBusy = false;
}


void RENDER_3D_OPENGL::JoinBgWorker()
{
    wxLogTrace( m_logTrace, wxT( "RENDER_3D_OPENGL::JoinBgWorker" ) );

    if( m_bgWorkerThread.joinable() )
        m_bgWorkerThread.join();
}


void RENDER_3D_OPENGL::addTopAndBottomTriangles( std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDst, const SFVEC2F& v0,
                                                 const SFVEC2F& v1, const SFVEC2F& v2, float top, float bot )
{
    aDst->m_layer_bot_triangles->AddTriangle( SFVEC3F( v0.x, v0.y, bot ),
                                              SFVEC3F( v1.x, v1.y, bot ),
                                              SFVEC3F( v2.x, v2.y, bot ) );

    aDst->m_layer_top_triangles->AddTriangle( SFVEC3F( v2.x, v2.y, top ),
                                              SFVEC3F( v1.x, v1.y, top ),
                                              SFVEC3F( v0.x, v0.y, top ) );
}


void RENDER_3D_OPENGL::getLayerZPos( PCB_LAYER_ID aLayer, float& aOutZtop, float& aOutZbot ) const
{
    aOutZbot = m_boardAdapter.GetLayerBottomZPos( aLayer );
    aOutZtop = m_boardAdapter.GetLayerTopZPos( aLayer );

    if( aOutZtop < aOutZbot )
    {
        float tmpFloat = aOutZbot;
        aOutZbot = aOutZtop;
        aOutZtop = tmpFloat;
    }
}


void RENDER_3D_OPENGL::generateCylinder( const SFVEC2F& aCenter, float aInnerRadius, float aOuterRadius, float aZtop,
                                         float aZbot, unsigned int aNr_sides_per_circle,
                                         std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer )
{
    std::vector< SFVEC2F > innerContour;
    std::vector< SFVEC2F > outerContour;

    generateRing( aCenter, aInnerRadius, aOuterRadius, aNr_sides_per_circle, innerContour,
                  outerContour, false );

    for( unsigned int i = 0; i < ( innerContour.size() - 1 ); ++i )
    {
        const SFVEC2F& vi0 = innerContour[i + 0];
        const SFVEC2F& vi1 = innerContour[i + 1];
        const SFVEC2F& vo0 = outerContour[i + 0];
        const SFVEC2F& vo1 = outerContour[i + 1];

        aDstLayer->m_layer_top_triangles->AddQuad( SFVEC3F( vi1.x, vi1.y, aZtop ),
                                                   SFVEC3F( vi0.x, vi0.y, aZtop ),
                                                   SFVEC3F( vo0.x, vo0.y, aZtop ),
                                                   SFVEC3F( vo1.x, vo1.y, aZtop ) );

        aDstLayer->m_layer_bot_triangles->AddQuad( SFVEC3F( vi1.x, vi1.y, aZbot ),
                                                   SFVEC3F( vo1.x, vo1.y, aZbot ),
                                                   SFVEC3F( vo0.x, vo0.y, aZbot ),
                                                   SFVEC3F( vi0.x, vi0.y, aZbot ) );
    }

    aDstLayer->AddToMiddleContours( outerContour, aZbot, aZtop, true );
    aDstLayer->AddToMiddleContours( innerContour, aZbot, aZtop, false );
}


void RENDER_3D_OPENGL::generateInvCone( const SFVEC2F& aCenter, float aInnerRadius, float aOuterRadius, float aZtop,
                                        float aZbot, unsigned int aNr_sides_per_circle,
                                        std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer, EDA_ANGLE aAngle )
{
    // For a countersink cone:
    // - The outer contour goes from aZbot to aZtop (full height)
    // - The inner contour goes from aZbot to aZbot + innerHeight
    // - The top surface is conical, sloping from inner top to outer top
    // - aAngle is the half-angle of the cone (in decidegrees from the vertical)

    // Calculate the inner contour height based on the cone angle
    // tan(angle) = (outerRadius - innerRadius) / innerHeight
    // innerHeight = (outerRadius - innerRadius) / tan(angle)
    float radialDiff = aOuterRadius - aInnerRadius;
    float angleRad = aAngle.AsRadians();

    // Clamp angle to avoid division by zero or negative heights
    if( angleRad < 0.01f )
        angleRad = 0.01f;

    float innerHeight = radialDiff / tanf( angleRad );
    float totalHeight = aZtop - aZbot;

    // Clamp inner height to not exceed total height
    if( innerHeight > totalHeight )
        innerHeight = totalHeight;

    float zInnerTop = aZbot + innerHeight;

    std::vector< SFVEC2F > innerContour;
    std::vector< SFVEC2F > outerContour;

    generateRing( aCenter, aInnerRadius, aOuterRadius, aNr_sides_per_circle, innerContour,
                  outerContour, false );

    for( unsigned int i = 0; i < ( innerContour.size() - 1 ); ++i )
    {
        const SFVEC2F& vi0 = innerContour[i + 0];
        const SFVEC2F& vi1 = innerContour[i + 1];
        const SFVEC2F& vo0 = outerContour[i + 0];
        const SFVEC2F& vo1 = outerContour[i + 1];

        // Conical top surface: from inner contour at zInnerTop to outer contour at aZtop
        aDstLayer->m_layer_top_triangles->AddQuad( SFVEC3F( vi1.x, vi1.y, zInnerTop ),
                                                   SFVEC3F( vi0.x, vi0.y, zInnerTop ),
                                                   SFVEC3F( vo0.x, vo0.y, aZtop ),
                                                   SFVEC3F( vo1.x, vo1.y, aZtop ) );

        // Flat bottom surface
        aDstLayer->m_layer_bot_triangles->AddQuad( SFVEC3F( vi1.x, vi1.y, aZbot ),
                                                   SFVEC3F( vo1.x, vo1.y, aZbot ),
                                                   SFVEC3F( vo0.x, vo0.y, aZbot ),
                                                   SFVEC3F( vi0.x, vi0.y, aZbot ) );
    }

    // Outer contour wall goes full height
    aDstLayer->AddToMiddleContours( outerContour, aZbot, aZtop, true );
    // Inner contour wall only goes up to zInnerTop
    aDstLayer->AddToMiddleContours( innerContour, aZbot, zInnerTop, false );
}


void RENDER_3D_OPENGL::generateDisk( const SFVEC2F& aCenter, float aRadius, float aZ, unsigned int aNr_sides_per_circle,
                                     std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer, bool aTop )
{
    const float delta = 2.0f * glm::pi<float>() / (float) aNr_sides_per_circle;

    for( unsigned int i = 0; i < aNr_sides_per_circle; ++i )
    {
        float a0 = delta * i;
        float a1 = delta * ( i + 1 );
        const SFVEC3F p0( aCenter.x + cosf( a0 ) * aRadius,
                          aCenter.y + sinf( a0 ) * aRadius, aZ );
        const SFVEC3F p1( aCenter.x + cosf( a1 ) * aRadius,
                          aCenter.y + sinf( a1 ) * aRadius, aZ );
        const SFVEC3F c( aCenter.x, aCenter.y, aZ );

        if( aTop )
            aDstLayer->m_layer_top_triangles->AddTriangle( p1, p0, c );
        else
            aDstLayer->m_layer_bot_triangles->AddTriangle( p0, p1, c );
    }
}


void RENDER_3D_OPENGL::generateDimple( const SFVEC2F& aCenter, float aRadius, float aZ, float aDepth,
                                       unsigned int                           aNr_sides_per_circle,
                                       std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer, bool aTop )
{
    const float delta = 2.0f * glm::pi<float>() / (float) aNr_sides_per_circle;
    const SFVEC3F c( aCenter.x, aCenter.y, aTop ? aZ - aDepth : aZ + aDepth );

    for( unsigned int i = 0; i < aNr_sides_per_circle; ++i )
    {
        float a0 = delta * i;
        float a1 = delta * ( i + 1 );
        const SFVEC3F p0( aCenter.x + cosf( a0 ) * aRadius,
                          aCenter.y + sinf( a0 ) * aRadius, aZ );
        const SFVEC3F p1( aCenter.x + cosf( a1 ) * aRadius,
                          aCenter.y + sinf( a1 ) * aRadius, aZ );

        if( aTop )
            aDstLayer->m_layer_top_triangles->AddTriangle( p0, p1, c );
        else
            aDstLayer->m_layer_bot_triangles->AddTriangle( p1, p0, c );
    }
}


bool RENDER_3D_OPENGL::appendPostMachiningGeometry( std::shared_ptr<TRIANGLE_DISPLAY_LIST> aDstLayer,
                                                    const SFVEC2F& aHoleCenter,
                                                    PAD_DRILL_POST_MACHINING_MODE aMode,
                                                    int aSizeIU,
                                                    int aDepthIU,
                                                    float aHoleInnerRadius,
                                                    float aZSurface,
                                                    bool aIsFront,
                                                    float aPlatingThickness3d,
                                                    float aUnitScale,
                                                    float* aZEnd )
{
    if( !m_boardAdapter.m_Cfg->m_Render.show_plated_barrels )
        return false;

    if( !aDstLayer || aPlatingThickness3d <= 0.0f || aHoleInnerRadius <= 0.0f )
        return false;

    if( aMode != PAD_DRILL_POST_MACHINING_MODE::COUNTERBORE
            && aMode != PAD_DRILL_POST_MACHINING_MODE::COUNTERSINK )
    {
        return false;
    }

    if( aSizeIU <= 0 || aDepthIU <= 0 )
        return false;

    const float radius = aSizeIU * 0.5f * aUnitScale;
    const float depth = aDepthIU * aUnitScale;

    if( radius <= aHoleInnerRadius || depth <= 0.0f )
        return false;

    float zEnd = aIsFront ? ( aZSurface - depth ) : ( aZSurface + depth );

    if( aZEnd )
        *aZEnd = zEnd;

    const float zTop = std::max( aZSurface, zEnd );
    const float zBot = std::min( aZSurface, zEnd );

    const int diameterBIU = std::max( aSizeIU,
                                      std::max( 1,
                                                (int) ( ( aHoleInnerRadius * 2.0f )
                                                        / aUnitScale ) ) );
    const unsigned int nrSegments =
            std::max( 12u, m_boardAdapter.GetCircleSegmentCount( diameterBIU ) );

    if( aMode == PAD_DRILL_POST_MACHINING_MODE::COUNTERBORE )
    {
        generateCylinder( aHoleCenter, radius, radius + aPlatingThickness3d, zTop, zBot,
                          nrSegments, aDstLayer );
        return true;
    }

    float csTopRadius = radius;
    float csBotRadius = aHoleInnerRadius;

    std::vector< SFVEC2F > innerContourTop, outerContourTop;
    std::vector< SFVEC2F > innerContourBot, outerContourBot;

    generateRing( aHoleCenter, csTopRadius, csTopRadius + aPlatingThickness3d, nrSegments,
                  innerContourTop, outerContourTop, false );
    generateRing( aHoleCenter, csBotRadius, csBotRadius + aPlatingThickness3d, nrSegments,
                  innerContourBot, outerContourBot, false );

    for( unsigned int i = 0; i < ( innerContourTop.size() - 1 ); ++i )
    {
        const SFVEC2F& vi0_top = innerContourTop[i + 0];
        const SFVEC2F& vi1_top = innerContourTop[i + 1];
        const SFVEC2F& vo0_top = outerContourTop[i + 0];
        const SFVEC2F& vo1_top = outerContourTop[i + 1];

        const SFVEC2F& vi0_bot = innerContourBot[i + 0];
        const SFVEC2F& vi1_bot = innerContourBot[i + 1];
        const SFVEC2F& vo0_bot = outerContourBot[i + 0];
        const SFVEC2F& vo1_bot = outerContourBot[i + 1];

        aDstLayer->m_layer_middle_contours_quads->AddQuad(
                SFVEC3F( vi1_top.x, vi1_top.y, zTop ),
                SFVEC3F( vi0_top.x, vi0_top.y, zTop ),
                SFVEC3F( vi0_bot.x, vi0_bot.y, zBot ),
                SFVEC3F( vi1_bot.x, vi1_bot.y, zBot ) );

        aDstLayer->m_layer_middle_contours_quads->AddQuad(
                SFVEC3F( vo1_top.x, vo1_top.y, zTop ),
                SFVEC3F( vo0_top.x, vo0_top.y, zTop ),
                SFVEC3F( vo0_bot.x, vo0_bot.y, zBot ),
                SFVEC3F( vo1_bot.x, vo1_bot.y, zBot ) );
    }

    return true;
}


void RENDER_3D_OPENGL::generateViaBarrels( float aPlatingThickness3d, float aUnitScale )
{
    if( !m_boardAdapter.GetBoard() || m_boardAdapter.GetViaCount() <= 0 )
        return;

    if( !m_boardAdapter.m_Cfg->m_Render.show_plated_barrels )
        return;

    float        averageDiameter = m_boardAdapter.GetAverageViaHoleDiameter();
    unsigned int averageSegCount = m_boardAdapter.GetCircleSegmentCount( averageDiameter );
    unsigned int trianglesEstimate = averageSegCount * 8 * m_boardAdapter.GetViaCount();

    std::shared_ptr<TRIANGLE_DISPLAY_LIST> layerTriangleVIA = std::make_shared<TRIANGLE_DISPLAY_LIST>( trianglesEstimate );

    for( const PCB_TRACK* track : m_boardAdapter.GetBoard()->Tracks() )
    {
        if( track->Type() != PCB_VIA_T )
            continue;

        const PCB_VIA* via = static_cast<const PCB_VIA*>( track );
        bool isBackdrilled = via->GetSecondaryDrillSize().has_value();
        bool isTertiarydrilled = via->GetTertiaryDrillSize().has_value();
        bool hasFrontPostMachining = via->GetFrontPostMachining().value_or( PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
                                     != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN
                                     && via->GetFrontPostMachining().value_or( PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
                                                != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED;
        bool hasBackPostMachining = via->GetBackPostMachining().value_or( PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
                                    != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN
                                    && via->GetBackPostMachining().value_or( PAD_DRILL_POST_MACHINING_MODE::UNKNOWN )
                                               != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED;

        if( via->GetViaType() == VIATYPE::THROUGH && !isBackdrilled
                && !hasFrontPostMachining && !hasBackPostMachining )
        {
            continue;
        }

        const float holediameter = via->GetDrillValue() * aUnitScale;
        const int nrSegments = m_boardAdapter.GetCircleSegmentCount( via->GetDrillValue() );
        const float hole_inner_radius = holediameter / 2.0f;

        const SFVEC2F via_center( via->GetStart().x * aUnitScale,
                                  -via->GetStart().y * aUnitScale );

        PCB_LAYER_ID top_layer, bottom_layer;
        via->LayerPair( &top_layer, &bottom_layer );

        float ztop, zbot, dummy;

        getLayerZPos( top_layer, ztop, dummy );
        getLayerZPos( bottom_layer, dummy, zbot );

        wxASSERT( zbot < ztop );

        float ztop_plated = ztop;
        float zbot_plated = zbot;

        if( isBackdrilled )
        {
            PCB_LAYER_ID secEnd = via->GetSecondaryDrillEndLayer();
            float secZEnd;

            // Backdrill goes from the back surface up to secEnd, so get the top of that layer
            // as the bottom of the plated barrel
            getLayerZPos( secEnd, secZEnd, dummy );
            zbot_plated = std::max( zbot_plated, secZEnd );
        }

        if( isTertiarydrilled )
        {
            PCB_LAYER_ID terEnd = via->GetTertiaryDrillEndLayer();
            float terZEnd;

            // Tertiary drill goes from the front surface down to terEnd, so get the bottom of that layer
            // as the top of the plated barrel
            getLayerZPos( terEnd, dummy, terZEnd );
            ztop_plated = std::min( ztop_plated, terZEnd );
        }

        auto applyViaPostMachining = [&]( bool isFront )
        {
            auto modeOpt = isFront ? via->GetFrontPostMachining()
                                   : via->GetBackPostMachining();

            if( !modeOpt
                    || modeOpt.value() == PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED )
            {
                return;
            }

            int sizeIU = isFront ? via->GetFrontPostMachiningSize()
                                  : via->GetBackPostMachiningSize();
            int depthIU = isFront ? via->GetFrontPostMachiningDepth()
                                   : via->GetBackPostMachiningDepth();
            float zSurface = isFront ? ztop : zbot;
            float zEnd = 0.0f;

            if( appendPostMachiningGeometry( layerTriangleVIA, via_center, modeOpt.value(),
                                             sizeIU, depthIU, hole_inner_radius, zSurface,
                                             isFront, aPlatingThickness3d, aUnitScale, &zEnd ) )
            {
                if( isFront )
                    ztop_plated = std::min( ztop_plated, zEnd );
                else
                    zbot_plated = std::max( zbot_plated, zEnd );
            }
        };

        if( hasFrontPostMachining )
            applyViaPostMachining( true );
        if( hasBackPostMachining )
            applyViaPostMachining( false );

        generateCylinder( via_center, hole_inner_radius, hole_inner_radius + aPlatingThickness3d, ztop_plated,
                          zbot_plated, nrSegments, layerTriangleVIA );
    }

    const float padFrontSurface =
            m_boardAdapter.GetLayerBottomZPos( F_Cu )
            + m_boardAdapter.GetFrontCopperThickness() * 0.99f;
    const float padBackSurface =
            m_boardAdapter.GetLayerBottomZPos( B_Cu )
            - m_boardAdapter.GetBackCopperThickness() * 0.99f;

    for( const FOOTPRINT* footprint : m_boardAdapter.GetBoard()->Footprints() )
    {
        for( const PAD* pad : footprint->Pads() )
        {
            if( pad->GetAttribute() == PAD_ATTRIB::NPTH )
                continue;

            if( !pad->HasHole() )
                continue;

            if( pad->GetDrillShape() != PAD_DRILL_SHAPE::CIRCLE )
                continue;

            const SFVEC2F padCenter( pad->GetPosition().x * aUnitScale,
                                        -pad->GetPosition().y * aUnitScale );
            const float holeInnerRadius =
                    pad->GetDrillSize().x * 0.5f * aUnitScale;

            auto emitPadPostMachining = [&]( bool isFront )
            {
                auto modeOpt = isFront ? pad->GetFrontPostMachining()
                                        : pad->GetBackPostMachining();

                if( !modeOpt
                        || modeOpt.value() == PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED )
                {
                    return;
                }

                int sizeIU = isFront ? pad->GetFrontPostMachiningSize()
                                        : pad->GetBackPostMachiningSize();
                int depthIU = isFront ? pad->GetFrontPostMachiningDepth()
                                        : pad->GetBackPostMachiningDepth();
                float zSurface = isFront ? padFrontSurface : padBackSurface;

                appendPostMachiningGeometry( layerTriangleVIA, padCenter, modeOpt.value(),
                                                sizeIU, depthIU, holeInnerRadius, zSurface,
                                                isFront, aPlatingThickness3d, aUnitScale,
                                                nullptr );
            };

            emitPadPostMachining( true );
            emitPadPostMachining( false );
        }
    }

    assignRenderPtr( m_microviaHoles,
                     std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( layerTriangleVIA, 0, 0.0f, 0.0f ) );
}


void RENDER_3D_OPENGL::generatePlatedHoleShells( int aPlatingThickness, float aUnitScale )
{
    if( !m_boardAdapter.GetBoard() )
        return;

    if( m_boardAdapter.GetHoleCount() <= 0 && m_boardAdapter.GetViaCount() <= 0 )
        return;

    SHAPE_POLY_SET tht_outer_holes_poly; // Stores the outer poly of the copper holes
    SHAPE_POLY_SET tht_inner_holes_poly; // Stores the inner poly of the copper holes
    // Same as ***_holes_poly but for vias that are capped
    SHAPE_POLY_SET capped_outer_vias_poly;
    SHAPE_POLY_SET capped_inner_vias_poly;

    tht_outer_holes_poly.RemoveAllContours();
    tht_inner_holes_poly.RemoveAllContours();
    capped_outer_vias_poly.RemoveAllContours();
    capped_inner_vias_poly.RemoveAllContours();

    for( const PCB_TRACK* track : m_boardAdapter.GetBoard()->Tracks() )
    {
        if( track->Type() != PCB_VIA_T )
            continue;

        const PCB_VIA* via = static_cast<const PCB_VIA*>( track );

        if( via->GetViaType() == VIATYPE::THROUGH )
        {
            const bool capped = via->GetCappingMode() == CAPPING_MODE::CAPPED;

            TransformCircleToPolygon( capped ? capped_outer_vias_poly : tht_outer_holes_poly, via->GetPosition(),
                                      via->GetDrill() / 2 + aPlatingThickness, via->GetMaxError(), ERROR_INSIDE );

            TransformCircleToPolygon( capped ? capped_inner_vias_poly : tht_inner_holes_poly, via->GetPosition(),
                                      via->GetDrill() / 2, via->GetMaxError(), ERROR_INSIDE );
        }
    }

    for( const FOOTPRINT* footprint : m_boardAdapter.GetBoard()->Footprints() )
    {
        for( const PAD* pad : footprint->Pads() )
        {
            if( pad->GetAttribute() != PAD_ATTRIB::NPTH )
            {
                if( !pad->HasHole() )
                    continue;

                pad->TransformHoleToPolygon( tht_outer_holes_poly, aPlatingThickness,
                                             pad->GetMaxError(), ERROR_INSIDE );
                pad->TransformHoleToPolygon( tht_inner_holes_poly, 0,
                                             pad->GetMaxError(), ERROR_INSIDE );
            }
        }
    }

    tht_outer_holes_poly.BooleanSubtract( tht_inner_holes_poly );
    tht_outer_holes_poly.BooleanSubtract( m_antiBoardPolys );

    tht_outer_holes_poly.BooleanAdd( capped_outer_vias_poly );
    tht_inner_holes_poly.BooleanAdd( capped_inner_vias_poly );

    CONTAINER_2D holesContainer;

    ConvertPolygonToTriangles( tht_outer_holes_poly, holesContainer,
                               aUnitScale, *m_boardAdapter.GetBoard() );

    const LIST_OBJECT2D& holes2D = holesContainer.GetList();

    if( holes2D.size() > 0 && m_boardAdapter.m_Cfg->m_Render.show_plated_barrels )
    {
        float layer_z_top, layer_z_bot, dummy;

        getLayerZPos( F_Cu, layer_z_top, dummy );
        getLayerZPos( B_Cu, dummy, layer_z_bot );

        std::shared_ptr<TRIANGLE_DISPLAY_LIST> layerTriangles = std::make_shared<TRIANGLE_DISPLAY_LIST>( holes2D.size() );

        for( const OBJECT_2D* itemOnLayer : holes2D )
        {
            const OBJECT_2D* object2d_A = itemOnLayer;

            wxASSERT( object2d_A->GetObjectType() == OBJECT_2D_TYPE::TRIANGLE );

            const TRIANGLE_2D* tri = static_cast<const TRIANGLE_2D*>( object2d_A );

            const SFVEC2F& v1 = tri->GetP1();
            const SFVEC2F& v2 = tri->GetP2();
            const SFVEC2F& v3 = tri->GetP3();

            addTopAndBottomTriangles( layerTriangles, v1, v2, v3, layer_z_top, layer_z_bot );
        }

        wxASSERT( tht_outer_holes_poly.OutlineCount() > 0 );

        if( tht_outer_holes_poly.OutlineCount() > 0 )
        {
            layerTriangles->AddToMiddleContours( tht_outer_holes_poly,
                                                 layer_z_bot, layer_z_top,
                                                 aUnitScale, false );

            assignRenderPtr( m_padHoles,
                             std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( layerTriangles, m_circleTexture, layer_z_top,
                                                                            layer_z_top ) );
        }
    }
}


void RENDER_3D_OPENGL::generateViaCovers( float aPlatingThickness3d, float aUnitScale )
{
    if( !m_boardAdapter.GetBoard() || m_boardAdapter.GetViaCount() <= 0 )
        return;

    std::shared_ptr<TRIANGLE_DISPLAY_LIST> frontCover = std::make_shared<TRIANGLE_DISPLAY_LIST>( m_boardAdapter.GetViaCount() );
    std::shared_ptr<TRIANGLE_DISPLAY_LIST> backCover = std::make_shared<TRIANGLE_DISPLAY_LIST>( m_boardAdapter.GetViaCount() );

    for( const PCB_TRACK* track : m_boardAdapter.GetBoard()->Tracks() )
    {
        if( track->Type() != PCB_VIA_T )
            continue;

        const PCB_VIA* via = static_cast<const PCB_VIA*>( track );

        const float holediameter = via->GetDrillValue() * aUnitScale;
        const float hole_radius = holediameter / 2.0f + 2.0f * aPlatingThickness3d;
        const SFVEC2F center( via->GetStart().x * aUnitScale,
                              -via->GetStart().y * aUnitScale );
        unsigned int seg = m_boardAdapter.GetCircleSegmentCount( via->GetDrillValue() );

        PCB_LAYER_ID top_layer, bottom_layer;
        via->LayerPair( &top_layer, &bottom_layer );
        float ztop, zbot, dummy;
        getLayerZPos( top_layer, ztop, dummy );
        getLayerZPos( bottom_layer, dummy, zbot );

        bool frontCovering = via->GetFrontCoveringMode() == COVERING_MODE::COVERED
                             || via->IsTented( F_Mask );
        bool backCovering = via->GetBackCoveringMode() == COVERING_MODE::COVERED
                            || via->IsTented( B_Mask );
        bool frontPlugged = via->GetFrontPluggingMode() == PLUGGING_MODE::PLUGGED;
        bool backPlugged  = via->GetBackPluggingMode() == PLUGGING_MODE::PLUGGED;
        bool filled = via->GetFillingMode() == FILLING_MODE::FILLED
                      || via->GetCappingMode() == CAPPING_MODE::CAPPED;

        const auto frontPostMachining =
                via->GetFrontPostMachining().value_or( PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED );
        const auto backPostMachining =
                via->GetBackPostMachining().value_or( PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED );

        bool hasFrontPostMachining = frontPostMachining != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED
                                     && frontPostMachining != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN;
        bool hasBackPostMachining = backPostMachining != PAD_DRILL_POST_MACHINING_MODE::NOT_POST_MACHINED
                                    && backPostMachining != PAD_DRILL_POST_MACHINING_MODE::UNKNOWN;

        bool hasFrontBackdrill = via->GetSecondaryDrillStartLayer() == F_Cu;
        bool hasBackBackdrill = via->GetSecondaryDrillStartLayer() == B_Cu;

        const float depth = hole_radius * 0.3f;

        if( frontCovering && !hasFrontPostMachining && !hasFrontBackdrill )
        {
            if( filled || !frontPlugged )
                generateDisk( center, hole_radius, ztop, seg, frontCover, true );
            else
                generateDimple( center, hole_radius, ztop, depth, seg, frontCover, true );
        }

        if( backCovering && !hasBackPostMachining && !hasBackBackdrill )
        {
            if( filled || !backPlugged )
                generateDisk( center, hole_radius, zbot, seg, backCover, false );
            else
                generateDimple( center, hole_radius, zbot, depth, seg, backCover, false );
        }
    }

    if( frontCover->m_layer_top_triangles->GetVertexSize() > 0 )
        assignRenderPtr( m_viaFrontCover,
                         std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( frontCover, 0, 0.0f, 0.0f ) );

    if( backCover->m_layer_bot_triangles->GetVertexSize() > 0 )
        assignRenderPtr( m_viaBackCover,
                         std::make_shared<OPENGL_RENDER_LIST_DEFERRED>( backCover, 0, 0.0f, 0.0f ) );
}


void RENDER_3D_OPENGL::generateViasAndPads()
{
    if( !m_boardAdapter.GetBoard() )
        return;

    const int   platingThickness    = m_boardAdapter.GetHolePlatingThickness();
    const float unitScale           = m_boardAdapter.BiuTo3dUnits();
    const float platingThickness3d  = platingThickness * unitScale;

    generateViaBarrels( platingThickness3d, unitScale );
    generatePlatedHoleShells( platingThickness, unitScale );
    generateViaCovers( platingThickness3d, unitScale );
}


void RENDER_3D_OPENGL::Load3dModelsIfNeeded()
{
    if( m_3dModelMap.size() > 0 )
        return;

    load3dModels();
}


void RENDER_3D_OPENGL::load3dModels( std::stop_token aStop )
{
    if( !m_boardAdapter.GetBoard() )
        return;

    // Building the 3D models late crashes on recent versions of macOS
    // Unclear the exact mechanism, but as a workaround, just build them
    // all the time.  See https://gitlab.com/kicad/code/kicad/-/issues/17198
#ifndef __WXMAC__
    if( !m_boardAdapter.m_IsPreviewer
          && !m_boardAdapter.m_Cfg->m_Render.show_footprints_normal
          && !m_boardAdapter.m_Cfg->m_Render.show_footprints_insert
          && !m_boardAdapter.m_Cfg->m_Render.show_footprints_virtual )
    {
        return;
    }
#endif

    S3D_CACHE* cacheMgr = m_boardAdapter.Get3dCacheManager();

    // Go for all footprints
    for( const FOOTPRINT* footprint : m_boardAdapter.GetBoard()->Footprints() )
    {
        const wxString footprintBasePath = m_boardAdapter.GetFootprintLibraryPath( footprint );

        for( const FP_3DMODEL& fp_model : footprint->Models() )
        {
            if( fp_model.m_Show && !fp_model.m_Filename.empty() )
            {
                if( m_activityReporter )
                {
                    // Display the short filename of the 3D fp_model loaded:
                    // (the full name is usually too long to be displayed)
                    wxFileName fn( fp_model.m_Filename );
                    m_activityReporter->Report( wxString::Format( _( "Loading %s..." ), fn.GetFullName() ) );
                }

                // Check if the fp_model is not present in our cache map
                // (Not already loaded in memory)
                if( !m_3dModelMap.contains( fp_model.m_Filename ) )
                {
                    // It is not present, try get it from cache
                    std::vector<const EMBEDDED_FILES*> embeddedFilesStack;
                    embeddedFilesStack.push_back( footprint->GetEmbeddedFiles() );
                    embeddedFilesStack.push_back( m_boardAdapter.GetBoard()->GetEmbeddedFiles() );

                    const S3DMODEL* modelPtr = cacheMgr->GetModel( fp_model.m_Filename, footprintBasePath,
                                                                   std::move( embeddedFilesStack ) );

                    // only add it if the return is not NULL
                    if( modelPtr )
                    {
                        MATERIAL_MODE materialMode = m_boardAdapter.m_Cfg->m_Render.material_mode;
                        auto          model = std::make_shared<MODEL_3D_DEFERRED>( *modelPtr, materialMode );

                        assignRenderMap( m_3dModelMap, fp_model.m_Filename, model );
                    }
                }
            }

            if( aStop.stop_requested() )
                return;
        }

        sendRefreshView();
    }
}
