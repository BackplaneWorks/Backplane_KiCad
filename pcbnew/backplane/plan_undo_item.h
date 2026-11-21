// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <backplane/board_plan.h>
#include <eda_item.h>

// A transient undo record of the whole routing plan (docs/backplane/floorplanner.md).
class PLAN_UNDO_ITEM : public EDA_ITEM
{
public:
    explicit PLAN_UNDO_ITEM( const BOARD_PLAN& aPlan ) :
            EDA_ITEM( TYPE_NOT_INIT ), plan( aPlan )
    {
        SetFlags( UR_TRANSIENT );
    }

    wxString GetClass() const override { return wxT( "PLAN_UNDO_ITEM" ); }
#if defined(DEBUG)
    void Show( int, std::ostream& ) const override {}
#endif
    BOARD_PLAN plan;
};
