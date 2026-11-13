/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <wx/version.h>
#include <wx/sizer.h>
#include <wx/evtloop.h>
#include <wx/utils.h>
#include <widgets/app_progress_dialog.h>
#include <backplane_embed.h>
#include <widgets/wx_event_utils.h>

/// What wxGenericProgressDialog::Update() does after each update (UI events only, no input);
/// the hidden embedded dialog skips its Update, so do it here.
static void yieldLikeTheDialog()
{
    if( wxEventLoopBase* loop = wxEventLoopBase::GetActive() )
        loop->YieldFor( wxEVT_CATEGORY_UI );
}


APP_PROGRESS_DIALOG::APP_PROGRESS_DIALOG( const wxString& aTitle, const wxString& aMessage,
                                          int aMaximum, wxWindow* aParent,
                                          bool aIndeterminateTaskBarStatus, int aStyle )
        : APP_PROGRESS_DIALOG_BASE(),
          m_appProgressIndicator( aParent, aMaximum ),
          m_indeterminateTaskBarStatus( aIndeterminateTaskBarStatus ),
          m_maximum( aMaximum )

{
    // Embedded, the host draws it (ET_PROGRESS) and the dialog never maps; see Show().
    if( BACKPLANE_EMBED::Get().Enabled() && !wxGetEnv( wxS( "BACKPLANE_KICAD_PROGRESS_DIALOGS" ), nullptr ) )
        m_embedJob = BACKPLANE_EMBED::Get().ProgressBegin( aParent, aTitle, ( aStyle & wxPD_CAN_ABORT ) != 0 );

    APP_PROGRESS_DIALOG_BASE::Create( aTitle, aMessage == wxEmptyString ? wxString( wxT( " " ) ) : aMessage,
                                      aMaximum, aParent, aStyle );

    if( m_indeterminateTaskBarStatus )
    {
        m_appProgressIndicator.Pulse();
    }

    Layout();

    if( GetSizer() )
        GetSizer()->Fit( this );
}


APP_PROGRESS_DIALOG::~APP_PROGRESS_DIALOG()
{
    if( m_embedJob )
        BACKPLANE_EMBED::Get().ProgressEnd( m_embedJob );
}


bool APP_PROGRESS_DIALOG::Show( bool aShow )
{
    if( aShow && m_embedJob )
        return false;

    return APP_PROGRESS_DIALOG_BASE::Show( aShow );
}


bool APP_PROGRESS_DIALOG::Pulse( const wxString& aNewMsg, bool* aSkip )
{
    if( m_embedJob )
    {
        BACKPLANE_EMBED::Get().ProgressUpdate( m_embedJob, -1.0, aNewMsg );
        yieldLikeTheDialog();
        DrainPendingEvents();
        return !BACKPLANE_EMBED::Get().ProgressCancelled( m_embedJob );
    }

    return APP_PROGRESS_DIALOG_BASE::Pulse( aNewMsg, aSkip );
}


bool APP_PROGRESS_DIALOG::Update( int aValue, const wxString& aNewMsg, bool* aSkip )
{
    if( m_embedJob )
    {
        BACKPLANE_EMBED::Get().ProgressUpdate( m_embedJob, m_maximum > 0 ? double( aValue ) / m_maximum : -1.0,
                                               aNewMsg );
        yieldLikeTheDialog();
        DrainPendingEvents();
        return !BACKPLANE_EMBED::Get().ProgressCancelled( m_embedJob );
    }

    if( !m_indeterminateTaskBarStatus )
    {
        m_appProgressIndicator.SetValue( aValue );
    }

    bool diag = APP_PROGRESS_DIALOG_BASE::Update( aValue, aNewMsg, aSkip );

    DrainPendingEvents();

    return diag;
}
