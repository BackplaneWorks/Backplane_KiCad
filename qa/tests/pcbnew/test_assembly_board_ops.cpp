#include <boost/test/unit_test.hpp>

#include <assembly/assembly_board_ops.h>
#include <assembly/assembly_model.h>
#include <board.h>
#include <footprint.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_io/pcb_io_mgr.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <fstream>
#include <iterator>
#include <filesystem>

static std::filesystem::path disposableBoard()
{
    const auto source = std::filesystem::path( __FILE__ ).parent_path().parent_path().parent_path().parent_path()
                        / "qa/data/pcbnew/api_kitchen_sink.kicad_pcb";
    const auto root = std::filesystem::temp_directory_path() / "backplane-board-ops-test";
    std::error_code ec;
    std::filesystem::remove_all( root, ec );
    std::filesystem::create_directories( root );
    const auto target = root / source.filename();
    std::filesystem::copy_file( source, target );
    const auto companion = std::filesystem::path( source.string() + ".backplane.json" );
    if( std::filesystem::exists( companion ) )
        std::filesystem::copy_file( companion, std::filesystem::path( target.string() + ".backplane.json" ) );
    return target;
}

static void cleanDisposable( const std::filesystem::path& path )
{
    std::error_code ec;
    std::filesystem::remove_all( path.parent_path(), ec );
}

BOOST_AUTO_TEST_SUITE( BackplaneAssemblyBoardOps )

BOOST_AUTO_TEST_CASE( ElectricalSnapshotReportsMalformedLinksWithoutMutation )
{
    ASSEMBLY_MANIFEST manifest;
    manifest.electrical = { { "links", nlohmann::json::array( {
        { { "name", "BUS" }, { "endpoints", nlohmann::json::array() } },
        { { "name", "BROKEN" } }
    } ) } };
    const auto snapshot = AssemblyElectricalSync( manifest );
    BOOST_CHECK( snapshot.contains( "links" ) );
    BOOST_CHECK( snapshot.contains( "errors" ) );
    BOOST_CHECK( !snapshot["errors"].empty() );
    BOOST_CHECK_EQUAL( manifest.electrical["links"].at( 0 ).at( "name" ), "BUS" );
}

BOOST_AUTO_TEST_CASE( MoveRejectsNonFiniteCoordinatesBeforeResolvingSource )
{
    ASSEMBLY_MANIFEST manifest;
    manifest.base_directory = ".";
    manifest.instances.push_back( { "board", "does-not-exist.kicad_pcb" } );
    BOOST_CHECK_THROW( AssemblyMoveComponent( manifest, "board", "R1",
                                              std::numeric_limits<double>::quiet_NaN(), 0, 0 ),
                       std::invalid_argument );
    BOOST_CHECK_THROW( AssemblyMoveComponent( manifest, "board", "R1", 3000, 0, 0 ),
                       std::invalid_argument );
}

BOOST_AUTO_TEST_CASE( ListAndActiveMoveUseLiveBoardAndPreserveDisk )
{
    const auto path = disposableBoard();
    auto board = PCB_IO_MGR::Load( PCB_IO_MGR::KICAD_SEXP, wxString::FromUTF8( path.string() ) );
    BOOST_REQUIRE( board );
    ASSEMBLY_MANIFEST manifest;
    manifest.base_directory = path.parent_path();
    manifest.instances.push_back( { "bridge", path.filename().string() } );
    const auto listed = AssemblyListComponents( manifest, "bridge", board.get() );
    BOOST_REQUIRE( listed.contains( "components" ) );
    BOOST_REQUIRE( !listed["components"].empty() );
    const std::string reference = listed["components"][0]["reference"];
    auto original = std::ifstream( path, std::ios::binary );
    const std::string disk( std::istreambuf_iterator<char>( original ), {} );
    BOOST_REQUIRE( AssemblyMoveComponent( manifest, "bridge", reference, 12.0, 13.0, 27.0,
                                          nullptr, board.get() ) );
    FOOTPRINT* liveFootprint = nullptr;
    for( FOOTPRINT* candidate : board->Footprints() )
        if( candidate->GetReference().ToStdString() == reference ) liveFootprint = candidate;
    BOOST_REQUIRE( liveFootprint );
    BOOST_CHECK_EQUAL( liveFootprint->GetPosition().x, 12000000 );
    BOOST_CHECK_EQUAL( liveFootprint->GetPosition().y, 13000000 );
    auto after = std::ifstream( path, std::ios::binary );
    BOOST_CHECK_EQUAL( disk, std::string( std::istreambuf_iterator<char>( after ), {} ) );
    cleanDisposable( path );
}

BOOST_AUTO_TEST_CASE( StandaloneMoveBackupsAndPreservesCompanion )
{
    const auto path = disposableBoard();
    const auto companion = std::filesystem::path( path.string() + ".backplane.json" );
    auto beforeCompanion = std::ifstream( companion, std::ios::binary );
    const std::string companionBytes( std::istreambuf_iterator<char>( beforeCompanion ), {} );
    ASSEMBLY_MANIFEST manifest;
    manifest.base_directory = path.parent_path();
    manifest.instances.push_back( { "bridge", path.filename().string() } );
    auto sourceBoard = PCB_IO_MGR::Load( PCB_IO_MGR::KICAD_SEXP, wxString::FromUTF8( path.string() ) );
    BOOST_REQUIRE( sourceBoard && !sourceBoard->Footprints().empty() );
    const std::string reference = sourceBoard->Footprints().front()->GetReference().ToStdString();
    BOOST_REQUIRE( AssemblyMoveComponent( manifest, "bridge", reference, 17.0, 18.0, 33.0 ) );
    auto moved = PCB_IO_MGR::Load( PCB_IO_MGR::KICAD_SEXP, wxString::FromUTF8( path.string() ) );
    BOOST_REQUIRE( moved );
    FOOTPRINT* movedFootprint = nullptr;
    for( FOOTPRINT* candidate : moved->Footprints() )
        if( candidate->GetReference().ToStdString() == reference ) movedFootprint = candidate;
    BOOST_REQUIRE( movedFootprint );
    BOOST_CHECK_EQUAL( movedFootprint->GetPosition().x, 17000000 );
    BOOST_CHECK_EQUAL( movedFootprint->GetPosition().y, 18000000 );
    const auto backup = path.parent_path() / ".backplane-backups" / ( path.filename().string() + ".bak" );
    BOOST_CHECK( std::filesystem::exists( backup ) );
    auto afterCompanion = std::ifstream( companion, std::ios::binary );
    BOOST_CHECK_EQUAL( companionBytes, std::string( std::istreambuf_iterator<char>( afterCompanion ), {} ) );
    auto beforeInvalid = std::ifstream( path, std::ios::binary );
    const std::string boardBytes( std::istreambuf_iterator<char>( beforeInvalid ), {} );
    BOOST_CHECK_THROW( AssemblyMoveComponent( manifest, "bridge", "missing", 1.0, 2.0, 3.0 ),
                       std::invalid_argument );
    auto afterInvalid = std::ifstream( path, std::ios::binary );
    BOOST_CHECK_EQUAL( boardBytes, std::string( std::istreambuf_iterator<char>( afterInvalid ), {} ) );
    cleanDisposable( path );
}

BOOST_AUTO_TEST_CASE( ElectricalSnapshotResolvesLinkedNativePads )
{
    const auto path = disposableBoard();
    auto board = PCB_IO_MGR::Load( PCB_IO_MGR::KICAD_SEXP, wxString::FromUTF8( path.string() ) );
    BOOST_REQUIRE( board );
    FOOTPRINT* footprint = nullptr;
    const PAD* pad = nullptr;
    PAD* differentPad = nullptr;
    for( FOOTPRINT* candidate : board->Footprints() )
        for( const PAD* candidatePad : candidate->Pads() )
            if( !pad && candidatePad->GetNetCode() > 0 ) { footprint = candidate; pad = candidatePad; }
    BOOST_REQUIRE( footprint && pad );
    // The kitchen sink has a single named net, so the link needs a second native net to join:
    // give the netless pad beside the first one a net "B" of its own, on disk, where the sync
    // reads each instance's board from.
    for( PAD* candidatePad : footprint->Pads() )
        if( !differentPad && candidatePad->GetNetCode() <= 0 && !candidatePad->GetNumber().IsEmpty() )
            differentPad = candidatePad;
    BOOST_REQUIRE( differentPad );
    NETINFO_ITEM* netB = new NETINFO_ITEM( board.get(), wxT( "B" ) );
    board->Add( netB );
    differentPad->SetNet( netB );
    BOOST_REQUIRE_NE( differentPad->GetNetCode(), pad->GetNetCode() );
    PCB_IO_MGR::Save( PCB_IO_MGR::KICAD_SEXP, wxString::FromUTF8( path.string() ), *board );
    ASSEMBLY_MANIFEST manifest;
    manifest.base_directory = path.parent_path();
    manifest.instances = { { "tx", path.filename().string() }, { "rx", path.filename().string() } };
    nlohmann::json endpointA = { { "instance", "tx" }, { "reference", footprint->GetReference().ToStdString() },
                                 { "pad", pad->GetNumber().ToStdString() } };
    nlohmann::json endpointB = { { "instance", "rx" }, { "reference", differentPad->GetParentFootprint()->GetReference().ToStdString() },
                                 { "pad", differentPad->GetNumber().ToStdString() } };
    nlohmann::json link = { { "name", "BUS" }, { "endpoints", { endpointA, endpointB } } };
    manifest.electrical = { { "links", { link } } };
    const auto snapshot = AssemblyElectricalSync( manifest, board.get() );
    BOOST_REQUIRE( snapshot["errors"].empty() );
    BOOST_CHECK( snapshot["links"][0]["endpoints"][0].contains( "uuid" ) );
    BOOST_CHECK( snapshot["links"][0].contains( "system_net" ) );
    manifest.electrical["snapshot"] = snapshot;
    const auto unchanged = AssemblyElectricalSync( manifest, board.get() );
    BOOST_CHECK_EQUAL( unchanged["changed"], false );
    manifest.electrical["links"].push_back( { { "name", "OTHER" },
        { "endpoints", { endpointA, endpointB } } } );
    const auto conflict = AssemblyElectricalSync( manifest, board.get() );
    BOOST_CHECK( !conflict["errors"].empty() );
    cleanDisposable( path );
}

BOOST_AUTO_TEST_SUITE_END()
