// A filesystem call on a pthread stops returning, and the main thread stops with it.
//
// Two threads do MEMFS I/O with raw syscalls while the main thread sits in a normal
// requestAnimationFrame loop. Each thread records the call it is about to enter, so a
// stall reports which one never came back.
#include <emscripten.h>
#include <emscripten/proxying.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef RUN_SECONDS
#define RUN_SECONDS 240
#endif

namespace
{

constexpr int cSeconds = RUN_SECONDS;
constexpr int cStallSeconds = 60;
constexpr int cDrainAfterSeconds = 90;
constexpr int cFileKiB = 100;

std::atomic<long long> gCopies{ 0 };
std::atomic<bool> gStop{ false };
std::atomic<int> gCopyPhase{ 0 };
std::atomic<int> gWritePhase{ 0 };

std::chrono::steady_clock::time_point gStart;
std::chrono::steady_clock::time_point gLastProgress;
long long gSeen = 0;
int gExitCountdown = -1;
bool gStalled = false;
bool gDrained = false;
std::chrono::steady_clock::time_point gStallAt;
long long gStalledFrames = 0;

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
    case 6: return "close-src";
    case 7: return "between calls";
    case 8: return "close-dst";
    default: return "?";
    }
}

int secondsSince( std::chrono::steady_clock::time_point t )
{
    return int( std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t ).count() );
}

void frame()
{
    if ( gStalled )
    {
        // every syscall under -pthread is proxied to this thread and the worker waits on a
        // futex for the reply; if running the queue by hand revives them, only the wakeup was lost
        if ( !gDrained && secondsSince( gStallAt ) >= cDrainAfterSeconds )
        {
            gDrained = true;
            const long long before = gCopies.load( std::memory_order_relaxed );
            emscripten_proxy_execute_queue( emscripten_proxy_get_system_queue() );
            std::printf( "DRAIN: executed the system proxying queue by hand, copies %lld -> %lld",
                before, gCopies.load( std::memory_order_relaxed ) );
            std::putchar( 10 );
            std::fflush( stdout );
        }
        if ( ++gStalledFrames % 600 == 0 )
        {
            std::printf( "still stalled, main loop alive, %lld copies%s", gCopies.load( std::memory_order_relaxed ), gDrained ? " (after drain)" : "" );
            std::putchar( 10 );
            std::fflush( stdout );
        }
        return;
    }

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
        // deliberately do NOT exit: the harness now SIGTERMs Firefox so the Gecko profiler
        // dumps every thread's stack while the wedge is still there
        gStalled = true;
        gStallAt = std::chrono::steady_clock::now();
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
        // let both threads leave their loops: exiting with one still running hangs
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

    std::error_code ec;
    const std::filesystem::path dir = "/tmp/repro";
    std::filesystem::create_directories( dir, ec );

    const auto src = dir / "src.bin";
    {
        const int fd = ::open( src.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644 );
        const std::string chunk( 1024, 'x' );
        for ( int i = 0; i < cFileKiB; ++i )
            ::write( fd, chunk.data(), chunk.size() );
        ::close( fd );
    }

    std::thread( [dir]
    {
        const int fd = ::open( ( dir / "log.txt" ).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644 );
        if ( fd < 0 )
            return;
        const std::string line = std::string( "[info] a line of about the length an application logs" ) + char( 10 );
        while ( !gStop.load( std::memory_order_acquire ) )
        {
            gWritePhase.store( 5, std::memory_order_relaxed );
            ::write( fd, line.data(), line.size() );
            gWritePhase.store( 7, std::memory_order_relaxed );
        }
        ::close( fd );
    } ).detach();

    std::thread( [src, dir]
    {
        const auto dst = dir / "dst.bin";
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
            gCopyPhase.store( 8, std::memory_order_relaxed );
            if ( out >= 0 )
                ::close( out );
            gCopyPhase.store( 7, std::memory_order_relaxed );
            gCopies.fetch_add( 1, std::memory_order_relaxed );
        }
    } ).detach();

    gStart = gLastProgress = std::chrono::steady_clock::now();

    // 0 fps means requestAnimationFrame, and the trailing 0 means main() returns rather
    // than blocking, so the main thread really is back in the event loop between frames
    emscripten_set_main_loop( frame, 0, 0 );
    return 0;
}
