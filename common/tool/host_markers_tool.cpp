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

#include <tool/host_markers_tool.h>
#include <view/view.h>


HOST_MARKERS_TOOL::HOST_MARKERS_TOOL() :
        TOOL_INTERACTIVE( "common.HostMarkers" )
{
}


HOST_MARKERS_TOOL::~HOST_MARKERS_TOOL()
{
    if( m_item && getView() )
        getView()->Remove( m_item.get() );
}


void HOST_MARKERS_TOOL::Reset( RESET_REASON aReason )
{
    // After a view clear (sheet change, board load) the item is no longer in the view
    if( m_item && getView() )
    {
        getView()->Remove( m_item.get() );
        getView()->Add( m_item.get() );
    }
}


HOST_MARKERS_ITEM& HOST_MARKERS_TOOL::Markers()
{
    if( !m_item )
    {
        m_item = std::make_unique<HOST_MARKERS_ITEM>();

        if( getView() )
            getView()->Add( m_item.get() );
    }

    return *m_item;
}


void HOST_MARKERS_TOOL::Changed()
{
    if( m_item && getView() )
        getView()->Update( m_item.get() );
}
