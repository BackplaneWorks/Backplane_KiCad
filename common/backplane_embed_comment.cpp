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

// Backplane fork: ET_COMMENT (bp.10) and the proto encoding of embed events.

#include <backplane_embed.h>
#include <backplane_embed_proto.h>

#include <algorithm>

#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/time.h>
#include <wx/utils.h>


void FillEmbedFrame( kiapi::backplane::embed::FrameInfo* aOut, const EMBED_FRAME_INFO& aIn )
{
    aOut->set_kind( static_cast<kiapi::backplane::embed::FrameKind>( aIn.kind ) );
    aOut->set_document( aIn.document.ToStdString( wxConvUTF8 ) );
    aOut->set_title( aIn.title.ToStdString( wxConvUTF8 ) );
    aOut->set_modified( aIn.modified );
    aOut->set_window( aIn.window );
    aOut->set_min_width( aIn.minWidth );
    aOut->set_min_height( aIn.minHeight );
    aOut->set_shown( aIn.shown );
    aOut->set_focused( aIn.focused );
    aOut->set_frame_name( aIn.frameName.ToStdString( wxConvUTF8 ) );
}


void FillEmbedItemNames( kiapi::backplane::embed::ItemNames* aOut, const EMBED_ITEM_NAMES& aIn )
{
    for( const std::string& name : aIn.references )
        aOut->add_references( name );

    for( const std::string& name : aIn.nets )
        aOut->add_nets( name );

    for( const std::string& name : aIn.pins )
        aOut->add_pins( name );
}


static void fillBox( kiapi::backplane::embed::BoxMm* aOut, const EMBED_BOX_MM& aIn )
{
    aOut->set_x_min( aIn.xMin );
    aOut->set_y_min( aIn.yMin );
    aOut->set_x_max( aIn.xMax );
    aOut->set_y_max( aIn.yMax );
}


void FillEmbedComment( kiapi::backplane::embed::Comment* aOut, const EMBED_COMMENT& aIn )
{
    aOut->set_note( aIn.note );
    FillEmbedItemNames( aOut->mutable_names(), aIn.names );

    for( const EMBED_COMMENT_ITEM& in : aIn.items )
    {
        kiapi::backplane::embed::CommentItem* item = aOut->add_items();
        item->set_kind( in.kind );
        item->set_id( in.id );
        item->set_reference( in.reference );
        item->set_pin( in.pin );
        item->set_net( in.net );
        item->set_layer( in.layer );
        item->set_sheet_path( in.sheetPath );
        item->set_x_mm( in.x );
        item->set_y_mm( in.y );
        fillBox( item->mutable_bbox(), in.bbox );
        item->set_value( in.value );
    }

    aOut->set_truncated( aIn.truncated );
    fillBox( aOut->mutable_bbox(), aIn.bbox );
    aOut->set_empty_selection( aIn.emptySelection );
    aOut->set_sheet_path( aIn.sheetPath );
    aOut->set_screenshot( aIn.screenshot );

    if( !aIn.screenshot.empty() )
        fillBox( aOut->mutable_screenshot_region(), aIn.screenshotRegion );

    aOut->set_screenshot_width( aIn.screenshotWidth );
    aOut->set_screenshot_height( aIn.screenshotHeight );
    aOut->set_from_api( aIn.fromApi );
}


void FillEmbedEvent( kiapi::backplane::embed::Event* aOut, const EMBED_EVENT& aIn )
{
    aOut->set_seq( aIn.seq );
    aOut->set_type( static_cast<kiapi::backplane::embed::EventType>( aIn.type ) );
    FillEmbedFrame( aOut->mutable_frame(), aIn.frame );
    aOut->set_detail( aIn.detail );
    FillEmbedItemNames( aOut->mutable_selection(), aIn.selection );

    if( aIn.comment )
        FillEmbedComment( aOut->mutable_comment(), *aIn.comment );

    if( aIn.progress )
        FillEmbedProgress( aOut->mutable_progress(), *aIn.progress );

    if( aIn.checks )
    {
        kiapi::backplane::embed::Checks* checks = aOut->mutable_checks();
        checks->set_kind( static_cast<kiapi::backplane::embed::FrameKind>( aIn.checks->kind ) );
        checks->set_errors( aIn.checks->errors );
        checks->set_warnings( aIn.checks->warnings );
        checks->set_exclusions( aIn.checks->exclusions );
        checks->set_unconnected( aIn.checks->unconnected );
        checks->set_cancelled( aIn.checks->cancelled );
        checks->set_progress_id( aIn.checks->progressId );
        checks->set_document_modified( aIn.checks->documentModified );
        checks->set_error( aIn.checks->error );
    }

    if( !aIn.markerId.empty() )
    {
        aOut->mutable_marker()->set_set( aIn.markerSet );
        aOut->mutable_marker()->set_id( aIn.markerId );
    }

    if( !aIn.drcMarker.empty() )
        aOut->mutable_drc_marker()->ParseFromString( aIn.drcMarker );
}


void FillEmbedProgress( kiapi::backplane::embed::Progress* aOut, const EMBED_PROGRESS& aIn )
{
    aOut->set_id( aIn.id );
    aOut->set_title( aIn.title.ToStdString( wxConvUTF8 ) );
    aOut->set_detail( aIn.detail.ToStdString( wxConvUTF8 ) );
    aOut->set_fraction( aIn.fraction );
    aOut->set_done( aIn.done );
    aOut->set_cancellable( aIn.cancellable );
    aOut->set_cancelled( aIn.cancelled );
}


wxString BACKPLANE_EMBED::NewCommentScreenshotPath()
{
    if( m_apiSocket.empty() )
        return wxEmptyString;

    wxString socketPath = wxString::FromUTF8( m_apiSocket );
    socketPath.StartsWith( wxS( "ipc://" ), &socketPath );

    wxFileName socket( socketPath );
    wxString   dir = socket.GetPath() + wxFileName::GetPathSeparator() + wxS( "backplane-comments" );

    if( !wxFileName::DirExists( dir ) && !wxFileName::Mkdir( dir, 0700, wxPATH_MKDIR_FULL ) )
        return wxEmptyString;

    // Keep the newest MAX_SCREENSHOTS - 1, so that with the new one there are MAX_SCREENSHOTS.
    // Only our own names are pruned.
    wxArrayString files;
    wxDir::GetAllFiles( dir, &files, wxS( "comment-*.png" ), wxDIR_FILES );

    std::vector<std::pair<time_t, wxString>> dated;

    for( const wxString& file : files )
        dated.emplace_back( wxFileModificationTime( file ), file );

    std::sort( dated.begin(), dated.end(),
               []( const auto& a, const auto& b )
               {
                   return a.first != b.first ? a.first > b.first : a.second > b.second;
               } );

    for( size_t i = MAX_SCREENSHOTS - 1; i < dated.size(); ++i )
        wxRemoveFile( dated[i].second );

    wxString name = wxString::Format( wxS( "comment-%lu-%lld-%d.png" ),
                                      static_cast<unsigned long>( wxGetProcessId() ),
                                      static_cast<long long>( wxGetUTCTimeMillis().GetValue() ),
                                      ++m_commentCounter );

    return dir + wxFileName::GetPathSeparator() + name;
}


uint64_t BACKPLANE_EMBED::PushComment( wxFrame* aFrame, EMBED_FRAME_KIND aKind,
                                       const EMBED_COMMENT& aComment )
{
    EMBED_FRAME_INFO frameInfo;

    if( const FRAME_ENTRY* e = entry( aFrame ) )
    {
        frameInfo = info( *e );
    }
    else
    {
        // Not embedded (--api-socket alone): what can be told without the holder
        frameInfo.kind = aKind;

        if( aFrame )
        {
            frameInfo.title = aFrame->GetTitle();
            frameInfo.frameName = aFrame->GetName();
            frameInfo.shown = aFrame->IsShown();
            frameInfo.focused = aFrame->IsActive();
        }
    }

    auto comment = std::make_shared<EMBED_COMMENT>( aComment );
    uint64_t seq = 0;

    {
        std::lock_guard<std::mutex> lock( m_eventMutex );

        EMBED_EVENT event;
        event.seq = seq = m_nextSeq++;
        event.type = EMBED_EVENT_TYPE::COMMENT;
        event.frame = frameInfo;
        event.detail = aComment.note;
        event.selection = aComment.names;
        event.comment = comment;
        m_events.push_back( event );

        while( m_events.size() > MAX_EVENTS )
            m_events.pop_front();
    }

    m_eventCv.notify_all();
    return seq;
}
