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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>

#include <wx/app.h>
#include <wx/window.h>
#include <windows.h>
#include <dbghelp.h>

// Windows: as on X11, the frames are WS_CHILD windows of a holder window KiCad owns, and the
// holder is a child of the host's container HWND (or, parked, a hidden off-screen popup).
// AttachToParent moves only the holder.  The holder follows its parent's client size and
// notices the host moving it or its parent going away.  Dialogs are owned by the host's
// top-level HWND.

namespace
{
KIPLATFORM::EMBED::HOOKS s_hooks;
HWND                     s_holder = nullptr;
HWND                     s_parent = nullptr;   ///< the container the holder belongs in, or null

constexpr UINT_PTR HOLDER_TIMER = 1;
constexpr UINT FOCUS_CANVAS_MESSAGE = WM_APP + 0x103;
constexpr wchar_t FOCUS_CANVAS_PROPERTY[] = L"Backplane.AsyncFocus.1";


/// BACKPLANE_KICAD_EMBED_TRACE: "1" (a log in %TEMP%) or a file path.  Each line also goes to
/// OutputDebugString.  A GUI-subsystem KiCad often has no usable stderr, hence the file.
std::wstring tracePath()
{
    const wchar_t* value = _wgetenv( L"BACKPLANE_KICAD_EMBED_TRACE" );

    if( !value || !*value )
        return {};

    std::wstring path = value;

    if( path == L"1" )
    {
        wchar_t dir[MAX_PATH] = {};
        GetTempPathW( MAX_PATH, dir );
        path = std::wstring( dir ) + L"backplane-kicad-embed-trace.log";
    }

    return path;
}


FILE* traceFile()
{
    static FILE* file = []() -> FILE*
    {
        std::wstring path = tracePath();
        return path.empty() ? nullptr : _wfopen( path.c_str(), L"a" );
    }();

    return file;
}


void trace( const char* aFormat, ... )
{
    FILE* file = traceFile();

    if( !file )
        return;

    char    line[4096];   // a stall line carries a whole stack
    va_list args;
    va_start( args, aFormat );
    vsnprintf( line, sizeof( line ), aFormat, args );
    va_end( args );

    fprintf( file, "%lu embed: %s\n", GetTickCount(), line );
    fflush( file );
    OutputDebugStringA( ( std::string( "embed: " ) + line + "\n" ).c_str() );
}


/// The window, its class and its process, for trace lines
std::string describe( HWND aWindow )
{
    if( !aWindow )
        return "null";

    char  cls[64] = {};
    DWORD pid = 0;
    GetClassNameA( aWindow, cls, sizeof( cls ) );
    GetWindowThreadProcessId( aWindow, &pid );

    char text[160];
    snprintf( text, sizeof( text ), "0x%llx(%s pid %lu%s)", (unsigned long long) (uintptr_t) aWindow,
              cls, pid, pid == GetCurrentProcessId() ? " ours" : "" );
    return text;
}


void clearNoActivate( HWND aHolder )
{
    LONG_PTR exStyle = GetWindowLongPtrW( aHolder, GWL_EXSTYLE );

    if( exStyle & ( WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE ) )
    {
        SetWindowLongPtrW( aHolder, GWL_EXSTYLE, exStyle & ~( WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE ) );
        trace( "holder ex-style 0x%llx cleared of TOOLWINDOW|NOACTIVATE", (unsigned long long) exStyle );
    }
}
constexpr int      PARK_X = -32000;
constexpr int      PARK_Y = -32000;


void fitToParent( HWND aHolder, HWND aParent )
{
    RECT client;

    if( !aParent || !GetClientRect( aParent, &client ) )
        return;

    RECT now;
    GetWindowRect( aHolder, &now );
    MapWindowPoints( HWND_DESKTOP, aParent, reinterpret_cast<POINT*>( &now ), 2 );

    if( now.left != 0 || now.top != 0 || now.right - now.left != client.right
        || now.bottom - now.top != client.bottom )
    {
        SetWindowPos( aHolder, nullptr, 0, 0, client.right, client.bottom,
                      SWP_NOZORDER | SWP_NOACTIVATE );
    }
}


void becomeChildOf( HWND aHolder, HWND aParent )
{
    LONG_PTR style = GetWindowLongPtrW( aHolder, GWL_STYLE );
    style = ( style & ~WS_POPUP ) | WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    SetWindowLongPtrW( aHolder, GWL_STYLE, style );

    // The parked popup's WS_EX_NOACTIVATE would answer every click inside with MA_NOACTIVATE:
    // no focus, no activation for menus and mouse capture.  Only the parked holder has them.
    clearNoActivate( aHolder );
    SetParent( aHolder, aParent );
    SetWindowPos( aHolder, HWND_TOP, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED );
    fitToParent( aHolder, aParent );
    ShowWindow( aHolder, SW_SHOWNA );
    trace( "holder %s now in %s", describe( aHolder ).c_str(), describe( aParent ).c_str() );
}


void park( HWND aHolder )
{
    // Hidden and off-screen, owned by nobody: the frames inside keep running, and a later
    // AttachToParent moves them back in one SetParent
    ShowWindow( aHolder, SW_HIDE );
    SetParent( aHolder, nullptr );
    LONG_PTR style = GetWindowLongPtrW( aHolder, GWL_STYLE );
    style = ( style & ~WS_CHILD ) | WS_POPUP | WS_CLIPCHILDREN;
    SetWindowLongPtrW( aHolder, GWL_STYLE, style );

    // Never in the taskbar or Alt+Tab, never activated while parked
    LONG_PTR exStyle = GetWindowLongPtrW( aHolder, GWL_EXSTYLE );
    SetWindowLongPtrW( aHolder, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE );
    SetWindowPos( aHolder, nullptr, PARK_X, PARK_Y, 0, 0,
                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
    trace( "holder %s parked", describe( aHolder ).c_str() );
}


LRESULT CALLBACK holderProc( HWND aHwnd, UINT aMsg, WPARAM aWParam, LPARAM aLParam )
{
    if( aMsg == FOCUS_CANVAS_MESSAGE )
    {
        // The parent token rejects a request queued before reparent/park. Resolve the
        // registered frame/canvas only on this UI thread; never trust a foreign wx pointer.
        // On Windows the embedded frame itself is often the foreground window after a cold
        // show (not a WS_CHILD of the host, so IsChild( host, frame ) is false): that state,
        // with focus on a toolbar Edit, is exactly what the request is for. Another
        // application's window, or no foreground window at all, still rejects it.
        HWND frame = reinterpret_cast<HWND>( aWParam );
        HWND parent = reinterpret_cast<HWND>( aLParam );
        HWND host = s_hooks.hostToplevel
                            ? reinterpret_cast<HWND>( s_hooks.hostToplevel() ) : nullptr;
        HWND foreground = GetForegroundWindow();
        GUITHREADINFO info = {};
        info.cbSize = sizeof( info );

        bool valid = aHwnd == s_holder && parent && parent == s_parent
                     && IsWindow( parent ) && GetParent( aHwnd ) == parent
                     && IsWindowVisible( aHwnd ) && frame && IsWindow( frame )
                     && IsChild( aHwnd, frame ) && IsWindowVisible( frame )
                     && IsWindowEnabled( frame ) && host && IsWindow( host )
                     && ( foreground == host || IsChild( host, foreground )
                          || foreground == frame || IsChild( frame, foreground ) );

        if( !GetGUIThreadInfo( GetCurrentThreadId(), &info ) || info.hwndCapture
            || ( info.flags & ( GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE ) ) )
            valid = false;

        bool focused = valid && s_hooks.focusCanvas
                       && s_hooks.focusCanvas( reinterpret_cast<uint64_t>( frame ) );
        trace( "async canvas focus %s parent %s: %s; %s", describe( frame ).c_str(),
               describe( parent ).c_str(), focused ? "accepted" : "ignored",
               KIPLATFORM::EMBED::InputState().c_str() );
        return 0;
    }

    if( aMsg == WM_MOUSEACTIVATE && s_parent )
    {
        // A click inside activates the host's window as a click on its own child would.  Not
        // asked of the container: a host's container may be WS_EX_NOACTIVATE itself.
        trace( "WM_MOUSEACTIVATE hit %d msg 0x%x -> MA_ACTIVATE (fg %s, focus %s)",
               (int) (short) LOWORD( aLParam ), (unsigned) HIWORD( aLParam ),
               describe( GetForegroundWindow() ).c_str(), describe( GetFocus() ).c_str() );
        return MA_ACTIVATE;
    }

    if( traceFile()
        && ( aMsg == WM_SETCURSOR || aMsg == WM_CONTEXTMENU || aMsg == WM_PARENTNOTIFY
             || aMsg == WM_CAPTURECHANGED || aMsg == WM_ACTIVATE || aMsg == WM_SETFOCUS
             || aMsg == WM_KILLFOCUS || aMsg == WM_CANCELMODE ) )
    {
        LRESULT result = DefWindowProcW( aHwnd, aMsg, aWParam, aLParam );

        if( aMsg != WM_SETCURSOR || HIWORD( aLParam ) != WM_MOUSEMOVE )
        {
            trace( "holder msg 0x%04x wparam 0x%llx lparam 0x%llx -> 0x%llx (capture %s)", aMsg,
                   (unsigned long long) aWParam, (unsigned long long) aLParam,
                   (unsigned long long) result, describe( GetCapture() ).c_str() );
        }

        return result;
    }

    if( aMsg == WM_TIMER && aWParam == HOLDER_TIMER && aHwnd == s_holder )
    {
        HWND actual = GetParent( aHwnd );

        if( s_parent && !IsWindow( s_parent ) )
        {
            // The host's container is gone: park, and tell the host when it comes back
            s_parent = nullptr;
            park( aHwnd );

            if( s_hooks.detached )
                s_hooks.detached();
        }
        else if( s_parent && actual && actual != s_parent )
        {
            // The host moved the holder into another of its windows itself
            s_parent = actual;
            fitToParent( aHwnd, actual );

            if( s_hooks.reparented )
                s_hooks.reparented( reinterpret_cast<uint64_t>( actual ) );
        }
        else if( s_parent && !actual )
        {
            // The host took it out (SetParent(holder, NULL)): treated as a detach
            s_parent = nullptr;
            park( aHwnd );

            if( s_hooks.detached )
                s_hooks.detached();
        }
        else if( s_parent )
        {
            fitToParent( aHwnd, s_parent );
            clearNoActivate( aHwnd );
        }

        return 0;
    }

    if( aMsg == WM_ERASEBKGND )
        return 1;   // the frames cover it; no flash of the class brush

    return DefWindowProcW( aHwnd, aMsg, aWParam, aLParam );
}


const wchar_t* holderClass()
{
    static const wchar_t* name = L"BackplaneKiCadEmbedHolder";
    static bool           registered = false;

    if( !registered )
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof( wc );
        wc.lpfnWndProc = holderProc;
        wc.hInstance = GetModuleHandleW( nullptr );
        wc.lpszClassName = name;
        wc.hCursor = LoadCursor( nullptr, IDC_ARROW );
        registered = RegisterClassExW( &wc ) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    return name;
}
}


bool KIPLATFORM::EMBED::Supported()
{
    return true;
}


void KIPLATFORM::EMBED::Install( const HOOKS& aHooks )
{
    s_hooks = aHooks;
}


namespace
{
DWORD s_mainThreadId = 0;
}


void KIPLATFORM::EMBED::NoteMainThread()
{
    s_mainThreadId = GetCurrentThreadId();
}


std::string KIPLATFORM::EMBED::MainThreadStack()
{
    if( !s_mainThreadId || s_mainThreadId == GetCurrentThreadId() )
        return {};

    // dbghelp at run time: nothing to link, and nothing loaded unless a stall is reported
    HMODULE dbghelp = LoadLibraryW( L"dbghelp.dll" );

    if( !dbghelp )
        return "(no dbghelp.dll)";

    using STACKWALK = BOOL( WINAPI* )( DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
                                       PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                       PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64 );
    using SYMINIT = BOOL( WINAPI* )( HANDLE, PCSTR, BOOL );

    auto stackWalk = reinterpret_cast<STACKWALK>( GetProcAddress( dbghelp, "StackWalk64" ) );
    auto symInit = reinterpret_cast<SYMINIT>( GetProcAddress( dbghelp, "SymInitialize" ) );
    auto tableAccess = reinterpret_cast<PFUNCTION_TABLE_ACCESS_ROUTINE64>(
            GetProcAddress( dbghelp, "SymFunctionTableAccess64" ) );
    auto moduleBase = reinterpret_cast<PGET_MODULE_BASE_ROUTINE64>(
            GetProcAddress( dbghelp, "SymGetModuleBase64" ) );

    if( !stackWalk || !symInit || !tableAccess || !moduleBase )
        return "(dbghelp has no StackWalk64)";

    HANDLE process = GetCurrentProcess();
    symInit( process, nullptr, TRUE );

    HANDLE thread = OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                FALSE, s_mainThreadId );

    if( !thread )
        return "(cannot open the UI thread)";

    std::string out;

    if( SuspendThread( thread ) != (DWORD) -1 )
    {
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_FULL;

        if( GetThreadContext( thread, &context ) )
        {
            STACKFRAME64 frame = {};
            DWORD        machine;
#if defined( _M_ARM64 )
            machine = IMAGE_FILE_MACHINE_ARM64;
            frame.AddrPC.Offset = context.Pc;
            frame.AddrFrame.Offset = context.Fp;
            frame.AddrStack.Offset = context.Sp;
#else
            machine = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = context.Rip;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrStack.Offset = context.Rsp;
#endif
            frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

            for( int depth = 0; depth < 48; ++depth )
            {
                if( !stackWalk( machine, process, thread, &frame, &context, nullptr, tableAccess,
                                moduleBase, nullptr )
                    || !frame.AddrPC.Offset )
                {
                    break;
                }

                HMODULE module = nullptr;
                char    name[MAX_PATH] = "?";

                if( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCSTR>( frame.AddrPC.Offset ), &module )
                    && module )
                {
                    GetModuleFileNameA( module, name, sizeof( name ) );
                }

                const char* base = strrchr( name, '\\' );
                char        entry[MAX_PATH + 32];
                snprintf( entry, sizeof( entry ), "%s%s+0x%llx", out.empty() ? "" : " | ",
                          base ? base + 1 : name,
                          (unsigned long long) ( frame.AddrPC.Offset
                                                 - reinterpret_cast<DWORD64>( module ) ) );
                out += entry;
            }
        }

        ResumeThread( thread );
    }

    CloseHandle( thread );
    return out.empty() ? "(no frames)" : out;
}


namespace
{
/// The popup menus of this thread on screen, with the monitor and DPI context they were placed
/// in (embed trace only, once per menu): where Windows put a context menu and why.
void traceMenuPlacement( HWND aFrame )
{
    static HWND s_lastMenu = nullptr;

    if( !traceFile() )
        return;

    std::vector<HWND> menus;

    EnumThreadWindows(
            GetCurrentThreadId(),
            []( HWND aWindow, LPARAM aList ) -> BOOL
            {
                char cls[16] = {};
                GetClassNameA( aWindow, cls, sizeof( cls ) );

                if( strcmp( cls, "#32768" ) == 0 && IsWindowVisible( aWindow ) )
                    reinterpret_cast<std::vector<HWND>*>( aList )->push_back( aWindow );

                return TRUE;
            },
            reinterpret_cast<LPARAM>( &menus ) );

    if( menus.empty() || menus.front() == s_lastMenu )
        return;

    s_lastMenu = menus.front();

    POINT cursor = {};
    GetCursorPos( &cursor );

    MONITORINFO monitor = {};
    monitor.cbSize = sizeof( monitor );
    GetMonitorInfoW( MonitorFromPoint( cursor, MONITOR_DEFAULTTONEAREST ), &monitor );

    // Per-monitor DPI functions are Windows 10 1607+: looked up, not linked
    typedef UINT( WINAPI * GETDPIFORWINDOW )( HWND );
    typedef HANDLE( WINAPI * GETTHREADDPICONTEXT )();
    typedef int( WINAPI * GETAWARENESS )( HANDLE );
    HMODULE user32 = GetModuleHandleW( L"user32.dll" );
    auto    dpiFor = reinterpret_cast<GETDPIFORWINDOW>( GetProcAddress( user32, "GetDpiForWindow" ) );
    auto    threadContext = reinterpret_cast<GETTHREADDPICONTEXT>(
            GetProcAddress( user32, "GetThreadDpiAwarenessContext" ) );
    auto    awareness = reinterpret_cast<GETAWARENESS>(
            GetProcAddress( user32, "GetAwarenessFromDpiAwarenessContext" ) );
    HWND    root = GetAncestor( aFrame, GA_ROOT );

    std::string line;

    for( HWND menu : menus )
    {
        RECT r = {};
        GetWindowRect( menu, &r );
        line += "menu " + describe( menu ) + " at (" + std::to_string( r.left ) + "," + std::to_string( r.top )
                + "," + std::to_string( r.right - r.left ) + "," + std::to_string( r.bottom - r.top ) + "); ";
    }

    RECT m = monitor.rcMonitor;
    RECT w = monitor.rcWork;
    trace( "context menu placed: %scursor (%ld,%ld); monitor (%ld,%ld,%ld,%ld) work (%ld,%ld,%ld,%ld); "
           "dpi frame %u root %s %u; thread awareness %d",
           line.c_str(), cursor.x, cursor.y, m.left, m.top, m.right, m.bottom, w.left, w.top, w.right,
           w.bottom, dpiFor ? dpiFor( aFrame ) : 0u, describe( root ).c_str(),
           dpiFor && root ? dpiFor( root ) : 0u,
           threadContext && awareness ? awareness( threadContext() ) : -1 );
}
} // namespace


void KIPLATFORM::EMBED::TraceLine( const std::string& aLine )
{
    trace( "%s", aLine.c_str() );
}


std::string KIPLATFORM::EMBED::InputState()
{
    if( !traceFile() )
        return {};

    // GetFocus and GetCapture answer for the calling thread's input state, which the frame's
    // thread shares with the host's (a child window attaches them); GetGUIThreadInfo says it for
    // the UI thread from any thread
    GUITHREADINFO info = {};
    info.cbSize = sizeof( info );
    DWORD thread = s_mainThreadId ? s_mainThreadId : GetCurrentThreadId();

    if( !GetGUIThreadInfo( thread, &info ) )
        return "input state unknown (error " + std::to_string( GetLastError() ) + ")";

    std::string flags;

    if( info.flags & GUI_INMENUMODE )
        flags += " in-menu-mode";

    if( info.flags & GUI_POPUPMENUMODE )
        flags += " popup-menu-mode";

    if( info.flags & GUI_SYSTEMMENUMODE )
        flags += " system-menu-mode";

    return "focus " + describe( info.hwndFocus ) + ", capture " + describe( info.hwndCapture )
           + ", active " + describe( info.hwndActive ) + ", menu owner " + describe( info.hwndMenuOwner )
           + ", foreground " + describe( GetForegroundWindow() ) + ( flags.empty() ? "" : "," + flags );
}


std::string KIPLATFORM::EMBED::WriteMiniDump( const std::string& aWhy )
{
    std::wstring tracefile = tracePath();

    if( tracefile.empty() )
        return "no dump (BACKPLANE_KICAD_EMBED_TRACE unset)";

    typedef BOOL( WINAPI * MINIDUMPWRITEDUMP )( HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                                PMINIDUMP_EXCEPTION_INFORMATION,
                                                PMINIDUMP_USER_STREAM_INFORMATION,
                                                PMINIDUMP_CALLBACK_INFORMATION );

    HMODULE dbghelp = LoadLibraryW( L"dbghelp.dll" );
    auto    writeDump = dbghelp ? reinterpret_cast<MINIDUMPWRITEDUMP>(
                                          GetProcAddress( dbghelp, "MiniDumpWriteDump" ) )
                                : nullptr;

    if( !writeDump )
        return "no dump (dbghelp.dll has no MiniDumpWriteDump)";

    size_t       slash = tracefile.find_last_of( L"\\/" );
    std::wstring dir = slash == std::wstring::npos ? std::wstring() : tracefile.substr( 0, slash + 1 );
    std::wstring path = dir + L"kicad-" + std::to_wstring( GetCurrentProcessId() ) + L"-"
                        + std::to_wstring( GetTickCount() ) + L".dmp";

    HANDLE file = CreateFileW( path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr );

    if( file == INVALID_HANDLE_VALUE )
        return "no dump (could not create it, error " + std::to_string( GetLastError() ) + ")";

    MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>( MiniDumpNormal | MiniDumpWithThreadInfo
                                                     | MiniDumpWithUnloadedModules
                                                     | MiniDumpWithHandleData );

    // Written from this (the watchdog's) thread: the stalled UI thread is caught where it is
    BOOL  ok = writeDump( GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr,
                          nullptr );
    DWORD error = ok ? 0 : GetLastError();
    CloseHandle( file );

    std::string utf8 = wxString( path ).ToStdString( wxConvUTF8 );

    if( !ok )
        return "no dump (MiniDumpWriteDump failed, error " + std::to_string( error ) + ")";

    trace( "minidump for %s: %s", aWhy.c_str(), utf8.c_str() );
    return utf8;
}


uint64_t KIPLATFORM::EMBED::CreateHolder( uint64_t aParent )
{
    HWND parent = reinterpret_cast<HWND>( aParent );

    if( parent && !IsWindow( parent ) )
        return 0;

    // Created parked (a hidden popup that never shows in the taskbar), then moved in
    HWND holder = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, holderClass(), L"",
                                   WS_POPUP | WS_CLIPCHILDREN, PARK_X, PARK_Y, 800, 600, nullptr,
                                   nullptr, GetModuleHandleW( nullptr ), nullptr );

    if( !holder )
        return 0;

    s_holder = holder;
    s_parent = parent;
    SetPropW( holder, FOCUS_CANVAS_PROPERTY, reinterpret_cast<HANDLE>( 1 ) );

    if( parent )
        becomeChildOf( holder, parent );

    SetTimer( holder, HOLDER_TIMER, 100, nullptr );
    trace( "tracing on; holder %s created, parent %s", describe( holder ).c_str(),
           describe( parent ).c_str() );
    return reinterpret_cast<uint64_t>( holder );
}


bool KIPLATFORM::EMBED::AttachHolder( uint64_t aHolder, uint64_t aParent )
{
    HWND holder = reinterpret_cast<HWND>( aHolder );
    HWND parent = reinterpret_cast<HWND>( aParent );

    if( !holder || !IsWindow( holder ) || ( parent && !IsWindow( parent ) ) )
        return false;

    s_parent = parent;

    if( parent )
        becomeChildOf( holder, parent );
    else
        park( holder );

    return true;
}


uint64_t KIPLATFORM::EMBED::PrepareFrame( wxWindow* aFrame, uint64_t aHolder )
{
    if( !aFrame || !aHolder )
        return 0;

    HWND hwnd = static_cast<HWND>( aFrame->GetHWND() );

    if( !hwnd )
        return 0;

    LONG_PTR style = GetWindowLongPtr( hwnd, GWL_STYLE );
    style &= ~( WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX
                | WS_MAXIMIZEBOX );
    style |= WS_CHILD | WS_CLIPCHILDREN;
    SetWindowLongPtr( hwnd, GWL_STYLE, style );

    LONG_PTR exStyle = GetWindowLongPtr( hwnd, GWL_EXSTYLE );
    exStyle &= ~( WS_EX_APPWINDOW | WS_EX_WINDOWEDGE );
    SetWindowLongPtr( hwnd, GWL_EXSTYLE, exStyle );

    HWND holder = reinterpret_cast<HWND>( aHolder );
    SetParent( hwnd, holder );

    // Fill the holder from the start (its client area is the container's): a new frame must
    // not show at its own default position and size until the host resizes it
    RECT client = { 0, 0, 0, 0 };
    GetClientRect( holder, &client );
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;

    if( client.right <= 0 || client.bottom <= 0 )
        flags |= SWP_NOSIZE;

    SetWindowPos( hwnd, nullptr, 0, 0, client.right, client.bottom, flags );

    return reinterpret_cast<uint64_t>( hwnd );
}


uint64_t KIPLATFORM::EMBED::WindowId( wxWindow* aWindow )
{
    return aWindow ? reinterpret_cast<uint64_t>( aWindow->GetHWND() ) : 0;
}


bool KIPLATFORM::EMBED::HasFocus( wxWindow* aFrame )
{
    HWND focus = GetFocus();
    HWND frame = aFrame ? static_cast<HWND>( aFrame->GetHWND() ) : nullptr;

    return frame && focus && ( focus == frame || IsChild( frame, focus ) );
}


void KIPLATFORM::EMBED::TakeFocus( wxWindow* aFrame )
{
    if( !aFrame )
        return;

    HWND frame = static_cast<HWND>( aFrame->GetHWND() );
    HWND before = GetFocus();
    HWND result = SetFocus( frame );
    trace( "TakeFocus %s: focus was %s, SetFocus -> %s (error %lu), focus now %s, fg %s",
           describe( frame ).c_str(), describe( before ).c_str(), describe( result ).c_str(),
           result ? 0 : GetLastError(), describe( GetFocus() ).c_str(),
           describe( GetForegroundWindow() ).c_str() );
}


void KIPLATFORM::EMBED::TakeFocusForClick( wxWindow* aFrame, wxWindow* aClicked )
{
    if( !aFrame )
        return;

    HWND frame = static_cast<HWND>( aFrame->GetHWND() );
    HWND clicked = aClicked ? static_cast<HWND>( aClicked->GetHWND() ) : nullptr;

    // The window clicked (the canvas), when it is inside the frame and takes the keyboard;
    // never the frame itself while its child is handling the click
    if( clicked && clicked != frame && IsChild( frame, clicked ) && aClicked->AcceptsFocus() )
    {
        HWND before = GetFocus();
        HWND result = SetFocus( clicked );
        // SetFocus answers the window that had the focus before; GetFocus after says where it went
        trace( "TakeFocusForClick %s in %s: focus was %s, SetFocus returned %s (error %lu), focus now %s, "
               "capture %s",
               describe( clicked ).c_str(), describe( frame ).c_str(), describe( before ).c_str(),
               describe( result ).c_str(), result ? 0 : GetLastError(), describe( GetFocus() ).c_str(),
               describe( GetCapture() ).c_str() );
        return;
    }

    TakeFocus( aFrame );
}


bool KIPLATFORM::EMBED::ReleaseStrayCapture( wxWindow* aFrame )
{
    if( !aFrame )
        return false;

    HWND frame = static_cast<HWND>( aFrame->GetHWND() );
    bool buttonDown = ( GetKeyState( VK_LBUTTON ) & 0x8000 ) || ( GetKeyState( VK_RBUTTON ) & 0x8000 )
                      || ( GetKeyState( VK_MBUTTON ) & 0x8000 );

    // While a menu is up its modal loop owns the capture (ReleaseCapture fails there): leave it,
    // and say once where Windows put the menu, against what KiCad asked for
    GUITHREADINFO gui = {};
    gui.cbSize = sizeof( gui );

    if( GetGUIThreadInfo( GetCurrentThreadId(), &gui )
        && ( gui.flags & ( GUI_INMENUMODE | GUI_POPUPMENUMODE ) ) )
    {
        traceMenuPlacement( frame );
        return false;
    }

    // The frame itself never needs the capture between clicks (its canvas and sashes take
    // their own); held, every later click goes to it instead of the canvas
    if( GetCapture() == frame && !buttonDown )
    {
        wxWindow* wxHolder = wxWindow::GetCapture();
        BOOL      released = ReleaseCapture();
        DWORD     error = released ? 0 : GetLastError();
        trace( "released a stray capture held by the frame %s: ReleaseCapture %s (error %lu), capture now "
               "%s; wx's capture was %s",
               describe( frame ).c_str(), released ? "ok" : "failed", error,
               describe( GetCapture() ).c_str(),
               wxHolder ? describe( static_cast<HWND>( wxHolder->GetHWND() ) ).c_str() : "none" );
        return true;
    }

    return false;
}


void KIPLATFORM::EMBED::SendHostMessage( uint64_t aTarget, const char*, int aWinMsgOffset,
                                         const long aData[5] )
{
    if( aTarget )
    {
        BOOL posted = PostMessage( reinterpret_cast<HWND>( aTarget ), WM_APP + aWinMsgOffset,
                                   static_cast<WPARAM>( aData[3] ), static_cast<LPARAM>( aData[0] ) );
        trace( "host message WM_APP+0x%x to %s: %s", aWinMsgOffset,
               describe( reinterpret_cast<HWND>( aTarget ) ).c_str(), posted ? "posted" : "failed" );
    }
}


void KIPLATFORM::EMBED::SetFrameGeometry( wxWindow* aFrame, int aX, int aY, int aWidth,
                                          int aHeight )
{
    if( aFrame )
        aFrame->SetSize( aX, aY, std::max( 1, aWidth ), std::max( 1, aHeight ) );
}


bool KIPLATFORM::EMBED::SetPreferDark( int aMode )
{
    // wxMSW's dark mode (3.3+) applies to windows created after it is enabled, so --theme dark
    // (before any frame exists) gives dark chrome throughout; a live change after startup
    // reaches only windows opened later (dialogs, new frames).  It cannot be turned off again.
#if wxCHECK_VERSION( 3, 3, 0 )
    if( aMode == 2 )
        return wxTheApp && wxTheApp->MSWEnableDarkMode( wxApp::DarkMode_Always );

    if( aMode == 0 )
        return wxTheApp && wxTheApp->MSWEnableDarkMode( wxApp::DarkMode_Auto );
#endif

    return false;
}


void KIPLATFORM::EMBED::InstallLoopTimer( std::function<void( double, double )> )
{
}


bool KIPLATFORM::EMBED::IsMapped( wxWindow* aFrame )
{
    // The frame's own visibility, as X11's map_state: IsWindowVisible() is false whenever an
    // ancestor (the host's container) is hidden, and a host may keep it hidden until a frame
    // reports shown
    HWND hwnd = aFrame ? static_cast<HWND>( aFrame->GetHWND() ) : nullptr;
    return hwnd && ( GetWindowLongPtrW( hwnd, GWL_STYLE ) & WS_VISIBLE ) != 0;
}


void KIPLATFORM::EMBED::EnsureMapped( wxWindow* aFrame )
{
    if( aFrame && aFrame->IsShown() )
        ShowWindow( static_cast<HWND>( aFrame->GetHWND() ), SW_SHOWNA );
}


void KIPLATFORM::EMBED::UnmapNative( wxWindow* aFrame )
{
    if( aFrame )
        ShowWindow( static_cast<HWND>( aFrame->GetHWND() ), SW_HIDE );
}


bool KIPLATFORM::EMBED::NativeModalTitle( wxString& )
{
    return false;
}


void KIPLATFORM::EMBED::MakeTransientFor( wxWindow* aDialog, uint64_t aToplevel )
{
    if( !aDialog || !aToplevel )
        return;

    HWND dialog = static_cast<HWND>( aDialog->GetHWND() );
    HWND owner = reinterpret_cast<HWND>( aToplevel );
    SetWindowLongPtr( dialog, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>( owner ) );

    RECT ownerRect, dialogRect;

    if( GetWindowRect( owner, &ownerRect ) && GetWindowRect( dialog, &dialogRect ) )
    {
        int w = dialogRect.right - dialogRect.left;
        int h = dialogRect.bottom - dialogRect.top;
        int x = ownerRect.left + ( ( ownerRect.right - ownerRect.left ) - w ) / 2;
        int y = ownerRect.top + ( ( ownerRect.bottom - ownerRect.top ) - h ) / 2;
        SetWindowPos( dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    }
}
