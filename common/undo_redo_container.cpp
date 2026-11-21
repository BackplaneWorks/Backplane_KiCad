/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2018 jp.charras at wanadoo.fr
 * Copyright (C) 2011 Wayne Stambaugh <stambaughw@gmail.com>
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

#include <eda_item.h>
#include <eda_group.h>
#include <undo_redo_container.h>
#include <backplane_coedit.h>
#include <eda_item.h>

#include <set>


ITEM_PICKER::ITEM_PICKER()
{
    m_undoRedoStatus = UNDO_REDO::UNSPECIFIED;
    SetItem( nullptr );
    m_pickerFlags = 0;
    m_link = nullptr;
    m_screen = nullptr;
}


ITEM_PICKER::ITEM_PICKER( BASE_SCREEN* aScreen, EDA_ITEM* aItem, UNDO_REDO aUndoRedoStatus )
{
    m_undoRedoStatus = aUndoRedoStatus;
    SetItem( aItem );
    m_pickerFlags = 0;
    m_link = nullptr;
    m_screen = aScreen;
}


PICKED_ITEMS_LIST::PICKED_ITEMS_LIST() :
        m_actor( COEDIT::CurrentActor() )
{
}


PICKED_ITEMS_LIST::~PICKED_ITEMS_LIST()
{
}


void ITEM_PICKER::SetItem( EDA_ITEM* aItem )
{
    m_pickedItem = nullptr;
    m_pickedItemType = TYPE_NOT_INIT;

    if( aItem )
    {
        m_pickedItem = aItem;
        m_pickedItemType = aItem->Type();

        if( EDA_GROUP* group = dynamic_cast<EDA_GROUP*>( aItem ) )
            m_groupMembers = group->GetGroupMemberIds();

        m_groupId = aItem->GetParentGroupId();
    }
}


void ITEM_PICKER::SetLink( EDA_ITEM* aItem )
{
    m_link = aItem;

    if( aItem )
    {
        if( EDA_GROUP* group = dynamic_cast<EDA_GROUP*>( aItem ) )
            m_groupMembers = group->GetGroupMemberIds();

        m_groupId = aItem->GetParentGroupId();
    }
}


void PICKED_ITEMS_LIST::PushItem( const ITEM_PICKER& aItem )
{
    m_ItemsList.push_back( aItem );
}


ITEM_PICKER PICKED_ITEMS_LIST::PopItem()
{
    ITEM_PICKER item;

    if( m_ItemsList.size() != 0 )
    {
        item = m_ItemsList.back();
        m_ItemsList.pop_back();
    }

    return item;
}


bool PICKED_ITEMS_LIST::ContainsItem( const EDA_ITEM* aItem ) const
{
    for( const ITEM_PICKER& picker : m_ItemsList )
    {
        if( picker.GetItem() == aItem )
            return true;
    }

    return false;
}


int PICKED_ITEMS_LIST::FindItem( const EDA_ITEM* aItem ) const
{
    for( size_t i = 0; i < m_ItemsList.size(); i++ )
    {
        if( m_ItemsList[i].GetItem() == aItem )
            return i;
    }

    return -1;
}


void PICKED_ITEMS_LIST::ClearItemsList()
{
    m_ItemsList.clear();
}


void PICKED_ITEMS_LIST::ClearListAndDeleteItems( std::function<void(EDA_ITEM*)> aItemDeleter )
{
    while( GetCount() > 0 )
    {
        ITEM_PICKER wrapper = PopItem();

        if( wrapper.GetItem() == nullptr ) // No more items in list.
            break;

        // The Link is an undo construct; it is always owned by the undo/redo container
        if( wrapper.GetLink() )
            aItemDeleter( wrapper.GetLink() );

        if( wrapper.GetFlags() & UR_TRANSIENT )
        {
            aItemDeleter( wrapper.GetItem() );
        }
        else if( wrapper.GetStatus() == UNDO_REDO::DELETED )
        {
            // This should really be replaced with UR_TRANSIENT, but currently many clients
            // (eeschema in particular) abuse this to achieve non-undo-related deletions.
            aItemDeleter( wrapper.GetItem() );
        }
    }
}


const ITEM_PICKER& PICKED_ITEMS_LIST::GetItemWrapper( unsigned int aIdx ) const
{
    return m_ItemsList.at( aIdx );
}


ITEM_PICKER& PICKED_ITEMS_LIST::GetItemWrapper( unsigned int aIdx )
{
    return m_ItemsList.at( aIdx );
}


EDA_ITEM* PICKED_ITEMS_LIST::GetPickedItem( unsigned int aIdx ) const
{
    if( aIdx < m_ItemsList.size() )
        return m_ItemsList[aIdx].GetItem();

    return nullptr;
}


BASE_SCREEN* PICKED_ITEMS_LIST::GetScreenForItem( unsigned int aIdx ) const
{
    if( aIdx < m_ItemsList.size() )
        return m_ItemsList[aIdx].GetScreen();

    return nullptr;
}


EDA_ITEM* PICKED_ITEMS_LIST::GetPickedItemLink( unsigned int aIdx ) const
{
    if( aIdx < m_ItemsList.size() )
        return m_ItemsList[aIdx].GetLink();

    return nullptr;
}


UNDO_REDO PICKED_ITEMS_LIST::GetPickedItemStatus( unsigned int aIdx ) const
{
    if( aIdx < m_ItemsList.size() )
        return m_ItemsList[aIdx].GetStatus();

    return UNDO_REDO::UNSPECIFIED;
}


EDA_ITEM_FLAGS PICKED_ITEMS_LIST::GetPickerFlags( unsigned aIdx ) const
{
    if( aIdx < m_ItemsList.size() )
        return m_ItemsList[aIdx].GetFlags();

    return 0;
}


bool PICKED_ITEMS_LIST::SetPickedItem( EDA_ITEM* aItem, unsigned aIdx )
{
    if( aIdx < m_ItemsList.size() )
    {
        m_ItemsList[aIdx].SetItem( aItem );
        return true;
    }

    return false;
}


bool PICKED_ITEMS_LIST::SetPickedItemLink( EDA_ITEM* aLink, unsigned aIdx )
{
    if( aIdx < m_ItemsList.size() )
    {
        m_ItemsList[aIdx].SetLink( aLink );
        return true;
    }

    return false;
}


bool PICKED_ITEMS_LIST::SetPickedItemStatus( UNDO_REDO aStatus, unsigned aIdx )
{
    if( aIdx < m_ItemsList.size() )
    {
        m_ItemsList[aIdx].SetStatus( aStatus );
        return true;
    }

    return false;
}


bool PICKED_ITEMS_LIST::SetPickerFlags( EDA_ITEM_FLAGS aFlags, unsigned aIdx )
{
    if( aIdx < m_ItemsList.size() )
    {
        m_ItemsList[aIdx].SetFlags( aFlags );
        return true;
    }

    return false;
}


bool PICKED_ITEMS_LIST::RemovePicker( unsigned aIdx )
{
    if( aIdx >= m_ItemsList.size() )
        return false;

    m_ItemsList.erase( m_ItemsList.begin() + aIdx );
    return true;
}


void PICKED_ITEMS_LIST::CopyList( const PICKED_ITEMS_LIST& aSource )
{
    m_ItemsList = aSource.m_ItemsList;  // Vector's copy
}


void PICKED_ITEMS_LIST::ReversePickersListOrder()
{
    std::vector <ITEM_PICKER> tmp;

    while( !m_ItemsList.empty() )
    {
        tmp.push_back( m_ItemsList.back() );
        m_ItemsList.pop_back();
    }

    m_ItemsList.swap( tmp );
}


UNDO_REDO_CONTAINER::UNDO_REDO_CONTAINER()
{
}


UNDO_REDO_CONTAINER::~UNDO_REDO_CONTAINER()
{
    ClearCommandList();
}


void UNDO_REDO_CONTAINER::ClearCommandList()
{
    for( PICKED_ITEMS_LIST* cmd : m_CommandsList )
    {
        cmd->ClearListAndDeleteItems(
                []( EDA_ITEM* item )
                {
                    delete item;
                } );

        delete cmd;
    }

    m_CommandsList.clear();
}


void UNDO_REDO_CONTAINER::PushCommand( PICKED_ITEMS_LIST* aItem )
{
    m_CommandsList.push_back( aItem );
}


PICKED_ITEMS_LIST* UNDO_REDO_CONTAINER::PopCommand()
{
    if( m_CommandsList.size() != 0 )
    {
        PICKED_ITEMS_LIST* item = m_CommandsList.back();
        m_CommandsList.pop_back();
        return item;
    }

    return nullptr;
}


namespace COEDIT
{
CHANGE ChangeFromUndoList( const PICKED_ITEMS_LIST& aList, const std::string& aDoc )
{
    CHANGE change;
    change.doc = aDoc;
    change.actor = aList.GetActor();
    change.description = aList.GetDescription().ToStdString( wxConvUTF8 );
    change.rebased = REBASE_SCOPE::Active();

    std::set<std::string> seen;

    for( unsigned ii = 0; ii < aList.GetCount(); ++ii )
    {
        const EDA_ITEM* item = aList.GetPickedItem( ii );

        if( !item )
            continue;

        std::string id = item->m_Uuid.AsStdString();

        switch( aList.GetPickedItemStatus( ii ) )
        {
        case UNDO_REDO::BOARD_PLAN:      id = "plan"; break;
        case UNDO_REDO::BOARD_STRUCTURE: id = "board_structure"; break;
        case UNDO_REDO::PAGESETTINGS:    id = "page_settings"; break;
        case UNDO_REDO::DRILLORIGIN:     id = "drill_origin"; break;
        case UNDO_REDO::GRIDORIGIN:      id = "grid_origin"; break;
        default:                         break;
        }

        if( !seen.insert( id ).second )
            continue;

        switch( aList.GetPickedItemStatus( ii ) )
        {
        case UNDO_REDO::NEWITEM: change.created.push_back( id ); break;
        case UNDO_REDO::DELETED: change.deleted.push_back( id ); break;
        default:                 change.modified.push_back( id ); break;
        }
    }

    return change;
}
} // namespace COEDIT


bool PICKED_ITEMS_LIST::Overlaps( const PICKED_ITEMS_LIST& aOther ) const
{
    // Whole-document snapshots (page settings, origins, board structure, the routing plan)
    // overlap any other snapshot of the same kind.
    auto snapshot = []( UNDO_REDO aStatus )
    {
        switch( aStatus )
        {
        case UNDO_REDO::PAGESETTINGS:
        case UNDO_REDO::DRILLORIGIN:
        case UNDO_REDO::GRIDORIGIN:
        case UNDO_REDO::BOARD_STRUCTURE:
        case UNDO_REDO::BOARD_PLAN:
        case UNDO_REDO::VARIANTS:
            return true;
        default:
            return false;
        }
    };

    std::set<const EDA_ITEM*> mine;
    std::set<KIID>            ids;
    std::set<UNDO_REDO>       snapshots;

    for( const ITEM_PICKER& picker : m_ItemsList )
    {
        if( snapshot( picker.GetStatus() ) )
            snapshots.insert( picker.GetStatus() );
        else if( EDA_ITEM* item = picker.GetItem() )
        {
            mine.insert( item );
            ids.insert( item->m_Uuid );
        }
    }

    for( unsigned ii = 0; ii < aOther.GetCount(); ++ii )
    {
        UNDO_REDO status = aOther.GetPickedItemStatus( ii );

        if( snapshot( status ) )
        {
            if( snapshots.count( status ) )
                return true;

            continue;
        }

        const EDA_ITEM* item = aOther.GetPickedItem( ii );

        if( item && ( mine.count( item ) || ids.count( item->m_Uuid ) ) )
            return true;
    }

    return false;
}


int UNDO_REDO_CONTAINER::FindNewest( const std::string& aActor ) const
{
    for( int ii = (int) m_CommandsList.size() - 1; ii >= 0; --ii )
    {
        if( m_CommandsList[ii] && m_CommandsList[ii]->GetActor() == aActor )
            return ii;
    }

    return -1;
}


std::vector<int> UNDO_REDO_CONTAINER::DependentsAfter( int aIndex ) const
{
    std::vector<int>                       deps;
    std::vector<const PICKED_ITEMS_LIST*>  frontier{ m_CommandsList[aIndex] };

    for( int ii = aIndex + 1; ii < (int) m_CommandsList.size(); ++ii )
    {
        const PICKED_ITEMS_LIST* cmd = m_CommandsList[ii];

        for( const PICKED_ITEMS_LIST* seed : frontier )
        {
            if( cmd->Overlaps( *seed ) )
            {
                deps.push_back( ii );
                frontier.push_back( cmd );
                break;
            }
        }
    }

    return deps;
}


PICKED_ITEMS_LIST* UNDO_REDO_CONTAINER::Extract( int aIndex )
{
    if( aIndex < 0 || aIndex >= (int) m_CommandsList.size() )
        return nullptr;

    PICKED_ITEMS_LIST* item = m_CommandsList[aIndex];
    m_CommandsList.erase( m_CommandsList.begin() + aIndex );
    return item;
}
