/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise both ADO interface families using real vtable calls and the 6.1
 * dispatch metadata. No external database, driver or account is required. */
#define COBJMACROS
#include <initguid.h>
#include <msado15_backcompat.h>
#include <stdio.h>

static unsigned int checks, failures;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; \
    printf("FAIL line %u: %s\n", __LINE__, #test); } } while (0)
#define REQUIRE(call) do { HRESULT hr_ = (call); CHECK(hr_ == S_OK); \
    if (hr_ != S_OK) { printf("HRESULT %#lx\n", hr_); return 1; } } while (0)

static IID modern_iid(REFIID legacy)
{
    IID result = *legacy;
    if (IsEqualIID(legacy, &IID__Command))
        CLSIDFromString(L"{986761E8-7269-4890-AA65-AD7C03697A6D}", &result);
    else
        result.Data1 += 0x1000;
    return result;
}

struct marshaled_objects
{
    IStream *connection, *recordset;
    HRESULT init, connection_hr, recordset_hr;
    LONG state;
    ADO_LONGPTR records;
};

static DWORD WINAPI read_marshaled_objects(void *param)
{
    struct marshaled_objects *data = param;
    _Connection *connection;
    _Recordset *recordset;
    IID iid;

    data->init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(data->init)) return 1;
    iid = modern_iid(&IID__Connection);
    data->connection_hr = CoGetInterfaceAndReleaseStream(data->connection, &iid, (void **)&connection);
    if (SUCCEEDED(data->connection_hr))
    {
        data->connection_hr = _Connection_get_State(connection, &data->state);
        _Connection_Release(connection);
    }
    iid = modern_iid(&IID__Recordset);
    data->recordset_hr = CoGetInterfaceAndReleaseStream(data->recordset, &iid, (void **)&recordset);
    if (SUCCEEDED(data->recordset_hr))
    {
        data->recordset_hr = _Recordset_get_RecordCount(recordset, &data->records);
        _Recordset_Release(recordset);
    }
    CoUninitialize();
    return 0;
}

static void check_interfaces(IUnknown *object, const IID **interfaces, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; ++i)
    {
        IID modern = modern_iid(interfaces[i]);
        IUnknown *old_view = NULL, *new_view = NULL, *old_identity = NULL, *new_identity = NULL;
        HRESULT hr;
        hr = IUnknown_QueryInterface(object, interfaces[i], (void **)&old_view);
        CHECK(hr == S_OK);
        hr = IUnknown_QueryInterface(object, &modern, (void **)&new_view);
        CHECK(hr == S_OK);
        if (old_view && new_view)
        {
            hr = IUnknown_QueryInterface(old_view, &IID_IUnknown, (void **)&old_identity);
            CHECK(hr == S_OK);
            hr = IUnknown_QueryInterface(new_view, &IID_IUnknown, (void **)&new_identity);
            CHECK(hr == S_OK);
            if (old_identity && new_identity)
                CHECK(old_identity == new_identity);
        }
        if (old_identity) IUnknown_Release(old_identity);
        if (new_identity) IUnknown_Release(new_identity);
        if (old_view) IUnknown_Release(old_view);
        if (new_view) IUnknown_Release(new_view);
    }
}

int main(void)
{
    const IID *connection_ids[] = { &IID_Connection15, &IID__Connection };
    const IID *recordset_ids[] = { &IID_Recordset15, &IID_Recordset20, &IID_Recordset21, &IID__Recordset };
    const IID *fields_ids[] = { &IID_Fields15, &IID_Fields20, &IID_Fields };
    const IID *field_ids[] = { &IID_Field20, &IID_Field };
    const IID *command_ids[] = { &IID_Command15, &IID_Command25, &IID__Command };
    const IID *parameter_ids[] = { &IID__Parameter };
    const IID *parameters_ids[] = { &IID_Parameters };
    const IID *stream_ids[] = { &IID__Stream };
    GUID library61, iid, alias;
    ITypeLib *libraries[2];
    TLIBATTR *attr;
    ITypeInfo *info;
    TYPEATTR *typeattr;
    _Connection *connection;
    _Recordset *recordset;
    Fields *fields;
    Field *field;
    _Command *command;
    _Parameter *parameter;
    Parameters *parameters;
    _Stream *stream;
    LONG state, count;
    ADO_LONGPTR records;
    VARIANT missing, index, value, result;
    DISPPARAMS args = {0};
    DISPID state_id;
    LPOLESTR state_name = L"State";
    BSTR name, text;
    unsigned int i;
    struct marshaled_objects marshaled = {0};
    HANDLE thread;
    MSG message;
    DWORD wait;

    REQUIRE(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    CLSIDFromString(L"{B691E011-1797-432E-907A-4D8C69339129}", &library61);
    REQUIRE(LoadRegTypeLib(&LIBID_ADODB, 2, 8, 0, &libraries[0]));
    REQUIRE(LoadRegTypeLib(&library61, 6, 1, 0, &libraries[1]));
    for (i = 0; i < 2; ++i)
    {
        REQUIRE(ITypeLib_GetLibAttr(libraries[i], &attr));
        CHECK(IsEqualGUID(&attr->guid, i ? &library61 : &LIBID_ADODB));
        CHECK(attr->wMajorVerNum == (i ? 6 : 2) && attr->wMinorVerNum == (i ? 1 : 8));
        ITypeLib_ReleaseTLibAttr(libraries[i], attr);
    }
    CLSIDFromString(L"{54D8B4B9-663B-4A9C-95F6-0E749ABD70F1}", &alias);
    REQUIRE(ITypeLib_GetTypeInfoOfGuid(libraries[1], &alias, &info));
    REQUIRE(ITypeInfo_GetTypeAttr(info, &typeattr));
    CHECK(typeattr->typekind == TKIND_ALIAS);
    CHECK(typeattr->tdescAlias.vt == (sizeof(void *) == 8 ? VT_I8 : VT_I4));
    ITypeInfo_ReleaseTypeAttr(info, typeattr);
    ITypeInfo_Release(info);

    iid = modern_iid(&IID__Connection);
    REQUIRE(CoCreateInstance(&CLSID_Connection, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&connection));
    check_interfaces((IUnknown *)connection, connection_ids, 2);
    REQUIRE(_Connection_get_State(connection, &state));
    CHECK(state == adStateClosed);
    REQUIRE(ITypeLib_GetTypeInfoOfGuid(libraries[1], &iid, &info));
    REQUIRE(ITypeInfo_GetIDsOfNames(info, &state_name, 1, &state_id));
    VariantInit(&result);
    REQUIRE(ITypeInfo_Invoke(info, connection, state_id, DISPATCH_PROPERTYGET, &args, &result, NULL, NULL));
    CHECK(V_VT(&result) == VT_I4 && V_I4(&result) == adStateClosed);
    VariantClear(&result);
    ITypeInfo_Release(info);

    iid = modern_iid(&IID__Recordset);
    REQUIRE(CoCreateInstance(&CLSID_Recordset, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&recordset));
    check_interfaces((IUnknown *)recordset, recordset_ids, 4);
    REQUIRE(_Recordset_get_Fields(recordset, &fields));
    check_interfaces((IUnknown *)fields, fields_ids, 3);
    V_VT(&missing) = VT_ERROR;
    V_ERROR(&missing) = DISP_E_PARAMNOTFOUND;
    name = SysAllocString(L"Quantity");
    REQUIRE(Fields_Append(fields, name, adInteger, 4, adFldUnspecified, missing));
    SysFreeString(name);
    REQUIRE(Fields_get_Count(fields, &count));
    CHECK(count == 1);
    V_VT(&index) = VT_I4;
    V_I4(&index) = 0;
    REQUIRE(Fields_get_Item(fields, index, &field));
    check_interfaces((IUnknown *)field, field_ids, 2);
    REQUIRE(_Recordset_Open(recordset, missing, missing, adOpenStatic, adLockBatchOptimistic, adCmdUnspecified));
    REQUIRE(_Recordset_AddNew(recordset, missing, missing));
    V_VT(&value) = VT_I4;
    V_I4(&value) = 42;
    REQUIRE(Field_put_Value(field, value));
    REQUIRE(_Recordset_Update(recordset, missing, missing));
    REQUIRE(_Recordset_get_RecordCount(recordset, &records));
    CHECK(records == 1);
    REQUIRE(Field_get_Value(field, &result));
    CHECK(V_VT(&result) == VT_I4 && V_I4(&result) == 42);
    VariantClear(&result);
    iid = modern_iid(&IID__Connection);
    REQUIRE(CoMarshalInterThreadInterfaceInStream(&iid, (IUnknown *)connection, &marshaled.connection));
    iid = modern_iid(&IID__Recordset);
    REQUIRE(CoMarshalInterThreadInterfaceInStream(&iid, (IUnknown *)recordset, &marshaled.recordset));
    marshaled.state = -1;
    marshaled.records = sizeof(void *) == 8 ? (ADO_LONGPTR)0x1234567800000000ULL : -1;
    thread = CreateThread(NULL, 0, read_marshaled_objects, &marshaled, 0, NULL);
    CHECK(thread != NULL);
    if (!thread) return 1;
    while ((wait = MsgWaitForMultipleObjects(1, &thread, FALSE, 10000, QS_ALLINPUT)) == WAIT_OBJECT_0 + 1)
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    CHECK(wait == WAIT_OBJECT_0);
    if (wait != WAIT_OBJECT_0) return 1;
    CloseHandle(thread);
    CHECK(SUCCEEDED(marshaled.init));
    CHECK(marshaled.connection_hr == S_OK && marshaled.state == adStateClosed);
    CHECK(marshaled.recordset_hr == S_OK && marshaled.records == 1);
    _Connection_Release(connection);
    Field_Release(field);
    Fields_Release(fields);
    REQUIRE(_Recordset_Close(recordset));
    _Recordset_Release(recordset);

    iid = modern_iid(&IID__Command);
    REQUIRE(CoCreateInstance(&CLSID_Command, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&command));
    check_interfaces((IUnknown *)command, command_ids, 3);
    name = SysAllocString(L"Quantity");
    REQUIRE(_Command_CreateParameter(command, name, adInteger, adParamInput, 4, value, &parameter));
    SysFreeString(name);
    check_interfaces((IUnknown *)parameter, parameter_ids, 1);
    REQUIRE(_Command_get_Parameters(command, &parameters));
    check_interfaces((IUnknown *)parameters, parameters_ids, 1);
    REQUIRE(Parameters_Append(parameters, (IDispatch *)parameter));
    REQUIRE(Parameters_get_Count(parameters, &count));
    CHECK(count == 1);
    Parameters_Release(parameters);
    _Parameter_Release(parameter);
    _Command_Release(command);

    iid = modern_iid(&IID__Stream);
    REQUIRE(CoCreateInstance(&CLSID_Stream, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&stream));
    check_interfaces((IUnknown *)stream, stream_ids, 1);
    REQUIRE(_Stream_Open(stream, missing, adModeUnknown, adOpenStreamUnspecified, NULL, NULL));
    text = SysAllocString(L"ADO 6.1 text");
    REQUIRE(_Stream_WriteText(stream, text, adWriteChar));
    REQUIRE(_Stream_put_Position(stream, 0));
    SysFreeString(text);
    REQUIRE(_Stream_ReadText(stream, adReadAll, &text));
    CHECK(!lstrcmpW(text, L"ADO 6.1 text"));
    SysFreeString(text);
    REQUIRE(_Stream_Close(stream));
    _Stream_Release(stream);

    ITypeLib_Release(libraries[0]);
    ITypeLib_Release(libraries[1]);
    CoUninitialize();
    printf("ADO interface families: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
