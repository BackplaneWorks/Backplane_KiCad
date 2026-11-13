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

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>

#include <thread_pool.h>

/**
 * Run @a aFunc( i ) for every i in [0, aCount) on the KiCad thread pool and return the futures
 * to wait on.
 *
 * Submitting one pool task per item makes every item pay for a queue lock and a thread wake-up,
 * which on boards with many cheap items costs more than the work (the workers spend their time
 * in the pool's lock).  Splitting the range into one fixed block per thread instead leaves the
 * threads idle behind the one block that holds the expensive items.  Here one task per thread
 * pulls small chunks of indices from a shared counter, so cheap items cost one atomic add per
 * chunk and expensive ones still spread over all threads.
 *
 * Each index is visited exactly once, by one thread.
 */
template <typename F>
BS::multi_future<void> DrcParallelFor( size_t aCount, F&& aFunc, size_t aChunk = 0 )
{
    BS::multi_future<void> futures;

    if( aCount == 0 )
        return futures;

    thread_pool& tp = GetKiCadThreadPool();
    size_t       threads = std::max<size_t>( 1, tp.get_thread_count() );

    if( aChunk == 0 )
        aChunk = std::clamp<size_t>( aCount / ( threads * 16 ), 1, 64 );

    size_t chunks = ( aCount + aChunk - 1 ) / aChunk;
    size_t tasks = std::min( threads, chunks );

    auto next = std::make_shared<std::atomic<size_t>>( 0 );
    auto func = std::make_shared<std::decay_t<F>>( std::forward<F>( aFunc ) );

    futures.reserve( tasks );

    for( size_t task = 0; task < tasks; ++task )
    {
        futures.push_back( tp.submit_task(
                [next, func, aCount, aChunk]()
                {
                    while( true )
                    {
                        size_t start = next->fetch_add( aChunk, std::memory_order_relaxed );

                        if( start >= aCount )
                            return;

                        size_t end = std::min( aCount, start + aChunk );

                        for( size_t ii = start; ii < end; ++ii )
                            ( *func )( ii );
                    }
                } ) );
    }

    return futures;
}
