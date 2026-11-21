/*
 * This program source code file is part of KiCad, a free EDA CAD application.
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

#include "agent_edit_flash.h"

#include <gal/graphics_abstraction_layer.h>
#include <layer_ids.h>
#include <view/view.h>

#include <algorithm>
#include <cmath>

namespace
{
// Shown at full strength, then removed: no fade, so an edit costs two overlay repaints
// (each one a full-frame composite, ~13 ms on a software renderer), not fifty.
constexpr double FLASH_SECONDS = 1.5;

// An expired flash goes with the next edit's own redraw; only after this long without one is
// it removed by a repaint of its own.  So while an agent works, an edit costs one redraw.
constexpr int IDLE_REMOVE_MS = 4000;
}


AGENT_EDIT_FLASH::AGENT_EDIT_FLASH( KIGFX::VIEW* aView, std::function<void()> aRefresh ) :
        EDA_ITEM( NOT_USED ),
        m_view( aView ),
        m_refresh( std::move( aRefresh ) ),
        m_timer( this )
{
    Bind( wxEVT_TIMER, &AGENT_EDIT_FLASH::onTimer, this );
}


AGENT_EDIT_FLASH::~AGENT_EDIT_FLASH()
{
    m_timer.Stop();

    if( m_added && m_view )
        m_view->Remove( this );
}


wxString AGENT_EDIT_FLASH::ActorLabel( const std::string& aActor )
{
    wxString label = wxString::FromUTF8( aActor );
    label.Replace( wxT( "#rebased" ), wxEmptyString );
    label.Replace( wxT( ":" ), wxT( " " ) );
    return label;
}


void AGENT_EDIT_FLASH::Flash( const std::vector<BOX2I>& aBoxes, const wxString& aLabel )
{
    if( aBoxes.empty() || !m_view )
        return;

    // Expired flashes leave in this same repaint.
    std::erase_if( m_entries, [&]( const ENTRY& e ) { return alpha( e ) <= 0.0; } );
    m_entries.push_back( { aBoxes, aLabel, std::chrono::steady_clock::now() } );

    if( !m_added )
    {
        m_view->Add( this );
        m_added = true;
    }

    repaint( false );   // drawn by the edit's own redraw

    // One wake-up, only if no further edit comes to take the flash away with its own redraw.
    m_timer.StartOnce( IDLE_REMOVE_MS );
}


void AGENT_EDIT_FLASH::SetZones( const std::vector<ZONE>& aZones, bool aRefresh )
{
    auto same = [&]()
    {
        if( aZones.size() != m_zones.size() )
            return false;

        for( size_t ii = 0; ii < aZones.size(); ++ii )
        {
            if( aZones[ii].box != m_zones[ii].box || aZones[ii].label != m_zones[ii].label
                || aZones[ii].paused != m_zones[ii].paused )
            {
                return false;
            }
        }

        return true;
    };

    if( same() )
        return;   // nothing to repaint

    m_zones = aZones;

    if( !m_zones.empty() && !m_added )
    {
        m_view->Add( this );
        m_added = true;
    }

    repaint( aRefresh );
}


void AGENT_EDIT_FLASH::SetGhost( bool aShow, const BOX2I& aMine, const BOX2I& aAgents,
                                 const wxString& aLabel )
{
    m_ghostShown = aShow;
    m_ghostMine = aMine;
    m_ghostAgents = aAgents;
    m_ghostLabel = aLabel;

    if( aShow && !m_added )
    {
        m_view->Add( this );
        m_added = true;
    }
    else if( !aShow && m_added && m_entries.empty() && m_zones.empty() )
    {
        m_view->Remove( this );
        m_added = false;
    }

    repaint( true );
}


void AGENT_EDIT_FLASH::repaint( bool aRefresh )
{
    // Only this overlay item: the overlay target composites over the cached board layers, so
    // nothing of the board is redrawn (never MarkDirty(), which redraws every target).
    if( m_added )
        m_view->Update( this, KIGFX::GEOMETRY | KIGFX::REPAINT );

    m_view->MarkTargetDirty( KIGFX::TARGET_OVERLAY );

    // Inside an edit's commit the board's own redraw follows and paints this too: asking for a
    // paint here would draw the frame twice.
    if( aRefresh && m_refresh )
        m_refresh();
}


double AGENT_EDIT_FLASH::alpha( const ENTRY& aEntry ) const
{
    double t = std::chrono::duration<double>( std::chrono::steady_clock::now() - aEntry.start ).count();

    return t < FLASH_SECONDS ? 1.0 : 0.0;
}


void AGENT_EDIT_FLASH::onTimer( wxTimerEvent& aEvent )
{
    std::erase_if( m_entries, [&]( const ENTRY& e ) { return alpha( e ) <= 0.0; } );

    if( m_entries.empty() )
    {
        if( m_added && !m_ghostShown && m_zones.empty() )
        {
            m_view->Remove( this );
            m_added = false;
        }
    }
    else
    {
        m_timer.StartOnce( IDLE_REMOVE_MS );
    }

    repaint( true );
}


const BOX2I AGENT_EDIT_FLASH::ViewBBox() const
{
    BOX2I bbox;
    bool  first = true;

    for( const ENTRY& e : m_entries )
    {
        for( const BOX2I& box : e.boxes )
        {
            if( first )
                bbox = box;
            else
                bbox.Merge( box );

            first = false;
        }
    }

    for( const ZONE& z : m_zones )
    {
        if( first )
            bbox = z.box;
        else
            bbox.Merge( z.box );

        first = false;
    }

    if( m_ghostShown )
    {
        if( first )
            bbox = m_ghostMine;
        else
            bbox.Merge( m_ghostMine );

        bbox.Merge( m_ghostAgents );
    }

    // Room for the label above the first box.
    bbox.Inflate( 3000000 );
    return bbox;
}


std::vector<int> AGENT_EDIT_FLASH::ViewGetLayers() const
{
    return { LAYER_SELECT_OVERLAY };
}


void AGENT_EDIT_FLASH::ViewDraw( int aLayer, KIGFX::VIEW* aView ) const
{
    KIGFX::GAL* gal = aView->GetGAL();
    KIGFX::GAL_SCOPED_ATTRS attrs( *gal, KIGFX::GAL_SCOPED_ATTRS::ALL_ATTRS );
    const KIGFX::COLOR4D color = AgentColor();
    const double pad = aView->ToWorld( 4.0 );
    const double outline = aView->ToWorld( 2.0 );

    // Working areas: a dashed frame per agent, its task on a chip.  Steady (no animation).
    for( const ZONE& z : m_zones )
    {
        KIGFX::COLOR4D c = z.paused ? KIGFX::COLOR4D( 0.6, 0.6, 0.6, 1.0 ) : color;
        BOX2I          b = z.box;
        b.Inflate( (int) aView->ToWorld( 10.0 ) );

        gal->SetIsFill( true );
        gal->SetIsStroke( false );
        gal->SetFillColor( c.WithAlpha( 0.06 ) );
        gal->DrawRectangle( b.GetOrigin(), b.GetEnd() );

        gal->SetIsFill( false );
        gal->SetIsStroke( true );
        gal->SetStrokeColor( c.WithAlpha( 0.8 ) );
        gal->SetLineWidth( aView->ToWorld( 1.5 ) );
        VECTOR2D pts[5] = { b.GetOrigin(), VECTOR2D( b.GetRight(), b.GetTop() ), b.GetEnd(),
                            VECTOR2D( b.GetLeft(), b.GetBottom() ), b.GetOrigin() };
        double   dash = aView->ToWorld( 8.0 );

        for( int ii = 0; ii < 4; ++ii )
        {
            VECTOR2D a = pts[ii], e = pts[ii + 1];
            double   len = ( e - a ).EuclideanNorm();
            VECTOR2D dir = len > 0 ? ( e - a ) / len : VECTOR2D();

            for( double t = 0; t < len; t += 2 * dash )
                gal->DrawLine( a + dir * t, a + dir * std::min( len, t + dash ) );
        }

        const double glyph = aView->ToWorld( 12.0 );
        const double chipH = glyph * 1.7;
        const double chipW = glyph * 0.62 * z.label.Length() + glyph;
        VECTOR2D     chip( b.GetLeft(), b.GetTop() - chipH );

        gal->SetIsFill( true );
        gal->SetIsStroke( false );
        gal->SetFillColor( c.WithAlpha( 0.92 ) );
        gal->DrawRectangle( chip, chip + VECTOR2D( chipW, chipH ) );
        gal->SetIsFill( false );
        gal->SetIsStroke( true );
        gal->SetStrokeColor( KIGFX::COLOR4D( 1, 1, 1, 1 ) );
        gal->SetGlyphSize( VECTOR2D( glyph, glyph ) );
        gal->SetLineWidth( glyph / 8 );
        gal->SetHorizontalJustify( GR_TEXT_H_ALIGN_LEFT );
        gal->SetVerticalJustify( GR_TEXT_V_ALIGN_CENTER );
        gal->BitmapText( z.label, chip + VECTOR2D( glyph * 0.5, chipH / 2 ), ANGLE_HORIZONTAL );
    }

    if( m_ghostShown )
    {
        BOX2I g = m_ghostAgents;
        g.Inflate( (int) pad );
        gal->SetIsFill( true );
        gal->SetIsStroke( false );
        gal->SetFillColor( color.WithAlpha( 0.15 ) );
        gal->DrawRectangle( g.GetOrigin(), g.GetEnd() );

        // Dashed outline: the agent's version is a proposal.
        gal->SetIsFill( false );
        gal->SetIsStroke( true );
        gal->SetStrokeColor( color.WithAlpha( 0.95 ) );
        gal->SetLineWidth( outline );
        VECTOR2D corners[5] = { g.GetOrigin(), VECTOR2D( g.GetRight(), g.GetTop() ), g.GetEnd(),
                                VECTOR2D( g.GetLeft(), g.GetBottom() ), g.GetOrigin() };
        double   dash = aView->ToWorld( 6.0 );

        for( int ii = 0; ii < 4; ++ii )
        {
            VECTOR2D a = corners[ii], b = corners[ii + 1];
            double   len = ( b - a ).EuclideanNorm();
            VECTOR2D dir = len > 0 ? ( b - a ) / len : VECTOR2D();

            for( double t = 0; t < len; t += 2 * dash )
                gal->DrawLine( a + dir * t, a + dir * std::min( len, t + dash ) );
        }

        // Yours -> theirs.
        VECTOR2D from = m_ghostMine.Centre(), to = m_ghostAgents.Centre();

        if( ( to - from ).EuclideanNorm() > aView->ToWorld( 8.0 ) )
        {
            gal->DrawLine( from, to );
            VECTOR2D d = ( to - from ).Resize( aView->ToWorld( 9.0 ) );

            for( double a : { 0.4, -0.4 } )
            {
                VECTOR2D r( d.x * std::cos( a ) - d.y * std::sin( a ),
                            d.x * std::sin( a ) + d.y * std::cos( a ) );
                gal->DrawLine( to, to - r );
            }
        }

        const double glyph = aView->ToWorld( 12.0 );
        gal->SetGlyphSize( VECTOR2D( glyph, glyph ) );
        gal->SetLineWidth( glyph / 8 );
        gal->SetHorizontalJustify( GR_TEXT_H_ALIGN_LEFT );
        gal->SetVerticalJustify( GR_TEXT_V_ALIGN_BOTTOM );
        gal->BitmapText( m_ghostLabel, VECTOR2I( g.GetLeft(), g.GetTop() - (int) pad ),
                         ANGLE_HORIZONTAL );
    }

    for( const ENTRY& e : m_entries )
    {
        double a = alpha( e );

        if( a <= 0.0 )
            continue;

        BOX2I all;
        bool  first = true;

        for( BOX2I box : e.boxes )
        {
            box.Inflate( (int) pad );
            gal->SetIsFill( true );
            gal->SetIsStroke( true );
            gal->SetFillColor( color.WithAlpha( 0.22 * a ) );
            gal->SetStrokeColor( color.WithAlpha( 0.95 * a ) );
            gal->SetLineWidth( outline );
            gal->DrawRectangle( box.GetOrigin(), box.GetEnd() );

            if( first )
                all = box;
            else
                all.Merge( box );

            first = false;
        }

        if( e.label.IsEmpty() )
            continue;

        // A label chip above the top-left of the change.
        const double glyph = aView->ToWorld( 12.0 );
        const double chipH = glyph * 1.7;
        const double chipW = glyph * 0.62 * e.label.Length() + glyph;
        VECTOR2D     topLeft( all.GetLeft(), all.GetTop() - chipH - pad );

        gal->SetIsFill( true );
        gal->SetIsStroke( false );
        gal->SetFillColor( color.WithAlpha( 0.92 * a ) );
        gal->DrawRectangle( topLeft, topLeft + VECTOR2D( chipW, chipH ) );

        gal->SetIsFill( false );
        gal->SetIsStroke( true );
        gal->SetStrokeColor( KIGFX::COLOR4D( 1, 1, 1, a ) );
        gal->SetGlyphSize( VECTOR2D( glyph, glyph ) );
        gal->SetLineWidth( glyph / 8 );
        gal->SetHorizontalJustify( GR_TEXT_H_ALIGN_LEFT );
        gal->SetVerticalJustify( GR_TEXT_V_ALIGN_CENTER );
        gal->BitmapText( e.label, topLeft + VECTOR2D( glyph * 0.5, chipH / 2 ), ANGLE_HORIZONTAL );
    }
}
