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

#ifndef KICAD_API_HANDLER_EMBED_H
#define KICAD_API_HANDLER_EMBED_H

#include <api/api_handler.h>
#include <api/backplane/embed.pb.h>

class KIWAY;

/**
 * kiapi.backplane.embed: control of the frames of `kicad --embedded` by its host.
 * Lives in the project manager process because it opens frames through the manager's KIWAY.
 */
class API_HANDLER_EMBED : public API_HANDLER
{
public:
    int DispatchRank() const override { return RANK_EMBED; }

    explicit API_HANDLER_EMBED( KIWAY& aKiway );

private:
    HANDLER_RESULT<kiapi::backplane::embed::AttachToParentResponse> handleAttachToParent(
            const HANDLER_CONTEXT<kiapi::backplane::embed::AttachToParent>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::OpenGerbersResponse> handleOpenGerbers(
            const HANDLER_CONTEXT<kiapi::backplane::embed::OpenGerbers>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::ShowFrameResponse> handleShowFrame(
            const HANDLER_CONTEXT<kiapi::backplane::embed::ShowFrame>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::HideFrameResponse> handleHideFrame(
            const HANDLER_CONTEXT<kiapi::backplane::embed::HideFrame>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::GetFramesResponse> handleGetFrames(
            const HANDLER_CONTEXT<kiapi::backplane::embed::GetFrames>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::NextEventsResponse> handleNextEvents(
            const HANDLER_CONTEXT<kiapi::backplane::embed::NextEvents>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::SetChromeResponse> handleSetChrome(
            const HANDLER_CONTEXT<kiapi::backplane::embed::SetChrome>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::SetReservedKeysResponse> handleSetReservedKeys(
            const HANDLER_CONTEXT<kiapi::backplane::embed::SetReservedKeys>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::CloseResponse> handleClose(
            const HANDLER_CONTEXT<kiapi::backplane::embed::Close>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::SetThemeResponse> handleSetTheme(
            const HANDLER_CONTEXT<kiapi::backplane::embed::SetTheme>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::ReloadDocumentResponse> handleReloadDocument(
            const HANDLER_CONTEXT<kiapi::backplane::embed::ReloadDocument>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::ResolveConflictResponse> handleResolveConflict(
            const HANDLER_CONTEXT<kiapi::backplane::embed::ResolveConflict>& aCtx );

    HANDLER_RESULT<kiapi::backplane::embed::CancelProgressResponse> handleCancelProgress(
            const HANDLER_CONTEXT<kiapi::backplane::embed::CancelProgress>& aCtx );

    KIWAY& m_kiway;
};

#endif // KICAD_API_HANDLER_EMBED_H
