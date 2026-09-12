// Standalone reproducer for a wasm stall seen in a large application: a worker thread doing
// nothing but std::filesystem::copy eventually stops returning, and the main thread stops with
// it. No application code, no framework -- the main thread only has to be in a real browser
// event loop, which emscripten_set_main_loop gives it.
#include <emscripten.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{

#ifndef RUN_SECONDS
#define RUN_SECONDS 480
#endif
#ifndef BALLAST_MIB
#define BALLAST_MIB 100
#endif
#ifndef TOUCH_BALLAST
#define TOUCH_BALLAST 0
#endif
#ifndef SECOND_THREAD_FS
#define SECOND_THREAD_FS 1
#endif
// 0 = both threads use stdio; 1 = the writer uses raw fds; 2 = both do, so no FILE
// lock is taken anywhere. emscripten#20059 blames the stdio FLOCK, so mode 2 is the test.
#ifndef IO_MODE
#define IO_MODE 0
#endif

#if IO_MODE >= 1
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#endif

constexpr int cSeconds = RUN_SECONDS;
constexpr int cStallSeconds = 60;
constexpr int cFileKiB = 100;
/// the application stalls with a heap around this size
constexpr size_t cBallastMiB = BALLAST_MIB;

std::atomic<long long> gCopies{ 0 };
// a wedged thread cannot report its own stack and nothing can walk it, so each thread
// leaves the syscall it is about to enter here; the watchdog prints both on a stall
std::atomic<int> gCopyPhase{ 0 };
std::atomic<int> gWritePhase{ 0 };

const char* phaseName( int p )
{
    switch ( p )
    {
    case 0: return "start";
    case 1: return "unlink";
    case 2: return "open-src";
    case 3: return "open-dst";
    case 4: return "read";
    case 5: return "write";
    case 6: return "close";
    case 7: return "loop-top";
    default: return "?";
    }
}
std::atomic<bool> gStop{ false };
int gExitCountdown = -1;
std::chrono::steady_clock::time_point gStart;
std::chrono::steady_clock::time_point gLastProgress;
long long gSeen = 0;

int secondsSince( std::chrono::steady_clock::time_point t )
{
    return int( std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t ).count() );
}

void frame()
{
    const long long now = gCopies.load( std::memory_order_relaxed );
    if ( now != gSeen )
    {
        gSeen = now;
        gLastProgress = std::chrono::steady_clock::now();
    }
    else if ( secondsSince( gLastProgress ) >= cStallSeconds )
    {
        std::printf( "STALLED: no copy finished for %d s after %lld copies; copier in %s, writer in %s",
            cStallSeconds, gSeen,
            phaseName( gCopyPhase.load( std::memory_order_relaxed ) ),
            phaseName( gWritePhase.load( std::memory_order_relaxed ) ) );
        std::putchar( 10 );
        std::fflush( stdout );
        emscripten_force_exit( 3 );
    }

    if ( gExitCountdown > 0 )
    {
        if ( --gExitCountdown == 0 )
            emscripten_force_exit( 0 );
        return;
    }

    if ( secondsSince( gStart ) >= cSeconds )
    {
        std::printf( "done: %lld copies in %d s", gSeen, cSeconds );
        std::putchar( 10 );
        std::fflush( stdout );
        // let both threads leave their loops first: exiting with one still running hangs
        gStop.store( true, std::memory_order_release );
        gExitCountdown = 60;
    }
}

} // namespace

int main()
{
    std::printf( "hardware_concurrency %u", std::thread::hardware_concurrency() );
    std::putchar( 10 );
    std::fflush( stdout );

    // a heap the size of the application's: with it and the log writer the stall
    // appears, with either one alone it does not
    char* ballast = nullptr;
    if ( cBallastMiB )
    {
        ballast = static_cast<char*>( std::malloc( cBallastMiB << 20 ) );
        if ( ballast && TOUCH_BALLAST )
            std::memset( ballast, 1, cBallastMiB << 20 );
    }
    std::printf( "ballast %zu MiB %s", cBallastMiB, !cBallastMiB ? "skipped" : ballast ? "allocated" : "FAILED" );
    std::putchar( 10 );
    std::fflush( stdout );

    std::error_code ec;
    const std::filesystem::path dir = "/tmp/repro";
    std::filesystem::create_directories( dir, ec );

    const auto src = dir / "src.bin";
    {
        std::ofstream ofs( src, std::ios::binary );
        const std::string chunk( 1024, 'x' );
        for ( int i = 0; i < cFileKiB; ++i )
            ofs << chunk;
    }

    std::thread( [dir]
    {
        if ( !SECOND_THREAD_FS )
        {
            while ( !gStop.load( std::memory_order_acquire ) )
                std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            return;
        }
#if IO_MODE >= 1
        const int fd = ::open( ( dir / "log.txt" ).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644 );
        if ( fd < 0 )
            return;
        const std::string line = std::string( "[info] a line of about the length the application writes" ) + char( 10 );
        while ( !gStop.load( std::memory_order_acquire ) )
        {
            gWritePhase.store( 5, std::memory_order_relaxed );
            ::write( fd, line.data(), line.size() );
            gWritePhase.store( 7, std::memory_order_relaxed );
        }
        ::close( fd );
#else
        std::ofstream log( dir / "log.txt", std::ios::binary | std::ios::app );
        while ( log && !gStop.load( std::memory_order_acquire ) )
        {
            log << "[info] a line of about the length the application writes";
            log.put( char( 10 ) );
            log.flush();
        }
#endif
    } ).detach();

    std::thread( [src, dir]
    {
        const auto dst = dir / "dst.bin";
#if IO_MODE >= 2
        std::vector<char> buf( 64 * 1024 );
        while ( !gStop.load( std::memory_order_acquire ) )
        {
            gCopyPhase.store( 1, std::memory_order_relaxed );
            ::unlink( dst.c_str() );
            gCopyPhase.store( 2, std::memory_order_relaxed );
            const int in = ::open( src.c_str(), O_RDONLY );
            gCopyPhase.store( 3, std::memory_order_relaxed );
            const int out = ::open( dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644 );
            if ( in >= 0 && out >= 0 )
            {
                for ( ssize_t got = 0; ; )
                {
                    gCopyPhase.store( 4, std::memory_order_relaxed );
                    got = ::read( in, buf.data(), buf.size() );
                    if ( got <= 0 )
                        break;
                    gCopyPhase.store( 5, std::memory_order_relaxed );
                    ::write( out, buf.data(), size_t( got ) );
                }
            }
            gCopyPhase.store( 6, std::memory_order_relaxed );
            if ( in >= 0 )
                ::close( in );
            if ( out >= 0 )
                ::close( out );
            gCopyPhase.store( 7, std::memory_order_relaxed );
            gCopies.fetch_add( 1, std::memory_order_relaxed );
        }
#else
        std::error_code workerEc;
        while ( !gStop.load( std::memory_order_acquire ) )
        {
            std::filesystem::remove( dst, workerEc );
            std::filesystem::copy( src, dst, workerEc );
            gCopies.fetch_add( 1, std::memory_order_relaxed );
        }
#endif
    } ).detach();

    gStart = gLastProgress = std::chrono::steady_clock::now();

    // 0 fps means requestAnimationFrame, and the final 0 means main() returns instead of
    // blocking -- so the main thread really is back in the browser's event loop between frames
    emscripten_set_main_loop( frame, 0, 0 );
    return 0;
}
