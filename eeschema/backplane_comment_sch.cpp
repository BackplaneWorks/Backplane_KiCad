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

#include "backplane_comment_sch.h"

#include <algorithm>

#include <backplane_comment.h>
#include <sch_draw_panel.h>
#include <sch_edit_frame.h>
#include <sch_field.h>
#include <sch_pin.h>
#include <sch_symbol.h>
#include <tool/tool_manager.h>
#include <tools/sch_selection_tool.h>
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


EMBED_COMMENT BackplaneBuildSchComment( SCH_EDIT_FRAME* aFrame, BOX2I& aBox )
{
    EMBED_COMMENT         comment;
    const SCH_SHEET_PATH& sheet = aFrame->GetCurrentSheet();
    const double          iuPerMm = schIUScale.IU_PER_MM;
    const std::string     sheetPath = sheet.PathHumanReadable( false ).ToStdString( wxConvUTF8 );
    bool                  hasBox = false;

    comment.sheetPath = sheetPath;

    SCH_SELECTION_TOOL* selTool = aFrame->GetToolManager()->GetTool<SCH_SELECTION_TOOL>();

    if( selTool )
    {
        for( EDA_ITEM* edaItem : selTool->GetSelection().GetItemsSortedBySelectionOrder() )
        {
            if( !edaItem->IsSCH_ITEM() )
                continue;

            SCH_ITEM*          item = static_cast<SCH_ITEM*>( edaItem );
            EMBED_COMMENT_ITEM out;
            BOX2I              box = item->GetBoundingBox();

            out.kind = item->GetClass().ToStdString( wxConvUTF8 );
            out.id = item->m_Uuid.AsStdString();
            out.sheetPath = sheetPath;

            if( item->Type() == SCH_SYMBOL_T )
            {
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
                wxString    ref = symbol->GetRef( &sheet, false );
                out.reference = ref.ToStdString( wxConvUTF8 );
                out.value = symbol->GetValue( &sheet, RAW_VALUE ).ToStdString( wxConvUTF8 );
                addUnique( comment.names.references, ref );
            }
            else if( item->Type() == SCH_PIN_T )
            {
                SCH_PIN* pin = static_cast<SCH_PIN*>( item );

                if( SYMBOL* symbol = pin->GetParentSymbol() )
                {
                    wxString ref = symbol->GetRef( &sheet, false );
                    wxString name = ref + wxS( "." ) + pin->GetNumber();
                    out.reference = ref.ToStdString( wxConvUTF8 );
                    out.pin = name.ToStdString( wxConvUTF8 );
                    addUnique( comment.names.pins, name );
                }
            }
            else if( item->Type() == SCH_FIELD_T )
            {
                if( SYMBOL* symbol = dynamic_cast<SYMBOL*>( item->GetParent() ) )
                    out.reference = symbol->GetRef( &sheet, false ).ToStdString( wxConvUTF8 );
            }

            if( item->Type() != SCH_SYMBOL_T )
            {
                if( std::optional<wxString> net = item->GetConnectionName( &sheet ) )
                {
                    out.net = net->ToStdString( wxConvUTF8 );
                    addUnique( comment.names.nets, *net );
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
        BOX2D view = aFrame->GetCanvas()->GetView()->GetViewport();
        view.Normalize();
        aBox = BOX2I( VECTOR2I( view.GetOrigin() ), VECTOR2L( view.GetSize() ) );
        comment.emptySelection = comment.items.empty();
    }

    comment.bbox = BackplaneBoxMm( aBox, iuPerMm );
    return comment;
}


uint64_t BackplaneSendSchComment( SCH_EDIT_FRAME* aFrame, EMBED_COMMENT& aComment,
                                  const BOX2I& aBox )
{
    BackplaneCommentScreenshot( aFrame, aBox, schIUScale.IU_PER_MM, aComment );
    return BACKPLANE_EMBED::Get().PushComment( aFrame, EMBED_FRAME_KIND::SCH, aComment );
}
