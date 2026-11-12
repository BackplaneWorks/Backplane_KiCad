/*
 * This program source code file is part of KiCad, a free EDA CAD application.
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

#include "board_plan.h"

#include <connectivity/connectivity_algo.h>
#include <board.h>
#include <board_design_settings.h>
#include <board_stackup_manager/board_stackup.h>
#include <footprint.h>
#include <geometry/seg.h>
#include <netinfo.h>
#include <pad.h>
#include <connectivity/connectivity_data.h>
#include <connectivity/connectivity_items.h>
#include <project/net_settings.h>
#include <pcb_marker.h>
#include <drc/drc_item.h>

#include <nlohmann/json.hpp>
#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <set>
#include <stdexcept>

using nlohmann::json;


namespace
{
constexpr double NM_PER_MM = 1e6;

json pt( const VECTOR2I& aPoint )
{
    return json::array( { aPoint.x / NM_PER_MM, aPoint.y / NM_PER_MM } );
}

VECTOR2I toPt( const json& aJson )
{
    if( !aJson.is_array() || aJson.size() != 2 )
        throw std::runtime_error( "a point is [x, y] in millimetres" );

    return VECTOR2I( KiROUND( aJson[0].get<double>() * NM_PER_MM ),
                     KiROUND( aJson[1].get<double>() * NM_PER_MM ) );
}

json pts( const std::vector<VECTOR2I>& aPoints )
{
    json out = json::array();

    for( const VECTOR2I& p : aPoints )
        out.push_back( pt( p ) );

    return out;
}

std::vector<VECTOR2I> toPts( const json& aJson )
{
    std::vector<VECTOR2I> out;

    if( aJson.is_null() )
        return out;

    for( const json& p : aJson )
        out.push_back( toPt( p ) );

    return out;
}

template <typename T>
T get( const json& aJson, const char* aKey, T aDefault )
{
    auto it = aJson.find( aKey );

    if( it == aJson.end() || it->is_null() )
        return aDefault;

    return it->get<T>();
}

std::vector<std::string> strings( const json& aJson, const char* aKey )
{
    return get<std::vector<std::string>>( aJson, aKey, {} );
}

std::string nowIso()
{
    return wxDateTime::Now().ToUTC().FormatISOCombined().ToStdString() + "Z";
}
} // namespace


std::string PlanStatusName( PLAN_STATUS aStatus )
{
    switch( aStatus )
    {
    case PLAN_STATUS::PROPOSED: return "proposed";
    case PLAN_STATUS::APPROVED: return "approved";
    case PLAN_STATUS::REJECTED: return "rejected";
    case PLAN_STATUS::ROUTING:  return "routing";
    case PLAN_STATUS::ROUTED:   return "routed";
    case PLAN_STATUS::VERIFIED: return "verified";
    case PLAN_STATUS::FAILED:   return "failed";
    }

    return "proposed";
}


PLAN_STATUS PlanStatusFromName( const std::string& aName )
{
    if( aName == "approved" ) return PLAN_STATUS::APPROVED;
    if( aName == "rejected" ) return PLAN_STATUS::REJECTED;
    if( aName == "routing" )  return PLAN_STATUS::ROUTING;
    if( aName == "routed" )   return PLAN_STATUS::ROUTED;
    if( aName == "verified" ) return PLAN_STATUS::VERIFIED;
    if( aName == "failed" )   return PLAN_STATUS::FAILED;
    if( aName.empty() || aName == "proposed" ) return PLAN_STATUS::PROPOSED;

    throw std::runtime_error( "unknown status '" + aName + "'" );
}


double PLAN_CORRIDOR::Length() const
{
    double length = 0;

    for( size_t ii = 1; ii < path.size(); ++ii )
        length += ( path[ii] - path[ii - 1] ).EuclideanNorm();

    return length;
}


bool BOARD_PLAN::Empty() const
{
    return layers.empty() && groups.empty() && corridors.empty() && planes.empty()
           && splits.empty() && notes.empty() && constraints.empty();
}


json BOARD_PLAN::ToJson() const
{
    json out;
    out["format"] = format;
    out["revision"] = revision;
    out["board"] = board;

    json jl = json::array();

    for( const PLAN_LAYER& l : layers )
        jl.push_back( { { "layer", l.layer }, { "role", l.role },
                        { "status", PlanStatusName( l.status ) }, { "note", l.note } } );

    out["layers"] = jl;

    json jg = json::array();

    for( const PLAN_GROUP& g : groups )
    {
        json pairs = json::array();

        for( const auto& [p, n] : g.diffPairs )
            pairs.push_back( json::array( { p, n } ) );

        jg.push_back( { { "id", g.id }, { "name", g.name }, { "color", g.color },
                        { "nets", g.nets }, { "diff_pairs", pairs }, { "netclass", g.netclass },
                        { "max_skew", g.maxSkew }, { "max_length", g.maxLength },
                        { "status", PlanStatusName( g.status ) }, { "locked", g.locked } } );
    }

    out["groups"] = jg;

    json jc = json::array();

    for( const PLAN_CORRIDOR& c : corridors )
    {
        json escapes = json::array();

        for( const PLAN_ESCAPE& e : c.escapes )
            escapes.push_back( { { "at", pt( e.at ) }, { "ref", e.ref } } );

        json entry = { { "id", c.id }, { "group", c.group }, { "layer", c.layer },
                       { "order", c.order }, { "path", pts( c.path ) },
                       { "width", c.width / NM_PER_MM }, { "from", c.from }, { "to", c.to },
                       { "vias", pts( c.vias ) }, { "escapes", escapes },
                       { "status", PlanStatusName( c.status ) }, { "locked", c.locked },
                       { "author", c.author }, { "notes", c.notes } };

        if( c.total || c.routed || c.failed )
            entry["progress"] = { { "routed", c.routed }, { "total", c.total },
                                  { "failed", c.failed } };

        if( c.edited )
            entry["edited"] = true;

        jc.push_back( entry );
    }

    out["corridors"] = jc;

    json jp = json::array();

    for( const PLAN_PLANE& p : planes )
    {
        json rings = json::array();

        for( const std::vector<VECTOR2I>& ring : p.outline )
            rings.push_back( pts( ring ) );

        jp.push_back( { { "id", p.id }, { "net", p.net }, { "layer", p.layer },
                        { "color", p.color }, { "outline", rings }, { "priority", p.priority },
                        { "reference", p.reference }, { "status", PlanStatusName( p.status ) },
                        { "locked", p.locked }, { "edited", p.edited }, { "author", p.author } } );
    }

    out["planes"] = jp;

    json js = json::array();

    for( const PLAN_SPLIT& s : splits )
        js.push_back( { { "id", s.id }, { "layer", s.layer }, { "path", pts( s.path ) },
                        { "gap", s.gap / NM_PER_MM } } );

    out["splits"] = js;

    json jn = json::array();

    for( const PLAN_NOTE& n : notes )
        jn.push_back( { { "id", n.id }, { "at", pt( n.at ) }, { "layer", n.layer },
                        { "text", n.text }, { "author", n.author }, { "target", n.target } } );

    out["notes"] = jn;

    json jk = json::array();

    for( const PLAN_CONSTRAINT& k : constraints )
        jk.push_back( { { "id", k.id }, { "kind", k.kind }, { "target", k.target },
                        { "ref", k.ref }, { "author", k.author }, { "text", k.text } } );

    out["constraints"] = jk;

    json jh = json::array();

    for( const PLAN_HISTORY& h : history )
        jh.push_back( { { "revision", h.revision }, { "actor", h.actor }, { "op", h.op },
                        { "items", h.items }, { "at", h.at } } );

    out["history"] = jh;
    return out;
}


BOARD_PLAN BOARD_PLAN::FromJson( const json& aJson )
{
    if( !aJson.is_object() )
        throw std::runtime_error( "a plan is a JSON object" );

    BOARD_PLAN plan;
    plan.format = get<int>( aJson, "format", FORMAT );

    if( plan.format > FORMAT )
        throw std::runtime_error( fmt::format( "plan format {} is newer than this KiCad ({})",
                                               plan.format, FORMAT ) );

    plan.revision = get<uint64_t>( aJson, "revision", 0 );
    plan.board = get<std::string>( aJson, "board", "" );

    for( const json& l : get<json>( aJson, "layers", json::array() ) )
    {
        PLAN_LAYER layer;
        layer.layer = l.at( "layer" ).get<std::string>();
        layer.role = get<std::string>( l, "role", "signal" );
        layer.status = PlanStatusFromName( get<std::string>( l, "status", "" ) );
        layer.note = get<std::string>( l, "note", "" );
        plan.layers.push_back( layer );
    }

    for( const json& g : get<json>( aJson, "groups", json::array() ) )
    {
        PLAN_GROUP group;
        group.id = g.at( "id" ).get<std::string>();
        group.name = get<std::string>( g, "name", group.id );
        group.color = get<std::string>( g, "color", "" );
        group.nets = strings( g, "nets" );

        for( const json& p : get<json>( g, "diff_pairs", json::array() ) )
            group.diffPairs.emplace_back( p.at( 0 ).get<std::string>(), p.at( 1 ).get<std::string>() );

        group.netclass = get<std::string>( g, "netclass", "" );
        group.maxSkew = get<double>( g, "max_skew", 0.0 );
        group.maxLength = get<double>( g, "max_length", 0.0 );
        group.status = PlanStatusFromName( get<std::string>( g, "status", "" ) );
        group.locked = get<bool>( g, "locked", false );
        plan.groups.push_back( group );
    }

    for( const json& c : get<json>( aJson, "corridors", json::array() ) )
    {
        PLAN_CORRIDOR corridor;
        corridor.id = c.at( "id" ).get<std::string>();
        corridor.group = get<std::string>( c, "group", "" );
        corridor.layer = c.at( "layer" ).get<std::string>();
        corridor.order = get<int>( c, "order", 0 );
        corridor.path = toPts( c.at( "path" ) );

        if( corridor.path.size() < 2 )
            throw std::runtime_error( "corridor " + corridor.id + " needs at least two points" );

        corridor.width = KiROUND( get<double>( c, "width", 1.0 ) * NM_PER_MM );
        corridor.from = strings( c, "from" );
        corridor.to = strings( c, "to" );
        corridor.vias = toPts( get<json>( c, "vias", json::array() ) );

        for( const json& e : get<json>( c, "escapes", json::array() ) )
            corridor.escapes.push_back( { toPt( e.at( "at" ) ), get<std::string>( e, "ref", "" ) } );

        corridor.status = PlanStatusFromName( get<std::string>( c, "status", "" ) );
        corridor.locked = get<bool>( c, "locked", false );
        corridor.author = get<std::string>( c, "author", "" );
        corridor.notes = strings( c, "notes" );
        if( auto it = c.find( "progress" ); it != c.end() && it->is_object() )
        {
            corridor.routed = get<int>( *it, "routed", 0 );
            corridor.total = get<int>( *it, "total", 0 );
            corridor.failed = get<int>( *it, "failed", 0 );
        }

        corridor.edited = get<bool>( c, "edited", false );
        plan.corridors.push_back( corridor );
    }

    for( const json& p : get<json>( aJson, "planes", json::array() ) )
    {
        PLAN_PLANE plane;
        plane.id = p.at( "id" ).get<std::string>();
        plane.net = get<std::string>( p, "net", "" );
        plane.layer = p.at( "layer" ).get<std::string>();
        plane.color = get<std::string>( p, "color", "" );

        for( const json& ring : p.at( "outline" ) )
            plane.outline.push_back( toPts( ring ) );

        if( plane.outline.empty() || plane.outline.front().size() < 3 )
            throw std::runtime_error( "plane " + plane.id + " needs an outline of 3+ points" );

        plane.priority = get<int>( p, "priority", 0 );
        plane.reference = get<std::string>( p, "reference", "" );
        plane.status = PlanStatusFromName( get<std::string>( p, "status", "" ) );
        plane.locked = get<bool>( p, "locked", false );
        plane.edited = get<bool>( p, "edited", false );
        plane.author = get<std::string>( p, "author", "" );
        plan.planes.push_back( plane );
    }

    for( const json& s : get<json>( aJson, "splits", json::array() ) )
    {
        PLAN_SPLIT split;
        split.id = s.at( "id" ).get<std::string>();
        split.layer = s.at( "layer" ).get<std::string>();
        split.path = toPts( s.at( "path" ) );
        split.gap = KiROUND( get<double>( s, "gap", 0.3 ) * NM_PER_MM );
        plan.splits.push_back( split );
    }

    for( const json& n : get<json>( aJson, "notes", json::array() ) )
    {
        PLAN_NOTE note;
        note.id = n.at( "id" ).get<std::string>();
        note.at = toPt( n.at( "at" ) );
        note.layer = get<std::string>( n, "layer", "*" );
        note.text = get<std::string>( n, "text", "" );
        note.author = get<std::string>( n, "author", "" );
        note.target = get<std::string>( n, "target", "" );
        plan.notes.push_back( note );
    }

    for( const json& k : get<json>( aJson, "constraints", json::array() ) )
    {
        PLAN_CONSTRAINT c;
        c.id = k.at( "id" ).get<std::string>();
        c.kind = get<std::string>( k, "kind", "free" );
        c.target = get<std::string>( k, "target", "" );
        c.ref = get<std::string>( k, "ref", "" );
        c.author = get<std::string>( k, "author", "" );
        c.text = get<std::string>( k, "text", "" );
        plan.constraints.push_back( c );
    }

    for( const json& h : get<json>( aJson, "history", json::array() ) )
    {
        PLAN_HISTORY entry;
        entry.revision = get<uint64_t>( h, "revision", 0 );
        entry.actor = get<std::string>( h, "actor", "" );
        entry.op = get<std::string>( h, "op", "" );
        entry.items = strings( h, "items" );
        entry.at = get<std::string>( h, "at", "" );
        plan.history.push_back( entry );
    }

    return plan;
}


wxString BOARD_PLAN::PathFor( const wxString& aBoardFile )
{
    wxFileName fn( aBoardFile );
    fn.SetExt( wxS( "kicad_plan" ) );
    return fn.GetFullPath();
}


BOARD_PLAN BOARD_PLAN::LoadFor( const wxString& aBoardFile )
{
    wxString path = PathFor( aBoardFile );

    if( aBoardFile.IsEmpty() || !wxFileExists( path ) )
        return BOARD_PLAN();

    wxFFile  file( path, wxS( "rb" ) );
    wxString text;

    if( !file.IsOpened() || !file.ReadAll( &text, wxConvUTF8 ) )
        throw std::runtime_error( "cannot read " + path.ToStdString( wxConvUTF8 ) );

    return FromJson( json::parse( text.ToStdString( wxConvUTF8 ) ) );
}


bool BOARD_PLAN::SaveFor( const wxString& aBoardFile ) const
{
    wxString path = PathFor( aBoardFile );

    if( !loadError.empty() || aBoardFile.IsEmpty() )
        return true;

    if( Empty() && !wxFileExists( path ) )
        return true;

    BOARD_PLAN copy = *this;
    copy.board = wxFileName( aBoardFile ).GetFullName().ToStdString( wxConvUTF8 );

    std::string text = copy.ToJson().dump( 2 ) + "\n";
    wxString    temp = path + wxS( ".tmp" );

    {
        wxFFile file( temp, wxS( "wb" ) );

        if( !file.IsOpened() || !file.Write( text.data(), text.size() ) )
            return false;
    }

    return wxRenameFile( temp, path, true );
}


PLAN_GROUP* BOARD_PLAN::FindGroup( const std::string& aId )
{
    for( PLAN_GROUP& g : groups )
        if( g.id == aId )
            return &g;

    return nullptr;
}


const PLAN_GROUP* BOARD_PLAN::FindGroup( const std::string& aId ) const
{
    for( const PLAN_GROUP& g : groups )
        if( g.id == aId )
            return &g;

    return nullptr;
}


PLAN_CORRIDOR* BOARD_PLAN::FindCorridor( const std::string& aId )
{
    for( PLAN_CORRIDOR& c : corridors )
        if( c.id == aId )
            return &c;

    return nullptr;
}


PLAN_PLANE* BOARD_PLAN::FindPlane( const std::string& aId )
{
    for( PLAN_PLANE& p : planes )
        if( p.id == aId )
            return &p;

    return nullptr;
}


PLAN_LAYER* BOARD_PLAN::FindLayer( const std::string& aLayer )
{
    for( PLAN_LAYER& l : layers )
        if( l.layer == aLayer )
            return &l;

    return nullptr;
}


std::string BOARD_PLAN::NewId( const std::string& aPrefix ) const
{
    std::set<std::string> used;

    for( const PLAN_GROUP& x : groups )           used.insert( x.id );
    for( const PLAN_CORRIDOR& x : corridors )     used.insert( x.id );
    for( const PLAN_PLANE& x : planes )           used.insert( x.id );
    for( const PLAN_SPLIT& x : splits )           used.insert( x.id );
    for( const PLAN_NOTE& x : notes )             used.insert( x.id );
    for( const PLAN_CONSTRAINT& x : constraints ) used.insert( x.id );

    for( int n = 1;; ++n )
    {
        std::string id = aPrefix + "-" + std::to_string( n );

        if( !used.count( id ) )
            return id;
    }
}


void BOARD_PLAN::Note( const std::string& aActor, const std::string& aOp,
                       const std::vector<std::string>& aItems )
{
    ++revision;
    history.push_back( { revision, aActor.empty() ? "person" : aActor, aOp, aItems, nowIso() } );

    while( history.size() > HISTORY_CAPACITY )
        history.erase( history.begin() );
}


PLAN_STATUS BOARD_PLAN::LayerStatus( const std::string& aLayer ) const
{
    for( const PLAN_LAYER& l : layers )
        if( l.layer == aLayer )
            return l.status;

    return PLAN_STATUS::PROPOSED;
}


bool BOARD_PLAN::Approved( const PLAN_CORRIDOR& aCorridor ) const
{
    if( aCorridor.status != PLAN_STATUS::APPROVED )
        return false;

    if( const PLAN_GROUP* g = FindGroup( aCorridor.group ) )
    {
        // A group is approved when it is approved itself, or it was never judged as a whole
        // and its corridor is (approving a corridor approves it for its group).
        if( g->status == PLAN_STATUS::REJECTED )
            return false;
    }

    return LayerStatus( aCorridor.layer ) != PLAN_STATUS::REJECTED
           && ( LayerStatus( aCorridor.layer ) == PLAN_STATUS::APPROVED
                || aCorridor.status == PLAN_STATUS::APPROVED );
}


bool BOARD_PLAN::Approved( const PLAN_PLANE& aPlane ) const
{
    return aPlane.status == PLAN_STATUS::APPROVED
           && LayerStatus( aPlane.layer ) != PLAN_STATUS::REJECTED;
}


void BOARD_PLAN::SetLayerStatus( const std::string& aLayer, PLAN_STATUS aStatus )
{
    PLAN_LAYER* l = FindLayer( aLayer );

    if( !l )
    {
        layers.push_back( { aLayer, "signal", aStatus, "" } );
        l = &layers.back();
    }

    l->status = aStatus;

    auto judgeable = []( PLAN_STATUS s )
    {
        return s == PLAN_STATUS::PROPOSED || s == PLAN_STATUS::APPROVED || s == PLAN_STATUS::REJECTED;
    };

    for( PLAN_CORRIDOR& c : corridors )
        if( c.layer == aLayer && judgeable( c.status ) )
            c.status = aStatus;

    for( PLAN_PLANE& p : planes )
        if( p.layer == aLayer && judgeable( p.status ) )
            p.status = aStatus;
}


bool BOARD_PLAN::SetItemStatus( const std::string& aId, PLAN_STATUS aStatus )
{
    if( PLAN_CORRIDOR* c = FindCorridor( aId ) )
    {
        c->status = aStatus;
        return true;
    }

    if( PLAN_PLANE* p = FindPlane( aId ) )
    {
        p->status = aStatus;
        return true;
    }

    return false;
}


bool BOARD_PLAN::PersonChangedSince( const std::string& aId, uint64_t aBase ) const
{
    for( const PLAN_HISTORY& h : history )
    {
        if( h.revision > aBase && h.actor == "person"
            && std::find( h.items.begin(), h.items.end(), aId ) != h.items.end() )
        {
            return true;
        }
    }

    return false;
}


bool BOARD_PLAN::operator==( const BOARD_PLAN& aOther ) const
{
    return ToJson() == aOther.ToJson();
}


// --- audit ------------------------------------------------------------------------------------

namespace
{
double polylineLength( const std::vector<VECTOR2I>& aPath )
{
    double length = 0;

    for( size_t ii = 1; ii < aPath.size(); ++ii )
        length += ( aPath[ii] - aPath[ii - 1] ).EuclideanNorm();

    return length;
}

std::vector<SEG> segs( const std::vector<VECTOR2I>& aPath )
{
    std::vector<SEG> out;

    for( size_t ii = 1; ii < aPath.size(); ++ii )
        out.emplace_back( aPath[ii - 1], aPath[ii] );

    return out;
}

std::string mm( double aNm, int aDigits = 1 )
{
    return fmt::format( "{:.{}f} mm", aNm / NM_PER_MM, aDigits );
}
} // namespace


PLAN_AUDIT PLAN_AUDIT::Run( const BOARD_PLAN& aPlan, const BOARD& aBoard )
{
    PLAN_AUDIT audit;

    // Crossings between corridors on the same layer.
    for( size_t ii = 0; ii < aPlan.corridors.size(); ++ii )
    {
        const PLAN_CORRIDOR& a = aPlan.corridors[ii];

        if( a.status == PLAN_STATUS::REJECTED )
            continue;

        for( size_t jj = ii + 1; jj < aPlan.corridors.size(); ++jj )
        {
            const PLAN_CORRIDOR& b = aPlan.corridors[jj];

            if( b.status == PLAN_STATUS::REJECTED || b.layer != a.layer )
                continue;

            bool found = false;

            for( const SEG& sa : segs( a.path ) )
            {
                for( const SEG& sb : segs( b.path ) )
                {
                    if( OPT_VECTOR2I hit = sa.Intersect( sb ) )
                    {
                        audit.warnings.push_back(
                                { "crossing", a.layer, { a.id, b.id },
                                  fmt::format( "{} crosses {} on {}", a.id, b.id, a.layer ),
                                  *hit } );
                        found = true;
                        break;
                    }
                }

                if( found )
                    break;
            }
        }
    }

    // Congestion: a corridor narrower than its nets need at the default net class.
    const BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
    int trackPitch = bds.m_TrackMinWidth > 0 ? bds.m_TrackMinWidth : 150000;
    int clearance = bds.m_MinClearance > 0 ? bds.m_MinClearance : 150000;

    if( std::shared_ptr<NETCLASS> def = bds.m_NetSettings->GetDefaultNetclass() )
    {
        if( def->HasTrackWidth() )
            trackPitch = def->GetTrackWidth();

        if( def->HasClearance() )
            clearance = def->GetClearance();
    }

    for( const PLAN_CORRIDOR& c : aPlan.corridors )
    {
        const PLAN_GROUP* g = aPlan.FindGroup( c.group );

        if( !g || g->nets.empty() || c.status == PLAN_STATUS::REJECTED )
            continue;

        double need = (double) g->nets.size() * ( trackPitch + clearance );

        if( need > c.width * 1.02 && c.path.size() >= 2 )
        {
            audit.warnings.push_back(
                    { "congestion", c.layer, { c.id },
                      fmt::format( "{}: {} nets need about {}, the corridor is {}", c.id,
                                   g->nets.size(), mm( need ), mm( c.width ) ),
                      c.path[c.path.size() / 2] } );
        }
    }

    // A corridor crossing a split on any power layer of the plan.
    for( const PLAN_SPLIT& s : aPlan.splits )
    {
        for( const PLAN_CORRIDOR& c : aPlan.corridors )
        {
            if( c.status == PLAN_STATUS::REJECTED )
                continue;

            for( const SEG& sa : segs( c.path ) )
            {
                bool hit = false;

                for( const SEG& sb : segs( s.path ) )
                {
                    if( OPT_VECTOR2I p = sa.Intersect( sb ) )
                    {
                        audit.warnings.push_back(
                                { "split", c.layer, { c.id, s.id },
                                  fmt::format( "{} crosses the plane split {} on {}", c.id, s.id,
                                               s.layer ),
                                  *p } );
                        hit = true;
                        break;
                    }
                }

                if( hit )
                    break;
            }
        }
    }

    // Nets the board does not have, and length / skew estimates per bus.
    std::map<std::string, std::vector<const PAD*>> padsByRefNet;

    for( const FOOTPRINT* fp : aBoard.Footprints() )
    {
        for( const PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 )
            {
                padsByRefNet[fp->GetReference().ToStdString() + "\n"
                             + pad->GetNetname().ToStdString()].push_back( pad );
            }
        }
    }

    for( const PLAN_GROUP& g : aPlan.groups )
    {
        std::vector<std::string> missing;

        for( const std::string& net : g.nets )
        {
            if( !aBoard.FindNet( wxString::FromUTF8( net ) ) )
                missing.push_back( net );
        }

        if( !missing.empty() )
        {
            audit.warnings.push_back(
                    { "missing_net", "", { g.id },
                      fmt::format( "{}: {} net(s) not on the board ({}{})", g.name,
                                   missing.size(), missing.front(),
                                   missing.size() > 1 ? ", ..." : "" ),
                      VECTOR2I() } );
        }

        BUS bus;
        bus.group = g.id;
        bus.nets = (int) g.nets.size();
        bool first = true;

        for( const PLAN_CORRIDOR& c : aPlan.corridors )
        {
            if( c.group != g.id || c.path.size() < 2 )
                continue;

            for( const std::string& net : g.nets )
            {
                double length = polylineLength( c.path );

                auto nearest = [&]( const std::vector<std::string>& aRefs, const VECTOR2I& aEnd )
                {
                    double best = -1;

                    for( const std::string& ref : aRefs )
                    {
                        auto it = padsByRefNet.find( ref + "\n" + net );

                        if( it == padsByRefNet.end() )
                            continue;

                        for( const PAD* pad : it->second )
                        {
                            double d = ( pad->GetPosition() - aEnd ).EuclideanNorm();

                            if( best < 0 || d < best )
                                best = d;
                        }
                    }

                    return std::max( 0.0, best );
                };

                length += nearest( c.from, c.path.front() ) + nearest( c.to, c.path.back() );

                if( first )
                {
                    bus.minLength = bus.maxLength = length;
                    first = false;
                }
                else
                {
                    bus.minLength = std::min( bus.minLength, length );
                    bus.maxLength = std::max( bus.maxLength, length );
                }
            }
        }

        bus.skew = bus.maxLength - bus.minLength;

        if( !first )
            audit.buses.push_back( bus );
    }

    return audit;
}


json PLAN_AUDIT::ToJson() const
{
    json w = json::array();

    for( const WARNING& x : warnings )
        w.push_back( { { "kind", x.kind }, { "layer", x.layer }, { "items", x.items },
                       { "text", x.text }, { "at", pt( x.at ) } } );

    json b = json::array();

    for( const BUS& x : buses )
        b.push_back( { { "group", x.group }, { "nets", x.nets },
                       { "min_length", x.minLength / NM_PER_MM },
                       { "max_length", x.maxLength / NM_PER_MM },
                       { "skew", x.skew / NM_PER_MM } } );

    return { { "warnings", w }, { "buses", b } };
}


// --- stack-up text ----------------------------------------------------------------------------

namespace
{
std::string um( int aNm )
{
    return fmt::format( "{} \xC2\xB5m", KiROUND( aNm / 1000.0 ) );
}
} // namespace


std::string PlanLayerChip( const BOARD& aBoard, const std::string& aLayer )
{
    PCB_LAYER_ID id = aBoard.GetLayerID( wxString::FromUTF8( aLayer ) );

    if( id == UNDEFINED_LAYER )
        return aLayer;

    int index = 1;

    for( PCB_LAYER_ID layer : aBoard.GetEnabledLayers().CuStack() )
    {
        if( layer == id )
            break;

        ++index;
    }

    return fmt::format( "L{} \xC2\xB7 {}", index, aBoard.GetLayerName( id ).ToStdString( wxConvUTF8 ) );
}


std::string DescribePlanLayer( const BOARD& aBoard, const std::string& aLayer )
{
    PCB_LAYER_ID id = aBoard.GetLayerID( wxString::FromUTF8( aLayer ) );

    if( id == UNDEFINED_LAYER || !IsCopperLayer( id ) )
        return "";

    const BOARD_STACKUP& stackup =
            const_cast<BOARD&>( aBoard ).GetDesignSettings().GetStackupDescriptor();
    const std::vector<BOARD_STACKUP_ITEM*>& items = stackup.GetList();

    int at = -1;

    for( int ii = 0; ii < (int) items.size(); ++ii )
    {
        if( items[ii]->GetType() == BS_ITEM_TYPE_COPPER && items[ii]->GetBrdLayerId() == id )
            at = ii;
    }

    if( at < 0 )
        return "";

    struct SIDE
    {
        std::string  name;
        std::string  dielectric;   ///< "50 µm core"
        int          thickness = 0;
        PCB_LAYER_ID layer = UNDEFINED_LAYER;
        bool         plane = false;
    };

    auto side = [&]( int aStep ) -> std::optional<SIDE>
    {
        SIDE s;

        for( int ii = at + aStep; ii >= 0 && ii < (int) items.size(); ii += aStep )
        {
            BOARD_STACKUP_ITEM* item = items[ii];

            if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC )
            {
                for( int sub = 0; sub < item->GetSublayersCount(); ++sub )
                    s.thickness += item->GetThickness( sub );

                std::string kind = item->GetTypeName().Lower().ToStdString( wxConvUTF8 );
                s.dielectric = um( s.thickness ) + ( kind.empty() ? "" : " " + kind );
            }
            else if( item->GetType() == BS_ITEM_TYPE_COPPER )
            {
                s.layer = item->GetBrdLayerId();
                s.name = aBoard.GetLayerName( s.layer ).ToStdString( wxConvUTF8 );
                LAYER_T type = aBoard.GetLayerType( s.layer );
                std::string upper = s.name;
                std::transform( upper.begin(), upper.end(), upper.begin(), ::toupper );
                s.plane = type == LT_POWER || upper.find( "GND" ) != std::string::npos
                          || upper.find( "PWR" ) != std::string::npos
                          || upper.find( "VCC" ) != std::string::npos;
                return s;
            }
        }

        return std::nullopt;
    };

    std::optional<SIDE> above = side( -1 );
    std::optional<SIDE> below = side( +1 );

    LAYER_T type = aBoard.GetLayerType( id );

    if( type == LT_POWER )
    {
        // A plane faces its nearest neighbour across the thinner dielectric.
        const std::optional<SIDE>& n =
                ( above && below ) ? ( above->thickness <= below->thickness ? above : below )
                                   : ( above ? above : below );

        if( !n )
            return "Plane layer";

        return fmt::format( "Faces {} across {}", n->name, um( n->thickness ) );
    }

    if( above && below && above->plane && below->plane )
    {
        return fmt::format( "Stripline between {} ({}) and {} ({})", above->name,
                            above->dielectric, below->name, below->dielectric );
    }

    const std::optional<SIDE>& ref = ( above && above->plane ) ? above
                                    : ( below && below->plane ) ? below
                                                               : ( above ? above : below );

    if( !ref )
        return "";

    bool outer = id == F_Cu || id == B_Cu;
    return fmt::format( "{} {} {} across {}", outer ? "Microstrip" : "Signal",
                        ref->plane ? "over" : "next to", ref->name, um( ref->thickness ) );
}


// --- progress -------------------------------------------------------------------------------

PLAN_PROGRESS PLAN_PROGRESS::Run( const BOARD_PLAN& aPlan, const BOARD& aBoard,
                                  const PLAN_AUDIT& aAudit )
{
    PLAN_PROGRESS out;

    // Nets with an unrouted connection, and per part the connected pins still unrouted.
    std::shared_ptr<CONNECTIVITY_DATA> conn = aBoard.GetConnectivity();
    std::set<int>                      unroutedNets;

    if( conn )
    {
        for( FOOTPRINT* fp : aBoard.Footprints() )
        {
            PART& part = out.parts[fp->GetReference().ToStdString( wxConvUTF8 )];

            for( PAD* pad : fp->Pads() )
            {
                if( pad->GetNetCode() <= 0 )
                    continue;

                // A pad alone on its net has nothing to route.
                if( conn->GetNetItems( pad->GetNetCode(), { PCB_PAD_T } ).size() < 2 )
                    continue;

                part.pins++;

                if( conn->GetRatsnestForPad( pad ).size() > 0 )
                {
                    unroutedNets.insert( pad->GetNetCode() );
                }
                else
                {
                    part.routed++;
                }
            }
        }
    }

    // DRC markers by net name.
    std::map<std::string, int> markersByNet;

    for( PCB_MARKER* marker : aBoard.Markers() )
    {
        std::shared_ptr<RC_ITEM> item = marker->GetRCItem();

        if( !item )
            continue;

        for( const KIID& id : { item->GetMainItemID(), item->GetAuxItemID() } )
        {
            if( BOARD_ITEM* bi = aBoard.ResolveItem( id, true ) )
            {
                if( BOARD_CONNECTED_ITEM* ci = dynamic_cast<BOARD_CONNECTED_ITEM*>( bi ) )
                    markersByNet[ci->GetNetname().ToStdString( wxConvUTF8 )]++;
            }
        }
    }

    auto stateOf = [&]( PLAN_STATUS aStatus, bool aEdited ) -> std::string
    {
        if( aStatus == PLAN_STATUS::PROPOSED && aEdited )
            return "edited";

        return PlanStatusName( aStatus );
    };

    for( const PLAN_CORRIDOR& c : aPlan.corridors )
    {
        ITEM item;
        item.state = stateOf( c.status, c.edited );
        item.routed = c.routed;
        item.total = c.total;

        // Once copper exists, DRC markers on the group's nets and unrouted nets are problems.
        if( const PLAN_GROUP* g = aPlan.FindGroup( c.group );
            g && ( c.status == PLAN_STATUS::ROUTED || c.status == PLAN_STATUS::VERIFIED
                   || c.status == PLAN_STATUS::ROUTING ) )
        {
            for( const std::string& net : g->nets )
            {
                if( auto it = markersByNet.find( net ); it != markersByNet.end() )
                    item.drc += it->second;

                if( NETINFO_ITEM* ni = aBoard.FindNet( wxString::FromUTF8( net ) ) )
                {
                    if( unroutedNets.count( ni->GetNetCode() ) && c.status != PLAN_STATUS::ROUTING )
                        item.unrouted++;
                }
            }
        }

        for( size_t ii = 0; ii < aAudit.warnings.size(); ++ii )
        {
            const auto& ids = aAudit.warnings[ii].items;

            if( std::find( ids.begin(), ids.end(), c.id ) != ids.end()
                || std::find( ids.begin(), ids.end(), c.group ) != ids.end() )
            {
                item.problems.push_back( (int) ii );
            }
        }

        out.items[c.id] = item;

        LAYER& layer = out.layers[c.layer];
        layer.groups++;

        if( c.status == PLAN_STATUS::ROUTED || c.status == PLAN_STATUS::VERIFIED )
            layer.done++;
    }

    for( const PLAN_PLANE& p : aPlan.planes )
    {
        ITEM item;
        item.state = stateOf( p.status, p.edited );

        for( size_t ii = 0; ii < aAudit.warnings.size(); ++ii )
        {
            const auto& ids = aAudit.warnings[ii].items;

            if( std::find( ids.begin(), ids.end(), p.id ) != ids.end() )
                item.problems.push_back( (int) ii );
        }

        out.items[p.id] = item;
    }

    return out;
}


nlohmann::json PLAN_PROGRESS::ToJson() const
{
    json items = json::object();

    for( const auto& [id, item] : this->items )
        items[id] = { { "state", item.state }, { "routed", item.routed }, { "total", item.total },
                      { "problems", item.problems }, { "drc", item.drc },
                      { "unrouted", item.unrouted },
                      { "problem_count", item.ProblemCount() } };

    json jl = json::object();

    for( const auto& [layer, l] : layers )
        jl[layer] = { { "groups", l.groups }, { "done", l.done } };

    json jp = json::object();

    for( const auto& [ref, p] : parts )
        jp[ref] = { { "pins", p.pins }, { "routed", p.routed } };

    return { { "items", items }, { "layers", jl }, { "parts", jp } };
}
