/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 1992-2015 Jean-Pierre Charras <jean-pierre.charras@ujf-grenoble.fr>
 * Copyright (C) 2012 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pcb_layer_box_selector.h"

#include <board.h>
#include <layer_ids.h>
#include <pcb_edit_frame.h>
#include <pcb_layer_presentation.h>
#include <settings/color_settings.h>
#include <tools/pcb_actions.h>
#include <dpi_scaling_common.h>
#include <wx/wupdlock.h>

namespace
{
constexpr int CONSTRUCTION_UI_BASE = -1000;
}

bool PCB_LAYER_BOX_SELECTOR::IsConstructionSelection( int aSelection )
{
    return aSelection <= CONSTRUCTION_UI_BASE
        && aSelection > CONSTRUCTION_UI_BASE - 16;
}

CONSTRUCTION_LAYER PCB_LAYER_BOX_SELECTOR::ConstructionFromSelection( int aSelection )
{
    if( !IsConstructionSelection( aSelection ) ) return CONSTRUCTION_LAYER::NONE;
    const int value = CONSTRUCTION_UI_BASE - aSelection;
    return value >= 1 && value <= 4 ? static_cast<CONSTRUCTION_LAYER>( value )
                                    : CONSTRUCTION_LAYER::NONE;
}

int PCB_LAYER_BOX_SELECTOR::SelectionForConstruction( CONSTRUCTION_LAYER aLayer )
{
    const int value = static_cast<int>( aLayer );
    return value >= 1 && value <= 4 ? CONSTRUCTION_UI_BASE - value : wxNOT_FOUND;
}


PCB_LAYER_BOX_SELECTOR::PCB_LAYER_BOX_SELECTOR( wxWindow* parent, wxWindowID id, const wxString& value,
                                                const wxPoint& pos, const wxSize& size, int n,
                                                const wxString choices[], int style ) :
        LAYER_BOX_SELECTOR( parent, id, pos, size, n, choices ),
        m_boardFrame( nullptr ),
        m_showNotEnabledBrdlayers( false ),
        m_layerPresentation( std::make_unique<PCB_LAYER_PRESENTATION>( nullptr ) ) // The parent isn't always the frame
{
}


void PCB_LAYER_BOX_SELECTOR::SetBoardFrame( PCB_BASE_FRAME* aFrame )
{
    m_boardFrame = aFrame;
    m_layerPresentation->SetBoardFrame( m_boardFrame );
}


// Reload the Layers
void PCB_LAYER_BOX_SELECTOR::Resync()
{
    wxWindowUpdateLocker updateLock( this );

    Clear();

    const int size = 14;

    LSET     show = LSET::AllLayersMask() & ~m_layerMaskDisable;
    LSET     activated = getEnabledLayers() & ~m_layerMaskDisable;
    wxString layerstatus;

    for( PCB_LAYER_ID layerid : show.UIOrder() )
    {
        if( !m_showNotEnabledBrdlayers && !activated[layerid] )
            continue;
        else if( !activated[layerid] )
            layerstatus = wxT( " " ) + _( "(not activated)" );
        else
            layerstatus.Empty();

        wxVector<wxBitmap> bitmaps;

        for( int scale = 1; scale <= 3; scale++ )
        {
            wxBitmap bmp( size * scale, size * scale );

            m_layerPresentation->DrawColorSwatch( bmp, layerid );

            bmp.SetScaleFactor( scale );
            bitmaps.push_back( bmp );
        }

        wxString layername = m_layerPresentation->getLayerName( layerid ) + layerstatus;

        if( m_layerhotkeys )
        {
            TOOL_ACTION* action = PCB_ACTIONS::LayerIDToAction( layerid );

            if( action )
                layername = AddHotkeyName( layername, action->GetHotKey(), IS_COMMENT );
        }

        Append( layername, wxBitmapBundle::FromBitmaps( bitmaps ), (void*) (intptr_t) layerid );
    }

    // The toolbar selector is also used by footprint editors and dialogs.  Do
    // not expose physical construction context there: only the real PCB
    // editor owns a construction-aware canvas.
    if( m_showConstructionLayers && dynamic_cast<PCB_EDIT_FRAME*>( m_boardFrame ) )
    {
        for( int value = 1; value <= 4; ++value )
        {
            const auto layer = static_cast<CONSTRUCTION_LAYER>( value );
            Append( wxString::FromUTF8( ConstructionLayerName( layer ) ), wxNullBitmap,
                    (void*) (intptr_t) SelectionForConstruction( layer ) );
        }
    }

    if( !m_undefinedLayerName.IsEmpty() )
        Append( m_undefinedLayerName, wxNullBitmap, (void*)(intptr_t)UNDEFINED_LAYER );

    // Ensure the size of the widget is enough to show the text and the icon
    // We have to have a selected item when doing this, because otherwise GTK
    // will just choose a random size that might not fit the actual data
    // (such as in cases where the font size is very large). So we select
    // the first item, get the size of the control and make that the minimum size,
    // then remove the selection (which was the initial state).
    SetSelection( 0 );

    SetMinSize( wxSize( -1, -1 ) );
    wxSize bestSize = GetBestSize();

#ifdef __WX_MAC__  // Mac widget GetBestSize() already includes icon width
    bestSize.x = GetBestSize().x + size + 10;
#endif
    SetMinSize( bestSize );

    SetSelection( wxNOT_FOUND );
    Fit();
}


// Returns true if the layer id is enabled (i.e. is it should be displayed)
bool PCB_LAYER_BOX_SELECTOR::isLayerEnabled( int aLayer ) const
{
    if( aLayer < 0 || aLayer >= PCB_LAYER_ID_COUNT )
        return false;
    return getEnabledLayers().test( aLayer );
}


LSET PCB_LAYER_BOX_SELECTOR::getEnabledLayers() const
{
    static const LSET footprintEditorLayers = LSET::AllLayersMask();

    if( m_boardFrame )
        return m_boardFrame->GetBoard()->GetEnabledLayers();
    else
        return footprintEditorLayers;
}
