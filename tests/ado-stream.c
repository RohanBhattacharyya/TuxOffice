/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Check stream bytes as well as BSTR lengths: C string comparisons miss
 * embedded NULs, and seeking/overwriting must preserve the remaining data. */
#define COBJMACROS
#include <initguid.h>
#include <msado15_backcompat.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MAKE_ADO_HRESULT(error) MAKE_HRESULT(SEVERITY_ERROR, FACILITY_CONTROL, error)

static unsigned int checks, failures;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; \
    printf("FAIL line %u: %s\n", __LINE__, #test); } } while (0)
#define REQUIRE(call) do { HRESULT hr_ = (call); CHECK(hr_ == S_OK); \
    if (hr_ != S_OK) { printf("HRESULT %#lx\n", hr_); return 1; } } while (0)

static void check_text(_Stream *stream, LONG count, const WCHAR *expected, UINT length)
{
    BSTR text = NULL;
    HRESULT hr = _Stream_ReadText(stream, count, &text);
    CHECK(hr == S_OK);
    CHECK(SysStringLen(text) == length);
    if (text && SysStringLen(text) == length)
        CHECK(!memcmp(text, expected, length * sizeof(WCHAR)));
    SysFreeString(text);
}

static void check_bytes(_Stream *stream, const BYTE *expected, LONG length)
{
    VARIANT bytes;
    BYTE *data;
    LONG lower, upper;
    HRESULT hr;
    VariantInit(&bytes);
    hr = _Stream_Read(stream, adReadAll, &bytes);
    CHECK(hr == S_OK);
    CHECK(V_VT(&bytes) == (VT_ARRAY | VT_UI1));
    if (V_VT(&bytes) == (VT_ARRAY | VT_UI1))
    {
        SafeArrayGetLBound(V_ARRAY(&bytes), 1, &lower);
        SafeArrayGetUBound(V_ARRAY(&bytes), 1, &upper);
        CHECK(upper - lower + 1 == length);
        if (upper - lower + 1 == length && SUCCEEDED(SafeArrayAccessData(V_ARRAY(&bytes), (void **)&data)))
        {
            CHECK(!memcmp(data, expected, length));
            SafeArrayUnaccessData(V_ARRAY(&bytes));
        }
    }
    VariantClear(&bytes);
}

static int benchmark(void)
{
    const ULONG length = 4 * 1024 * 1024;
    LARGE_INTEGER frequency, start, written, read;
    VARIANT missing, input, output;
    _Stream *stream;
    BYTE *source, *result;
    LONG upper;
    unsigned int i, sample;
    double write_ms, read_ms;

    REQUIRE(CoInitialize(NULL));
    QueryPerformanceFrequency(&frequency);
    V_VT(&missing) = VT_ERROR;
    V_ERROR(&missing) = DISP_E_PARAMNOTFOUND;
    VariantInit(&input);
    V_VT(&input) = VT_ARRAY | VT_UI1;
    V_ARRAY(&input) = SafeArrayCreateVector(VT_UI1, 0, length);
    REQUIRE(SafeArrayAccessData(V_ARRAY(&input), (void **)&source));
    for (i = 0; i < length; ++i) source[i] = i * 13 % 251;
    REQUIRE(SafeArrayUnaccessData(V_ARRAY(&input)));
    printf("{\"bits\":%u,\"bytes\":%lu,\"samples\":[", (unsigned int)(sizeof(void *) * 8), length);
    for (sample = 0; sample < 5; ++sample)
    {
        REQUIRE(CoCreateInstance(&CLSID_Stream, NULL, CLSCTX_INPROC_SERVER, &IID__Stream, (void **)&stream));
        REQUIRE(_Stream_put_Type(stream, adTypeBinary));
        REQUIRE(_Stream_Open(stream, missing, adModeUnknown, adOpenStreamUnspecified, NULL, NULL));
        QueryPerformanceCounter(&start);
        REQUIRE(_Stream_Write(stream, input));
        QueryPerformanceCounter(&written);
        write_ms = (written.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
        REQUIRE(_Stream_put_Position(stream, 0));
        VariantInit(&output);
        QueryPerformanceCounter(&start); /* exclude the seek from read time */
        REQUIRE(_Stream_Read(stream, adReadAll, &output));
        QueryPerformanceCounter(&read);
        read_ms = (read.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
        REQUIRE(SafeArrayGetUBound(V_ARRAY(&output), 1, &upper));
        CHECK(upper == length - 1);
        REQUIRE(SafeArrayAccessData(V_ARRAY(&input), (void **)&source));
        REQUIRE(SafeArrayAccessData(V_ARRAY(&output), (void **)&result));
        CHECK(!memcmp(source, result, length));
        REQUIRE(SafeArrayUnaccessData(V_ARRAY(&input)));
        REQUIRE(SafeArrayUnaccessData(V_ARRAY(&output)));
        VariantClear(&output);
        REQUIRE(_Stream_Close(stream));
        _Stream_Release(stream);
        printf("%s{\"write_ms\":%.6f,\"read_ms\":%.6f}", sample ? "," : "", write_ms, read_ms);
    }
    printf("],\"failures\":%u}\n", failures);
    VariantClear(&input);
    CoUninitialize();
    return failures != 0;
}

int main(int argc, char **argv)
{
    const BYTE unicode[] = {0xff, 0xfe, 'a', 0, 'b', 0, 'c', 0, 'd', 0};
    const BYTE binary[] = {1, 2, 3, 4}, overwritten[] = {9, 8, 3, 4};
    const BYTE extended[] = {9, 8, 0, 0, 0, 0, 0, 0};
    const WCHAR embedded[] = {'x', 0, 'y'};
    _Stream *stream;
    VARIANT missing, bytes;
    BSTR text;
    ADO_LONGPTR size, position;
    BYTE *data;

    if (argc == 2 && !strcmp(argv[1], "--benchmark")) return benchmark();
    REQUIRE(CoInitialize(NULL));
    REQUIRE(CoCreateInstance(&CLSID_Stream, NULL, CLSCTX_INPROC_SERVER, &IID__Stream, (void **)&stream));
    V_VT(&missing) = VT_ERROR;
    V_ERROR(&missing) = DISP_E_PARAMNOTFOUND;
    REQUIRE(_Stream_Open(stream, missing, adModeUnknown, adOpenStreamUnspecified, NULL, NULL));
    text = SysAllocString(L"abcd");
    REQUIRE(_Stream_WriteText(stream, text, adWriteChar));
    SysFreeString(text);
    REQUIRE(_Stream_get_Size(stream, &size));
    CHECK(size == sizeof(unicode));
    REQUIRE(_Stream_put_Position(stream, 0));
    check_text(stream, 2, L"ab", 2);
    REQUIRE(_Stream_get_Position(stream, &position));
    CHECK(position == 6);
    REQUIRE(_Stream_get_Size(stream, &size));
    CHECK(size == sizeof(unicode));
    check_text(stream, 100, L"cd", 2);
    check_text(stream, adReadAll, L"", 0);
    REQUIRE(_Stream_put_Position(stream, 0));
    REQUIRE(_Stream_put_Type(stream, adTypeBinary));
    check_bytes(stream, unicode, sizeof(unicode));
    REQUIRE(_Stream_put_Position(stream, 0));
    REQUIRE(_Stream_put_Type(stream, adTypeText));
    REQUIRE(_Stream_put_Position(stream, 4));
    text = SysAllocString(L"B");
    REQUIRE(_Stream_WriteText(stream, text, adWriteChar));
    SysFreeString(text);
    REQUIRE(_Stream_get_Size(stream, &size));
    CHECK(size == sizeof(unicode));
    REQUIRE(_Stream_put_Position(stream, 0));
    check_text(stream, adReadAll, L"aBcd", 4);
    text = SysAllocString(L"EF");
    REQUIRE(_Stream_WriteText(stream, text, adWriteChar));
    SysFreeString(text);
    REQUIRE(_Stream_put_Position(stream, 0));
    check_text(stream, adReadAll, L"aBcdEF", 6);
    REQUIRE(_Stream_put_Position(stream, 0));
    text = SysAllocStringLen(embedded, ARRAY_SIZE(embedded));
    REQUIRE(_Stream_WriteText(stream, text, adWriteChar));
    SysFreeString(text);
    REQUIRE(_Stream_SetEOS(stream));
    REQUIRE(_Stream_put_Position(stream, 0));
    check_text(stream, adReadAll, embedded, ARRAY_SIZE(embedded));
    REQUIRE(_Stream_Close(stream));
    text = NULL;
    CHECK(_Stream_ReadText(stream, adReadAll, &text) == MAKE_ADO_HRESULT(adErrObjectClosed));
    SysFreeString(text);
    REQUIRE(_Stream_put_Type(stream, adTypeBinary));
    REQUIRE(_Stream_Open(stream, missing, adModeUnknown, adOpenStreamUnspecified, NULL, NULL));
    VariantInit(&bytes);
    V_VT(&bytes) = VT_ARRAY | VT_UI1;
    V_ARRAY(&bytes) = SafeArrayCreateVector(VT_UI1, 7, sizeof(binary));
    REQUIRE(SafeArrayAccessData(V_ARRAY(&bytes), (void **)&data));
    memcpy(data, binary, sizeof(binary));
    REQUIRE(SafeArrayUnaccessData(V_ARRAY(&bytes)));
    REQUIRE(_Stream_Write(stream, bytes));
    VariantClear(&bytes);
    REQUIRE(_Stream_get_Size(stream, &size));
    CHECK(size == sizeof(binary));
    REQUIRE(_Stream_put_Position(stream, 0));
    check_bytes(stream, binary, sizeof(binary));
    REQUIRE(_Stream_put_Position(stream, 0));
    V_VT(&bytes) = VT_ARRAY | VT_UI1;
    V_ARRAY(&bytes) = SafeArrayCreateVector(VT_UI1, 0, 2);
    REQUIRE(SafeArrayAccessData(V_ARRAY(&bytes), (void **)&data));
    memcpy(data, overwritten, 2);
    REQUIRE(SafeArrayUnaccessData(V_ARRAY(&bytes)));
    REQUIRE(_Stream_Write(stream, bytes));
    VariantClear(&bytes);
    REQUIRE(_Stream_put_Position(stream, 0));
    check_bytes(stream, overwritten, sizeof(overwritten));
    REQUIRE(_Stream_put_Position(stream, 2));
    REQUIRE(_Stream_SetEOS(stream));
    REQUIRE(_Stream_put_Position(stream, 8));
    REQUIRE(_Stream_get_Size(stream, &size));
    CHECK(size == sizeof(extended));
    REQUIRE(_Stream_put_Position(stream, 0));
    check_bytes(stream, extended, sizeof(extended));
    REQUIRE(_Stream_Close(stream));
    _Stream_Release(stream);
    CoUninitialize();
    printf("ADO stream contents: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
