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
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EMBED_HOST_ORDER_H
#define EMBED_HOST_ORDER_H

#include <kicommon.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

/**
 * The order in which an embedding host's window requests are applied (#1199).
 *
 * A host sends its window geometry (SetHostGeometry, on the main API socket or the events
 * socket) and shows and hides frames (ShowFrame, HideFrame, OpenGerbers: main socket only).
 * Each request takes a slot in one queue when it is received, on the thread that received it;
 * the UI thread applies the slots strictly in that receipt order.  A slot whose request has not
 * been handled yet holds back everything received after it, so nothing later overtakes a show
 * or a hide, whichever socket it came on and whichever event queue wx runs first.
 *
 * Pure: no wx, no windows.  BACKPLANE_EMBED drives it and applies its results.
 */
namespace EMBED_HOST_ORDER
{

struct GEOMETRY
{
    int  x = 0;
    int  y = 0;
    int  width = 0;
    int  height = 0;
    bool visible = true;
    bool raise = false;

    bool operator==( const GEOMETRY& aOther ) const
    {
        return x == aOther.x && y == aOther.y && width == aOther.width && height == aOther.height
               && visible == aOther.visible && raise == aOther.raise;
    }
};


enum class KIND
{
    GEOMETRY,   ///< SetHostGeometry
    SHOW,       ///< an explicit show of a frame (ShowFrame, not a prewarm): the host is visible
    BARRIER     ///< a request that orders but changes no host state (HideFrame, a prewarm, ...)
};


struct OP
{
    KIND     kind = KIND::GEOMETRY;
    GEOMETRY geometry;           ///< KIND::GEOMETRY only
    uint64_t seq = 0;            ///< its slot (filled in by QUEUE)
};


/// The host as last applied.
struct STATE
{
    bool     known = false;      ///< a geometry with a size has been applied
    GEOMETRY geometry;           ///< its rect and visibility; raise is per step, never kept
    uint64_t seq = 0;            ///< the slot of the last op applied
};


/**
 * The state after aOp.  A geometry replaces the rect and the visibility (the newest wins); an
 * explicit show makes the host visible (the host would not show a frame in a window nobody can
 * see); a barrier changes nothing.
 */
KICOMMON_API STATE Reduce( const STATE& aState, const OP& aOp );


/**
 * Fold a run of ready ops for one apply: consecutive geometries become one (the last rect and
 * visibility; raise kept if any of them raised after the run's last hide, so a pending raise is
 * never lost and never revives a later hide).  A show or barrier is never folded and never
 * crossed: geometries on either side of it stay on their side.
 */
KICOMMON_API std::vector<OP> Fold( const std::vector<OP>& aOps );


/**
 * Slots in receipt order.  Thread-safe.
 */
class KICOMMON_API QUEUE
{
public:
    /// A slot for a request just received (any thread).  Its op is given later (Fill) or the
    /// slot is dropped (Cancel).
    uint64_t Reserve();

    /// The op for slot aSeq (any thread).  A slot reserved and never filled holds back every
    /// later one, so every reserved slot must be filled or cancelled.
    void Fill( uint64_t aSeq, OP aOp );

    /// Slot aSeq applies nothing (the request was refused or failed).
    void Cancel( uint64_t aSeq );

    /// Reserve and fill at once (the events socket: received and known at the same moment).
    uint64_t Submit( OP aOp );

    /// The ops ready to apply, in receipt order and folded: every filled slot up to the first one
    /// still open, and only slots before aBefore.  They are removed from the queue.
    std::vector<OP> TakeReady( uint64_t aBefore = UINT64_MAX );

    /// True if a slot before aBefore is still waiting to be applied.
    bool HasReadyOrOpen( uint64_t aBefore = UINT64_MAX ) const;

private:
    struct SLOT
    {
        bool              done = false;   ///< filled or cancelled
        std::optional<OP> op;             ///< empty when cancelled
    };

    mutable std::mutex       m_mutex;
    uint64_t                 m_next = 1;
    std::map<uint64_t, SLOT> m_slots;
};

} // namespace EMBED_HOST_ORDER

#endif // EMBED_HOST_ORDER_H
