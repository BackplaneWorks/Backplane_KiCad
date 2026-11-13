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
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <wx/event.h>
#include <wx/gdicmn.h>
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
    OTHER = 6,
    GERBVIEW = 7
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
    DOCUMENT_CHANGED = 13,
    SELECTION_CHANGED = 14,
    DOCUMENT_RELOADED = 15,
    RELOAD_CONFLICT = 16,
    THEME_CHANGED = 17,
    ATTACHED = 18,
    COMMENT = 19,
    DOCUMENT_EDITED = 20,
    AGENT_NOTICE = 21,
    PLAN_CHANGED = 22,
    AGENT_CONTROL = 23,
    PROGRESS = 24,
    EDITOR_REQUESTED = 25,
    MARKER_CLICKED = 26,
    CHECKS_DONE = 27,
    FRAME_DRAWN = 28
};

/// Values match kiapi.backplane.embed.ThemeMode.
enum class EMBED_THEME : int
{
    OWN_SETTINGS = 0,  ///< no override: the person's own settings
    LIGHT_MODE = 1,
    DARK_MODE = 2
};

/// Values match kiapi.backplane.embed.ReloadResult.
enum class EMBED_RELOAD_RESULT : int
{
    UNKNOWN = 0,
    RELOADED = 1,
    UNCHANGED = 2,
    CONFLICT = 3
};

/// Item names as people write them (kiapi.backplane.embed.ItemNames).
struct EMBED_ITEM_NAMES
{
    std::vector<std::string> references;
    std::vector<std::string> nets;
    std::vector<std::string> pins;   ///< "U3.4"

    bool Empty() const { return references.empty() && nets.empty() && pins.empty(); }
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
    std::function<void()>                      settingsChanged;  ///< CommonSettingsChanged()
    std::function<std::vector<wxString>()>     watchedFiles;     ///< files to reload from
    std::function<bool()>                      reload;           ///< reload, keeping the view
    std::function<void( const wxString& )>     showConflict;     ///< infobar; empty hides it
    std::function<bool()>                      busy;             ///< a dialog or tool is active
    std::function<void()>                      syncSideFiles;    ///< merge external project/rules changes
    std::function<wxString()>                  libId;            ///< library editors: "Lib:Name" on screen
    std::function<std::vector<wxString>()>     files;            ///< GerbView: the files loaded
    /// GerbView: replace what it shows with aFiles; aErrors gets one (path, reason) per failure
    std::function<void( const std::vector<wxString>& aFiles, std::vector<wxString>& aLoaded,
                        std::vector<std::pair<wxString, wxString>>& aErrors )> openFiles;
    std::function<void()>                      autoPanels;       ///< side panels for the width (EmbedAutoPanels)
    std::function<wxWindow*()>                 canvas;           ///< the frame's drawing canvas, if any
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
    bool             realized = false;  ///< the host may map/unmap its window directly
    uint64_t         window = 0;
    int              minWidth = 0;
    int              minHeight = 0;
    wxString         libId;            ///< FK_FOOTPRINT_EDITOR / FK_SYMBOL_EDITOR
    std::vector<wxString> files;       ///< FK_GERBVIEW
};

/// A box in millimetres, the document's own frame (Y down), kiapi.backplane.embed.BoxMm.
struct EMBED_BOX_MM
{
    double xMin = 0, yMin = 0, xMax = 0, yMax = 0;
};

/// One selected item of a comment (kiapi.backplane.embed.CommentItem).
struct EMBED_COMMENT_ITEM
{
    std::string  kind;        ///< KiCad class name (FOOTPRINT, PAD, SCH_SYMBOL, ...)
    std::string  id;          ///< KIID
    std::string  reference;
    std::string  pin;         ///< "U3.4"
    std::string  net;
    std::string  layer;       ///< PCB only
    std::string  sheetPath;   ///< schematic only
    std::string  value;
    double       x = 0, y = 0;
    EMBED_BOX_MM bbox;
};

/// "Comment to Backplane..." (kiapi.backplane.embed.Comment).
struct EMBED_COMMENT
{
    static constexpr size_t MAX_ITEMS = 500;

    std::string                     note;
    EMBED_ITEM_NAMES                names;
    std::vector<EMBED_COMMENT_ITEM> items;
    bool                            truncated = false;
    EMBED_BOX_MM                    bbox;
    bool                            emptySelection = false;
    std::string                     sheetPath;
    std::string                     screenshot;   ///< absolute PNG path, or empty
    EMBED_BOX_MM                    screenshotRegion;
    int                             screenshotWidth = 0;
    int                             screenshotHeight = 0;
    bool                            fromApi = false;
};

/// One long job's progress, kiapi.backplane.embed.Progress (bp.12).
struct EMBED_PROGRESS
{
    uint64_t id = 0;
    wxString title;
    wxString detail;
    double   fraction = -1.0;   ///< 0..1, or -1 when indeterminate
    bool     done = false;
    bool     cancellable = false;
    bool     cancelled = false;
};


/// RunChecks' outcome (ET_CHECKS_DONE); kind as EMBED_FRAME_KIND.
struct EMBED_CHECKS
{
    EMBED_FRAME_KIND kind = EMBED_FRAME_KIND::UNKNOWN;
    int              errors = 0;
    int              warnings = 0;
    int              exclusions = 0;
    int              unconnected = 0;
    bool             cancelled = false;
    uint64_t         progressId = 0;
    bool             documentModified = false;
    std::string      error;
};


struct EMBED_EVENT
{
    uint64_t         seq = 0;
    EMBED_EVENT_TYPE type = EMBED_EVENT_TYPE::UNKNOWN;
    EMBED_FRAME_INFO frame;
    std::string      detail;
    EMBED_ITEM_NAMES selection;
    std::shared_ptr<const EMBED_COMMENT> comment;   ///< COMMENT only
    std::shared_ptr<const EMBED_PROGRESS> progress; ///< PROGRESS only
    std::shared_ptr<const EMBED_CHECKS>   checks;   ///< CHECKS_DONE only
    std::string      markerSet;       ///< MARKER_CLICKED on a host marker: its set and id
    std::string      markerId;
    std::string      drcMarker;       ///< MARKER_CLICKED on a DRC/ERC marker: a serialized
                                      ///< kiapi.backplane.embed.Marker
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

    /**
     * A canvas finished a redraw.  The first one in a frame mapped by ShowEmbedded posts
     * ET_FRAME_DRAWN: the frame is on screen, laid out at its size and painted.
     */
    void CanvasDrawn( wxWindow* aCanvas );

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

    /// Lay out and paint a hidden frame once off-screen, then hide it again (ShowFrame.prewarm).
    void Prewarm( wxFrame* aFrame, int aWidth, int aHeight );
    void EndPrewarm( wxFrame* aFrame );
    bool Prewarming() const;

    /// While > 0, KiCad's own show requests are swallowed silently (a frame opened for a prewarm).
    int m_quietShowRequests = 0;

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

    /// The KEY_ATOM index that means "the comment key" (';'): out of band (a host reserves far
    /// fewer chords), so it never names one of the host's reserved chords, however many it sets.
    /// The X11 message also carries keysym 0x3b; on Windows the index is WM_APP's wParam.
    static constexpr long COMMENT_KEY_INDEX = 63;

    /// Embedded aFrame: hand the editor's comment key to the host as a KEY_ATOM message with
    /// COMMENT_KEY_INDEX (capability comment-key-63), so the host opens its own comment
    /// window.  False, and nothing sent, when aFrame is not embedded.
    bool SendCommentKey( const wxFrame* aFrame );

    /// Called from wxApp::FilterEvent.  Returns -1 to continue, or wxApp's processed value.
    int FilterEvent( wxEvent& aEvent );

    /// While set, KiCad's "save changes?" prompts answer this (wxID_YES / wxID_NO) silently.
    std::optional<int> UnsavedChangesAnswer() const { return m_unsavedAnswer; }
    void SetUnsavedChangesAnswer( std::optional<int> aAnswer ) { m_unsavedAnswer = aAnswer; }

    /**
     * The title of the modal or quasi-modal dialog (or modal frame) that is open, if one is.
     * While one is open, API requests that change state are refused with AS_BUSY.
     * aNestedLoops false: only dialogs count, not a nested event loop (a popup menu runs one).
     */
    static std::optional<wxString> ModalBlocker( bool aNestedLoops = true );

    // Progress of long jobs (bp.12): in --embedded the progress dialogs stay hidden and report
    // here instead; the host hears ET_PROGRESS.  Begin/Update/End are main-thread only;
    // RequestCancel and ProgressCancelled are thread-safe.

    /// A job starts in aOwner's frame (any window inside it, or null).
    uint64_t ProgressBegin( wxWindow* aOwner, const wxString& aTitle, bool aCancellable );

    /// aFraction 0..1 or -1 when indeterminate; an empty aDetail keeps the last one.
    void ProgressUpdate( uint64_t aId, double aFraction, const wxString& aDetail = wxEmptyString,
                         const wxString& aTitle = wxEmptyString );

    void ProgressEnd( uint64_t aId );

    /// An id for a job that begins later (RunChecks answers with it before its run starts).
    uint64_t ProgressReserveId();

    /// While alive, the next ProgressBegin on this (main) thread takes aId instead of a new one.
    struct KICOMMON_API PROGRESS_ID_SCOPE
    {
        explicit PROGRESS_ID_SCOPE( uint64_t aId );
        ~PROGRESS_ID_SCOPE();
    };

    /// A RunChecks ended: ET_CHECKS_DONE for aFrame's frame.
    void ChecksDone( wxWindow* aFrame, const EMBED_CHECKS& aChecks );

    /// The person clicked a marker in aFrame's canvas: a host marker (aSet, aId) or a DRC/ERC
    /// marker (aDrcMarker, a serialized kiapi.backplane.embed.Marker).  ET_MARKER_CLICKED.
    void MarkerClicked( wxWindow* aFrame, const std::string& aSet, const std::string& aId,
                        const std::string& aDrcMarker );

    /// The host asked to stop it (CancelProgress).
    bool ProgressCancelled( uint64_t aId ) const;

    /// CancelProgress{id}: true if the job is running and cancellable.  Any thread.
    bool RequestCancel( uint64_t aId );

    std::vector<EMBED_PROGRESS> ActiveProgress() const;

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

    // ---- Theme (bp.9) ----

    /// Apply a theme override live (GTK prefer-dark, colour themes, icons).  aRefresh false
    /// is for the command line, before any frame exists.
    void SetTheme( EMBED_THEME aTheme, bool aRefresh = true );
    EMBED_THEME Theme() const { return m_theme; }
    bool        GtkDark() const { return m_gtkDark; }

    /// The colour theme every canvas draws with while a theme override is set, else empty.
    wxString ColorThemeOverride() const;

    /// Name of the fork's built-in dark colour theme.
    static const wxString COLOR_THEME_DARK;

    // ---- Selection (bp.9) ----

    /// A frame's selection changed because the person changed it.  Ignored while a host or
    /// cross-probe selection is being applied (see SelectionScope).
    void SelectionChanged( wxFrame* aFrame, const EMBED_ITEM_NAMES& aSelection );

    /// While alive, selection changes are not reported (SelectItems applying the host's).
    struct KICOMMON_API SELECTION_SCOPE
    {
        SELECTION_SCOPE() { ++BACKPLANE_EMBED::Get().m_hostSelecting; }
        ~SELECTION_SCOPE() { --BACKPLANE_EMBED::Get().m_hostSelecting; }
    };

    // ---- Comments (bp.10) ----

    /// The API socket given with --api-socket (a Backplane host is there even if not embedded).
    void SetApiSocket( const std::string& aPath ) { m_apiSocket = aPath; }
    const std::string& ApiSocket() const { return m_apiSocket; }

    /// "Comment to Backplane..." is offered: embedded, or a Backplane API socket is configured.
    bool CommentsAvailable() const { return m_enabled || !m_apiSocket.empty(); }

    /**
     * A new PNG path for a comment's screenshot, in <dir of the API socket>/backplane-comments/
     * (created), keeping the newest MAX_SCREENSHOTS - 1 there.  Empty if there is no socket or
     * the folder cannot be made.
     */
    wxString NewCommentScreenshotPath();
    static constexpr int MAX_SCREENSHOTS = 32;

    /// Push ET_COMMENT for aFrame (its kind aKind if it is not an embedded frame).  Returns its
    /// seq.  detail = the note, selection = the comment's names.
    uint64_t PushComment( wxFrame* aFrame, EMBED_FRAME_KIND aKind, const EMBED_COMMENT& aComment );

    // ---- Reload on disk change (bp.9) ----

    /// KiCad is about to write / has written aPath itself: not an external change.
    void NoteWriting( const wxString& aPath );
    void NoteWritten( const wxString& aPath );

    /// RAII around a save of aPath.
    struct KICOMMON_API WRITE_SCOPE
    {
        explicit WRITE_SCOPE( const wxString& aPath ) : m_path( aPath )
        {
            BACKPLANE_EMBED::Get().NoteWriting( m_path );
        }
        ~WRITE_SCOPE() { BACKPLANE_EMBED::Get().NoteWritten( m_path ); }
        wxString m_path;
    };

    /// ReloadDocument / ResolveConflict.  aDiscard reloads over unsaved changes.
    EMBED_RELOAD_RESULT Reload( wxFrame* aFrame, bool aDiscard );
    EMBED_RELOAD_RESULT ResolveConflict( wxFrame* aFrame, bool aReload );

    /// The frame whose watched files include aPath.
    wxFrame* FindFrameByFile( const wxString& aPath ) const;

    /// True while a reload is in progress (KiCad's own Show() calls are not host requests).
    bool Reloading() const { return m_reloading > 0; }

    // ---- Frame trace (BACKPLANE_KICAD_FRAMETRACE=<file>) ----

    /// Log every UI-thread loop iteration over aThresholdMs (with what ran in it) and every
    /// span reported with TraceSpan (GAL redraws, saves, IPC commands) to aPath, JSON lines.
    bool TraceStart( const wxString& aPath, double aThresholdMs = 8.0 );
    bool Tracing() const { return m_traceFile != nullptr; }

    /// Something that ran in the current loop iteration ("paint", "ipc:ShowFrame", ...).
    void TraceNote( const std::string& aWhat );

    /// A timed piece of work; always logged when tracing.
    void TraceSpan( const std::string& aWhat, double aMs );

    /// RAII: TraceSpan( aWhat, elapsed ) when it ends (if tracing and over aMinMs), and a note.
    struct KICOMMON_API TRACE_SCOPE
    {
        TRACE_SCOPE( std::string aWhat, double aMinMs = 0.0 );
        ~TRACE_SCOPE();
        std::string m_what;
        double      m_minMs;
        long long   m_startUs;
        bool        m_on;
    };

    /// Consecutive parts of one piece of work: Lap( "x" ) logs "<prefix>x" for the time since
    /// the last lap (or the start).  Costs a clock read when not tracing.
    struct TRACE_LAPS
    {
        explicit TRACE_LAPS( std::string aPrefix ) :
                m_prefix( std::move( aPrefix ) ),
                m_last( std::chrono::steady_clock::now() )
        {}

        void Lap( const char* aWhat )
        {
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

            if( Get().Tracing() )
                Get().TraceSpan( m_prefix + aWhat, std::chrono::duration<double, std::milli>( now - m_last ).count() );

            m_last = now;
        }

        std::string                           m_prefix;
        std::chrono::steady_clock::time_point m_last;
    };

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
        wxSize           shownSize;   ///< its size at the last ShowEmbedded; a change relayouts
        bool             awaitDrawn = false;  ///< shown by ShowEmbedded, not yet drawn
        std::chrono::steady_clock::time_point shownAt;
        bool             editorRequestPending = false;   ///< an ET_EDITOR_REQUESTED is on its way
    };

    FRAME_ENTRY*       entry( const wxFrame* aFrame );
    const FRAME_ENTRY* entry( const wxFrame* aFrame ) const;
    EMBED_FRAME_INFO   info( const FRAME_ENTRY& aEntry ) const;

    /// ET_EDITOR_REQUESTED for a footprint or symbol editor KiCad itself opened or raised.
    void editorRequested( const FRAME_ENTRY& aEntry );

    bool registerFrame( wxFrame* aFrame, FRAME_T aType, const EMBED_FRAME_OPS& aOps );

    struct FILE_STATE
    {
        std::string signature;      ///< mtime, size, inode as last accepted
        std::string hash;           ///< content hash as last accepted
        std::string pendingSig;     ///< a signature seen but not yet quiet long enough
        long long   pendingSince = 0;
        int         writing = 0;    ///< KiCad itself is writing it
    };

    void pollFiles();
    void baseline( const wxString& aPath );
    void externalChange( wxFrame* aFrame, const wxString& aPath );
    EMBED_RELOAD_RESULT doReload( wxFrame* aFrame );
    void onPoll( wxTimerEvent& aEvent );
    void onFrameActivate( wxFrame* aFrame, bool aActive );
    bool isUnmanaged( uint64_t aWindow ) const;
    void sendFocusMessage( wxFrame* aFrame );

    /// Hand reserved chord aIndex (pressed as aKeyCode with aModifiers) to the host from aWindow:
    /// _BACKPLANE_KEY on X11, WM_APP+0x102 on Windows.
    void postReservedKey( uint64_t aWindow, size_t aIndex, int aKeyCode, int aModifiers );
    void eventServerThread();

    std::string m_apiSocket;
    int         m_commentCounter = 0;

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

    EMBED_THEME m_theme = EMBED_THEME::OWN_SETTINGS;
    bool        m_gtkDark = false;
    int         m_hostSelecting = 0;
    int         m_reloading = 0;

    std::map<wxString, FILE_STATE> m_files;

    FILE*                    m_traceFile = nullptr;
    double                   m_traceThreshold = 8.0;
    std::vector<std::pair<std::string, int>> m_traceNotes;
    std::mutex               m_traceMutex;

    void traceLoop( double aStartMs, double aDurMs );
    std::map<wxFrame*, wxString>   m_conflicts;   ///< frame -> file with a pending conflict
    std::set<wxFrame*>             m_prewarming;  ///< frames being painted off-screen

    wxTimer m_pollTimer;
    wxTimer m_parentTimer;
    long long m_parentPid = 0;
    std::function<void()> m_onParentGone;

    // Event queue
    struct PROGRESS_JOB
    {
        EMBED_PROGRESS                        state;
        wxFrame*                              frame = nullptr;  ///< checked against the frame list before use
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point lastSent;
        bool                                  statusShown = false;
    };

    void pushProgress( const PROGRESS_JOB& aJob );
    void showProgressInStatusBar( PROGRESS_JOB& aJob, bool aClear );

    mutable std::mutex                   m_progressMutex;
    std::map<uint64_t, PROGRESS_JOB>     m_progress;
    std::set<uint64_t>                   m_cancelRequests;
    uint64_t                             m_nextProgressId = 1;
    uint64_t                             m_adoptProgressId = 0;   ///< PROGRESS_ID_SCOPE
    std::atomic<int64_t>                 m_lastPollMs{ 0 };   ///< when the host last asked NextEvents

    mutable std::mutex       m_eventMutex;
    std::condition_variable  m_eventCv;
    std::deque<EMBED_EVENT>  m_events;
    uint64_t                 m_nextSeq = 1;
    static constexpr size_t  MAX_EVENTS = 1024;

    std::string       m_eventUrl;
    std::thread       m_eventThread;
    std::atomic<bool> m_eventShutdown{ false };
    int               m_awaitingDrawn = 0;   ///< frames shown but not yet drawn (UI thread)
};

#endif // BACKPLANE_EMBED_H_
