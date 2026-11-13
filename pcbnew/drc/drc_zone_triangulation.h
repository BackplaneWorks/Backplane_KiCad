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

#include <layer_ids.h>
#include <wx/string.h>

class SHAPE_POLY_SET;
class ZONE;

/**
 * DRC measures zone clearances against a fill's triangles (the zone R-trees index them).  A
 * triangulation that does not cover the fill exactly makes DRC see copper where there is none
 * (false errors) or, worse, miss copper that is there (missed shorts).  The DRC cache validates
 * each zone fill's triangulation once per run; DRC_RTREE then indexes a zone that failed by its
 * outline instead of its triangles, which is exact (closed outlines collide as filled areas).
 */
namespace DRC_ZONE_TRIANGULATION
{
/**
 * Check that the cached triangulation of @a aFill tiles it: all triangles of a polygon turn the
 * same way (none is folded over its neighbours), every triangle edge is either shared with
 * exactly one other triangle in the opposite direction or runs along the fill's outline with
 * every outline point it skips within 100 nm of it, no outline vertex lies inside a triangle,
 * and the triangles' area matches the fill's.
 *
 * Fill the triangles miss is bounded either by an edge that is neither shared nor on the
 * outline, which fails the check, or by a boundary edge and the outline, which are less than
 * 100 nm apart.  So a validated triangulation misses no copper wider than 100 nm, below DRC's
 * resolution.
 *
 * @param aWhy if not null, receives the first failure found.
 * @return true if the triangulation can be trusted.
 */
bool Validate( const SHAPE_POLY_SET& aFill, wxString* aWhy = nullptr );

/// Validate @a aZone's fill on @a aLayer and record the result for Trusted().  Thread-safe.
void Check( const ZONE* aZone, PCB_LAYER_ID aLayer );

/// Whether DRC may use @a aZone's fill triangles on @a aLayer (true unless Check() failed it).
bool Trusted( const ZONE* aZone, PCB_LAYER_ID aLayer );
} // namespace DRC_ZONE_TRIANGULATION
