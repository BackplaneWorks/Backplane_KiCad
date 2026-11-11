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

#include "backplane_comment_pcb.h"

#include <algorithm>

#include <backplane_comment.h>
#include <board.h>
#include <board_connected_item.h>
#include <pcb_draw_panel_gal.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <tool/tool_manager.h>
#include <tools/pcb_selection_tool.h>
#include <view/view.h>


namespace
{
void addUnique( std::vector<std::string>& aList, const wxString& aName )
{
    std::string name = aName.ToStdString( wxConvUTF8 );

    if( !name.empty() && std::find( aList.begin(), aList.end(), name ) == aList.end() )
        aList.push_back( name );
}
} // namespace


EMBED_COMMENT BackplaneBuildPcbComment( PCB_EDIT_FRAME* aFrame, BOX2I& aBox )
{
    EMBED_COMMENT comment;
    BOARD*        board = aFrame->GetBoard();
    const double  iuPerMm = pcbIUScale.IU_PER_MM;
    bool          hasBox = false;

    PCB_SELECTION_TOOL* selTool = aFrame->GetToolManager()->GetTool<PCB_SELECTION_TOOL>();

    if( selTool )
    {
        for( EDA_ITEM* edaItem : selTool->GetSelection().GetItemsSortedBySelectionOrder() )
        {
            if( !edaItem->IsBOARD_ITEM() )
                continue;

            BOARD_ITEM*        item = static_cast<BOARD_ITEM*>( edaItem );
            EMBED_COMMENT_ITEM out;
            BOX2I              box = item->GetBoundingBox();

            out.kind = item->GetClass().ToStdString( wxConvUTF8 );
            out.id = item->m_Uuid.AsStdString();
            out.layer = board->GetLayerName( item->GetLayer() ).ToStdString( wxConvUTF8 );

            if( item->Type() == PCB_FOOTPRINT_T )
            {
                FOOTPRINT* fp = static_cast<FOOTPRINT*>( item );
                box = fp->GetBoundingBox( false );
                out.reference = fp->GetReference().ToStdString( wxConvUTF8 );
                out.value = fp->GetValue().ToStdString( wxConvUTF8 );
                addUnique( comment.names.references, fp->GetReference() );
            }
            else if( item->Type() == PCB_PAD_T )
            {
                PAD* pad = static_cast<PAD*>( item );
                out.layer = board->GetLayerName( pad->GetPrincipalLayer() ).ToStdString( wxConvUTF8 );

                if( FOOTPRINT* fp = pad->GetParentFootprint() )
                {
                    wxString pin = fp->GetReference() + wxS( "." ) + pad->GetNumber();
                    out.reference = fp->GetReference().ToStdString( wxConvUTF8 );
                    out.pin = pin.ToStdString( wxConvUTF8 );
                    addUnique( comment.names.pins, pin );
                }
            }
            else if( FOOTPRINT* parent = item->GetParentFootprint() )
            {
                out.reference = parent->GetReference().ToStdString( wxConvUTF8 );   // fields, ...
            }

            if( BOARD_CONNECTED_ITEM* connected = dynamic_cast<BOARD_CONNECTED_ITEM*>( item ) )
            {
                if( connected->GetNetCode() > 0 )
                {
                    out.net = connected->GetNetname().ToStdString( wxConvUTF8 );
                    addUnique( comment.names.nets, connected->GetNetname() );
                }
            }

            VECTOR2I pos = item->GetPosition();
            out.x = pos.x / iuPerMm;
            out.y = pos.y / iuPerMm;
            box.Normalize();
            out.bbox = BackplaneBoxMm( box, iuPerMm );
            BackplaneMergeBox( aBox, hasBox, box );

            if( comment.items.size() < EMBED_COMMENT::MAX_ITEMS )
                comment.items.push_back( out );
            else
                comment.truncated = true;
        }
    }

    if( !hasBox )
    {
        // Nothing selected: the area in view
        BOX2D view = aFrame->GetCanvas()->GetView()->GetViewport();
        view.Normalize();
        aBox = BOX2I( VECTOR2I( view.GetOrigin() ), VECTOR2L( view.GetSize() ) );
        comment.emptySelection = comment.items.empty();
    }

    comment.bbox = BackplaneBoxMm( aBox, iuPerMm );
    return comment;
}


uint64_t BackplaneSendPcbComment( PCB_EDIT_FRAME* aFrame, EMBED_COMMENT& aComment,
                                  const BOX2I& aBox )
{
    BackplaneCommentScreenshot( aFrame, aBox, pcbIUScale.IU_PER_MM, aComment );
    return BACKPLANE_EMBED::Get().PushComment( aFrame, EMBED_FRAME_KIND::PCB, aComment );
}
