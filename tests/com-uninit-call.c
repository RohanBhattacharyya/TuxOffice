/* A COM call that reaches an apartment thread after it has uninitialized COM.
 *
 * Usage: com-uninit-call.exe
 * Thread A creates a stream object in a single-threaded apartment and
 * marshals it. Thread B, in the multithreaded apartment, calls it while A
 * is not pumping messages, so the call waits in A's apartment window. A then
 * calls CoUninitialize and pumps messages, as Office's UI threads do. The
 * call must fail cleanly instead of crashing thread A.
 */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <stdio.h>

static IStream *marshaled;
static HANDLE ready, called;
static HRESULT call_result = E_PENDING;

static DWORD WINAPI apartment_thread( void *arg )
{
    IStream *object;
    MSG msg;
    DWORD end;

    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    CreateStreamOnHGlobal( NULL, TRUE, &object );
    CreateStreamOnHGlobal( NULL, TRUE, &marshaled );
    CoMarshalInterface( marshaled, &IID_IStream, (IUnknown *)object, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL );
    IStream_Seek( marshaled, (LARGE_INTEGER){{0}}, STREAM_SEEK_SET, NULL );
    SetEvent( ready );

    /* Let the call arrive and wait in this thread's queue. */
    Sleep( 1000 );
    IStream_Release( object );
    CoUninitialize();

    end = GetTickCount() + 2000;
    while (GetTickCount() < end)
    {
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );
        Sleep( 10 );
    }
    return 0;
}

static DWORD WINAPI caller_thread( void *arg )
{
    LARGE_INTEGER zero = {{0}};
    IStream *proxy;
    HRESULT hr;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    WaitForSingleObject( ready, INFINITE );
    hr = CoUnmarshalInterface( marshaled, &IID_IStream, (void **)&proxy );
    if (SUCCEEDED(hr))
    {
        SetEvent( called );
        call_result = IStream_Seek( proxy, zero, STREAM_SEEK_SET, NULL );
        IStream_Release( proxy );
    }
    else call_result = hr;
    CoUninitialize();
    return 0;
}

int main(void)
{
    HANDLE threads[2];
    DWORD wait;

    ready = CreateEventW( NULL, TRUE, FALSE, NULL );
    called = CreateEventW( NULL, TRUE, FALSE, NULL );
    threads[0] = CreateThread( NULL, 0, apartment_thread, NULL, 0, NULL );
    threads[1] = CreateThread( NULL, 0, caller_thread, NULL, 0, NULL );
    wait = WaitForMultipleObjects( 2, threads, TRUE, 10000 );
    printf( "call returned %#lx\n", call_result );
    if (wait == WAIT_TIMEOUT || call_result == E_PENDING)
    {
        printf( "FAILED (the call did not complete)\n" );
        return 1;
    }
    printf( "PASSED (apartment thread survived)\n" );
    return 0;
}
