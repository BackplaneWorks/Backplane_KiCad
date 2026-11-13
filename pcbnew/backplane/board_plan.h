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

/*
 * The routing plan (docs/backplane/floorplanner.md): corridors for buses and net groups on
 * signal layers, plane regions on power layers, splits, notes and the person's constraints.
 * Stored beside the board as <board>.kicad_plan (JSON, millimetres); internal units here.
 */

#pragma once

#include <math/vector2d.h>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

class BOARD;
class wxString;


enum class PLAN_STATUS
{
    PROPOSED,
    APPROVED,
    REJECTED,
    ROUTING,
    ROUTED,
    VERIFIED,
    FAILED
};

std::string PlanStatusName( PLAN_STATUS aStatus );
PLAN_STATUS PlanStatusFromName( const std::string& aName );


struct PLAN_LAYER
{
    std::string layer;              ///< canonical KiCad name, "In6.Cu"
    std::string role = "signal";    ///< "signal" or "power"
    PLAN_STATUS status = PLAN_STATUS::PROPOSED;
    std::string note;
};


struct PLAN_GROUP
{
    std::string              id;
    std::string              name;
    std::string              color;      ///< "#rrggbb"
    std::vector<std::string> nets;
    std::vector<std::pair<std::string, std::string>> diffPairs;
    std::string              netclass;
    double                   maxSkew = 0;     ///< mm; 0 = no target
    double                   maxLength = 0;   ///< mm; 0 = no target
    PLAN_STATUS              status = PLAN_STATUS::PROPOSED;
    bool                     locked = false;
};


struct PLAN_ESCAPE
{
    VECTOR2I    at;
    std::string ref;
};


struct PLAN_CORRIDOR
{
    std::string              id;
    std::string              group;
    std::string              layer;
    int                      order = 0;
    std::vector<VECTOR2I>    path;
    int                      width = 0;       ///< nm
    std::vector<std::string> from;
    std::vector<std::string> to;
    std::vector<VECTOR2I>    vias;
    std::vector<PLAN_ESCAPE> escapes;
    PLAN_STATUS              status = PLAN_STATUS::PROPOSED;
    bool                     locked = false;
    std::string              author;
    std::vector<std::string> notes;
    int                      routed = 0;      ///< connections routed so far (progress)
    int                      total = 0;       ///< connections to route (0 = not started)
    int                      failed = 0;
    bool                     edited = false;  ///< the person changed it since it was proposed

    double Length() const;                    ///< centreline length, nm
};


struct PLAN_PLANE
{
    std::string                        id;
    std::string                        net;
    std::string                        layer;
    std::string                        color;
    std::vector<std::vector<VECTOR2I>> outline;   ///< first = outline, others = holes
    int                                priority = 0;
    std::string                        reference;  ///< the adjacent reference plane layer
    PLAN_STATUS                        status = PLAN_STATUS::PROPOSED;
    bool                               locked = false;
    bool                               edited = false;
    std::string                        author;
};


struct PLAN_SPLIT
{
    std::string           id;
    std::string           layer;
    std::vector<VECTOR2I> path;
    int                   gap = 300000;
};


struct PLAN_NOTE
{
    std::string id;
    VECTOR2I    at;
    std::string layer = "*";
    std::string text;
    std::string author;
    std::string target;
};


struct PLAN_CONSTRAINT
{
    std::string id;
    std::string kind;     ///< "avoid", "under", "keep", "layer", "order", "width", "free"
    std::string target;
    std::string ref;
    std::string author;
    std::string text;
};


struct PLAN_HISTORY
{
    uint64_t                 revision = 0;
    std::string              actor;
    std::string              op;
    std::vector<std::string> items;
    std::string              at;
};


class BOARD_PLAN
{
public:
    static constexpr int FORMAT = 1;
    static constexpr size_t HISTORY_CAPACITY = 500;

    int                          format = FORMAT;
    uint64_t                     revision = 0;
    std::string                  board;
    std::vector<PLAN_LAYER>      layers;
    std::vector<PLAN_GROUP>      groups;
    std::vector<PLAN_CORRIDOR>   corridors;
    std::vector<PLAN_PLANE>      planes;
    std::vector<PLAN_SPLIT>      splits;
    std::vector<PLAN_NOTE>       notes;
    std::vector<PLAN_CONSTRAINT> constraints;
    std::vector<PLAN_HISTORY>    history;

    /// Not stored: set when the file beside the board could not be parsed.  Such a plan is
    /// never written back, so an unreadable file is not replaced by an empty one.
    std::string                  loadError;

    bool Empty() const;

    nlohmann::json ToJson() const;

    /// Parses aJson; throws std::exception (with a message) on malformed input.
    static BOARD_PLAN FromJson( const nlohmann::json& aJson );

    /// The plan file beside a board file: "x.kicad_pcb" -> "x.kicad_plan".
    static wxString PathFor( const wxString& aBoardFile );

    /// Loads the plan beside aBoardFile; an empty plan when there is none.  Throws on a file
    /// that exists but cannot be parsed.
    static BOARD_PLAN LoadFor( const wxString& aBoardFile );

    /// Writes the plan beside aBoardFile (atomically).  An empty plan is written only when a
    /// plan file is already there (so clearing a plan persists), and a plan that failed to
    /// load is never written.  Returns false on an IO error.
    bool SaveFor( const wxString& aBoardFile ) const;

    PLAN_GROUP*      FindGroup( const std::string& aId );
    PLAN_CORRIDOR*   FindCorridor( const std::string& aId );
    PLAN_PLANE*      FindPlane( const std::string& aId );
    PLAN_LAYER*      FindLayer( const std::string& aLayer );
    const PLAN_GROUP* FindGroup( const std::string& aId ) const;

    /// A fresh id with this prefix ("c", "g", "p", "s", "n", "k").
    std::string NewId( const std::string& aPrefix ) const;

    /// Records an operation in the history and bumps the revision.
    void Note( const std::string& aActor, const std::string& aOp,
               const std::vector<std::string>& aItems );

    /// The layer's status, or PROPOSED if the layer has no entry.
    PLAN_STATUS LayerStatus( const std::string& aLayer ) const;

    /// True when the item and its group and layer are all approved: routing may use it.
    bool Approved( const PLAN_CORRIDOR& aCorridor ) const;
    bool Approved( const PLAN_PLANE& aPlane ) const;

    /// Approve or reject a layer: its status and every item on it not yet routing.
    void SetLayerStatus( const std::string& aLayer, PLAN_STATUS aStatus );

    /// Set an item's status (corridor or plane); false if there is no such item.
    bool SetItemStatus( const std::string& aId, PLAN_STATUS aStatus );

    /// True when the person changed aId after revision aBase (from the history).
    bool PersonChangedSince( const std::string& aId, uint64_t aBase ) const;

    bool operator==( const BOARD_PLAN& aOther ) const;
};


/// The derived audit of a plan against its board (not stored).
struct PLAN_AUDIT
{
    struct WARNING
    {
        std::string kind;          ///< "crossing", "congestion", "split", "missing_net", "unrouted"
        std::string layer;
        std::vector<std::string> items;
        std::string text;
        VECTOR2I    at;
    };

    struct BUS
    {
        std::string group;
        int         nets = 0;
        double      minLength = 0;   ///< nm, estimate
        double      maxLength = 0;
        double      skew = 0;        ///< max - min, nm
    };

    std::vector<WARNING> warnings;
    std::vector<BUS>     buses;

    static PLAN_AUDIT Run( const BOARD_PLAN& aPlan, const BOARD& aBoard );

    nlohmann::json ToJson() const;
};


/// Live progress of a plan against its board (not stored): what the overlay, the panel,
/// GetPlan and Backplane's viewer show.
struct PLAN_PROGRESS
{
    struct ITEM
    {
        std::string              state;      ///< proposed, edited, approved, routing, routed,
                                             ///< verified, rejected, failed
        int                      routed = 0;
        int                      total = 0;
        std::vector<int>         problems;   ///< indices into audit.warnings
        int                      drc = 0;      ///< DRC markers on the group's nets
        int                      unrouted = 0; ///< group nets still unrouted after routing

        int ProblemCount() const { return (int) problems.size() + drc + unrouted; }
    };

    struct LAYER
    {
        int groups = 0;
        int done = 0;                        ///< routed or verified
    };

    struct PART
    {
        int pins = 0;                        ///< connected pins
        int routed = 0;                      ///< of those, with no unrouted connection
    };

    std::map<std::string, ITEM>  items;      ///< by corridor / plane id
    std::map<std::string, LAYER> layers;
    std::map<std::string, PART>  parts;      ///< by reference

    static PLAN_PROGRESS Run( const BOARD_PLAN& aPlan, const BOARD& aBoard,
                              const PLAN_AUDIT& aAudit );

    nlohmann::json ToJson() const;
};


/// "Stripline between GND2 (50 µm core) and GND3 (30 µm)" and the like, from the stack-up.
std::string DescribePlanLayer( const BOARD& aBoard, const std::string& aLayer,
                               const BOARD_PLAN* aPlan = nullptr );

/// "L6 · SIG1": the copper index (1-based, top = 1) and the user name.
std::string PlanLayerChip( const BOARD& aBoard, const std::string& aLayer );
