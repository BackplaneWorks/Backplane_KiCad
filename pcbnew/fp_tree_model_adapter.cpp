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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <iterator>
#include <ranges>
#include <pgm_base.h>
#include <pcb_base_frame.h>
#include <core/kicad_algo.h>
#include <settings/common_settings.h>
#include <pcbnew_settings.h>
#include <project/project_file.h>
#include <wx/log.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>
#include <string_utils.h>
#include <trace_helpers.h>
#include <footprint_library_adapter.h>
#include <footprint.h>
#include <generate_footprint_info.h>

#include "fp_tree_model_adapter.h"

wxObjectDataPtr<LIB_TREE_MODEL_ADAPTER>
FP_TREE_MODEL_ADAPTER::Create( PCB_BASE_FRAME* aParent, FOOTPRINT_LIBRARY_ADAPTER* aLibs )
{
    auto* adapter = new FP_TREE_MODEL_ADAPTER( aParent, aLibs );
    return wxObjectDataPtr<LIB_TREE_MODEL_ADAPTER>( adapter );
}


FP_TREE_MODEL_ADAPTER::FP_TREE_MODEL_ADAPTER( PCB_BASE_FRAME* aParent, FOOTPRINT_LIBRARY_ADAPTER* aLibs ) :
        LIB_TREE_MODEL_ADAPTER( aParent, wxT( "pinned_footprint_libs" ),
                                aParent->GetViewerSettingsBase()->m_LibTree ),
        m_libs( aLibs )
{}


FP_TREE_MODEL_ADAPTER::~FP_TREE_MODEL_ADAPTER()
{
    StopLazyLoad();
}


void FP_TREE_MODEL_ADAPTER::StopLazyLoad()
{
    *m_alive = false;

    if( m_check_pending_libraries_timer )
    {
        m_check_pending_libraries_timer->Stop();
        m_check_pending_libraries_timer.reset();
    }

    m_pending_load_libraries.clear();
}


void FP_TREE_MODEL_ADAPTER::AddLibraries( EDA_BASE_FRAME* aParent )
{
    COMMON_SETTINGS* cfg = Pgm().GetCommonSettings();
    PROJECT_FILE&    project = aParent->Prj().GetProjectFile();

    std::vector<wxString> toLoad;
    std::ranges::copy( m_pending_load_libraries, std::back_inserter( toLoad ) );

    bool isLazyLoad = !m_pending_load_libraries.empty();
    m_pending_load_libraries.clear();

    if( toLoad.empty() )
    {
        for( const LIBRARY_TABLE_ROW* row : m_libs->Rows() )
        {
            if( row->Disabled() || row->Hidden() )
                continue;

            toLoad.emplace_back( row->Nickname() );
        }

        StrNumSort( toLoad, CASE_SENSITIVITY::INSENSITIVE );
    }

    bool anyLoaded = false;

    for( const wxString& libName : toLoad )
    {
        std::optional<LIB_STATUS> status = m_libs->GetLibraryStatus( libName );

        if( status && status->load_status == LOAD_STATUS::LOAD_ERROR )
            continue;

        if( !status || status->load_status != LOAD_STATUS::LOADED || !m_libs->HasLibrary( libName, true ) )
        {
            if( auto row = m_libs->GetRow( libName ); row && !( *row )->Disabled() && !( *row )->Hidden() )
                m_pending_load_libraries.insert( libName );

            continue;
        }

        anyLoaded = true;

        bool pinned = alg::contains( cfg->m_Session.pinned_fp_libs, libName )
                        || alg::contains( project.m_PinnedFootprintLibs, libName );

        std::vector<FOOTPRINT*> footprints = m_libs->GetFootprints( libName, true );
        std::vector<LIB_TREE_ITEM*> treeItems;
        treeItems.reserve( footprints.size() );

        for( FOOTPRINT* fp : footprints )
            treeItems.push_back( fp );

        wxString desc = m_libs->GetLibraryDescription( libName ).value_or( wxEmptyString );
        DoAddLibrary( libName, desc, treeItems, pinned, true );
    }

    // Libraries an embedded KiCad left for later (DeferFullLoad) are loaded now, in the
    // background; the timer below adds them as they arrive, as for a preload still running.
    if( !m_pending_load_libraries.empty() )
    {
        if( !m_libs->StartDeferredFullLoad() && !m_libs->FullLoadDeferred() )
            m_libs->AsyncLoad();
    }

    if( !m_pending_load_libraries.empty() && !m_check_pending_libraries_timer )
    {
        m_check_pending_libraries_timer = std::make_unique<wxTimer>( aParent );

        wxLogTrace( traceLibraries, "%zu pending footprint libraries, starting lazy load...",
                    m_pending_load_libraries.size() );

        std::shared_ptr<bool> alive = m_alive;

        aParent->Bind( wxEVT_TIMER,
                [this, alive, aParent]( wxTimerEvent& )
                {
                    if( !*alive )
                        return;

                    AddLibraries( aParent );

                    if( m_pending_load_libraries.empty() && m_check_pending_libraries_timer )
                    {
                        m_check_pending_libraries_timer->Stop();
                        m_check_pending_libraries_timer.reset();
                        wxLogTrace( traceLibraries, "Done lazy-loading footprint libraries" );
                    }
                },
                m_check_pending_libraries_timer->GetId() );

        m_check_pending_libraries_timer->Start( 1000 );
    }

    m_tree.AssignIntrinsicRanks( m_shownColumns );

    if( isLazyLoad && anyLoaded && m_lazyLoadHandler )
    {
        createMissingColumns();
        m_lazyLoadHandler();
    }
}


wxString FP_TREE_MODEL_ADAPTER::GenerateInfo( LIB_ID const& aLibId, int aUnit )
{
    return GenerateFootprintInfo( m_libs, aLibId );
}
