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

#include "plan_view_item.h"
#include "board_plan.h"

#include <board.h>
#include <footprint.h>
#include <gal/graphics_abstraction_layer.h>
#include <geometry/shape_line_chain.h>
#include <geometry/shape_poly_set.h>
#include <pad.h>
#include <view/view.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

using KIGFX::COLOR4D;


namespace
{
// On top of every board layer (the general-purpose overlay sits below them in pcbnew's order, so
// pads, silk and the ratsnest drew over the plan and its labels).
constexpr int PLAN_GAL_LAYER = LAYER_SELECT_OVERLAY;

// The plan view's palette: a near-black board, light outlines, grey parts.
const COLOR4D BACKDROP( 0.075, 0.090, 0.090, 1.0 );   // plan view: the board recedes entirely
const COLOR4D OUTLINE( 0.86, 0.88, 0.88, 0.95 );
const COLOR4D PART_FILL( 0.20, 0.23, 0.23, 0.55 );
const COLOR4D PART_EDGE( 0.52, 0.56, 0.56, 0.85 );
const COLOR4D PART_TEXT( 0.80, 0.83, 0.83, 0.95 );
const COLOR4D HEADER_TEXT( 0.90, 0.92, 0.92, 1.0 );
const COLOR4D HEADER_DIM( 0.62, 0.66, 0.66, 1.0 );
const COLOR4D SELECT( 1.0, 1.0, 1.0, 1.0 );
const COLOR4D WARN( 0.95, 0.35, 0.30, 1.0 );
const COLOR4D NOTE_FILL( 0.98, 0.84, 0.36, 0.95 );
const COLOR4D VERIFIED( 0.36, 0.82, 0.45, 1.0 );
const COLOR4D PROBLEM( 0.95, 0.30, 0.27, 1.0 );

const char* const PALETTE[] = { "#e8a33d", "#d9534f", "#3b7ddd", "#2bb3a3", "#9aa3a3",
                                "#9b6bd3", "#6dbf4b", "#e36fa7", "#c0583a", "#5a8fd0" };

int segsFor( KIGFX::VIEW* aView )
{
    // Corner rounding fine enough for the zoom.
    return std::max( 2000, (int) aView->ToWorld( 0.5 ) );
}

SHAPE_POLY_SET band( const std::vector<VECTOR2I>& aPath, int aWidth, int aMaxError )
{
    SHAPE_POLY_SET out;

    if( aPath.size() < 2 || aWidth <= 0 )
        return out;

    SHAPE_LINE_CHAIN chain;

    for( const VECTOR2I& p : aPath )
        chain.Append( p );

    out.OffsetLineChain( chain, aWidth / 2, CORNER_STRATEGY::ROUND_ALL_CORNERS, aMaxError, true );
    return out;
}

VECTOR2I midpoint( const std::vector<VECTOR2I>& aPath )
{
    double total = 0;

    for( size_t ii = 1; ii < aPath.size(); ++ii )
        total += ( aPath[ii] - aPath[ii - 1] ).EuclideanNorm();

    double half = total / 2;

    for( size_t ii = 1; ii < aPath.size(); ++ii )
    {
        double seg = ( aPath[ii] - aPath[ii - 1] ).EuclideanNorm();

        if( half <= seg && seg > 0 )
        {
            VECTOR2D d = VECTOR2D( aPath[ii] - aPath[ii - 1] ) * ( half / seg );
            return aPath[ii - 1] + VECTOR2I( KiROUND( d.x ), KiROUND( d.y ) );
        }

        half -= seg;
    }

    return aPath.empty() ? VECTOR2I() : aPath.front();
}

void text( KIGFX::GAL* aGal, const wxString& aText, const VECTOR2D& aAt, double aSize,
           const COLOR4D& aColor, bool aBold = false,
           GR_TEXT_H_ALIGN_T aH = GR_TEXT_H_ALIGN_CENTER,
           GR_TEXT_V_ALIGN_T aV = GR_TEXT_V_ALIGN_CENTER )
{
    aGal->AdvanceDepth();
    aGal->SetIsFill( false );
    aGal->SetIsStroke( true );
    aGal->SetStrokeColor( aColor );
    aGal->SetFillColor( aColor );
    aGal->SetGlyphSize( VECTOR2D( aSize, aSize ) );
    aGal->SetLineWidth( aSize / 7 );
    aGal->SetFontBold( aBold );
    aGal->SetFontItalic( false );
    aGal->SetHorizontalJustify( aH );
    aGal->SetVerticalJustify( aV );
    aGal->BitmapText( aText, VECTOR2I( KiROUND( aAt.x ), KiROUND( aAt.y ) ), ANGLE_HORIZONTAL );
}

void pill( KIGFX::GAL* aGal, const VECTOR2D& aCenter, double aW, double aH, const COLOR4D& aFill,
           const COLOR4D& aEdge, double aEdgeWidth )
{
    // A rounded pill: a thick segment.
    double r = aH / 2;
    double half = std::max( 0.0, aW / 2 - r );
    aGal->AdvanceDepth();
    aGal->SetIsFill( true );
    aGal->SetIsStroke( false );
    aGal->SetFillColor( aEdge );
    aGal->DrawSegment( aCenter - VECTOR2D( half, 0 ), aCenter + VECTOR2D( half, 0 ),
                       aH + 2 * aEdgeWidth );
    aGal->SetFillColor( aFill );
    aGal->DrawSegment( aCenter - VECTOR2D( half, 0 ), aCenter + VECTOR2D( half, 0 ), aH );
}

void dashed( KIGFX::GAL* aGal, const std::vector<VECTOR2D>& aPts, bool aClosed, double aDash,
             double aGap )
{
    size_t n = aPts.size();

    for( size_t ii = 0; ii + ( aClosed ? 0 : 1 ) < n; ++ii )
    {
        VECTOR2D a = aPts[ii];
        VECTOR2D b = aPts[( ii + 1 ) % n];
        double   len = ( b - a ).EuclideanNorm();

        if( len <= 0 )
            continue;

        VECTOR2D dir = ( b - a ) / len;

        for( double t = 0; t < len; t += aDash + aGap )
            aGal->DrawLine( a + dir * t, a + dir * std::min( len, t + aDash ) );
    }
}

void outline( KIGFX::GAL* aGal, const SHAPE_POLY_SET& aPoly, bool aDashed, double aPx )
{
    for( int ii = 0; ii < aPoly.OutlineCount(); ++ii )
    {
        const SHAPE_LINE_CHAIN& chain = aPoly.COutline( ii );

        if( aDashed )
        {
            std::vector<VECTOR2D> pts;

            for( int jj = 0; jj < chain.PointCount(); ++jj )
                pts.emplace_back( chain.CPoint( jj ) );

            dashed( aGal, pts, true, 5 * aPx, 4 * aPx );
        }
        else
        {
            aGal->DrawPolyline( chain );
        }
    }
}

/// The first aFraction (0..1) of a path, by length.
std::vector<VECTOR2I> prefix( const std::vector<VECTOR2I>& aPath, double aFraction )
{
    double total = 0;

    for( size_t ii = 1; ii < aPath.size(); ++ii )
        total += ( aPath[ii] - aPath[ii - 1] ).EuclideanNorm();

    double                want = total * std::clamp( aFraction, 0.0, 1.0 );
    std::vector<VECTOR2I> out;

    if( aPath.empty() )
        return out;

    out.push_back( aPath.front() );

    for( size_t ii = 1; ii < aPath.size() && want > 0; ++ii )
    {
        double seg = ( aPath[ii] - aPath[ii - 1] ).EuclideanNorm();

        if( seg <= want )
        {
            out.push_back( aPath[ii] );
            want -= seg;
        }
        else
        {
            VECTOR2D d = VECTOR2D( aPath[ii] - aPath[ii - 1] ) * ( want / seg );
            out.push_back( aPath[ii - 1] + VECTOR2I( KiROUND( d.x ), KiROUND( d.y ) ) );
            want = 0;
        }
    }

    return out;
}

// Small vector icons, drawn at a centre with a size (the font has no such glyphs).
void iconCheck( KIGFX::GAL* aGal, const VECTOR2D& aAt, double aSize, const COLOR4D& aColor )
{
    aGal->SetIsStroke( true );
    aGal->AdvanceDepth();
    aGal->SetIsFill( false );
    aGal->SetStrokeColor( aColor );
    aGal->SetLineWidth( aSize * 0.18 );
    aGal->DrawLine( aAt + VECTOR2D( -0.4, 0.0 ) * aSize, aAt + VECTOR2D( -0.1, 0.3 ) * aSize );
    aGal->DrawLine( aAt + VECTOR2D( -0.1, 0.3 ) * aSize, aAt + VECTOR2D( 0.45, -0.35 ) * aSize );
}

void iconLock( KIGFX::GAL* aGal, const VECTOR2D& aAt, double aSize, const COLOR4D& aColor )
{
    aGal->AdvanceDepth();
    aGal->SetIsFill( true );
    aGal->SetIsStroke( false );
    aGal->SetFillColor( aColor );
    aGal->DrawRectangle( aAt + VECTOR2D( -0.35, -0.05 ) * aSize, aAt + VECTOR2D( 0.35, 0.45 ) * aSize );
    aGal->AdvanceDepth();
    aGal->SetIsFill( false );
    aGal->SetIsStroke( true );
    aGal->SetStrokeColor( aColor );
    aGal->SetLineWidth( aSize * 0.13 );
    aGal->DrawArc( aAt + VECTOR2D( 0, -0.12 ) * aSize, aSize * 0.22, ANGLE_180, ANGLE_180 );
    aGal->DrawLine( aAt + VECTOR2D( -0.22, -0.12 ) * aSize, aAt + VECTOR2D( -0.22, -0.05 ) * aSize );
    aGal->DrawLine( aAt + VECTOR2D( 0.22, -0.12 ) * aSize, aAt + VECTOR2D( 0.22, -0.05 ) * aSize );
}

void iconPencil( KIGFX::GAL* aGal, const VECTOR2D& aAt, double aSize, const COLOR4D& aColor )
{
    aGal->SetIsStroke( true );
    aGal->AdvanceDepth();
    aGal->SetIsFill( false );
    aGal->SetStrokeColor( aColor );
    aGal->SetLineWidth( aSize * 0.2 );
    aGal->DrawLine( aAt + VECTOR2D( -0.35, 0.35 ) * aSize, aAt + VECTOR2D( 0.3, -0.3 ) * aSize );
    aGal->SetLineWidth( aSize * 0.08 );
    aGal->DrawLine( aAt + VECTOR2D( -0.45, 0.45 ) * aSize, aAt + VECTOR2D( -0.3, 0.3 ) * aSize );
}

void iconWarn( KIGFX::GAL* aGal, const VECTOR2D& aAt, double aSize, const COLOR4D& aColor )
{
    std::deque<VECTOR2D> tri = { aAt + VECTOR2D( 0, -0.45 ) * aSize,
                                 aAt + VECTOR2D( 0.5, 0.4 ) * aSize,
                                 aAt + VECTOR2D( -0.5, 0.4 ) * aSize };
    aGal->AdvanceDepth();
    aGal->SetIsFill( true );
    aGal->SetIsStroke( false );
    aGal->SetFillColor( aColor );
    aGal->DrawPolygon( tri );
    aGal->AdvanceDepth();
    aGal->SetIsFill( false );
    aGal->SetIsStroke( true );
    aGal->SetStrokeColor( COLOR4D( 0.1, 0.1, 0.1, 1 ) );
    aGal->SetLineWidth( aSize * 0.12 );
    aGal->DrawLine( aAt + VECTOR2D( 0, -0.15 ) * aSize, aAt + VECTOR2D( 0, 0.12 ) * aSize );
    aGal->DrawLine( aAt + VECTOR2D( 0, 0.24 ) * aSize, aAt + VECTOR2D( 0, 0.27 ) * aSize );
}

std::string layerName( const BOARD* aBoard, PCB_LAYER_ID aLayer )
{
    return LSET::Name( aLayer ).ToStdString( wxConvUTF8 );
}
} // namespace


PLAN_VIEW_ITEM::PLAN_VIEW_ITEM( BOARD* aBoard ) :
        EDA_ITEM( NOT_USED ),
        m_board( aBoard )
{
}


COLOR4D PLAN_VIEW_ITEM::ParseColor( const std::string& aHex, const COLOR4D& aDefault )
{
    if( aHex.size() != 7 || aHex[0] != '#' )
        return aDefault;

    try
    {
        unsigned long v = std::stoul( aHex.substr( 1 ), nullptr, 16 );
        return COLOR4D( ( ( v >> 16 ) & 0xff ) / 255.0, ( ( v >> 8 ) & 0xff ) / 255.0,
                        ( v & 0xff ) / 255.0, 1.0 );
    }
    catch( ... )
    {
        return aDefault;
    }
}


VECTOR2D PLAN_VIEW_ITEM::labelSpot( const SHAPE_POLY_SET& aPoly, double aW, double aH ) const
{
    // The bbox centre if it is inside and clear of part boxes, else the clearest of a grid of
    // candidates inside the region.
    BOX2I box = aPoly.BBox();
    auto  clear = [&]( const VECTOR2D& c )
    {
        BOX2I r( VECTOR2I( c.x - aW / 2, c.y - aH / 2 ), VECTOR2I( aW, aH ) );
        int   hits = 0;

        for( const FOOTPRINT* fp : m_board->Footprints() )
            hits += fp->GetBoundingBox( false ).Intersects( r );

        return hits;
    };

    VECTOR2D best = box.Centre();
    int      bestHits = aPoly.Contains( box.Centre() ) ? clear( best ) : INT_MAX;

    for( int iy = 1; iy < 6 && bestHits > 0; ++iy )
    {
        for( int ix = 1; ix < 6; ++ix )
        {
            VECTOR2D c( box.GetLeft() + box.GetWidth() * ix / 6.0, box.GetTop() + box.GetHeight() * iy / 6.0 );

            if( !aPoly.Contains( VECTOR2I( c ) ) )
                continue;

            int h = clear( c );

            if( h < bestHits )
            {
                best = c;
                bestHits = h;
            }
        }
    }

    return best;
}


wxString PLAN_VIEW_ITEM::ProblemText( const PLAN_PROGRESS::ITEM& aItem ) const
{
    if( !aItem.problems.empty() && aItem.problems.front() >= 0
        && aItem.problems.front() < (int) m_audit.warnings.size() )
    {
        wxString t = wxString::FromUTF8( m_audit.warnings[aItem.problems.front()].text );

        if( aItem.ProblemCount() > 1 )
            t << wxString::Format( _( " (+%d more)" ), aItem.ProblemCount() - 1 );

        return t;
    }

    if( aItem.drc > 0 )
        return wxString::Format( _( "%d DRC errors on its nets" ), aItem.drc );

    if( aItem.unrouted > 0 )
        return wxString::Format( _( "%d nets still unrouted" ), aItem.unrouted );

    return wxEmptyString;
}


wxString PLAN_VIEW_ITEM::Ordinal( int aN )
{
    switch( aN )
    {
    case 1: return _( "1st" );
    case 2: return _( "2nd" );
    case 3: return _( "3rd" );
    default: return wxString::Format( _( "%dth" ), aN );
    }
}


COLOR4D PLAN_VIEW_ITEM::CorridorColor( const BOARD_PLAN& aPlan, const PLAN_CORRIDOR& aCorridor )
{
    const std::string& key = aCorridor.group.empty() ? aCorridor.id : aCorridor.group;
    size_t hash = std::hash<std::string>()( key );
    COLOR4D fallback = ParseColor( PALETTE[hash % std::size( PALETTE )], COLOR4D::WHITE );

    if( const PLAN_GROUP* g = aPlan.FindGroup( aCorridor.group ) )
        return ParseColor( g->color, fallback );

    return fallback;
}


COLOR4D PLAN_VIEW_ITEM::PlaneColor( const PLAN_PLANE& aPlane )
{
    size_t hash = std::hash<std::string>()( aPlane.net );
    return ParseColor( aPlane.color,
                       ParseColor( PALETTE[hash % std::size( PALETTE )], COLOR4D::WHITE ) );
}


const BOX2I PLAN_VIEW_ITEM::ViewBBox() const
{
    // The header is drawn in screen space, anywhere on the canvas.
    BOX2I all;
    all.SetMaximum();
    return all;
}


std::vector<int> PLAN_VIEW_ITEM::ViewGetLayers() const
{
    return { PLAN_GAL_LAYER };
}


void PLAN_VIEW_ITEM::ViewDraw( int aLayer, KIGFX::VIEW* aView ) const
{
    if( aLayer != PLAN_GAL_LAYER || !m_board || !m_shown )
        return;

    const BOARD_PLAN& plan = m_board->GetPlan();

    if( plan.Empty() && !m_planView && m_draft.empty() )
        return;

    KIGFX::GAL* gal = aView->GetGAL();
    KIGFX::GAL_SCOPED_ATTRS attrs( *gal, KIGFX::GAL_SCOPED_ATTRS::ALL_ATTRS );

    std::string layer = layerName( m_board, m_activeLayer );

    m_audit = PLAN_AUDIT::Run( plan, *m_board );
    m_progress = PLAN_PROGRESS::Run( plan, *m_board, m_audit );

    if( m_planView )
        drawPlanBackdrop( gal, aView );

    drawPlanes( gal, aView, layer );

    drawCorridors( gal, aView, layer, false );
    drawNotesAndAudit( gal, aView, layer );

    if( m_planView || !plan.Empty() )
        drawHeader( gal, aView, layer );

    if( m_planView && plan.layers.size() > 1 )
        drawOverview( gal, aView, layer );
}


void PLAN_VIEW_ITEM::drawOverview( KIGFX::GAL* aGal, KIGFX::VIEW* aView,
                                   const std::string& aActive ) const
{
    // Every plan layer at once: a column of small panels at the right edge, the active one framed,
    // so the person sees that (and how) the other layers are planned.
    const BOARD_PLAN& plan = m_board->GetPlan();
    BOX2I             board = m_board->GetBoardEdgesBoundingBox();

    if( board.GetWidth() <= 0 || board.GetHeight() <= 0 )
        return;

    BOX2D  vp = aView->GetViewport();
    double px = aView->ToWorld( 1.0 );

    // A grid in a 180 px column: as many columns as it takes for every layer to fit the height
    // with each panel as large as possible.
    const double colW = 96 * px;
    const double labelH = 16 * px;
    const double gap = 10 * px;
    double       aspect = (double) board.GetHeight() / board.GetWidth();
    double       maxH = vp.GetHeight() - 24 * px;
    int          n = (int) plan.layers.size();
    int          cols = 1;
    double       panelW = colW;

    for( ; cols <= 4; ++cols )
    {
        panelW = ( colW - ( cols - 1 ) * gap ) / cols;
        int rows = ( n + cols - 1 ) / cols;

        if( rows * ( panelW * aspect + labelH + gap ) <= maxH )
            break;
    }

    cols = std::min( cols, 4 );
    int rows = ( n + cols - 1 ) / cols;

    if( rows * ( panelW * aspect + labelH + gap ) > maxH )
        panelW = ( maxH / rows - labelH - gap ) / aspect;

    double scale = panelW / board.GetWidth();
    double panelH = board.GetHeight() * scale;
    bool   narrow = panelW < 120 * px;

    VECTOR2D origin( vp.GetEnd().x - colW - 14 * px, vp.GetOrigin().y + 12 * px );
    VECTOR2D first = origin;
    int      index = 0;
    m_overviewHits.clear();

    // Its own column: an opaque strip, so it never reads as part of the board.
    aGal->AdvanceDepth();
    aGal->SetIsFill( true );
    aGal->SetIsStroke( false );
    aGal->SetFillColor( COLOR4D( 0.05, 0.06, 0.06, 1.0 ) );
    aGal->DrawRectangle( VECTOR2D( origin.x - 10 * px, vp.GetOrigin().y ), vp.GetEnd() );

    // Top of the stack first.
    std::vector<const PLAN_LAYER*> layers;

    for( const PLAN_LAYER& l : plan.layers )
        layers.push_back( &l );

    auto stackIndex = [&]( const std::string& aName )
    {
        PCB_LAYER_ID id = m_board->GetLayerID( wxString::FromUTF8( aName ) );
        int          i = 0;

        for( PCB_LAYER_ID layer : m_board->GetEnabledLayers().CuStack() )
        {
            if( layer == id )
                return i;

            ++i;
        }

        return i;
    };

    std::stable_sort( layers.begin(), layers.end(), [&]( const PLAN_LAYER* a, const PLAN_LAYER* b )
                      { return stackIndex( a->layer ) < stackIndex( b->layer ); } );

    for( const PLAN_LAYER* lp : layers )
    {
        const PLAN_LAYER& l = *lp;
        origin = first + VECTOR2D( ( index % cols ) * ( panelW + gap ),
                                   ( index / cols ) * ( panelH + labelH + gap ) );
        ++index;

        VECTOR2D at = origin + VECTOR2D( 0, labelH );
        m_overviewHits.emplace_back( l.layer, BOX2D( origin, VECTOR2D( panelW, panelH + labelH ) ) );
        auto     map = [&]( const VECTOR2I& p )
        {
            return at + VECTOR2D( ( p.x - board.GetLeft() ) * scale, ( p.y - board.GetTop() ) * scale );
        };

        bool active = l.layer == aActive;

        aGal->AdvanceDepth();
        aGal->SetIsFill( true );
        aGal->SetIsStroke( true );
        aGal->SetFillColor( BACKDROP );
        aGal->SetStrokeColor( active ? SELECT : PART_EDGE.WithAlpha( 0.6 ) );
        aGal->SetLineWidth( ( active ? 1.6 : 1.0 ) * px );
        aGal->DrawRectangle( at - VECTOR2D( 4 * px, 4 * px ), at + VECTOR2D( panelW + 4 * px, panelH + 4 * px ) );

        wxString chipText = wxString::FromUTF8( PlanLayerChip( *m_board, l.layer ) );

        if( narrow )   // "L3 PWR": the stack position and the role
            chipText = chipText.BeforeFirst( ' ' ) + ( l.role == "power" ? wxS( " PWR" ) : wxS( " SIG" ) );
        else
            chipText += wxString::FromUTF8( l.role == "power" ? " \xC2\xB7 PWR" : " \xC2\xB7 SIG" );
        text( aGal, chipText, origin + VECTOR2D( 0, 6 * px ), ( narrow ? 9 : 10 ) * px, active ? HEADER_TEXT : HEADER_DIM,
              active, GR_TEXT_H_ALIGN_LEFT );

        for( const PLAN_PLANE& p : plan.planes )
        {
            if( p.layer != l.layer || p.outline.empty() || p.outline.front().size() < 3 )
                continue;

            SHAPE_POLY_SET   poly;
            SHAPE_LINE_CHAIN chain;

            for( const VECTOR2I& pt : p.outline.front() )
                chain.Append( VECTOR2I( map( pt ) ) );

            chain.SetClosed( true );
            poly.AddOutline( chain );

            // Inside the panel only.
            SHAPE_POLY_SET   frame;
            SHAPE_LINE_CHAIN fr;
            fr.Append( VECTOR2I( at ) );
            fr.Append( VECTOR2I( at + VECTOR2D( panelW, 0 ) ) );
            fr.Append( VECTOR2I( at + VECTOR2D( panelW, panelH ) ) );
            fr.Append( VECTOR2I( at + VECTOR2D( 0, panelH ) ) );
            fr.SetClosed( true );
            frame.AddOutline( fr );
            poly.BooleanIntersection( frame );

            if( poly.IsEmpty() )
                continue;

            poly.CacheTriangulation();
            aGal->AdvanceDepth();
            aGal->SetIsFill( true );
            aGal->SetIsStroke( false );
            aGal->SetFillColor( PlaneColor( p ).WithAlpha( 0.85 ) );
            aGal->DrawPolygon( poly );
        }

        for( const PLAN_CORRIDOR& c : plan.corridors )
        {
            if( c.layer != l.layer || c.path.size() < 2 )
                continue;

            aGal->AdvanceDepth();
            aGal->SetIsFill( false );
            aGal->SetIsStroke( true );
            aGal->SetStrokeColor( CorridorColor( plan, c ).WithAlpha( 0.95 ) );
            aGal->SetLineWidth( std::max( 3.0 * px, c.width * scale ) );

            for( size_t ii = 1; ii < c.path.size(); ++ii )
                aGal->DrawLine( map( c.path[ii - 1] ), map( c.path[ii] ) );
        }

        SHAPE_POLY_SET outline;

        if( m_board->GetBoardPolygonOutlines( outline, true ) )
        {
            aGal->AdvanceDepth();
            aGal->SetIsFill( false );
            aGal->SetIsStroke( true );
            aGal->SetStrokeColor( OUTLINE.WithAlpha( 0.8 ) );
            aGal->SetLineWidth( 1.0 * px );

            for( int ii = 0; ii < outline.OutlineCount(); ++ii )
            {
                const SHAPE_LINE_CHAIN& o = outline.COutline( ii );

                for( int k = 0; k < o.PointCount(); ++k )
                    aGal->DrawLine( map( o.CPoint( k ) ), map( o.CPoint( ( k + 1 ) % o.PointCount() ) ) );
            }
        }

    }
}


void PLAN_VIEW_ITEM::drawPlanBackdrop( KIGFX::GAL* aGal, KIGFX::VIEW* aView ) const
{
    BOX2D vp = aView->GetViewport();

    aGal->AdvanceDepth();
    aGal->SetIsFill( true );
    aGal->SetIsStroke( false );
    aGal->SetFillColor( BACKDROP );
    aGal->DrawRectangle( vp.GetOrigin(), vp.GetEnd() );

    double px = aView->ToWorld( 1.0 );

    SHAPE_POLY_SET outline;

    if( m_board->GetBoardPolygonOutlines( outline, true ) )
    {
        aGal->AdvanceDepth();
        aGal->SetIsFill( false );
        aGal->SetIsStroke( true );
        aGal->SetStrokeColor( OUTLINE );
        aGal->SetLineWidth( 1.6 * px );

        for( int ii = 0; ii < outline.OutlineCount(); ++ii )
        {
            aGal->DrawPolyline( outline.COutline( ii ) );

            for( int h = 0; h < outline.HoleCount( ii ); ++h )
                aGal->DrawPolyline( outline.CHole( ii, h ) );
        }
    }

    std::vector<std::pair<wxString, BOX2I>> refs;

    for( const FOOTPRINT* fp : m_board->Footprints() )
    {
        const SHAPE_POLY_SET& court = fp->GetCourtyard( fp->IsFlipped() ? B_CrtYd : F_CrtYd );
        BOX2I box = fp->GetBoundingBox( false );

        // The outline is the part; its fill is pin completion: empty when no connected pin is
        // routed, filled bottom-up in proportion, solid when every pin is.
        if( court.OutlineCount() > 0 )
            box = court.BBox();

        double done = 0.0;

        if( auto it = m_progress.parts.find( fp->GetReference().ToStdString( wxConvUTF8 ) );
            it != m_progress.parts.end() && it->second.pins > 0 )
        {
            done = (double) it->second.routed / it->second.pins;
        }

        aGal->AdvanceDepth();
        aGal->SetIsFill( true );
        aGal->SetIsStroke( false );
        aGal->SetFillColor( PART_FILL.WithAlpha( 0.35 ) );
        aGal->DrawRectangle( box.GetOrigin(), box.GetEnd() );

        if( done > 0 )
        {
            double top = box.GetBottom() - box.GetHeight() * done;
            aGal->SetFillColor( done >= 1.0 ? PART_EDGE.WithAlpha( 0.75 )
                                            : PART_EDGE.WithAlpha( 0.45 ) );
            aGal->DrawRectangle( VECTOR2D( box.GetLeft(), top ),
                                 VECTOR2D( box.GetRight(), box.GetBottom() ) );
        }

        aGal->AdvanceDepth();
        aGal->SetIsFill( false );
        aGal->SetIsStroke( true );
        aGal->SetStrokeColor( PART_EDGE );
        aGal->SetLineWidth( 1.0 * px );

        if( court.OutlineCount() > 0 )
        {
            for( int ii = 0; ii < court.OutlineCount(); ++ii )
                aGal->DrawPolyline( court.COutline( ii ) );
        }
        else
        {
            aGal->DrawRectangle( box.GetOrigin(), box.GetEnd() );
        }

        refs.push_back( { fp->GetReference(), box } );
    }

    // References: big parts first; one only where it fits inside its box at a readable size and
    // clears every label already placed. Passives stay plain boxes until zoomed in.
    std::stable_sort( refs.begin(), refs.end(), []( const auto& a, const auto& b )
                      { return (double) a.second.GetWidth() * a.second.GetHeight()
                               > (double) b.second.GetWidth() * b.second.GetHeight(); } );

    std::vector<BOX2D> placed;

    for( const auto& [ref, box] : refs )
    {
        double size = std::clamp( std::min( box.GetWidth(), box.GetHeight() ) * 0.28,
                                  aView->ToWorld( 7.0 ), aView->ToWorld( 15.0 ) );
        double w = size * 0.75 * ref.Length();

        if( w > box.GetWidth() * 1.05 || size > box.GetHeight() * 0.9 )
            continue;

        BOX2D r( VECTOR2D( box.Centre() ) - VECTOR2D( w / 2, size * 0.6 ), VECTOR2D( w, size * 1.2 ) );
        bool  clash = false;

        for( const BOX2D& o : placed )
            clash |= o.Intersects( r );

        if( clash )
            continue;

        placed.push_back( r );
        text( aGal, ref, box.Centre(), size, PART_TEXT );
    }
}


void PLAN_VIEW_ITEM::drawPlanes( KIGFX::GAL* aGal, KIGFX::VIEW* aView,
                                 const std::string& aLayer ) const
{
    const BOARD_PLAN& plan = m_board->GetPlan();
    double            px = aView->ToWorld( 1.0 );

    std::vector<const PLAN_PLANE*> planes;

    for( const PLAN_PLANE& p : plan.planes )
    {
        if( p.layer == aLayer )
            planes.push_back( &p );
    }

    // Higher priority first: each region keeps what no higher-priority region took, so regions
    // never blend; where two claim the same copper the overlap is a conflict, drawn as red hatch.
    std::stable_sort( planes.begin(), planes.end(),
                      []( const PLAN_PLANE* a, const PLAN_PLANE* b )
                      { return a->priority > b->priority; } );

    SHAPE_POLY_SET taken;
    SHAPE_POLY_SET overlaps;
    std::vector<std::pair<const PLAN_PLANE*, SHAPE_POLY_SET>> shown;

    for( const PLAN_PLANE* p : planes )
    {
        if( p->outline.empty() || p->outline.front().size() < 3 )
            continue;

        SHAPE_POLY_SET poly;
        SHAPE_LINE_CHAIN outer;

        for( const VECTOR2I& pt : p->outline.front() )
            outer.Append( pt );

        outer.SetClosed( true );
        poly.AddOutline( outer );

        for( size_t h = 1; h < p->outline.size(); ++h )
        {
            SHAPE_LINE_CHAIN hole;

            for( const VECTOR2I& pt : p->outline[h] )
                hole.Append( pt );

            hole.SetClosed( true );
            poly.AddHole( hole );
        }

        SHAPE_POLY_SET visible = poly;

        if( !taken.IsEmpty() )
        {
            SHAPE_POLY_SET both = poly;
            both.BooleanIntersection( taken );

            if( !both.IsEmpty() )
                overlaps.BooleanAdd( both );

            visible.BooleanSubtract( taken );
        }

        taken.BooleanAdd( poly );
        shown.emplace_back( p, visible );
    }

    // Draw lowest priority first (labels of higher ones on top).
    std::reverse( shown.begin(), shown.end() );

    for( auto& [p, poly] : shown )
    {
        if( poly.IsEmpty() )
            continue;

        poly.CacheTriangulation();

        COLOR4D color = PlaneColor( *p );
        bool    selected = m_selected.count( p->id ) > 0;
        double  fillAlpha = m_planView ? 0.92 : 0.42;

        if( p->status == PLAN_STATUS::REJECTED )
            color = COLOR4D( 0.45, 0.47, 0.47, 1.0 );

        int problems = 0;

        if( auto it = m_progress.items.find( p->id ); it != m_progress.items.end() )
            problems = it->second.ProblemCount();

        if( p->status == PLAN_STATUS::PROPOSED )
            fillAlpha *= 0.7;
        else if( p->status == PLAN_STATUS::ROUTED || p->status == PLAN_STATUS::VERIFIED )
            fillAlpha *= 0.35;   // the zone is real copper now

        aGal->AdvanceDepth();
        aGal->SetIsFill( true );
        aGal->SetIsStroke( false );
        aGal->SetFillColor( color.WithAlpha( fillAlpha ) );
        aGal->DrawPolygon( poly );

        aGal->AdvanceDepth();
        aGal->SetIsFill( false );
        aGal->SetIsStroke( true );
        aGal->SetStrokeColor( selected ? SELECT
                              : problems ? PROBLEM
                              : p->status == PLAN_STATUS::VERIFIED ? VERIFIED
                                                                   : color.Brightened( 0.3 ).WithAlpha( 0.95 ) );
        aGal->SetLineWidth( ( selected ? 2.5 : problems ? 2.0 : 1.0 ) * px );
        outline( aGal, poly, p->status == PLAN_STATUS::PROPOSED && !p->edited && !selected, px );

        if( selected || p->id == m_hovered )
        {
            aGal->AdvanceDepth();
            aGal->SetIsFill( true );
            aGal->SetFillColor( SELECT );

            for( const VECTOR2I& pt : p->outline.front() )
                aGal->DrawRectangle( pt - VECTOR2I( 3 * px, 3 * px ), pt + VECTOR2I( 3 * px, 3 * px ) );
        }

        BOX2I box = poly.BBox();

        if( std::min( box.GetWidth(), box.GetHeight() ) > aView->ToWorld( 40.0 ) )
        {
            double   size = aView->ToWorld( 12.0 );
            wxString label = wxString::FromUTF8( p->net );
            VECTOR2D at = labelSpot( poly, size * 0.7 * label.Length() + size, size * 1.5 );
            pill( aGal, at, size * 0.7 * label.Length() + size, size * 1.5,
                  BACKDROP.WithAlpha( 0.85 ), color.Brightened( 0.35 ), 1.0 * px );
            text( aGal, label, at, size, COLOR4D( 1, 1, 1, 0.95 ), true );

            VECTOR2D mark = at + VECTOR2D( size * 0.35 * label.Length() + size, 0 );

            if( p->status == PLAN_STATUS::VERIFIED )
            {
                iconCheck( aGal, mark, size, VERIFIED );
                iconCheck( aGal, mark + VECTOR2D( size * 0.55, 0 ), size, VERIFIED );
                mark.x += size * 1.6;
            }
            else if( p->status == PLAN_STATUS::APPROVED || p->status == PLAN_STATUS::ROUTED )
            {
                iconCheck( aGal, mark, size, COLOR4D( 1, 1, 1, 0.9 ) );
                mark.x += size;
            }

            if( p->edited )
            {
                iconPencil( aGal, mark, size * 0.9, COLOR4D( 1, 1, 1, 0.9 ) );
                mark.x += size;
            }

            if( p->locked )
            {
                iconLock( aGal, mark, size * 0.9, COLOR4D( 1, 1, 1, 0.9 ) );
                mark.x += size;
            }

            if( problems )
            {
                iconWarn( aGal, mark, size, PROBLEM );
                text( aGal, wxString::Format( wxS( "%d" ), problems ),
                      mark + VECTOR2D( size * 0.6, 0 ), size, PROBLEM, true, GR_TEXT_H_ALIGN_LEFT );
            }
        }
    }

    if( !overlaps.IsEmpty() )
    {
        // Red hatch: 45° stripes 6 px apart, clipped to the overlap.
        BOX2I    ob = overlaps.BBox();
        double   step = aView->ToWorld( 6.0 );
        double   half = aView->ToWorld( 1.2 );
        SHAPE_POLY_SET stripes;

        for( double k = ob.GetLeft() - ob.GetHeight(); k < ob.GetRight(); k += step )
        {
            SHAPE_LINE_CHAIN st;
            st.Append( VECTOR2I( k - half, ob.GetBottom() ) );
            st.Append( VECTOR2I( k + half, ob.GetBottom() ) );
            st.Append( VECTOR2I( k + half + ob.GetHeight(), ob.GetTop() ) );
            st.Append( VECTOR2I( k - half + ob.GetHeight(), ob.GetTop() ) );
            st.SetClosed( true );
            stripes.AddOutline( st );
        }

        stripes.BooleanIntersection( overlaps );
        aGal->AdvanceDepth();
        aGal->SetIsFill( true );
        aGal->SetIsStroke( false );
        aGal->SetFillColor( PROBLEM.WithAlpha( 0.25 ) );
        overlaps.CacheTriangulation();
        aGal->DrawPolygon( overlaps );

        if( !stripes.IsEmpty() )
        {
            stripes.CacheTriangulation();
            aGal->AdvanceDepth();
            aGal->SetFillColor( PROBLEM.WithAlpha( 0.9 ) );
            aGal->DrawPolygon( stripes );
        }

        aGal->AdvanceDepth();
        aGal->SetIsFill( false );
        aGal->SetIsStroke( true );
        aGal->SetStrokeColor( PROBLEM );
        aGal->SetLineWidth( 1.5 * px );
        outline( aGal, overlaps, false, px );
    }

    // Split lines: dashed.
    for( const PLAN_SPLIT& s : plan.splits )
    {
        if( s.layer != aLayer )
            continue;

        aGal->SetIsStroke( true );
        aGal->AdvanceDepth();
        aGal->SetIsFill( false );
        aGal->SetStrokeColor( COLOR4D( 1, 1, 1, 0.9 ) );
        aGal->SetLineWidth( 1.5 * px );

        double dash = 6 * px;

        for( size_t ii = 1; ii < s.path.size(); ++ii )
        {
            VECTOR2D a = s.path[ii - 1];
            VECTOR2D b = s.path[ii];
            double   len = ( b - a ).EuclideanNorm();
            VECTOR2D dir = len > 0 ? ( b - a ) / len : VECTOR2D();

            for( double t = 0; t < len; t += 2 * dash )
                aGal->DrawLine( a + dir * t, a + dir * std::min( len, t + dash ) );
        }
    }
}


void PLAN_VIEW_ITEM::drawCorridors( KIGFX::GAL* aGal, KIGFX::VIEW* aView,
                                    const std::string& aLayer, bool aFaint ) const
{
    const BOARD_PLAN& plan = m_board->GetPlan();
    double            px = aView->ToWorld( 1.0 );
    int               maxError = segsFor( aView );

    // Pads of each (reference, net), for the fan-out lines to the parts.
    std::map<std::string, std::vector<VECTOR2I>> pads;

    if( !aFaint )
    {
        for( const FOOTPRINT* fp : m_board->Footprints() )
        {
            for( const PAD* pad : fp->Pads() )
            {
                if( pad->GetNetCode() > 0 )
                {
                    pads[fp->GetReference().ToStdString( wxConvUTF8 ) + "\n"
                         + pad->GetNetname().ToStdString( wxConvUTF8 )]
                            .push_back( pad->GetPosition() );
                }
            }
        }
    }

    std::vector<const PLAN_CORRIDOR*> corridors;

    for( const PLAN_CORRIDOR& c : plan.corridors )
    {
        if( ( c.layer == aLayer ) != aFaint )
            corridors.push_back( &c );
    }

    // Later in routing order draws on top.
    std::stable_sort( corridors.begin(), corridors.end(),
                      []( const PLAN_CORRIDOR* a, const PLAN_CORRIDOR* b )
                      { return a->order < b->order; } );

    for( const PLAN_CORRIDOR* c : corridors )
    {
        COLOR4D color = CorridorColor( plan, *c );
        bool    selected = m_selected.count( c->id ) > 0;
        bool    hovered = c->id == m_hovered;

        if( c->status == PLAN_STATUS::REJECTED )
            color = COLOR4D( 0.45, 0.47, 0.47, 1.0 );

        double fill = aFaint ? 0.10 : ( c->status == PLAN_STATUS::ROUTED ? 0.22 : 0.42 );
        double edge = aFaint ? 0.18 : 0.85;

        const PLAN_GROUP* group = plan.FindGroup( c->group );

        // Fan-out: thin lines from the corridor's ends to each net's pads on its parts.
        if( !aFaint && group && c->path.size() >= 2 )
        {
            aGal->SetIsStroke( true );
            aGal->AdvanceDepth();
            aGal->SetIsFill( false );
            aGal->SetStrokeColor( color.WithAlpha( 0.55 ) );
            aGal->SetLineWidth( 0.8 * px );

            for( const std::string& net : group->nets )
            {
                for( const auto& [refs, end] : { std::make_pair( &c->from, c->path.front() ),
                                                 std::make_pair( &c->to, c->path.back() ) } )
                {
                    for( const std::string& ref : *refs )
                    {
                        auto it = pads.find( ref + "\n" + net );

                        if( it == pads.end() )
                            continue;

                        for( const VECTOR2I& pad : it->second )
                            aGal->DrawLine( end, pad );
                    }
                }
            }
        }

        // At least 12 px on screen, so a bundle reads as a band and can be grabbed; its true
        // width (n x (track + gap)) is what routing uses and what the hover shows.
        int            shown = std::max( c->width, (int) aView->ToWorld( 12.0 ) );
        SHAPE_POLY_SET poly = band( c->path, shown, maxError );

        if( poly.OutlineCount() == 0 )
            continue;

        poly.CacheTriangulation();

        const PLAN_PROGRESS::ITEM* prog = nullptr;

        if( auto it = m_progress.items.find( c->id ); it != m_progress.items.end() )
            prog = &it->second;

        int  problems = prog ? prog->ProblemCount() : 0;
        bool routed = c->status == PLAN_STATUS::ROUTED;
        bool verified = c->status == PLAN_STATUS::VERIFIED;
        bool routing = c->status == PLAN_STATUS::ROUTING;
        bool approved = c->status == PLAN_STATUS::APPROVED;
        bool proposed = c->status == PLAN_STATUS::PROPOSED;

        // Fill: proposed is translucent, approved is solid, routed and verified fade to an
        // outline so the copper shows through.
        double fillA = aFaint ? 0.10
                       : ( routed || verified ) ? 0.0
                       : approved || routing ? 0.62
                                             : 0.36;

        if( hovered && !aFaint )
            fillA += 0.10;

        aGal->AdvanceDepth();

        if( fillA > 0 )
        {
            aGal->AdvanceDepth();
            aGal->SetIsFill( true );
            aGal->SetIsStroke( false );
            aGal->SetFillColor( color.WithAlpha( fillA ) );
            aGal->DrawPolygon( poly );
        }

        // Routing: the band fills from the source toward the destination as connections land.
        if( routing && !aFaint && c->total > 0 )
        {
            SHAPE_POLY_SET done = band( prefix( c->path, (double) c->routed / c->total ),
                                        shown, maxError );

            if( done.OutlineCount() )
            {
                done.CacheTriangulation();
                aGal->AdvanceDepth();
                aGal->SetIsFill( true );
                aGal->SetFillColor( color.Brightened( 0.15 ).WithAlpha( 0.92 ) );
                aGal->DrawPolygon( done );
            }
        }

        COLOR4D edgeColor = selected ? SELECT
                            : problems > 0 && !aFaint ? PROBLEM
                            : verified ? VERIFIED
                                       : color.WithAlpha( aFaint ? 0.18 : 0.9 );
        double  edgeW = ( selected ? 2.5 : problems > 0 || verified ? 2.0
                          : routed ? 1.0 : c->locked ? 2.0 : 1.3 ) * px;

        aGal->AdvanceDepth();
        aGal->SetIsFill( false );
        aGal->SetIsStroke( true );
        aGal->SetStrokeColor( edgeColor );
        aGal->SetLineWidth( edgeW );
        outline( aGal, poly, proposed && !c->edited && !selected && problems == 0, px );

        if( aFaint )
            continue;

        // The centreline, so a wide band still reads as a path (not once copper is there).
        if( !routed && !verified )
        {
            aGal->AdvanceDepth();
            aGal->SetIsFill( false );
            aGal->SetIsStroke( true );
            aGal->SetStrokeColor( color.WithAlpha( 0.9 ) );
            aGal->SetLineWidth( 1.2 * px );

            for( size_t ii = 1; ii < c->path.size(); ++ii )
                aGal->DrawLine( c->path[ii - 1], c->path[ii] );
        }

        // Vias and escapes belong to the corridor: a leader to its nearest end.
        auto nearestEnd = [&]( const VECTOR2I& aAt ) -> VECTOR2I
        {
            return ( aAt - c->path.front() ).EuclideanNorm() <= ( aAt - c->path.back() ).EuclideanNorm()
                           ? c->path.front()
                           : c->path.back();
        };

        aGal->AdvanceDepth();
        aGal->SetIsFill( false );
        aGal->SetIsStroke( true );
        aGal->SetStrokeColor( color.WithAlpha( 0.7 ) );
        aGal->SetLineWidth( 1.0 * px );

        for( const VECTOR2I& v : c->vias )
            aGal->DrawLine( v, nearestEnd( v ) );

        for( const PLAN_ESCAPE& e : c->escapes )
            aGal->DrawLine( e.at, nearestEnd( e.at ) );

        // Vias: small diamonds.
        double d = 4.0 * px;
        aGal->SetStrokeColor( COLOR4D( 0.95, 0.96, 0.96, 0.95 ) );
        aGal->SetLineWidth( 1.2 * px );

        for( const VECTOR2I& v : c->vias )
        {
            std::deque<VECTOR2D> diamond = { VECTOR2D( v.x, v.y - d ), VECTOR2D( v.x + d, v.y ),
                                             VECTOR2D( v.x, v.y + d ), VECTOR2D( v.x - d, v.y ),
                                             VECTOR2D( v.x, v.y - d ) };
            aGal->AdvanceDepth();
            aGal->SetIsFill( true );
            aGal->SetFillColor( BACKDROP.WithAlpha( 0.9 ) );
            aGal->DrawPolygon( diamond );
            aGal->AdvanceDepth();
            aGal->SetIsFill( false );
            aGal->DrawPolyline( diamond );
        }

        for( const PLAN_ESCAPE& e : c->escapes )
        {
            aGal->SetStrokeColor( color.WithAlpha( 0.95 ) );
            aGal->SetLineWidth( 2.0 * px );
            aGal->DrawCircle( e.at, 3.0 * px );
        }

        if( selected )
        {
            aGal->AdvanceDepth();
            aGal->SetIsFill( true );
            aGal->SetIsStroke( true );
            aGal->SetFillColor( color );
            aGal->SetStrokeColor( SELECT );
            aGal->SetLineWidth( 1.0 * px );

            for( const VECTOR2I& pt : c->path )
                aGal->DrawRectangle( pt - VECTOR2I( 3.5 * px, 3.5 * px ),
                                     pt + VECTOR2I( 3.5 * px, 3.5 * px ) );
        }

        // The label pill: the net count (or routed/total while routing), then the marks:
        // ✓ approved, ✓✓ verified, pencil edited, lock locked, ⚠ n problems.
        VECTOR2I mid = midpoint( c->path );
        int      nets = group ? (int) group->nets.size() : 0;
        double   size = aView->ToWorld( 12.0 );
        double   icon = size * 1.05;

        wxString label = routing && c->total > 0
                                 ? wxString::Format( wxS( "%d/%d" ), c->routed, c->total )
                                 : nets > 0 ? wxString::Format( nets == 1 ? _( "%d net" ) : _( "%d nets" ), nets )
                                            : wxString();

        if( c->order > 0 && !routed && !verified )
            label << wxString::FromUTF8( " \xC2\xB7 " ) << Ordinal( c->order );

        int marks = ( approved || routing || routed ? 1 : 0 ) + ( verified ? 2 : 0 )
                    + ( c->edited ? 1 : 0 ) + ( c->locked ? 1 : 0 );
        double textW = size * 0.8 * label.Length();   // the bitmap font's advance
        double marksW = marks * icon;
        double warnW = problems > 0 ? icon + size * 0.68 * wxString::Format( wxS( "%d" ), problems ).Length() : 0;
        double gap = size * 0.3;
        double w = textW + marksW + warnW + gap * ( ( marks ? 1 : 0 ) + ( warnW > 0 ? 1 : 0 ) )
                   + size * 0.9;

        if( !label.IsEmpty() || marks || problems )
        {
            COLOR4D rim = problems > 0 ? PROBLEM : verified ? VERIFIED : color.WithAlpha( 0.9 );
            pill( aGal, mid, w, size * 1.45, BACKDROP.WithAlpha( 0.88 ), rim, 1.2 * px );

            double x = mid.x - w / 2 + size * 0.45;

            if( !label.IsEmpty() )
            {
                text( aGal, label, VECTOR2D( x, mid.y ), size, COLOR4D( 1, 1, 1, 1 ), true,
                      GR_TEXT_H_ALIGN_LEFT );
                x += textW + gap;
            }

            VECTOR2D at( x + icon / 2, mid.y );

            if( verified )
            {
                iconCheck( aGal, at, icon, VERIFIED );
                at.x += icon * 0.55;
                iconCheck( aGal, at, icon, VERIFIED );
                at.x += icon;
            }
            else if( approved || routing || routed )
            {
                iconCheck( aGal, at, icon, COLOR4D( 1, 1, 1, 0.95 ) );
                at.x += icon;
            }

            if( c->edited )
            {
                iconPencil( aGal, at, icon * 0.9, COLOR4D( 1, 1, 1, 0.9 ) );
                at.x += icon;
            }

            if( c->locked )
            {
                iconLock( aGal, at, icon * 0.9, COLOR4D( 1, 1, 1, 0.9 ) );
                at.x += icon;
            }

            if( problems > 0 )
            {
                at.x += gap;
                iconWarn( aGal, at, icon, PROBLEM );
                text( aGal, wxString::Format( wxS( "%d" ), problems ),
                      VECTOR2D( at.x + icon * 0.6, mid.y ), size, PROBLEM, true,
                      GR_TEXT_H_ALIGN_LEFT );
            }

            // A glyph says what it means: the first problem in words beside the pill.
            if( problems > 0 && prog )
            {
                wxString why = ProblemText( *prog );

                if( why.Length() > 48 )
                    why = why.Left( 47 ) + wxString::FromUTF8( "\xE2\x80\xA6" );
                VECTOR2D whyAt( mid.x + w / 2 + gap, mid.y );
                double   whyW = size * 0.62 * why.Length() + size * 0.8;
                pill( aGal, whyAt + VECTOR2D( whyW / 2, 0 ), whyW, size * 1.3,
                      BACKDROP.WithAlpha( 0.92 ), PROBLEM.WithAlpha( 0.6 ), 1.0 * px );
                text( aGal, why, whyAt + VECTOR2D( size * 0.4, 0 ), size * 0.85, PROBLEM, false,
                      GR_TEXT_H_ALIGN_LEFT );
            }
        }

        if( false )   // the routing order is in the pill now
        {
            VECTOR2D start = c->path.front();
            VECTOR2D dir = VECTOR2D( c->path[1] - c->path[0] );
            double   len = dir.EuclideanNorm();

            if( len > 0 )
                start += dir / len * std::min( len / 2, aView->ToWorld( 16.0 ) );

            double small = aView->ToWorld( 9.0 );
            aGal->AdvanceDepth();
            aGal->SetIsFill( true );
            aGal->SetIsStroke( true );
            aGal->SetFillColor( color.WithAlpha( 0.95 ) );
            aGal->SetStrokeColor( BACKDROP );
            aGal->SetLineWidth( 1.0 * px );
            aGal->DrawCircle( start, small * 0.85 );
            text( aGal, wxString::Format( wxS( "%d" ), c->order ), start, small,
                  BACKDROP.WithAlpha( 1.0 ), true );

            // ...and in words: the routing order.
            text( aGal, Ordinal( c->order ) + _( " to route" ),
                  start + VECTOR2D( small * 1.1, 0 ), small * 0.85, color.Brightened( 0.3 ), false,
                  GR_TEXT_H_ALIGN_LEFT );
        }

        if( selected || hovered )
        {
            wxString name = group ? wxString::FromUTF8( group->name ) : wxString::FromUTF8( c->id );
            wxString state = wxString::FromUTF8( prog ? prog->state : PlanStatusName( c->status ) );

            text( aGal, name + wxString::FromUTF8( "  \xC2\xB7  " ) + state, VECTOR2D( mid ) + VECTOR2D( 0, -size * 1.6 ),
                  size * 0.9, COLOR4D( 1, 1, 1, 0.95 ), false );
        }
    }

    // A corridor being drawn.
    if( !aFaint && m_draft.size() >= 1 && m_draftLayer == aLayer )
    {
        std::vector<VECTOR2I> path = m_draft;

        if( path.size() == 1 )
            path.push_back( path.front() + VECTOR2I( 1, 0 ) );

        SHAPE_POLY_SET poly = band( path, m_draftWidth, maxError );

        if( poly.OutlineCount() )
        {
            poly.CacheTriangulation();
            aGal->AdvanceDepth();
            aGal->SetIsFill( true );
            aGal->SetIsStroke( true );
            aGal->SetFillColor( COLOR4D( 1, 1, 1, 0.18 ) );
            aGal->SetStrokeColor( COLOR4D( 1, 1, 1, 0.9 ) );
            aGal->SetLineWidth( 1.5 * px );
            aGal->DrawPolygon( poly );
        }
    }
}


void PLAN_VIEW_ITEM::drawNotesAndAudit( KIGFX::GAL* aGal, KIGFX::VIEW* aView,
                                        const std::string& aLayer ) const
{
    const BOARD_PLAN& plan = m_board->GetPlan();
    double            px = aView->ToWorld( 1.0 );
    double            size = aView->ToWorld( 11.0 );

    for( const PLAN_NOTE& n : plan.notes )
    {
        if( n.layer != "*" && n.layer != aLayer )
            continue;

        wxString label = wxString::FromUTF8( n.text );
        double   w = size * 0.62 * label.Length() + size;
        VECTOR2D at( n.at );

        aGal->AdvanceDepth();
        aGal->SetIsFill( true );
        aGal->SetIsStroke( true );
        aGal->SetFillColor( NOTE_FILL );
        aGal->SetStrokeColor( m_selected.count( n.id ) ? SELECT : NOTE_FILL.Darkened( 0.3 ) );
        aGal->SetLineWidth( 1.0 * px );
        aGal->DrawRectangle( at, at + VECTOR2D( w, size * 1.7 ) );
        text( aGal, label, at + VECTOR2D( size * 0.5, size * 0.85 ), size,
              COLOR4D( 0.1, 0.1, 0.1, 1.0 ), false, GR_TEXT_H_ALIGN_LEFT );
    }

    for( const PLAN_AUDIT::WARNING& w : m_audit.warnings )
    {
        if( w.layer != aLayer || w.at == VECTOR2I() )
            continue;

        aGal->AdvanceDepth();
        aGal->SetIsFill( true );
        aGal->SetIsStroke( true );
        aGal->SetFillColor( WARN.WithAlpha( 0.9 ) );
        aGal->SetStrokeColor( COLOR4D( 1, 1, 1, 0.95 ) );
        aGal->SetLineWidth( 1.2 * px );
        aGal->DrawCircle( w.at, 7 * px );
        text( aGal, wxS( "!" ), w.at, size, COLOR4D( 1, 1, 1, 1 ), true );
    }
}


void PLAN_VIEW_ITEM::drawHeader( KIGFX::GAL* aGal, KIGFX::VIEW* aView,
                                 const std::string& aLayer ) const
{
    const BOARD_PLAN& plan = m_board->GetPlan();
    BOX2D             vp = aView->GetViewport();
    double            px = aView->ToWorld( 1.0 );

    // Screen-space: 16 px in from the top left, whatever the zoom.
    VECTOR2D at = vp.GetOrigin() + VECTOR2D( 16 * px, 22 * px );

    bool     power = false;

    for( const PLAN_LAYER& l : plan.layers )
        power |= l.layer == aLayer && l.role == "power";

    wxString chip = wxString::FromUTF8( PlanLayerChip( *m_board, aLayer ) )
                    + wxString::FromUTF8( power ? " \xC2\xB7 PWR" : " \xC2\xB7 SIG" );
    wxString stack = wxString::FromUTF8( DescribePlanLayer( *m_board, aLayer, &plan ) );

    double big = 14 * px;
    double small = 11 * px;
    double chipW = big * 0.78 * chip.Length() + 14 * px;

    aGal->AdvanceDepth();
    aGal->AdvanceDepth();
    aGal->SetIsFill( true );
    aGal->SetIsStroke( false );

    if( m_planView )
    {
        // A solid band: the header never sits on the board.
        aGal->SetFillColor( COLOR4D( 0.05, 0.06, 0.06, 1.0 ) );
        aGal->DrawRectangle( vp.GetOrigin(), VECTOR2D( vp.GetEnd().x, vp.GetOrigin().y + 92 * px ) );
    }

    aGal->SetFillColor( COLOR4D( 0.0, 0.0, 0.0, m_planView ? 0.0 : 0.55 ) );
    double boxW = std::max( chipW, small * 0.62 * stack.Length() + 24 * px );
    aGal->DrawRectangle( at - VECTOR2D( 8 * px, 14 * px ),
                         at + VECTOR2D( boxW + 80 * px, ( stack.IsEmpty() ? 28 : 50 ) * px ) );

    text( aGal, chip, at, big, HEADER_TEXT, true, GR_TEXT_H_ALIGN_LEFT );

    if( !stack.IsEmpty() )
        text( aGal, stack, at + VECTOR2D( 0, 22 * px ), small, HEADER_DIM, false,
              GR_TEXT_H_ALIGN_LEFT );

    // Status at the right of the chip.
    PLAN_STATUS status = plan.LayerStatus( aLayer );
    int         items = 0;

    for( const PLAN_CORRIDOR& c : plan.corridors )
        items += c.layer == aLayer;

    for( const PLAN_PLANE& p : plan.planes )
        items += p.layer == aLayer;

    // Progress: groups on this layer routed or verified.
    if( auto it = m_progress.layers.find( aLayer ); it != m_progress.layers.end()
                                                    && it->second.groups > 0 )
    {
        const PLAN_PROGRESS::LAYER& lp = it->second;
        double   barW = 90 * px;
        double   barH = 5 * px;
        VECTOR2D bar = at + VECTOR2D( 0, ( stack.IsEmpty() ? 16 : 38 ) * px );
        aGal->SetIsStroke( false );
        aGal->AdvanceDepth();
        aGal->SetIsFill( true );
        aGal->SetFillColor( COLOR4D( 1, 1, 1, 0.15 ) );
        aGal->DrawRectangle( bar, bar + VECTOR2D( barW, barH ) );
        aGal->SetFillColor( lp.done == lp.groups ? VERIFIED : COLOR4D( 0.95, 0.75, 0.30, 1.0 ) );
        aGal->DrawRectangle( bar, bar + VECTOR2D( barW * lp.done / lp.groups, barH ) );
        text( aGal, wxString::Format( lp.groups == 1 ? _( "%d of %d corridor routed" )
                                                     : _( "%d of %d corridors routed" ), lp.done, lp.groups ),
              bar + VECTOR2D( barW + 8 * px, barH / 2 ), small, HEADER_DIM, false,
              GR_TEXT_H_ALIGN_LEFT );
    }

    // Every problem on this layer, in words.
    std::vector<wxString> problemLines;

    for( const PLAN_AUDIT::WARNING& w : m_audit.warnings )
    {
        if( w.layer == aLayer || w.layer.empty() )
            problemLines.push_back( wxString::FromUTF8( w.text ) );
    }

    if( !problemLines.empty() )
    {
        VECTOR2D line = at + VECTOR2D( 0, ( stack.IsEmpty() ? 34 : 58 ) * px );
        text( aGal, wxString::Format( problemLines.size() == 1 ? _( "%zu problem:" ) : _( "%zu problems:" ),
                                      problemLines.size() ),
              line, small, PROBLEM, true, GR_TEXT_H_ALIGN_LEFT );

        for( size_t i = 0; i < std::min<size_t>( problemLines.size(), 4 ); ++i )
        {
            wxString t = problemLines[i];

            if( t.Length() > 64 )
                t = t.Left( 63 ) + wxString::FromUTF8( "\xE2\x80\xA6" );

            text( aGal, t, line + VECTOR2D( 0, ( i + 1 ) * 16 * px ), small, PROBLEM, false,
                  GR_TEXT_H_ALIGN_LEFT );
        }
    }

    // How far the person's audit is on this layer, counted: "1 of 3 approved".
    int approvedItems = 0;

    for( const PLAN_CORRIDOR& c : plan.corridors )
        approvedItems += c.layer == aLayer && c.status != PLAN_STATUS::PROPOSED && c.status != PLAN_STATUS::REJECTED;

    for( const PLAN_PLANE& p : plan.planes )
        approvedItems += p.layer == aLayer && p.status != PLAN_STATUS::PROPOSED && p.status != PLAN_STATUS::REJECTED;

    if( items > 0 && status == PLAN_STATUS::PROPOSED && approvedItems > 0 )
    {
        status = approvedItems == items ? PLAN_STATUS::APPROVED : PLAN_STATUS::PROPOSED;
    }

    if( items > 0 )
    {
        wxString tag = wxString::FromUTF8( PlanStatusName( status ) );

        if( approvedItems > 0 && approvedItems < items )
            tag = wxString::Format( _( "%d of %d approved" ), approvedItems, items );
        COLOR4D  tagColor = status == PLAN_STATUS::APPROVED ? COLOR4D( 0.35, 0.80, 0.45, 1.0 )
                            : status == PLAN_STATUS::REJECTED ? WARN
                                                              : COLOR4D( 0.95, 0.75, 0.30, 1.0 );
        text( aGal, tag, at + VECTOR2D( chipW + 4 * px, 0 ), small, tagColor, true,
              GR_TEXT_H_ALIGN_LEFT );
    }
}
