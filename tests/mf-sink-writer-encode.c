/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Write uncompressed frames to an H.264 MP4 through the sink writer, as
 * PowerPoint's video export does: the writer has to put a converter and an
 * encoder in front of the sink and finalize the file. Then read it back.
 * Usage: mf-sink-writer-encode.exe [async] */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>

#ifndef WIDTH
#define WIDTH 320
#endif
#ifndef HEIGHT
#define HEIGHT 240
#endif
#define FRAMES 60

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

struct callback { IMFSinkWriterCallback iface; HANDLE done; HRESULT hr; };
static HRESULT WINAPI cb_query(IMFSinkWriterCallback *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMFSinkWriterCallback)) { *out = iface; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI cb_addref(IMFSinkWriterCallback *iface) { return 2; }
static ULONG WINAPI cb_release(IMFSinkWriterCallback *iface) { return 1; }
static HRESULT WINAPI cb_finalize(IMFSinkWriterCallback *iface, HRESULT hr)
{
    struct callback *cb = CONTAINING_RECORD(iface, struct callback, iface);
    cb->hr = hr; SetEvent(cb->done); return S_OK;
}
static HRESULT WINAPI cb_marker(IMFSinkWriterCallback *iface, DWORD index, void *context) { return S_OK; }
static IMFSinkWriterCallbackVtbl cb_vtbl = {cb_query, cb_addref, cb_release, cb_finalize, cb_marker};

/* Frame sizes and ratios are two UINT32s in one UINT64, high part first. */
static void set_pair(IMFMediaType *type, const GUID *key, UINT32 high, UINT32 low)
{
    IMFMediaType_SetUINT64(type, key, (UINT64)high << 32 | low);
}

static IMFMediaType *video_type(const GUID *subtype)
{
    IMFMediaType *type;
    MFCreateMediaType(&type);
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, subtype);
    set_pair(type, &MF_MT_FRAME_SIZE, WIDTH, HEIGHT);
    set_pair(type, &MF_MT_FRAME_RATE, 30, 1);
    set_pair(type, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    IMFMediaType_SetUINT32(type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return type;
}

int main(int argc, char **argv)
{
    BOOL async = argc > 1 && !strcmp(argv[1], "async"), reading = FALSE;
    struct callback cb = {{&cb_vtbl}};
    IMFMediaType *output, *input, *read_type;
    IMFAttributes *attributes = NULL;
    IMFSourceReader *reader;
    IMFSinkWriter *writer;
    IMFMediaBuffer *buffer;
    WCHAR path[MAX_PATH];
    IMFSample *sample;
    DWORD stream, flags, frames = 0;
    LONGLONG time;
    UINT32 width = 0, height = 0;
    HANDLE file;
    LARGE_INTEGER size = {0};
    GUID subtype;
    HRESULT hr;
    BYTE *data;
    int i, j;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    GetTempPathW(MAX_PATH, path); wcscat(path, L"tuxoffice-sink-writer.mp4");
    if (argc > 2 && !strcmp(argv[1], "read"))
    {
        reading = TRUE;
        /* Only read an existing file back. */
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, path, MAX_PATH);
        goto read_back;
    }
    DeleteFileW(path);

    if (async)
    {
        cb.done = CreateEventW(NULL, TRUE, FALSE, NULL);
        MFCreateAttributes(&attributes, 1);
        IMFAttributes_SetUnknown(attributes, &MF_SINK_WRITER_ASYNC_CALLBACK, (IUnknown *)&cb.iface);
    }
    hr = MFCreateSinkWriterFromURL(path, NULL, attributes, &writer);
    check(hr == S_OK, "a sink writer for an MP4 file");
    if (FAILED(hr)) return 1;

    output = video_type(&MFVideoFormat_H264);
    IMFMediaType_SetUINT32(output, &MF_MT_AVG_BITRATE, 800000);
    hr = IMFSinkWriter_AddStream(writer, output, &stream);
    check(hr == S_OK, "an H.264 stream");
    input = video_type(&MFVideoFormat_ARGB32);
    hr = IMFSinkWriter_SetInputMediaType(writer, stream, input, NULL);
    check(hr == S_OK, "ARGB32 input to an H.264 stream");
    hr = IMFSinkWriter_BeginWriting(writer);
    check(hr == S_OK, "writing begins");

    for (i = 0; i < FRAMES && SUCCEEDED(hr); i++)
    {
        MFCreateMemoryBuffer(WIDTH * HEIGHT * 4, &buffer);
        IMFMediaBuffer_Lock(buffer, &data, NULL, NULL);
        for (j = 0; j < WIDTH * HEIGHT; j++)
            ((DWORD *)data)[j] = 0xff000000 | ((j % WIDTH + i * 4) & 0xff) << 16 | (j / WIDTH & 0xff) << 8 | (i * 4 & 0xff);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_SetCurrentLength(buffer, WIDTH * HEIGHT * 4);
        MFCreateSample(&sample);
        IMFSample_AddBuffer(sample, buffer);
        IMFSample_SetSampleTime(sample, i * 333333LL);
        IMFSample_SetSampleDuration(sample, 333333);
        hr = IMFSinkWriter_WriteSample(writer, stream, sample);
        IMFSample_Release(sample);
        IMFMediaBuffer_Release(buffer);
    }
    check(hr == S_OK, "60 frames written");

    hr = IMFSinkWriter_Finalize(writer);
    if (async && SUCCEEDED(hr))
    {
        check(WaitForSingleObject(cb.done, 60000) == WAIT_OBJECT_0, "OnFinalize is called");
        hr = cb.hr;
    }
    if (FAILED(hr)) printf("Finalize hr %#lx\n", hr);
    check(hr == S_OK, "the file is finalized");
    IMFSinkWriter_Release(writer);

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) { GetFileSizeEx(file, &size); CloseHandle(file); }
    printf("file size %lld\n", size.QuadPart);
    check(size.QuadPart > 1000, "the file has content");

read_back:
    hr = MFCreateSourceReaderFromURL(path, NULL, &reader);
    if (FAILED(hr)) printf("MFCreateSourceReaderFromURL hr %#lx\n", hr);
    check(hr == S_OK, "the file opens again");
    if (SUCCEEDED(hr))
    {
        IMFSourceReader_GetNativeMediaType(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &read_type);
        IMFMediaType_GetGUID(read_type, &MF_MT_MAJOR_TYPE, &subtype);
        {
            UINT64 frame_size = 0;
            IMFMediaType_GetUINT64(read_type, &MF_MT_FRAME_SIZE, &frame_size);
            width = frame_size >> 32; height = (UINT32)frame_size;
        }
        IMFMediaType_Release(read_type);
        /* Wine's fallback source reports the decoded format, Windows H.264. */
        check(IsEqualGUID(&subtype, &MFMediaType_Video), "it holds video");
        printf("frame size %ux%u\n", width, height);
        check(reading || (width == WIDTH && height == HEIGHT), "at the frame size written");
        for (;;)
        {
            sample = NULL;
            hr = IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &time, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
            if (sample) { frames++; IMFSample_Release(sample); }
        }
        printf("frames read %lu\n", frames);
        check(reading ? frames > 0 : frames == FRAMES, "every frame is there");
        IMFSourceReader_Release(reader);
    }

    if (!reading && !GetEnvironmentVariableA("TUXOFFICE_KEEP_OUTPUT", NULL, 0)) DeleteFileW(path);
    MFShutdown();
    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
