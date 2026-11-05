/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The media engine's timed text: captions and subtitles from WebVTT, SRT and
 * TTML, tracks an application builds, and cue events that follow playback.
 * PowerPoint asks for this service when it plays a video.
 * Usage: mf-timed-text.exe VIDEO.mp4 (a video of at least two seconds) */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mferror.h>
#include <dxgiformat.h>
#include <stdio.h>
#include <math.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) failures++;
}

/* Media engine notifications: only playing and errors matter here. */
static HANDLE playing_event;
static HRESULT WINAPI engine_notify_query(IMFMediaEngineNotify *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMFMediaEngineNotify)) { *out = iface; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI engine_notify_addref(IMFMediaEngineNotify *iface) { return 2; }
static ULONG WINAPI engine_notify_release(IMFMediaEngineNotify *iface) { return 1; }
static HRESULT WINAPI engine_notify_event(IMFMediaEngineNotify *iface, DWORD event, DWORD_PTR p1, DWORD p2)
{
    if (event == MF_MEDIA_ENGINE_EVENT_PLAYING) SetEvent(playing_event);
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) printf("media engine error %Iu %#lx\n", p1, p2);
    return S_OK;
}
static IMFMediaEngineNotifyVtbl engine_notify_vtbl = {engine_notify_query, engine_notify_addref,
        engine_notify_release, engine_notify_event};
static IMFMediaEngineNotify engine_notify = {&engine_notify_vtbl};

/* Timed text notifications, recorded in order. */
struct record { int type; DWORD id; BOOL selected; int cue_event; double time; DWORD cue_id; };
static struct record records[256];
static LONG record_count;
static CRITICAL_SECTION record_cs;
enum { REC_ADDED, REC_REMOVED, REC_SELECTED, REC_READY, REC_ERROR, REC_CUE, REC_RESET };

static void add_record(struct record r)
{
    EnterCriticalSection(&record_cs);
    if (record_count < ARRAYSIZE(records)) records[record_count++] = r;
    LeaveCriticalSection(&record_cs);
}

static HRESULT WINAPI tt_notify_query(IMFTimedTextNotify *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMFTimedTextNotify)) { *out = iface; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI tt_notify_addref(IMFTimedTextNotify *iface) { return 2; }
static ULONG WINAPI tt_notify_release(IMFTimedTextNotify *iface) { return 1; }
static void WINAPI tt_notify_added(IMFTimedTextNotify *iface, DWORD id) { add_record((struct record){REC_ADDED, id}); }
static void WINAPI tt_notify_removed(IMFTimedTextNotify *iface, DWORD id) { add_record((struct record){REC_REMOVED, id}); }
static void WINAPI tt_notify_selected(IMFTimedTextNotify *iface, DWORD id, BOOL selected)
{
    add_record((struct record){REC_SELECTED, id, selected});
}
static void WINAPI tt_notify_ready(IMFTimedTextNotify *iface, DWORD id) { add_record((struct record){REC_READY, id}); }
static void WINAPI tt_notify_error(IMFTimedTextNotify *iface, MF_TIMED_TEXT_ERROR_CODE code, HRESULT hr, DWORD id)
{
    add_record((struct record){REC_ERROR, id});
}
static void WINAPI tt_notify_cue(IMFTimedTextNotify *iface, MF_TIMED_TEXT_CUE_EVENT event, double time,
        IMFTimedTextCue *cue)
{
    add_record((struct record){REC_CUE, 0, 0, event, time, cue ? IMFTimedTextCue_GetId(cue) : 0});
}
static void WINAPI tt_notify_reset(IMFTimedTextNotify *iface) { add_record((struct record){REC_RESET}); }
static IMFTimedTextNotifyVtbl tt_notify_vtbl = {tt_notify_query, tt_notify_addref, tt_notify_release,
        tt_notify_added, tt_notify_removed, tt_notify_selected, tt_notify_ready, tt_notify_error, tt_notify_cue,
        tt_notify_reset};
static IMFTimedTextNotify tt_notify = {&tt_notify_vtbl};

static BOOL wait_for_record(int type, DWORD id, DWORD timeout)
{
    DWORD start = GetTickCount();
    LONG i;

    do
    {
        EnterCriticalSection(&record_cs);
        for (i = 0; i < record_count; ++i)
            if (records[i].type == type && (!id || records[i].id == id)) { LeaveCriticalSection(&record_cs); return TRUE; }
        LeaveCriticalSection(&record_cs);
        Sleep(10);
    } while (GetTickCount() - start < timeout);
    return FALSE;
}

static IMFByteStream *stream_from_text(const char *text)
{
    IMFByteStream *bytestream;
    IStream *stream;
    HGLOBAL memory;
    size_t size = strlen(text);

    memory = GlobalAlloc(GMEM_MOVEABLE, size);
    memcpy(GlobalLock(memory), text, size);
    GlobalUnlock(memory);
    CreateStreamOnHGlobal(memory, TRUE, &stream);
    IStream_SetSize(stream, (ULARGE_INTEGER){.QuadPart = size});
    MFCreateMFByteStreamOnStream(stream, &bytestream);
    IStream_Release(stream);
    return bytestream;
}

static BOOL line_is(IMFTimedTextCue *cue, DWORD index, const WCHAR *expected)
{
    IMFTimedTextFormattedText *line;
    WCHAR *text = NULL;
    BOOL ret;

    if (FAILED(IMFTimedTextCue_GetLine(cue, index, &line))) return FALSE;
    IMFTimedTextFormattedText_GetText(line, &text);
    ret = text && !wcscmp(text, expected);
    if (!ret) printf("  line %lu is %ls\n", index, text ? text : L"(null)");
    CoTaskMemFree(text);
    IMFTimedTextFormattedText_Release(line);
    return ret;
}

static IMFTimedTextTrack *get_track(IMFTimedText *tt, DWORD id)
{
    IMFTimedTextTrackList *list;
    IMFTimedTextTrack *track = NULL;

    if (SUCCEEDED(IMFTimedText_GetTracks(tt, &list)))
    {
        IMFTimedTextTrackList_GetTrackById(list, id, &track);
        IMFTimedTextTrackList_Release(list);
    }
    return track;
}

static IMFTimedTextCue *get_cue(IMFTimedTextTrack *track, DWORD index)
{
    IMFTimedTextCueList *cues;
    IMFTimedTextCue *cue = NULL;

    if (SUCCEEDED(IMFTimedTextTrack_GetCueList(track, &cues)))
    {
        IMFTimedTextCueList_GetCueByIndex(cues, index, &cue);
        IMFTimedTextCueList_Release(cues);
    }
    return cue;
}

static const char vtt[] =
    "WEBVTT - TuxOffice test\n"
    "\n"
    "NOTE comments are skipped\n"
    "\n"
    "REGION\n"
    "id:top\n"
    "width:40%\n"
    "lines:2\n"
    "viewportanchor:10%,20%\n"
    "\n"
    "first\n"
    "00:00.300 --> 00:00.900 align:start line:10%\n"
    "Hello <b>bold</b> and <i>italic</i>\n"
    "\n"
    "00:01.200 --> 00:01.800 region:top\n"
    "Second &amp; <u>line one</u>\n"
    "line two\n";

static const char srt[] =
    "1\r\n"
    "00:00:02,000 --> 00:00:03,500\r\n"
    "SRT <font color=\"#ff0000\">red</font> text\r\n"
    "\r\n"
    "2\r\n"
    "00:00:04,000 --> 00:00:05,000\r\n"
    "Last\r\n";

static const char ttml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\">\n"
    " <head>\n"
    "  <styling><style xml:id=\"s1\" tts:color=\"yellow\" tts:fontWeight=\"bold\"/></styling>\n"
    "  <layout><region xml:id=\"bottom\" tts:origin=\"10% 80%\" tts:extent=\"80% 15%\" tts:displayAlign=\"after\"/></layout>\n"
    " </head>\n"
    " <body region=\"bottom\">\n"
    "  <div begin=\"1s\">\n"
    "   <p begin=\"0.5s\" end=\"2s\" style=\"s1\">TTML first<br/>and <span tts:fontStyle=\"italic\">second</span></p>\n"
    "   <p begin=\"00:00:03.000\" dur=\"500ms\">Later</p>\n"
    "  </div>\n"
    " </body>\n"
    "</tt>\n";

int main(int argc, char **argv)
{
    IMFMediaEngineClassFactory *factory;
    IMFTimedTextTrack *track, *added;
    IMFTimedTextFormattedText *line;
    IMFTimedTextCueList *cues;
    IMFTimedTextTrackList *list;
    IMFTimedTextRegion *region;
    IMFTimedTextStyle *style;
    IMFTimedTextBinary *binary;
    IMFTimedTextCue *cue;
    IMFMediaEngine *engine;
    IMFAttributes *attributes;
    IMFByteStream *stream;
    IMFGetService *gs;
    IMFTimedText *tt;
    MF_TIMED_TEXT_UNIT_TYPE unit;
    MF_TIMED_TEXT_ALIGNMENT alignment;
    MF_TIMED_TEXT_DISPLAY_ALIGNMENT display;
    MF_TIMED_TEXT_FONT_STYLE font_style;
    DWORD vtt_id = 0, srt_id = 0, ttml_id = 0, bad_id = 0, first, length, decoration;
    double x, y, w, h;
    const BYTE *data;
    WCHAR *str, url[MAX_PATH];
    BOOL bold;
    MFARGB color;
    GUID format;
    HRESULT hr;
    LONG i;

    InitializeCriticalSection(&record_cs);
    playing_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    hr = CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER,
            &IID_IMFMediaEngineClassFactory, (void **)&factory);
    check(hr == S_OK, "media engine factory");
    if (FAILED(hr)) return 1;
    MFCreateAttributes(&attributes, 2);
    IMFAttributes_SetUnknown(attributes, &MF_MEDIA_ENGINE_CALLBACK, (IUnknown *)&engine_notify);
    IMFAttributes_SetUINT32(attributes, &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    hr = IMFMediaEngineClassFactory_CreateInstance(factory, 0, attributes, &engine);
    check(hr == S_OK, "media engine");
    if (FAILED(hr)) return 1;

    IMFMediaEngine_QueryInterface(engine, &IID_IMFGetService, (void **)&gs);
    hr = IMFGetService_GetService(gs, &MF_MEDIA_ENGINE_TIMEDTEXT, &IID_IMFTimedText, (void **)&tt);
    check(hr == S_OK, "timed text service");
    if (FAILED(hr)) return 1;
    check(IMFTimedText_RegisterNotifications(tt, &tt_notify) == S_OK, "notifications registered");

    /* WebVTT, the default caption track. */
    stream = stream_from_text(vtt);
    hr = IMFTimedText_AddDataSource(tt, stream, L"English", L"en", MF_TIMED_TEXT_TRACK_KIND_CAPTIONS, TRUE, &vtt_id);
    IMFByteStream_Release(stream);
    check(hr == S_OK && vtt_id, "WebVTT source added");
    check(wait_for_record(REC_ADDED, vtt_id, 2000), "track added notified");
    check(wait_for_record(REC_SELECTED, vtt_id, 2000), "default track selected");
    track = get_track(tt, vtt_id);
    check(track && IMFTimedTextTrack_GetReadyState(track) == MF_TIMED_TEXT_TRACK_READY_STATE_LOADED, "WebVTT loaded");
    check(IMFTimedTextTrack_GetDataFormat(track, &format) == S_OK && IsEqualGUID(&format, &MFSubtitleFormat_WebVTT),
            "WebVTT format reported");
    check(IMFTimedTextTrack_GetTrackKind(track) == MF_TIMED_TEXT_TRACK_KIND_CAPTIONS && IMFTimedTextTrack_IsActive(track),
            "kind and active state");
    IMFTimedTextTrack_GetLabel(track, &str); check(!wcscmp(str, L"English"), "label"); CoTaskMemFree(str);
    IMFTimedTextTrack_GetLanguage(track, &str); check(!wcscmp(str, L"en"), "language"); CoTaskMemFree(str);
    IMFTimedTextTrack_GetCueList(track, &cues);
    check(IMFTimedTextCueList_GetLength(cues) == 2, "two WebVTT cues (NOTE and REGION skipped)");
    IMFTimedTextCueList_GetCueByOriginalId(cues, L"first", &cue);
    check(cue != NULL, "cue found by its identifier");
    check(fabs(IMFTimedTextCue_GetStartTime(cue) - 0.3) < 1e-6 && fabs(IMFTimedTextCue_GetDuration(cue) - 0.6) < 1e-6,
            "cue timing");
    check(IMFTimedTextCue_GetTrackId(cue) == vtt_id && IMFTimedTextCue_GetCueKind(cue) == MF_TIMED_TEXT_TRACK_KIND_CAPTIONS,
            "cue track and kind");
    check(IMFTimedTextCue_GetLineCount(cue) == 1 && line_is(cue, 0, L"Hello bold and italic"), "markup removed from text");
    IMFTimedTextCue_GetLine(cue, 0, &line);
    check(IMFTimedTextFormattedText_GetSubformattingCount(line) == 2, "two styled runs");
    IMFTimedTextFormattedText_GetSubformatting(line, 0, &first, &length, &style);
    IMFTimedTextStyle_GetBold(style, &bold);
    check(first == 6 && length == 4 && bold, "bold run");
    IMFTimedTextStyle_Release(style);
    IMFTimedTextFormattedText_GetSubformatting(line, 1, &first, &length, &style);
    IMFTimedTextStyle_GetFontStyle(style, &font_style);
    check(first == 15 && length == 6 && font_style == MF_TIMED_TEXT_FONT_STYLE_ITALIC, "italic run");
    IMFTimedTextStyle_Release(style);
    IMFTimedTextFormattedText_Release(line);
    IMFTimedTextCue_GetStyle(cue, &style);
    IMFTimedTextStyle_GetTextAlignment(style, &alignment);
    check(alignment == MF_TIMED_TEXT_ALIGNMENT_START, "align:start");
    IMFTimedTextStyle_Release(style);
    IMFTimedTextCue_GetRegion(cue, &region);
    IMFTimedTextRegion_GetPosition(region, &x, &y, &unit);
    IMFTimedTextRegion_GetDisplayAlignment(region, &display);
    check(fabs(y - 10.0) < 1e-6 && unit == MF_TIMED_TEXT_UNIT_TYPE_PERCENTAGE && display == MF_TIMED_TEXT_DISPLAY_ALIGNMENT_BEFORE,
            "line:10% places the cue");
    IMFTimedTextRegion_Release(region);
    IMFTimedTextCue_Release(cue);
    IMFTimedTextCueList_GetCueByIndex(cues, 1, &cue);
    check(IMFTimedTextCue_GetLineCount(cue) == 2 && line_is(cue, 0, L"Second & line one") && line_is(cue, 1, L"line two"),
            "entities and two lines");
    IMFTimedTextCue_GetLine(cue, 0, &line);
    IMFTimedTextFormattedText_GetSubformatting(line, 0, &first, &length, &style);
    IMFTimedTextStyle_GetTextDecoration(style, &decoration);
    check(first == 9 && length == 8 && (decoration & MF_TIMED_TEXT_DECORATION_UNDERLINE), "underline run");
    IMFTimedTextStyle_Release(style);
    IMFTimedTextFormattedText_Release(line);
    IMFTimedTextCue_GetRegion(cue, &region);
    IMFTimedTextRegion_GetName(region, &str);
    IMFTimedTextRegion_GetExtent(region, &w, &h, &unit);
    check(!wcscmp(str, L"top") && fabs(w - 40.0) < 1e-6, "REGION block used by region:top");
    CoTaskMemFree(str);
    IMFTimedTextRegion_Release(region);
    IMFTimedTextCue_Release(cue);
    IMFTimedTextCueList_Release(cues);
    IMFTimedTextTrack_Release(track);

    /* SRT subtitles. */
    stream = stream_from_text(srt);
    hr = IMFTimedText_AddDataSource(tt, stream, L"SRT", L"en", MF_TIMED_TEXT_TRACK_KIND_SUBTITLES, FALSE, &srt_id);
    IMFByteStream_Release(stream);
    track = get_track(tt, srt_id);
    check(hr == S_OK && track && IMFTimedTextTrack_GetDataFormat(track, &format) == S_OK
            && IsEqualGUID(&format, &MFSubtitleFormat_SRT) && !IMFTimedTextTrack_IsActive(track), "SRT loaded, not selected");
    cue = get_cue(track, 0);
    check(cue && fabs(IMFTimedTextCue_GetStartTime(cue) - 2.0) < 1e-6 && fabs(IMFTimedTextCue_GetDuration(cue) - 1.5) < 1e-6
            && line_is(cue, 0, L"SRT red text"), "SRT cue");
    IMFTimedTextCue_GetLine(cue, 0, &line);
    IMFTimedTextFormattedText_GetSubformatting(line, 0, &first, &length, &style);
    IMFTimedTextStyle_GetColor(style, &color);
    check(first == 4 && length == 3 && color.rgbRed == 0xff && !color.rgbGreen && !color.rgbBlue, "SRT font colour");
    IMFTimedTextStyle_Release(style);
    IMFTimedTextFormattedText_Release(line);
    IMFTimedTextCue_Release(cue);
    IMFTimedTextTrack_Release(track);

    /* TTML. */
    stream = stream_from_text(ttml);
    hr = IMFTimedText_AddDataSource(tt, stream, L"TTML", L"en", MF_TIMED_TEXT_TRACK_KIND_SUBTITLES, FALSE, &ttml_id);
    IMFByteStream_Release(stream);
    track = get_track(tt, ttml_id);
    check(hr == S_OK && track && IMFTimedTextTrack_GetReadyState(track) == MF_TIMED_TEXT_TRACK_READY_STATE_LOADED
            && IMFTimedTextTrack_GetDataFormat(track, &format) == S_OK && IsEqualGUID(&format, &MFSubtitleFormat_TTML),
            "TTML loaded");
    cue = get_cue(track, 0);
    if (!cue) { check(0, "TTML has cues"); return 1; }
    check(cue && fabs(IMFTimedTextCue_GetStartTime(cue) - 1.5) < 1e-6 && fabs(IMFTimedTextCue_GetDuration(cue) - 1.5) < 1e-6,
            "TTML times add up through the div");
    check(cue && IMFTimedTextCue_GetLineCount(cue) == 2 && line_is(cue, 0, L"TTML first") && line_is(cue, 1, L"and second"),
            "TTML lines split at br");
    IMFTimedTextCue_GetStyle(cue, &style);
    IMFTimedTextStyle_GetBold(style, &bold);
    IMFTimedTextStyle_GetColor(style, &color);
    check(bold && color.rgbRed == 0xff && color.rgbGreen == 0xff && !color.rgbBlue, "TTML referenced style");
    IMFTimedTextStyle_Release(style);
    IMFTimedTextCue_GetRegion(cue, &region);
    IMFTimedTextRegion_GetPosition(region, &x, &y, &unit);
    IMFTimedTextRegion_GetExtent(region, &w, &h, &unit);
    check(fabs(x - 10.0) < 1e-6 && fabs(y - 80.0) < 1e-6 && fabs(w - 80.0) < 1e-6 && fabs(h - 15.0) < 1e-6,
            "TTML region origin and extent");
    IMFTimedTextRegion_Release(region);
    IMFTimedTextCue_GetLine(cue, 1, &line);
    IMFTimedTextFormattedText_GetSubformatting(line, 0, &first, &length, &style);
    IMFTimedTextStyle_GetFontStyle(style, &font_style);
    check(first == 4 && length == 6 && font_style == MF_TIMED_TEXT_FONT_STYLE_ITALIC, "TTML span style");
    IMFTimedTextStyle_Release(style);
    IMFTimedTextFormattedText_Release(line);
    IMFTimedTextCue_Release(cue);
    cue = get_cue(track, 1);
    check(cue && fabs(IMFTimedTextCue_GetStartTime(cue) - 4.0) < 1e-6 && fabs(IMFTimedTextCue_GetDuration(cue) - 0.5) < 1e-6,
            "TTML clock time and dur");
    if (cue) IMFTimedTextCue_Release(cue);
    IMFTimedTextTrack_Release(track);

    /* Not timed text at all. */
    stream = stream_from_text("just some words");
    IMFTimedText_AddDataSource(tt, stream, L"bad", L"", MF_TIMED_TEXT_TRACK_KIND_SUBTITLES, FALSE, &bad_id);
    IMFByteStream_Release(stream);
    check(wait_for_record(REC_ERROR, bad_id, 2000), "a bad source reports an error");
    track = get_track(tt, bad_id);
    check(track && IMFTimedTextTrack_GetReadyState(track) == MF_TIMED_TEXT_TRACK_READY_STATE_ERROR
            && IMFTimedTextTrack_GetErrorCode(track) == MF_TIMED_TEXT_ERROR_CODE_DATA_FORMAT, "data format error");
    IMFTimedText_RemoveTrack(tt, track);
    check(wait_for_record(REC_REMOVED, bad_id, 2000), "track removed notified");
    IMFTimedTextTrack_Release(track);

    IMFTimedText_GetTextTracks(tt, &list);
    check(IMFTimedTextTrackList_GetLength(list) == 3, "three text tracks");
    IMFTimedTextTrackList_Release(list);
    IMFTimedText_GetActiveTracks(tt, &list);
    check(IMFTimedTextTrackList_GetLength(list) == 1, "one active track");
    IMFTimedTextTrackList_Release(list);

    /* A metadata track the application fills. */
    hr = IMFTimedText_AddTrack(tt, L"chapters", L"", MF_TIMED_TEXT_TRACK_KIND_METADATA, &added);
    check(hr == S_OK, "track added by the application");
    IMFTimedTextTrack_GetCueList(added, &cues);
    hr = IMFTimedTextCueList_AddDataCue(cues, 1.0, 2.0, (const BYTE *)"chapter", 7, &cue);
    IMFTimedTextCue_GetData(cue, &binary);
    IMFTimedTextBinary_GetData(binary, &data, &length);
    check(hr == S_OK && length == 7 && !memcmp(data, "chapter", 7), "data cue");
    IMFTimedTextBinary_Release(binary);
    check(IMFTimedTextCueList_RemoveCue(cues, cue) == S_OK && !IMFTimedTextCueList_GetLength(cues), "cue removed");
    IMFTimedTextCue_Release(cue);
    hr = IMFTimedTextCueList_AddTextCue(cues, 0.5, 1.0, L"added <b>text</b>", &cue);
    check(hr == S_OK && line_is(cue, 0, L"added text"), "text cue added with markup");
    IMFTimedTextCue_Release(cue);
    IMFTimedTextCueList_Release(cues);
    IMFTimedText_GetMetadataTracks(tt, &list);
    check(IMFTimedTextTrackList_GetLength(list) == 1, "one metadata track");
    IMFTimedTextTrackList_Release(list);

    /* Playback: cues of the selected track go active and inactive with the position. */
    if (argc > 1)
    {
        BSTR source;
        DWORD start;
        double active_at = -1, inactive_at = -1, second_at = -1;

        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, url, MAX_PATH);
        source = SysAllocString(url);
        record_count = 0;
        IMFMediaEngine_SetSource(engine, source);
        SysFreeString(source);
        IMFMediaEngine_Play(engine);
        check(WaitForSingleObject(playing_event, 10000) == WAIT_OBJECT_0, "video plays");
        start = GetTickCount();
        while (GetTickCount() - start < 3000)
        {
            /* The frame server wants its frames taken. */
            LONGLONG pts;
            IMFMediaEngine_OnVideoStreamTick(engine, &pts);
            Sleep(20);
        }
        EnterCriticalSection(&record_cs);
        for (i = 0; i < record_count; ++i)
        {
            if (records[i].type != REC_CUE) continue;
            printf("  cue %lu event %d at %.3f\n", records[i].cue_id, records[i].cue_event, records[i].time);
            if (records[i].cue_event == MF_TIMED_TEXT_CUE_EVENT_ACTIVE && active_at < 0) active_at = records[i].time;
            else if (records[i].cue_event == MF_TIMED_TEXT_CUE_EVENT_INACTIVE && inactive_at < 0) inactive_at = records[i].time;
            else if (records[i].cue_event == MF_TIMED_TEXT_CUE_EVENT_ACTIVE && second_at < 0) second_at = records[i].time;
        }
        LeaveCriticalSection(&record_cs);
        check(active_at >= 0.3 && active_at < 0.5, "first cue becomes active at 0.3 s");
        check(inactive_at >= 0.9 && inactive_at < 1.1, "and inactive at 0.9 s");
        check(second_at >= 1.2 && second_at < 1.4, "second cue at 1.2 s");
        IMFMediaEngine_Pause(engine);
    }

    IMFTimedText_SelectTrack(tt, vtt_id, FALSE);
    check(wait_for_record(REC_SELECTED, vtt_id, 2000), "deselecting notified");
    IMFTimedText_Release(tt);
    IMFTimedTextTrack_Release(added);
    IMFGetService_Release(gs);
    IMFMediaEngine_Shutdown(engine);
    IMFMediaEngine_Release(engine);
    IMFAttributes_Release(attributes);
    IMFMediaEngineClassFactory_Release(factory);
    MFShutdown();
    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
