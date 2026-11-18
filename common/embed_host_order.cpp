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

#include <embed_host_order.h>

namespace EMBED_HOST_ORDER
{

STATE Reduce( const STATE& aState, const OP& aOp )
{
    STATE next = aState;
    next.seq = aOp.seq;

    switch( aOp.kind )
    {
    case KIND::GEOMETRY:
        next.geometry = aOp.geometry;
        next.geometry.raise = false;
        next.known = aOp.geometry.width > 0 && aOp.geometry.height > 0;
        break;

    case KIND::SHOW:
        next.geometry.visible = true;
        break;

    case KIND::BARRIER:
        break;
    }

    return next;
}


std::vector<OP> Fold( const std::vector<OP>& aOps )
{
    std::vector<OP> folded;

    for( const OP& op : aOps )
    {
        if( op.kind == KIND::GEOMETRY && !folded.empty() && folded.back().kind == KIND::GEOMETRY )
        {
            OP&  run = folded.back();
            bool raise = op.geometry.visible && ( run.geometry.raise || op.geometry.raise );

            run.geometry = op.geometry;
            run.geometry.raise = raise;   // a hide inside the run drops the raises before it
            run.seq = op.seq;
            continue;
        }

        OP first = op;

        if( first.kind == KIND::GEOMETRY && !first.geometry.visible )
            first.geometry.raise = false;

        folded.push_back( first );
    }

    return folded;
}


uint64_t QUEUE::Reserve()
{
    std::lock_guard<std::mutex> lock( m_mutex );
    uint64_t                    seq = m_next++;
    m_slots[seq] = SLOT();
    return seq;
}


void QUEUE::Fill( uint64_t aSeq, OP aOp )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    auto                        it = m_slots.find( aSeq );

    if( it == m_slots.end() || it->second.done )
        return;

    aOp.seq = aSeq;
    it->second.done = true;
    it->second.op = aOp;
}


void QUEUE::Cancel( uint64_t aSeq )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    auto                        it = m_slots.find( aSeq );

    if( it == m_slots.end() || it->second.done )
        return;

    it->second.done = true;
    it->second.op.reset();
}


uint64_t QUEUE::Submit( OP aOp )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    uint64_t                    seq = m_next++;
    aOp.seq = seq;
    m_slots[seq] = SLOT{ true, aOp };
    return seq;
}


std::vector<OP> QUEUE::TakeReady( uint64_t aBefore )
{
    std::vector<OP> ready;

    {
        std::lock_guard<std::mutex> lock( m_mutex );

        for( auto it = m_slots.begin(); it != m_slots.end() && it->first < aBefore; )
        {
            if( !it->second.done )
            {
                if( !it->second.passable )
                    break;

                ++it;   // stays until filled, holding nothing back
                continue;
            }

            // A barrier filled after later slots went past it applies nothing now: its effect
            // was applied when it began (MarkPassable is only for such slots)
            if( it->second.op && !it->second.passable )
                ready.push_back( *it->second.op );

            it = m_slots.erase( it );
        }
    }

    return Fold( ready );
}


bool QUEUE::HasReadyOrOpen( uint64_t aBefore ) const
{
    std::lock_guard<std::mutex> lock( m_mutex );

    for( const auto& [seq, slot] : m_slots )
    {
        if( seq >= aBefore )
            break;

        if( !slot.passable )
            return true;
    }

    return false;
}


void QUEUE::MarkPassable( uint64_t aSeq )
{
    std::lock_guard<std::mutex> lock( m_mutex );
    auto                        it = m_slots.find( aSeq );

    if( it != m_slots.end() && !it->second.done )
        it->second.passable = true;
}

} // namespace EMBED_HOST_ORDER
