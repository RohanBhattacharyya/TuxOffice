/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Media Foundation capture devices, as PowerPoint's recording uses them:
 * enumerate microphones and cameras, create their sources directly and through
 * an activation object, and read live samples through a source reader.
 * Run it against a private PipeWire with a test microphone, not real hardware.
 * Build: winegcc -b x86_64-windows -mconsole -I$BD/include -I wine/include
 *        -L$BD/dlls/mf/x86_64-windows -L$BD/libs/mfuuid/x86_64-windows
 *        -o mf-capture.exe mf-capture.c -lmf -lmfplat -lmfreadwrite -lmfuuid -lole32 -luuid */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <math.h>
#include <stdio.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* Reads about a second of audio; reports the loudest 16-bit or float sample. */
static void read_audio(IMFMediaSource *source, BOOL pcm16, const char *what)
{
    IMFSourceReader *reader;
    IMFMediaType *type;
    LONGLONG time, last = -1, first = -1;
    DWORD flags, total = 0, samples = 0, ordered = 1;
    float peak = 0;
    char line[200];
    HRESULT hr;

    hr = MFCreateSourceReaderFromMediaSource(source, NULL, &reader);
    sprintf(line, "a source reader on the %s", what);
    check(hr == S_OK, line);
    if (FAILED(hr)) return;
    if (pcm16)
    {
        MFCreateMediaType(&type);
        IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = IMFSourceReader_SetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, type);
        IMFMediaType_Release(type);
        check(hr == S_OK, "16-bit PCM is offered");
    }
    while (total < 48000 * 1.0 && samples < 400)
    {
        IMFMediaBuffer *buffer;
        IMFSample *sample = NULL;
        DWORD length, i;
        BYTE *data;

        hr = IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, &time, &sample);
        if (FAILED(hr) || !sample) break;
        if (first < 0) first = time;
        if (time < last) ordered = 0;
        last = time;
        IMFSample_ConvertToContiguousBuffer(sample, &buffer);
        IMFMediaBuffer_Lock(buffer, &data, NULL, &length);
        if (pcm16)
            for (i = 0; i + 1 < length; i += 2) { float v = abs(*(short *)(data + i)) / 32768.0f; if (v > peak) peak = v; }
        else
            for (i = 0; i + 3 < length; i += 4) { float v = fabsf(*(float *)(data + i)); if (v > peak) peak = v; }
        total += length / (pcm16 ? 4 : 8);
        IMFMediaBuffer_Unlock(buffer);
        IMFMediaBuffer_Release(buffer);
        IMFSample_Release(sample);
        samples++;
    }
    printf("  %lu samples, %lu frames, first %lld last %lld, peak %.2f, hr %#lx\n", samples, total, first, last, peak, hr);
    sprintf(line, "a second of %s audio arrives", what);
    check(hr == S_OK && total >= 48000 * 0.9, line);
    check(ordered && first >= 0 && first < 2000000, "timestamps start near zero and rise");
    check(peak > 0.25f, "and carry the test tone");
    IMFSourceReader_Release(reader);
}

int main(void)
{
    IMFAttributes *attributes;
    IMFActivate **devices = NULL, *activate;
    IMFMediaSource *source;
    UINT32 count = 0, i, size;
    WCHAR *name, *id;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    MFCreateAttributes(&attributes, 1);
    IMFAttributes_SetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
    hr = MFEnumDeviceSources(attributes, &devices, &count);
    check(hr == S_OK && count >= 1, "microphones are listed");
    for (i = 0; i < count; i++)
    {
        IMFActivate_GetAllocatedString(devices[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &size);
        printf("  microphone %u: %ls\n", i, name);
        CoTaskMemFree(name);
    }
    if (!count) return 1;

    hr = IMFActivate_ActivateObject(devices[0], &IID_IMFMediaSource, (void **)&source);
    check(hr == S_OK, "a microphone's activation object makes its source");
    if (SUCCEEDED(hr))
    {
        DWORD characteristics = 0;
        IMFMediaSource_GetCharacteristics(source, &characteristics);
        check(characteristics & MFMEDIASOURCE_IS_LIVE, "the source is live");
        read_audio(source, FALSE, "mix format");
        IMFActivate_ShutdownObject(devices[0]);
        IMFMediaSource_Release(source);
    }

    IMFActivate_GetAllocatedString(devices[0], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, &id, &size);
    IMFAttributes_SetString(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, id);
    hr = MFCreateDeviceSource(attributes, &source);
    check(hr == S_OK, "MFCreateDeviceSource makes the same microphone's source");
    if (SUCCEEDED(hr))
    {
        read_audio(source, TRUE, "16-bit");
        IMFMediaSource_Shutdown(source);
        IMFMediaSource_Release(source);
    }
    hr = MFCreateDeviceSourceActivate(attributes, &activate);
    check(hr == S_OK, "MFCreateDeviceSourceActivate");
    if (SUCCEEDED(hr))
    {
        hr = IMFActivate_ActivateObject(activate, &IID_IMFMediaSource, (void **)&source);
        check(hr == S_OK, "and its source");
        if (SUCCEEDED(hr)) { IMFActivate_ShutdownObject(activate); IMFMediaSource_Release(source); }
        IMFActivate_Release(activate);
    }
    CoTaskMemFree(id);
    for (i = 0; i < count; i++) IMFActivate_Release(devices[i]);
    CoTaskMemFree(devices);

    IMFAttributes_DeleteAllItems(attributes);
    IMFAttributes_SetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    count = 0;
    hr = MFEnumDeviceSources(attributes, &devices, &count);
    check(hr == S_OK, "cameras can be listed");
    for (i = 0; i < count; i++)
    {
        IMFActivate_GetAllocatedString(devices[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &size);
        IMFActivate_GetAllocatedString(devices[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &id, &size);
        printf("  camera %u: %ls, %ls\n", i, name, id);
        CoTaskMemFree(name);
        CoTaskMemFree(id);
        IMFActivate_Release(devices[i]);
    }
    CoTaskMemFree(devices);
    IMFAttributes_SetString(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, L"\\\\?\\no-such-camera");
    hr = MFCreateDeviceSource(attributes, &source);
    check(FAILED(hr), "a camera that is not there gives no source");

    IMFAttributes_Release(attributes);
    MFShutdown();
    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
