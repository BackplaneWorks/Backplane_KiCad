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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Default hotkeys the Backplane fork adds (fork-only).  People can rebind them in
 * Preferences > Hotkeys like any other.
 */

#ifndef BACKPLANE_HOTKEYS_H_
#define BACKPLANE_HOTKEYS_H_

#include <tool/tool_event.h>

/// Align the selection on a common X centre (a vertical line).  PCB and schematic editors.
constexpr int BACKPLANE_HK_ALIGN_VERTICAL = MD_ALT + 'V';

/// Align the selection on a common Y centre (a horizontal line).  PCB and schematic editors.
constexpr int BACKPLANE_HK_ALIGN_HORIZONTAL = MD_ALT + 'H';

#endif // BACKPLANE_HOTKEYS_H_
