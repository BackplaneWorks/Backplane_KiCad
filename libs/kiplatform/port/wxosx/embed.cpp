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

#include <kiplatform/embed.h>

// macOS cannot parent an NSView across processes; embedding is not supported (a host keeps
// KiCad as a separate window over the same IPC).

bool KIPLATFORM::EMBED::Supported() { return false; }
void KIPLATFORM::EMBED::Install( const HOOKS& ) {}
void KIPLATFORM::EMBED::TraceLine( const std::string& )
{
}


uint64_t KIPLATFORM::EMBED::CreateHolder( uint64_t ) { return 0; }
bool KIPLATFORM::EMBED::AttachHolder( uint64_t, uint64_t ) { return false; }
uint64_t KIPLATFORM::EMBED::PrepareFrame( wxWindow*, uint64_t ) { return 0; }
uint64_t KIPLATFORM::EMBED::WindowId( wxWindow* ) { return 0; }
bool KIPLATFORM::EMBED::HasFocus( wxWindow* ) { return false; }
void KIPLATFORM::EMBED::TakeFocus( wxWindow* ) {}
void KIPLATFORM::EMBED::SendHostMessage( uint64_t, const char*, int, const long[5] ) {}
void KIPLATFORM::EMBED::SetFrameGeometry( wxWindow*, int, int, int, int ) {}
bool KIPLATFORM::EMBED::SetPreferDark( int ) { return false; }
void KIPLATFORM::EMBED::InstallLoopTimer( std::function<void( double, double )> ) {}
bool KIPLATFORM::EMBED::IsMapped( wxWindow* aFrame ) { return aFrame && aFrame->IsShown(); }
void KIPLATFORM::EMBED::EnsureMapped( wxWindow* ) {}
void KIPLATFORM::EMBED::UnmapNative( wxWindow* aFrame ) { if( aFrame ) aFrame->Show( false ); }
bool KIPLATFORM::EMBED::NativeModalTitle( wxString& ) { return false; }
void KIPLATFORM::EMBED::MakeTransientFor( wxWindow*, uint64_t ) {}


void KIPLATFORM::EMBED::TakeFocusForClick( wxWindow* aFrame, wxWindow* )
{
    TakeFocus( aFrame );
}


bool KIPLATFORM::EMBED::ReleaseStrayCapture( wxWindow* )
{
    return false;
}


void KIPLATFORM::EMBED::NoteMainThread()
{
}


std::string KIPLATFORM::EMBED::MainThreadStack()
{
    return {};
}


std::string KIPLATFORM::EMBED::InputState()
{
    return {};
}


std::string KIPLATFORM::EMBED::WriteMiniDump( const std::string& )
{
    return {};
}
