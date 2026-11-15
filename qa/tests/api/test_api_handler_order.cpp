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
#include <memory>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <api/api_handler.h>
#include <api/api_server.h>


namespace
{

class RANKED_HANDLER : public API_HANDLER
{
public:
    explicit RANKED_HANDLER( int aRank ) :
            m_rank( aRank )
    {
    }

    int DispatchRank() const override { return m_rank; }

private:
    int m_rank;
};


std::vector<int> ranks( const KICAD_API_SERVER& aServer )
{
    std::vector<int> out;

    for( const API_HANDLER* handler : aServer.Handlers() )
        out.push_back( handler->DispatchRank() );

    return out;
}

} // namespace


BOOST_AUTO_TEST_SUITE( ApiHandlerOrder )


// A request type several handlers register goes to the editors in a fixed order, whatever the
// order they registered in and wherever they were allocated (it was pointer order: the
// RunAction board undo and the Symbol Editor save refusal both came from it)
BOOST_AUTO_TEST_CASE( HandlersAreAskedByRankThenRegistration )
{
    KICAD_API_SERVER server( false );

    // Registered in an order unlike the ranks; allocated so their addresses run the other way
    std::vector<std::unique_ptr<RANKED_HANDLER>> handlers;

    for( int rank : { API_HANDLER::RANK_COMMON, API_HANDLER::RANK_SYMBOL_EDITOR,
                      API_HANDLER::RANK_PCB_EDITOR, API_HANDLER::RANK_LIBRARIES,
                      API_HANDLER::RANK_SCH_EDITOR, API_HANDLER::RANK_FOOTPRINT_EDITOR,
                      API_HANDLER::RANK_EMBED } )
    {
        handlers.push_back( std::make_unique<RANKED_HANDLER>( rank ) );
    }

    for( auto it = handlers.rbegin(); it != handlers.rend(); ++it )
        server.RegisterHandler( it->get() );

    std::vector<int> expected = { API_HANDLER::RANK_PCB_EDITOR, API_HANDLER::RANK_SCH_EDITOR,
                                  API_HANDLER::RANK_FOOTPRINT_EDITOR, API_HANDLER::RANK_SYMBOL_EDITOR,
                                  API_HANDLER::RANK_LIBRARIES, API_HANDLER::RANK_EMBED,
                                  API_HANDLER::RANK_COMMON };
    std::vector<int> got = ranks( server );
    BOOST_CHECK_EQUAL_COLLECTIONS( got.begin(), got.end(), expected.begin(), expected.end() );

    // Same rank: registration order (a headless handler and an editor's of the same kind)
    RANKED_HANDLER secondSch( API_HANDLER::RANK_SCH_EDITOR );
    RANKED_HANDLER firstSch( API_HANDLER::RANK_SCH_EDITOR );
    server.RegisterHandler( &secondSch );
    server.RegisterHandler( &firstSch );

    const std::vector<API_HANDLER*>& order = server.Handlers();
    auto secondAt = std::find( order.begin(), order.end(), &secondSch );
    auto firstAt = std::find( order.begin(), order.end(), &firstSch );
    BOOST_REQUIRE( secondAt != order.end() && firstAt != order.end() );
    BOOST_CHECK( secondAt < firstAt );
    BOOST_CHECK( ( *std::prev( secondAt ) )->DispatchRank() == API_HANDLER::RANK_SCH_EDITOR );
    BOOST_CHECK( ( *std::next( firstAt ) )->DispatchRank() == API_HANDLER::RANK_FOOTPRINT_EDITOR );

    // Registering twice changes nothing; deregistering takes it out, the rest keep their order
    size_t count = order.size();
    server.RegisterHandler( &firstSch );
    BOOST_CHECK_EQUAL( server.Handlers().size(), count );

    server.DeregisterHandler( &secondSch );
    server.DeregisterHandler( &firstSch );

    for( auto& handler : handlers )
        server.DeregisterHandler( handler.get() );

    BOOST_CHECK( server.Handlers().empty() );
}


BOOST_AUTO_TEST_SUITE_END()
