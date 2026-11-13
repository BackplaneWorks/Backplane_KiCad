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
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <preview_items/host_markers_item.h>

#include <gal/color4d.h>
#include <gal/graphics_abstraction_layer.h>
#include <view/view.h>

#include <limits>


HOST_MARKERS_ITEM::HOST_MARKERS_ITEM() :
        EDA_ITEM( NOT_USED )   // never part of a document
{
}


int HOST_MARKERS_ITEM::SetMarkers( const std::string& aSet, std::vector<HOST_MARKER> aMarkers )
{
    int count = static_cast<int>( aMarkers.size() );

    if( aMarkers.empty() )
        m_sets.erase( aSet );
    else
        m_sets[aSet] = std::move( aMarkers );

    return count;
}


std::vector<const HOST_MARKER*> HOST_MARKERS_ITEM::All() const
{
    std::vector<const HOST_MARKER*> all;

    for( const auto& [set, markers] : m_sets )
    {
        for( const HOST_MARKER& marker : markers )
            all.push_back( &marker );
    }

    return all;
}


bool HOST_MARKERS_ITEM::Empty() const
{
    return m_sets.empty();
}


const HOST_MARKER* HOST_MARKERS_ITEM::HitTest( const VECTOR2I& aWorld, int aRadius ) const
{
    const HOST_MARKER* best = nullptr;
    double             bestDist = std::numeric_limits<double>::max();

    for( const auto& [set, markers] : m_sets )
    {
        for( const HOST_MARKER& marker : markers )
        {
            if( !shown( marker ) )
                continue;

            double dist = ( marker.pos - aWorld ).EuclideanNorm();

            if( dist <= aRadius && dist < bestDist )
            {
                best = &marker;
                bestDist = dist;
            }
        }
    }

    return best;
}


const BOX2I HOST_MARKERS_ITEM::ViewBBox() const
{
    // Drawn a fixed size on screen: the whole plane, like the other overlays
    BOX2I bbox;
    bbox.SetMaximum();
    return bbox;
}


void HOST_MARKERS_ITEM::ViewDraw( int aLayer, KIGFX::VIEW* aView ) const
{
    KIGFX::GAL* gal = aView->GetGAL();

    // A ring and a dot a fixed size on screen, in the host's colours: distinct from KiCad's own
    // DRC/ERC arrows at a glance
    const double radius = aView->ToWorld( 9.0 );
    const double ring = aView->ToWorld( 2.5 );
    const double dot = aView->ToWorld( 3.0 );

    auto colour = []( int aSeverity )
                  {
                      switch( aSeverity )
                      {
                      case 2:  return KIGFX::COLOR4D( 0.93, 0.27, 0.62, 1.0 );   // error: magenta
                      case 1:  return KIGFX::COLOR4D( 0.98, 0.66, 0.18, 1.0 );   // warning: amber
                      default: return KIGFX::COLOR4D( 0.25, 0.70, 0.98, 1.0 );   // info: sky
                      }
                  };

    gal->Save();
    gal->SetIsStroke( true );
    gal->SetIsFill( false );

    for( const auto& [set, markers] : m_sets )
    {
        for( const HOST_MARKER& marker : markers )
        {
            if( !shown( marker ) )
                continue;

            KIGFX::COLOR4D c = colour( marker.severity );
            VECTOR2D       at( marker.pos );

            gal->SetIsFill( true );
            gal->SetIsStroke( false );
            gal->SetFillColor( c.WithAlpha( 0.22 ) );
            gal->DrawCircle( at, radius );

            gal->SetIsFill( false );
            gal->SetIsStroke( true );
            gal->SetStrokeColor( c );
            gal->SetLineWidth( ring );
            gal->DrawCircle( at, radius );

            gal->SetIsFill( true );
            gal->SetIsStroke( false );
            gal->SetFillColor( c );
            gal->DrawCircle( at, dot );
        }
    }

    gal->Restore();
}
