/*
 * This program source code file is part of KiCad, a free EDA application.
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

#pragma once

#include <array>

namespace BACKPLANE
{

inline constexpr std::array<const char*, 21> CAPABILITIES = {
    "api-server",
    "headless-pcb",
    "headless-sch",
    "export-v10",
    "parent-pid",
    "capabilities",
    "sketch-route",
    "board-structure",
    "assembly",
    "idle-exit",
    "shutdown",
    "api-descriptor",
    "symbol-from-libid",
    "footprint-partial-update",
    "coedit",          // docs/backplane/coedit.md: live co-editing, per-actor undo, deferral
    "rules-merge",     // project settings and custom rules merge, never overwrite
    "routing-plan",    // docs/backplane/floorplanner.md: kiapi.backplane.plan
    "comment-key-63",  // embedded: the comment action sends KEY_ATOM index 63; text fields keep ';'
    "drc-zone-guard",  // DRC checks a zone against its outline when its fill triangulation is bad
    "rules-insert-position", // EditCustomRules.insert: new rules first, last or before a named rule
    "plan-keys",       // in plan view the plan tool's keys (A, R, W, L...) are its own, never KiCad's defaults
};

} // namespace BACKPLANE
