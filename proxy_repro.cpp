// Does the wedge need the filesystem at all?
//
// Under -pthread every syscall is proxied synchronously to the main thread, and the
// calling thread waits on a futex for the reply. This program cuts the filesystem out
// and proxies a no-op function instead, through the very same system queue. If it still
// stops, the bug is in the proxying machinery and MEMFS was only ever the messenger.
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <pthread.h>

#include <thread>

#ifndef RUN_SECONDS
#define RUN_SECONDS 240
#endif

// MODE 0: emscripten_proxy_sync. MODE 2: a proxied SYSCALL that touches no file data
// (open of a path that does not exist), which adds the JS proxying layer and the FS
// lookup on the other end. MODE 1: emscripten_proxy_async plus our own
// mutex/condvar, which splits "the task never ran" from "it ran and the wake was lost".
#ifndef MODE
#define MODE 0
#endif

#ifndef WORKERS
#define WORKERS 2
#endif

namespace
{

constexpr int cSeconds = RUN_SECONDS;
constexpr int cStallSeconds = 60;
constexpr int cDrainAfterSeconds = 90;

std::atomic<long long> gCalls{ 0 };
std::atomic<bool> gStop{ false };
std::atomic<int> gWaiting{ 0 };

std::chrono::steady_clock::time_point gStart;
std::chrono::steady_clock::time_point gLastProgress;
std::chrono::steady_clock::time_point gStallAt;
long long gSeen = 0;
int gExitCountdown = -1;
bool gStalled = false;
bool gDrained = false;
long long gStalledFrames = 0;

std::atomic<long long> gRan{ 0 };

void noop( void* )
{
    gRan.fetch_add( 1, std::memory_order_relaxed );
}

#if MODE == 1

struct Slot
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool done = false;
};

// runs on the main thread
void finish( void* p )
{
    Slot& s = *static_cast< Slot* >( p );
    gRan.fetch_add( 1, std::memory_order_relaxed );
    pthread_mutex_lock( &s.mutex );
    s.done = true;
    pthread_cond_signal( &s.cond );
    pthread_mutex_unlock( &s.mutex );
}

#endif

int secondsSince( std::chrono::steady_clock::time_point t )
{
    return int( std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t ).count() );
}

void frame()
{
    if ( gStalled )
    {
        if ( !gDrained && secondsSince( gStallAt ) >= cDrainAfterSeconds )
        {
            gDrained = true;
            const long long before = gCalls.load( std::memory_order_relaxed );
            emscripten_proxy_execute_queue( emscripten_proxy_get_system_queue() );
            std::printf( "DRAIN: ran the system queue by hand, calls %lld -> %lld, executed %lld",
                before, gCalls.load( std::memory_order_relaxed ), gRan.load( std::memory_order_relaxed ) );
            std::putchar( 10 );
            std::fflush( stdout );
        }
        if ( ++gStalledFrames % 600 == 0 )
        {
            std::printf( "still stalled, main loop alive, %lld calls, %d waiting%s",
                gCalls.load( std::memory_order_relaxed ), gWaiting.load( std::memory_order_relaxed ),
                gDrained ? " (after drain)" : "" );
            std::putchar( 10 );
            std::fflush( stdout );
        }
        return;
    }

    const long long now = gCalls.load( std::memory_order_relaxed );
    if ( now != gSeen )
    {
        gSeen = now;
        gLastProgress = std::chrono::steady_clock::now();
    }
    else if ( secondsSince( gLastProgress ) >= cStallSeconds )
    {
        std::printf( "STALLED: no proxied call finished for %d s after %lld calls; %d threads waiting, %lld ran on this thread",
            cStallSeconds, gSeen, gWaiting.load( std::memory_order_relaxed ),
            gRan.load( std::memory_order_relaxed ) );
        std::putchar( 10 );
        std::fflush( stdout );
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
        std::printf( "done: %lld proxied calls in %d s", gSeen, cSeconds );
        std::putchar( 10 );
        std::fflush( stdout );
        gStop.store( true, std::memory_order_release );
        gExitCountdown = 60;
    }
}

} // namespace

int main()
{
    std::printf( "hardware_concurrency %u, workers %d, mode %d",
        std::thread::hardware_concurrency(), int( WORKERS ), int( MODE ) );
    std::putchar( 10 );
    std::fflush( stdout );

    const pthread_t mainThread = emscripten_main_runtime_thread_id();

    for ( int i = 0; i < WORKERS; ++i )
    {
        std::thread( [mainThread]
        {
            em_proxying_queue* q = emscripten_proxy_get_system_queue();
            while ( !gStop.load( std::memory_order_acquire ) )
            {
#if MODE == 0
                gWaiting.fetch_add( 1, std::memory_order_relaxed );
                emscripten_proxy_sync( q, mainThread, noop, nullptr );
                gWaiting.fetch_sub( 1, std::memory_order_relaxed );
#elif MODE == 2
                gWaiting.fetch_add( 1, std::memory_order_relaxed );
                const int fd = ::open( "/no/such/file", O_RDONLY );
                if ( fd >= 0 )
                    ::close( fd );
                gWaiting.fetch_sub( 1, std::memory_order_relaxed );
#else
                Slot slot;
                gWaiting.fetch_add( 1, std::memory_order_relaxed );
                if ( !emscripten_proxy_async( q, mainThread, finish, &slot ) )
                    break;
                pthread_mutex_lock( &slot.mutex );
                while ( !slot.done )
                    pthread_cond_wait( &slot.cond, &slot.mutex );
                pthread_mutex_unlock( &slot.mutex );
                gWaiting.fetch_sub( 1, std::memory_order_relaxed );
#endif
                gCalls.fetch_add( 1, std::memory_order_relaxed );
            }
        } ).detach();
    }

    gStart = gLastProgress = std::chrono::steady_clock::now();
    emscripten_set_main_loop( frame, 0, 0 );
    return 0;
}
