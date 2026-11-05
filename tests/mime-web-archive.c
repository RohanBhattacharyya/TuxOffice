/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* inetcomm as Office uses it for Single File Web Pages (MHT): the property
 * schema class Office checks for, parts found by URL or Content-ID, body data
 * in each encoding, and message-level properties.
 * Also run Wine's dlls/inetcomm/tests, which check GetData against Windows.
 * Build: winegcc -b x86_64-windows -mconsole -I$BD/include -I wine/include
 *        -L$BD/dlls/inetcomm/x86_64-windows -o mime-web-archive.exe mime-web-archive.c
 *        -linetcomm -lole32 -luuid */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mimeole.h>
#include <stdio.h>

static int failures;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static const char archive[] =
    "MIME-Version: 1.0\r\n"
    "X-Document-Type: Workbook\r\n"
    "Content-Type: multipart/related; boundary=\"XX\"\r\n"
    "\r\n"
    "This is a Single File Web Page.\r\n"
    "\r\n"
    "--XX\r\n"
    "Content-Location: file:///C:/ABCD/book.htm\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n"
    "Content-Type: text/html; charset=\"windows-1252\"\r\n"
    "\r\n"
    "<frame src=3D\"book_files/sheet001.htm\"> caf=E9\r\n"
    "--XX\r\n"
    "Content-Location: file:///C:/ABCD/book_files/sheet001.htm\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "Content-Type: text/html\r\n"
    "\r\n"
    "PHRkPjEwPC90ZD4=\r\n"
    "--XX\r\n"
    "Content-ID: <image001@part>\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "Content-Type: image/png\r\n"
    "\r\n"
    "iVBORw0K\r\n"
    "--XX--\r\n";

static char *read_all(IStream *stream, ULONG *size)
{
    static char buf[4096];
    LARGE_INTEGER zero = {{0}};
    IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
    IStream_Read(stream, buf, sizeof(buf) - 2, size);
    buf[*size] = buf[*size + 1] = 0;
    return buf;
}

static BOOL body_data(IMimeMessage *msg, HBODY hbody, ENCODINGTYPE type, const char *expected, ULONG expected_size)
{
    IMimeBody *body;
    IStream *stream;
    ULONG size;
    char *data;
    BOOL ok;

    if (FAILED(IMimeMessage_BindToObject(msg, hbody, &IID_IMimeBody, (void **)&body))) return FALSE;
    if (FAILED(IMimeBody_GetData(body, type, &stream))) { IMimeBody_Release(body); return FALSE; }
    data = read_all(stream, &size);
    ok = size == expected_size && !memcmp(data, expected, size);
    if (!ok) printf("  got %lu bytes: %.*s\n", size, (int)size, data);
    IStream_Release(stream);
    IMimeBody_Release(body);
    return ok;
}

int main(void)
{
    IMimePropertySchema *schema = NULL;
    IMimeMessage *msg;
    IStream *stream;
    PROPVARIANT value;
    HBODY root, sheet, image, found;
    HRESULT hr;

    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_IMimePropertySchema, NULL, CLSCTX_INPROC_SERVER, &IID_IMimePropertySchema, (void **)&schema);
    check(hr == S_OK && schema, "the MIME property schema class is registered");
    if (schema) IMimePropertySchema_Release(schema);

    CreateStreamOnHGlobal(NULL, TRUE, &stream);
    IStream_Write(stream, archive, sizeof(archive) - 1, NULL);
    {
        LARGE_INTEGER zero = {{0}};
        IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
    }
    hr = CoCreateInstance(&CLSID_IMimeMessage, NULL, CLSCTX_INPROC_SERVER, &IID_IMimeMessage, (void **)&msg);
    check(hr == S_OK, "a MIME message");
    if (FAILED(hr)) return 1;
    hr = IMimeMessage_Load(msg, stream);
    check(hr == S_OK, "the web archive loads");

    hr = IMimeMessage_ResolveURL(msg, NULL, NULL, "file:///C:/ABCD/book.htm", URL_RESULVE_NO_BASE, &root);
    check(hr == S_OK, "the page is found by its URL");
    hr = IMimeMessage_ResolveURL(msg, NULL, NULL, "C:\\ABCD\\book_files\\sheet001.htm", URL_RESULVE_NO_BASE, &sheet);
    check(hr == S_OK, "a part is found by its path, as Excel asks");
    hr = IMimeMessage_ResolveURL(msg, NULL, "file:///C:/ABCD/book.htm", "book_files/sheet001.htm", 0, &found);
    check(hr == S_OK && found == sheet, "and by a URL relative to a base");
    hr = IMimeMessage_ResolveURL(msg, NULL, NULL, "cid:image001@part", 0, &image);
    check(hr == S_OK && image != sheet, "a part is found by its Content-ID");
    hr = IMimeMessage_ResolveURL(msg, NULL, NULL, "file:///C:/ABCD/none.htm", URL_RESULVE_NO_BASE, &found);
    check(hr == MIME_E_NOT_FOUND, "a URL of no part is not found");

    check(body_data(msg, root, IET_BINARY, "<frame src=\"book_files/sheet001.htm\"> caf\xe9", 42),
          "decoded quoted-printable");
    check(body_data(msg, root, IET_CURRENT, "<frame src=\"book_files/sheet001.htm\"> caf\xe9", 42),
          "IET_CURRENT gives the content decoded, as Excel reads it");
    check(body_data(msg, root, IET_QP, "<frame src=3D\"book_files/sheet001.htm\"> caf=E9", 46),
          "the stored quoted-printable");
    check(body_data(msg, sheet, IET_BINARY, "<td>10</td>", 11), "decoded base64");
    check(body_data(msg, sheet, IET_QP, "<td>10</td>", 11), "re-encoded as quoted-printable");
    check(body_data(msg, root, IET_BASE64, "PGZyYW1lIHNyYz0iYm9va19maWxlcy9zaGVldDAwMS5odG0iPiBjYWbp\r\n", 58),
          "re-encoded as base64");
    check(body_data(msg, sheet, IET_UNICODE, (const char *)L"<td>10</td>", 22), "converted to UTF-16");

    PropVariantInit(&value);
    hr = IMimeMessage_GetProp(msg, "X-Document-Type", 0, &value);
    check(hr == S_OK && value.vt == VT_LPSTR && !strcmp(value.pszVal, "Workbook"), "a message header, as Excel reads it");
    PropVariantClear(&value);
    hr = IMimeMessage_GetProp(msg, "X-MSO-SharedFileIndex", 0, &value);
    check(hr == MIME_E_NOT_FOUND, "a header that is not there");
    check(IMimeMessage_QueryProp(msg, "X-Document-Type", "book", TRUE, FALSE) == S_OK, "a header holds a substring");
    check(IMimeMessage_QueryProp(msg, "X-Document-Type", "Chart", FALSE, FALSE) == S_FALSE, "but not another value");

    IMimeMessage_Release(msg);
    IStream_Release(stream);
    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
