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
 * Field-level three-way merge of API item messages, for co-editing
 * (docs/backplane/coedit.md): rebases an agent's update of an item onto the person's edit
 * of the same item.
 */

#ifndef PROTO_MERGE_H
#define PROTO_MERGE_H

#include <kicommon.h>
#include <google/protobuf/any.pb.h>
#include <math/vector2d.h>

#include <optional>
#include <string>
#include <vector>

namespace PROTO_MERGE
{

/// The item's KIID (its `id.value` field), if the message has one.
KICOMMON_API std::optional<std::string> ItemId( const google::protobuf::Any& aItem );

/// The item's "position" (a Vector2 in nm), if it has one.
KICOMMON_API std::optional<VECTOR2I> Position( const google::protobuf::Any& aItem );

struct RESULT
{
    google::protobuf::Any    merged;
    std::vector<std::string> fromAgent;       ///< field paths the agent's value was taken for
    std::vector<std::string> keptFromPerson;  ///< both changed: the person's value was kept
};

/**
 * Merge an agent's version of an item onto the person's, both derived from aBase.  Fields the
 * agent did not change keep the person's value; fields only the agent changed take the
 * agent's; fields both changed descend into composite messages, and otherwise keep the
 * person's (the person's in-progress edit wins), or the agent's with aAgentWins (the person
 * chose theirs).  Messages with only scalar fields (points,
 * distances, ids) and oneofs are single values.  Fails when the types differ.
 */
KICOMMON_API std::optional<RESULT> ThreeWay( const google::protobuf::Any& aBase,
                                             const google::protobuf::Any& aPerson,
                                             const google::protobuf::Any& aAgent,
                                             bool aAgentWins = false );

} // namespace PROTO_MERGE

#endif
