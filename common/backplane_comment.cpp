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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <backplane_comment.h>

#include <algorithm>
#include <cmath>

#include <class_draw_panel_gal.h>
#include <dialog_shim.h>
#include <eda_draw_frame.h>
#include <gal/graphics_abstraction_layer.h>
#include <view/view.h>

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>


EMBED_BOX_MM BackplaneBoxMm( const BOX2I& aBox, double aIuPerMm )
{
    EMBED_BOX_MM box;
    box.xMin = aBox.GetLeft() / aIuPerMm;
    box.yMin = aBox.GetTop() / aIuPerMm;
    box.xMax = aBox.GetRight() / aIuPerMm;
    box.yMax = aBox.GetBottom() / aIuPerMm;
    return box;
}


void BackplaneMergeBox( BOX2I& aBox, bool& aHasBox, const BOX2I& aOther )
{
    BOX2I other = aOther;
    other.Normalize();

    if( !aHasBox )
    {
        aBox = other;
        aHasBox = true;
    }
    else
    {
        aBox.Merge( other );
    }
}


wxString BackplaneCommentSummary( const EMBED_COMMENT& aComment )
{
    if( aComment.emptySelection )
        return _( "Nothing is selected: the comment is about the area in view." );

    std::vector<wxString> names;

    for( const std::string& ref : aComment.names.references )
        names.push_back( wxString::FromUTF8( ref ) );

    for( const std::string& pin : aComment.names.pins )
        names.push_back( wxString::FromUTF8( pin ) );

    for( const std::string& net : aComment.names.nets )
        names.push_back( wxString::FromUTF8( net ) );

    wxString text;
    size_t   shown = std::min<size_t>( names.size(), 4 );

    for( size_t i = 0; i < shown; ++i )
        text << ( i ? wxS( ", " ) : wxS( "" ) ) << names[i];

    if( names.size() > shown )
        text << wxString::Format( _( " and %d more" ), (int) ( names.size() - shown ) );

    int count = (int) aComment.items.size();

    if( text.IsEmpty() )
        return wxString::Format( count == 1 ? _( "%d item selected" ) : _( "%d items selected" ),
                                 count );

    return wxString::Format( count == 1 ? _( "%s (%d item)" ) : _( "%s (%d items)" ), text,
                             count );
}


namespace
{
class DIALOG_BACKPLANE_COMMENT : public DIALOG_SHIM
{
public:
    DIALOG_BACKPLANE_COMMENT( wxWindow* aParent, const wxString& aSummary ) :
            DIALOG_SHIM( aParent, wxID_ANY, _( "Comment to Backplane" ), wxDefaultPosition,
                         wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER,
                         wxS( "BackplaneComment" ) )
    {
        wxBoxSizer* main = new wxBoxSizer( wxVERTICAL );

        wxStaticText* summary = new wxStaticText( this, wxID_ANY, aSummary );
        summary->Wrap( FromDIP( 440 ) );
        main->Add( summary, 0, wxALL | wxEXPAND, 10 );

        m_note = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 FromDIP( wxSize( 440, 110 ) ), wxTE_MULTILINE );
        m_note->SetHint( _( "Your note for Backplane" ) );
        m_note->SetName( wxS( "BackplaneCommentNote" ) );
        main->Add( m_note, 1, wxLEFT | wxRIGHT | wxEXPAND, 10 );

        wxStdDialogButtonSizer* buttons = new wxStdDialogButtonSizer();
        buttons->AddButton( new wxButton( this, wxID_OK ) );
        buttons->AddButton( new wxButton( this, wxID_CANCEL ) );
        buttons->Realize();
        main->Add( buttons, 0, wxALL | wxEXPAND, 10 );

        SetSizer( main );
        SetupStandardButtons( { { wxID_OK, _( "Send" ) } } );
        SetInitialFocus( m_note );
        finishDialogSettings();
    }

    wxString Note() const { return m_note->GetValue(); }

private:
    wxTextCtrl* m_note;
};
} // namespace


bool BackplaneCommentDialog( wxWindow* aParent, const wxString& aSummary, wxString& aNote )
{
    DIALOG_BACKPLANE_COMMENT dlg( aParent, aSummary );

    if( dlg.ShowModal() != wxID_OK )
        return false;

    aNote = dlg.Note().Strip( wxString::both );
    return !aNote.IsEmpty();
}


namespace
{
/// The screen rectangle (GAL pixels) showing aWorld.
wxRect screenRect( KIGFX::VIEW* aView, const BOX2D& aWorld )
{
    VECTOR2D a = aView->ToScreen( aWorld.GetOrigin() );
    VECTOR2D b = aView->ToScreen( aWorld.GetEnd() );

    int x0 = (int) std::floor( std::min( a.x, b.x ) );
    int y0 = (int) std::floor( std::min( a.y, b.y ) );
    int x1 = (int) std::ceil( std::max( a.x, b.x ) );
    int y1 = (int) std::ceil( std::max( a.y, b.y ) );
    return wxRect( x0, y0, x1 - x0, y1 - y0 );
}


bool grab( EDA_DRAW_PANEL_GAL* aCanvas, wxImage& aImage )
{
    if( aCanvas->GetScreenshot( aImage ) && aImage.IsOk() )
        return true;

    // As EDA_DRAW_FRAME::SaveCanvasImageToFile: a backend without readback
    wxSize     size = aCanvas->GetClientSize();
    wxClientDC dc( aCanvas );
    wxBitmap   bitmap( size.x, size.y );
    wxMemoryDC memdc;

    memdc.SelectObject( bitmap );
    memdc.Blit( 0, 0, size.x, size.y, &dc, 0, 0 );
    memdc.SelectObject( wxNullBitmap );

    aImage = bitmap.ConvertToImage();
    return aImage.IsOk();
}
} // namespace


void BackplaneCommentScreenshot( EDA_DRAW_FRAME* aFrame, const BOX2I& aBox, double aIuPerMm,
                                 EMBED_COMMENT& aComment )
{
    EDA_DRAW_PANEL_GAL* canvas = aFrame ? aFrame->GetCanvas() : nullptr;

    if( !canvas || !canvas->GetView() || !canvas->GetGAL() || !canvas->IsShownOnScreen() )
        return;

    wxString path = BACKPLANE_EMBED::Get().NewCommentScreenshotPath();

    if( path.IsEmpty() )
        return;

    KIGFX::VIEW* view = canvas->GetView();

    // The region: the selection padded by a tenth of its size and at least 2 mm
    BOX2D region( VECTOR2D( aBox.GetOrigin() ), VECTOR2D( aBox.GetSize() ) );
    region.Normalize();

    if( !aComment.emptySelection )
    {
        double pad = std::max( std::max( region.GetWidth(), region.GetHeight() ) * 0.1,
                               2.0 * aIuPerMm );
        region.Inflate( pad, pad );
    }

    VECTOR2I screen = canvas->GetGAL()->GetScreenPixelSize();
    wxRect   rect = screenRect( view, region );
    wxRect   whole( 0, 0, screen.x, screen.y );

    // Zoom for the shot only if the region is partly out of view, or too small in it to read
    bool     zoom = !whole.Contains( rect ) || std::max( rect.width, rect.height ) < 240;
    VECTOR2D savedCenter = view->GetCenter();
    double   savedScale = view->GetScale();

    if( zoom )
    {
        view->SetViewport( region );
        rect = screenRect( view, region ).Intersect( whole );
    }

    wxImage image;
    bool    ok = grab( canvas, image ) && rect.width > 0 && rect.height > 0;
    wxRect  crop;
    BOX2D   shown;

    if( ok )
    {
        // The image may be in device pixels while GAL works in its own
        double sx = screen.x > 0 ? (double) image.GetWidth() / screen.x : 1.0;
        double sy = screen.y > 0 ? (double) image.GetHeight() / screen.y : 1.0;

        crop = wxRect( (int) std::floor( rect.x * sx ), (int) std::floor( rect.y * sy ),
                       (int) std::ceil( rect.width * sx ), (int) std::ceil( rect.height * sy ) );
        crop = crop.Intersect( wxRect( 0, 0, image.GetWidth(), image.GetHeight() ) );
        ok = crop.width > 0 && crop.height > 0;

        // What the crop shows, in world coordinates, while the view is still the shot's
        VECTOR2D a = view->ToWorld( VECTOR2D( crop.x / sx, crop.y / sy ) );
        VECTOR2D b = view->ToWorld( VECTOR2D( ( crop.x + crop.width ) / sx,
                                              ( crop.y + crop.height ) / sy ) );
        shown = BOX2D( a, b - a );
        shown.Normalize();
    }

    if( zoom )
    {
        view->SetScale( savedScale );
        view->SetCenter( savedCenter );
        canvas->ForceRefresh();
    }

    if( !ok )
        return;

    image = image.GetSubImage( crop );

    if( image.HasAlpha() )
        image.ClearAlpha();

    int longSide = std::max( image.GetWidth(), image.GetHeight() );

    if( longSide > BACKPLANE_COMMENT_MAX_SIDE )
    {
        double f = (double) BACKPLANE_COMMENT_MAX_SIDE / longSide;
        image.Rescale( std::max( 1, (int) std::lround( image.GetWidth() * f ) ),
                       std::max( 1, (int) std::lround( image.GetHeight() * f ) ),
                       wxIMAGE_QUALITY_HIGH );
    }

    if( !image.SaveFile( path, wxBITMAP_TYPE_PNG ) )
        return;

    aComment.screenshot = path.ToStdString( wxConvUTF8 );
    aComment.screenshotWidth = image.GetWidth();
    aComment.screenshotHeight = image.GetHeight();
    aComment.screenshotRegion.xMin = shown.GetLeft() / aIuPerMm;
    aComment.screenshotRegion.yMin = shown.GetTop() / aIuPerMm;
    aComment.screenshotRegion.xMax = shown.GetRight() / aIuPerMm;
    aComment.screenshotRegion.yMax = shown.GetBottom() / aIuPerMm;
}
