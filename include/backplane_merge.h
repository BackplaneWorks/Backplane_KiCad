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
 * Three-way merges for the documents beside a design that two writers edit at once
 * (docs/backplane/coedit.md, "Rules and settings"): the project file (.kicad_pro: net classes,
 * board setup, rule severities) per key and per net class, and the custom rules file
 * (.kicad_dru) per rule.  Never a whole-file overwrite.
 */

#ifndef BACKPLANE_MERGE_H
#define BACKPLANE_MERGE_H

#include <kicommon.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace BACKPLANE_MERGE
{

/// One place where both sides changed the same value differently.
struct CONFLICT
{
    std::string    path;     ///< "/net_settings/classes/[name=HS]/clearance", "rule DDR skew"
    nlohmann::json base;
    nlohmann::json ours;
    nlohmann::json theirs;
};

/**
 * Merge two edits of a JSON document made from aBase.  Objects merge per key; arrays of
 * objects that all carry "name" (net classes, presets) merge per name; anything else is one
 * value.  A value changed on one side only takes that side; changed on both sides
 * differently is a conflict: aOurs is kept and the conflict is reported.
 */
KICOMMON_API nlohmann::json MergeJson( const nlohmann::json& aBase, const nlohmann::json& aOurs,
                                       const nlohmann::json& aTheirs,
                                       std::vector<CONFLICT>& aConflicts,
                                       const std::string& aPath = "" );

/// A custom rules file split into its parts: the version header, then each top-level form
/// keyed by "rule <name>" (or its own text for anything else: comments stay attached to the
/// form that follows them).
struct RULES_PART
{
    std::string key;
    std::string text;
};

KICOMMON_API std::vector<RULES_PART> SplitRules( const std::string& aText );

/// Merge two edits of a .kicad_dru made from aBase, per rule (same rules as MergeJson; the
/// conflict's ours/theirs hold the two rule texts).
KICOMMON_API std::string MergeRules( const std::string& aBase, const std::string& aOurs,
                                     const std::string& aTheirs,
                                     std::vector<CONFLICT>& aConflicts );

/// Insert or replace rules by name in a rules file, and remove rules by name, leaving every
/// other rule, comment and the header as they are.
KICOMMON_API std::string EditRules( const std::string& aText,
                                    const std::vector<std::string>& aUpsertRuleTexts,
                                    const std::vector<std::string>& aRemoveNames );

/// The name of a "(rule <name> ...)" form, or "".
KICOMMON_API std::string RuleName( const std::string& aForm );

} // namespace BACKPLANE_MERGE

#endif
