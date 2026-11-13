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

#pragma once

#include <array>
#include <memory>
#include <vector>

#include <math/vector2d.h>

#include <layer_ids.h>
#include <wx/string.h>

class SHAPE_POLY_SET;
class ZONE;

/**
 * DRC measures zone clearances against a fill's triangles (the zone R-trees index them).  A
 * triangulation that does not cover the fill exactly makes DRC see copper where there is none
 * (false errors) or, worse, miss copper that is there (missed shorts).  The DRC cache validates
 * each zone fill's triangulation once per run; DRC_RTREE then indexes a zone that failed by a
 * re-triangulation that validates or, failing that, by its outline, which is exact (closed
 * outlines collide as filled areas).
 */
namespace DRC_ZONE_TRIANGULATION
{
using TRIANGLE = std::array<VECTOR2I, 3>;

/**
 * Check that the cached triangulation of @a aFill tiles it.  Turn every triangle
 * counter-clockwise, so it lies on the left of its three directed edges.  The check passes when:
 *  - every edge either is matched by the same edge, the other way, of as many other triangles
 *    (an inside edge), or is the only one of its kind and runs along an outline or hole of the
 *    fill the way that ring goes with the fill on its left, with every ring point it skips
 *    within 100 nm of it (a boundary edge); and
 *  - the pieces of ring the boundary edges stand for go round every ring exactly once, end to
 *    end (a ring no wider than 100 nm may be left out).
 *
 * Why that is enough: the number of triangles covering a point p is the winding number about p
 * of the triangles' boundaries added up.  Inside edges cancel, leaving the boundary edges.
 * Each boundary edge differs from its piece of ring by a loop within 100 nm of the edge, and the
 * pieces add up to the fill's own outlines and holes.  So, at any p more than 200 nm from the
 * fill's outline, the triangles cover p exactly as often as the fill does: once inside the
 * fill and never outside.  No copper wider than that is missed (no missed shorts) and none is
 * added (no false errors); 200 nm is below anything DRC resolves.
 *
 * Two quicker tests run first and can only fail a triangulation, never pass one: the triangles
 * of each polygon turn one way, and their area matches the fill's.
 *
 * @param aWhy if not null, receives the first failure found.
 * @return true if the triangulation can be trusted.
 */
bool Validate( const SHAPE_POLY_SET& aFill, wxString* aWhy = nullptr );

/**
 * Validate( aFill ) with the triangles given, one list per triangulated polygon, instead of
 * the fill's cached triangulation (for tests: a saved triangulation of a real fill).  With
 * @a aQuickTests false, only the tests the guarantee rests on run (pairing, boundary edges,
 * rings covered once), to show those alone reject a triangulation.
 */
bool Validate( const SHAPE_POLY_SET& aFill, const std::vector<std::vector<TRIANGLE>>& aTriangles,
               wxString* aWhy = nullptr, bool aQuickTests = true );

/// Write @a aFill's outlines and holes and its cached triangles to @a aPath, as text.  Check()
/// does so for each fill that fails when KICAD_DRC_DUMP_TRIANGULATION names a directory.
bool Dump( const SHAPE_POLY_SET& aFill, const wxString& aPath );

/// Read back what Dump() wrote: the fill into @a aFill, its triangles into @a aTriangles.
bool Load( const wxString& aPath, SHAPE_POLY_SET& aFill, std::vector<std::vector<TRIANGLE>>& aTriangles );

/// Validate @a aZone's fill on @a aLayer and record the result for Trusted().  Thread-safe.
void Check( const ZONE* aZone, PCB_LAYER_ID aLayer );

/// Whether DRC may use @a aZone's fill triangles on @a aLayer (true unless Check() failed it).
bool Trusted( const ZONE* aZone, PCB_LAYER_ID aLayer );

/**
 * For a fill that failed validation, the same copper re-triangulated (after a union) whose
 * triangulation validates, or null if there is none and DRC must use the fill's outline.
 */
std::shared_ptr<SHAPE_POLY_SET> Replacement( const ZONE* aZone, PCB_LAYER_ID aLayer );
} // namespace DRC_ZONE_TRIANGULATION
