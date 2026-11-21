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

#ifndef __APP_PROGRESS_REPORTER
#define __APP_PROGRESS_REPORTER

#include <wx/progdlg.h>
#include <wx/appprogress.h>
#include <cstdint>

// The generic dialog everywhere (see WX_PROGRESS_REPORTER_BASE): in --embedded it is created and
// never shown, and its progress goes to the host as ET_PROGRESS.
#include <wx/generic/progdlgg.h>
#define APP_PROGRESS_DIALOG_BASE wxGenericProgressDialog

/**
 * wxProgressDialog with the option to also update the application progress on the taskbar
 */
class APP_PROGRESS_DIALOG : public APP_PROGRESS_DIALOG_BASE
{
public:
    APP_PROGRESS_DIALOG( const wxString& aTitle, const wxString& aMessage, int aMaximum = 100,
                         wxWindow* aParent = nullptr, bool aIndeterminateTaskBarStatus = false,
                         int aStyle = wxPD_APP_MODAL | wxPD_AUTO_HIDE );

    ~APP_PROGRESS_DIALOG() override;

    virtual bool Update( int aValue, const wxString& aNewMsg = wxEmptyString,
                         bool* aSkip = nullptr ) override;

    bool Pulse( const wxString& aNewMsg = wxEmptyString, bool* aSkip = nullptr ) override;

    bool Show( bool aShow = true ) override;


private:
    wxAppProgressIndicator m_appProgressIndicator;
    bool m_indeterminateTaskBarStatus;
    int  m_maximum;
    uint64_t m_embedJob = 0;   ///< the host's ET_PROGRESS job, when embedded
};

#endif
