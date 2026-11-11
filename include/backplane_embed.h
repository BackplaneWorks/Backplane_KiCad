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

#ifndef BACKPLANE_EMBED_H_
#define BACKPLANE_EMBED_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <frame_type.h>
#include <kicommon.h>

class wxFrame;
class wxWindow;

/**
 * Kinds of embedded frame.  The values match kiapi.backplane.embed.FrameKind.
 */
enum class EMBED_FRAME_KIND : int
{
    UNKNOWN = 0,
    PCB = 1,
    SCH = 2,
    SYMBOL_EDITOR = 3,
    FOOTPRINT_EDITOR = 4,
    PCB_3D = 5,
    OTHER = 6
};

/// Values match kiapi.backplane.embed.EventType.
enum class EMBED_EVENT_TYPE : int
{
    UNKNOWN = 0,
    FRAME_OPENED = 1,
    FRAME_CLOSED = 2,
    FRAME_SHOWN = 3,
    FRAME_HIDDEN = 4,
    TITLE_CHANGED = 5,
    MODIFIED_CHANGED = 6,
    FOCUS_IN = 7,
    FOCUS_OUT = 8,
    SHOW_REQUESTED = 9,
    DIALOG_OPENED = 10,
    DIALOG_CLOSED = 11,
    DETACHED = 12,
    DOCUMENT_CHANGED = 13
};

struct EMBED_CHROME
{
    bool menubar = true;
    bool topToolbar = true;
    bool sideToolbars = true;
    bool statusBar = true;
};

/**
 * Operations a frame provides to the embedding state.  They are bound in EDA_BASE_FRAME (in
 * the frame's own module), so the embedding code never calls non-virtual frame code directly.
 */
struct EMBED_FRAME_OPS
{
    std::function<void( const EMBED_CHROME& )> applyChrome;
    std::function<bool( const std::string& )>  runAction;
    std::function<wxString()>                  document;
    std::function<bool()>                      modified;
};

struct EMBED_FRAME_INFO
{
    EMBED_FRAME_KIND kind = EMBED_FRAME_KIND::UNKNOWN;
    wxString         document;
    wxString         title;
    wxString         frameName;
    bool             modified = false;
    bool             shown = false;
    bool             focused = false;
    uint64_t         window = 0;
    int              minWidth = 0;
    int              minHeight = 0;
};

struct EMBED_EVENT
{
    uint64_t         seq = 0;
    EMBED_EVENT_TYPE type = EMBED_EVENT_TYPE::UNKNOWN;
    EMBED_FRAME_INFO frame;
    std::string      detail;
};


/**
 * Embedding of KiCad's frames as child windows of a host application (`kicad --embedded`).
 *
 * Every frame except the project manager is "born embedded": before its first Show() it is
 * realized and parked, unmapped, in a holder window inside the host's container, so the window
 * manager never sees it.  A Show( true ) from KiCad itself does not map a hidden frame; it posts
 * SHOW_REQUESTED and the host decides with ShowFrame.  See BACKPLANE_EMBED.md.
 *
 * All methods except the event queue are main-thread only.
 */
class KICOMMON_API BACKPLANE_EMBED : public wxEvtHandler
{
public:
    static BACKPLANE_EMBED& Get();

    /// Turn embedding on (from the command line).  aParent/aToplevel may be 0 (attach later).
    bool Enable( uint64_t aParent, uint64_t aToplevel );

    bool Enabled() const { return m_enabled; }

    uint64_t Parent() const { return m_parent; }
    uint64_t Toplevel() const { return m_toplevel ? m_toplevel : m_parent; }
    uint64_t Holder() const { return m_parent ? m_holder : 0; }
    double   Scale() const { return m_scale; }

    /// AttachToParent.  aParent 0 detaches.  Returns false if the parent cannot be used.
    bool Attach( uint64_t aParent, uint64_t aToplevel, double aScale );

    static EMBED_FRAME_KIND KindOf( FRAME_T aType );

    /**
     * Called by EDA_BASE_FRAME::Show().  Returns the value Show() should return, or nothing to
     * let the frame show normally (embedding is off, or the frame stays a top-level).
     */
    std::optional<bool> OnFrameShow( wxFrame* aFrame, FRAME_T aType, bool aIsModal, bool aShow,
                                     const EMBED_FRAME_OPS& aOps );

    /// Called by EDA_BASE_FRAME::Raise().  True if the raise was handled (do not raise).
    bool OnFrameRaise( wxFrame* aFrame, FRAME_T aType, bool aIsModal,
                       const EMBED_FRAME_OPS& aOps );

    /// Called by the EDA_BASE_FRAME destructor.
    void OnFrameDestroyed( wxFrame* aFrame );

    bool IsEmbedded( const wxFrame* aFrame ) const;

    /// Put a helper top-level (not a frame the host sees) in the holder, so it is never
    /// managed by the window manager.  No-op unless embedding is on.
    void ParkWindow( wxWindow* aWindow );

    /// Show or hide an embedded frame for the host (bypasses the show filter).
    bool ShowEmbedded( wxFrame* aFrame, bool aShow );

    /// Run aFunc with KiCad's own Show() calls allowed to map embedded frames.
    void WithShowAllowed( const std::function<void()>& aFunc );

    /// Embedded frames, in creation order.
    std::vector<wxFrame*> Frames() const;
    wxFrame*              FindFrame( EMBED_FRAME_KIND aKind, uint64_t aWindow = 0 ) const;
    EMBED_FRAME_INFO      Info( wxFrame* aFrame ) const;
    std::vector<EMBED_FRAME_INFO> AllInfo() const;
    const EMBED_FRAME_OPS* Ops( wxFrame* aFrame ) const;

    /// The (hidden) project manager frame, which is never embedded or shown.
    void SetManager( wxFrame* aManager, const EMBED_FRAME_OPS& aOps );
    wxFrame* Manager() const { return m_manager; }
    const EMBED_FRAME_OPS& ManagerOps() const { return m_managerOps; }

    // Chrome
    const EMBED_CHROME& Chrome() const { return m_chrome; }
    void SetChrome( const EMBED_CHROME& aChrome );

    // Reserved keys
    /// Parse and set chords; false (and no change) if any chord does not parse.
    bool SetReservedKeys( const std::vector<std::string>& aChords, wxString* aError );
    std::vector<std::string> ReservedKeys() const;

    /// Called from wxApp::FilterEvent.  Returns -1 to continue, or wxApp's processed value.
    int FilterEvent( wxEvent& aEvent );

    /// While set, KiCad's "save changes?" prompts answer this (wxID_YES / wxID_NO) silently.
    std::optional<int> UnsavedChangesAnswer() const { return m_unsavedAnswer; }
    void SetUnsavedChangesAnswer( std::optional<int> aAnswer ) { m_unsavedAnswer = aAnswer; }

    /**
     * The title of the modal or quasi-modal dialog (or modal frame) that is open, if one is.
     * While one is open, API requests that change state are refused with AS_BUSY.
     */
    static std::optional<wxString> ModalBlocker();

    // Events (thread-safe)
    void PushEvent( EMBED_EVENT_TYPE aType, const EMBED_FRAME_INFO& aFrame,
                    const std::string& aDetail = std::string() );

    /// Events with seq > aSince, waiting up to aWaitMs for the first.  aLost is set when some
    /// were dropped.  Returns the seq to pass next time.
    uint64_t NextEvents( uint64_t aSince, int aWaitMs, size_t aMax,
                         std::vector<EMBED_EVENT>& aEvents, bool& aLost );

    /// Start the long-poll socket at aUrl (ipc://...), answering with aToken.  False if it
    /// cannot bind.
    bool StartEventServer( const std::string& aUrl, const std::string& aToken );
    void StopEventServer();
    const std::string& EventServerUrl() const { return m_eventUrl; }

    /// Exit when aPid goes away (0 = no supervision).
    void WatchParentPid( long long aPid, std::function<void()> aOnGone );

    /// Format a chord for display / IPC (e.g. "Ctrl+Shift+Tab").
    static std::string FormatChord( int aModifiers, int aKeyCode );

    /// Parse a chord.  Returns false if it does not parse.
    static bool ParseChord( const std::string& aChord, int& aModifiers, int& aKeyCode );

    ~BACKPLANE_EMBED();

private:
    BACKPLANE_EMBED();

    struct FRAME_ENTRY
    {
        wxFrame*         frame = nullptr;
        FRAME_T          type = FRAME_T_COUNT;
        EMBED_FRAME_KIND kind = EMBED_FRAME_KIND::UNKNOWN;
        EMBED_FRAME_OPS  ops;
        uint64_t         window = 0;
        int              minWidth = 0;
        int              minHeight = 0;
        // Last values seen by the poll timer
        wxString         title;
        wxString         document;
        bool             modified = false;
        bool             shown = false;
        bool             focused = false;
    };

    FRAME_ENTRY*       entry( const wxFrame* aFrame );
    const FRAME_ENTRY* entry( const wxFrame* aFrame ) const;
    EMBED_FRAME_INFO   info( const FRAME_ENTRY& aEntry ) const;

    bool registerFrame( wxFrame* aFrame, FRAME_T aType, const EMBED_FRAME_OPS& aOps );
    void onPoll( wxTimerEvent& aEvent );
    void onFrameActivate( wxFrame* aFrame, bool aActive );
    bool isUnmanaged( uint64_t aWindow ) const;
    void sendFocusMessage( wxFrame* aFrame );
    void eventServerThread();

    bool     m_enabled = false;
    uint64_t m_parent = 0;
    uint64_t m_toplevel = 0;
    uint64_t m_holder = 0;
    double   m_scale = 1.0;
    int      m_showAllowed = 0;

    std::vector<FRAME_ENTRY> m_frames;
    wxFrame*                 m_manager = nullptr;
    EMBED_FRAME_OPS          m_managerOps;
    uint64_t                 m_managerWindow = 0;
    std::map<uint64_t, wxString> m_openDialogs;
    std::set<uint64_t>           m_parked;

    EMBED_CHROME m_chrome;

    struct CHORD
    {
        int         modifiers = 0;
        int         keyCode = 0;
        std::string text;
    };

    std::vector<CHORD> m_reserved;

    std::optional<int> m_unsavedAnswer;

    wxTimer m_pollTimer;
    wxTimer m_parentTimer;
    long long m_parentPid = 0;
    std::function<void()> m_onParentGone;

    // Event queue
    mutable std::mutex       m_eventMutex;
    std::condition_variable  m_eventCv;
    std::deque<EMBED_EVENT>  m_events;
    uint64_t                 m_nextSeq = 1;
    static constexpr size_t  MAX_EVENTS = 1024;

    std::string       m_eventUrl;
    std::thread       m_eventThread;
    std::atomic<bool> m_eventShutdown{ false };
};

#endif // BACKPLANE_EMBED_H_
