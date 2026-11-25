/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Public ADO 6.1 metadata facts from Microsoft's msado15.dll 10.0.26100.5074,
 * SHA256 dd30eea1494e8c26c26b951e5bbe359b900ea27bb685902ea88e3d19d141a79d.
 * Compiled VBA stores these indexes, including coclasses 83 and 92.
 * No Microsoft binary or implementation is included. */
#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>

static unsigned int checks, failures;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; \
    printf("FAIL line %u: %s\n", __LINE__, #test); } } while (0)
#define REQUIRE(call) do { HRESULT hr_ = (call); CHECK(hr_ == S_OK); \
    if (hr_ != S_OK) { printf("HRESULT %#lx\n", hr_); return 1; } } while (0)

static const struct
{
    const WCHAR *name, *guid;
    TYPEKIND kind;
    WORD functions, variables, bases, flags, size64, vtable64;
} expected[] =
{
    {L"CursorTypeEnum", L"{0000051B-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"CursorOptionEnum", L"{0000051C-0000-0010-8000-00AA006D2EA4}", 0, 0, 13, 0, 0, 4, 0},
    {L"LockTypeEnum", L"{0000051D-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"ExecuteOptionEnum", L"{0000051E-0000-0010-8000-00AA006D2EA4}", 0, 0, 7, 0, 0, 4, 0},
    {L"ConnectOptionEnum", L"{00000541-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"ObjectStateEnum", L"{00000532-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"CursorLocationEnum", L"{0000052F-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"DataTypeEnum", L"{0000051F-0000-0010-8000-00AA006D2EA4}", 0, 0, 40, 0, 0, 4, 0},
    {L"FieldAttributeEnum", L"{00000525-0000-0010-8000-00AA006D2EA4}", 0, 0, 17, 0, 0, 4, 0},
    {L"EditModeEnum", L"{00000526-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"RecordStatusEnum", L"{00000527-0000-0010-8000-00AA006D2EA4}", 0, 0, 18, 0, 0, 4, 0},
    {L"GetRowsOptionEnum", L"{00000542-0000-0010-8000-00AA006D2EA4}", 0, 0, 1, 0, 0, 4, 0},
    {L"PositionEnum", L"{00000528-0000-0010-8000-00AA006D2EA4}", 0, 0, 3, 0, 0, 4, 0},
    {L"PositionEnum_Param", L"{A56187C5-D690-4037-AE32-A00EDC376AC3}", 6, 0, 0, 0, 0x10, 8, 0},
    {L"BookmarkEnum", L"{00000000-0000-0000-0000-000000000000}", 0, 0, 3, 0, 0, 4, 0},
    {L"MarshalOptionsEnum", L"{00000540-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"AffectEnum", L"{00000543-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"ResyncEnum", L"{00000544-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"CompareEnum", L"{00000545-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"FilterGroupEnum", L"{00000546-0000-0010-8000-00AA006D2EA4}", 0, 0, 6, 0, 0, 4, 0},
    {L"SearchDirectionEnum", L"{00000547-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"SearchDirection", L"{00000000-0000-0000-0000-000000000000}", 6, 0, 0, 0, 0x10, 4, 0},
    {L"PersistFormatEnum", L"{00000548-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"StringFormatEnum", L"{00000549-0000-0010-8000-00AA006D2EA4}", 0, 0, 1, 0, 0, 4, 0},
    {L"ConnectPromptEnum", L"{00000520-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"ConnectModeEnum", L"{00000521-0000-0010-8000-00AA006D2EA4}", 0, 0, 9, 0, 0, 4, 0},
    {L"RecordCreateOptionsEnum", L"{00000570-0000-0010-8000-00AA006D2EA4}", 0, 0, 6, 0, 0, 4, 0},
    {L"RecordOpenOptionsEnum", L"{00000571-0000-0010-8000-00AA006D2EA4}", 0, 0, 7, 0, 0, 4, 0},
    {L"IsolationLevelEnum", L"{00000523-0000-0010-8000-00AA006D2EA4}", 0, 0, 9, 0, 0, 4, 0},
    {L"XactAttributeEnum", L"{00000524-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"PropertyAttributesEnum", L"{00000529-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"ErrorValueEnum", L"{0000052A-0000-0010-8000-00AA006D2EA4}", 0, 0, 64, 0, 0, 4, 0},
    {L"ParameterAttributesEnum", L"{0000052B-0000-0010-8000-00AA006D2EA4}", 0, 0, 3, 0, 0, 4, 0},
    {L"ParameterDirectionEnum", L"{0000052C-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"CommandTypeEnum", L"{0000052E-0000-0010-8000-00AA006D2EA4}", 0, 0, 7, 0, 0, 4, 0},
    {L"EventStatusEnum", L"{00000530-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"EventReasonEnum", L"{00000531-0000-0010-8000-00AA006D2EA4}", 0, 0, 15, 0, 0, 4, 0},
    {L"SchemaEnum", L"{00000533-0000-0010-8000-00AA006D2EA4}", 0, 0, 46, 0, 0, 4, 0},
    {L"FieldStatusEnum", L"{0000057E-0000-0010-8000-00AA006D2EA4}", 0, 0, 30, 0, 0, 4, 0},
    {L"SeekEnum", L"{00000552-0000-0010-8000-00AA006D2EA4}", 0, 0, 6, 0, 0, 4, 0},
    {L"ADCPROP_UPDATECRITERIA_ENUM", L"{0000054A-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"ADCPROP_ASYNCTHREADPRIORITY_ENUM", L"{0000054B-0000-0010-8000-00AA006D2EA4}", 0, 0, 5, 0, 0, 4, 0},
    {L"ADCPROP_AUTORECALC_ENUM", L"{00000554-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"ADCPROP_UPDATERESYNC_ENUM", L"{00000553-0000-0010-8000-00AA006D2EA4}", 0, 0, 6, 0, 0, 4, 0},
    {L"MoveRecordOptionsEnum", L"{00000573-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"CopyRecordOptionsEnum", L"{00000574-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"StreamTypeEnum", L"{00000576-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"LineSeparatorEnum", L"{00000577-0000-0010-8000-00AA006D2EA4}", 0, 0, 3, 0, 0, 4, 0},
    {L"StreamOpenOptionsEnum", L"{0000057A-0000-0010-8000-00AA006D2EA4}", 0, 0, 3, 0, 0, 4, 0},
    {L"StreamWriteEnum", L"{0000057B-0000-0010-8000-00AA006D2EA4}", 0, 0, 4, 0, 0, 4, 0},
    {L"SaveOptionsEnum", L"{0000057C-0000-0010-8000-00AA006D2EA4}", 0, 0, 2, 0, 0, 4, 0},
    {L"FieldEnum", L"{00000000-0000-0000-0000-000000000000}", 0, 0, 2, 0, 0, 4, 0},
    {L"StreamReadEnum", L"{00000000-0000-0000-0000-000000000000}", 0, 0, 2, 0, 0, 4, 0},
    {L"RecordTypeEnum", L"{0000057D-0000-0010-8000-00AA006D2EA4}", 0, 0, 3, 0, 0, 4, 0},
    {L"_Collection", L"{00000512-0000-0010-8000-00AA006D2EA4}", 4, 10, 0, 1, 0x10c0, 8, 56},
    {L"_DynaCollection", L"{00000513-0000-0010-8000-00AA006D2EA4}", 4, 12, 0, 1, 0x10c0, 8, 56},
    {L"_ADO", L"{00000534-0000-0010-8000-00AA006D2EA4}", 4, 8, 0, 1, 0x10c0, 8, 56},
    {L"Properties", L"{00000504-0000-0010-8000-00AA006D2EA4}", 4, 11, 0, 1, 0x10c0, 8, 56},
    {L"Property", L"{00000503-0000-0010-8000-00AA006D2EA4}", 4, 13, 0, 1, 0x10c0, 8, 56},
    {L"Error", L"{00000500-0000-0010-8000-00AA006D2EA4}", 4, 14, 0, 1, 0x10c0, 8, 56},
    {L"Errors", L"{00000501-0000-0010-8000-00AA006D2EA4}", 4, 12, 0, 1, 0x10c0, 8, 56},
    {L"Command15", L"{00001508-0000-0010-8000-00AA006D2EA4}", 4, 24, 0, 1, 0x10d0, 8, 56},
    {L"_Connection", L"{00001550-0000-0010-8000-00AA006D2EA4}", 4, 37, 0, 1, 0x1040, 8, 56},
    {L"Connection15", L"{00001515-0000-0010-8000-00AA006D2EA4}", 4, 36, 0, 1, 0x1050, 8, 56},
    {L"_Recordset", L"{00001556-0000-0010-8000-00AA006D2EA4}", 4, 85, 0, 1, 0x10d0, 8, 56},
    {L"Recordset21", L"{00001555-0000-0010-8000-00AA006D2EA4}", 4, 84, 0, 1, 0x10d0, 8, 56},
    {L"Recordset20", L"{0000154F-0000-0010-8000-00AA006D2EA4}", 4, 81, 0, 1, 0x10d0, 8, 56},
    {L"Recordset15", L"{0000150E-0000-0010-8000-00AA006D2EA4}", 4, 68, 0, 1, 0x10d0, 8, 56},
    {L"Fields", L"{00001564-0000-0010-8000-00AA006D2EA4}", 4, 17, 0, 1, 0x10d0, 8, 56},
    {L"Fields20", L"{0000154D-0000-0010-8000-00AA006D2EA4}", 4, 13, 0, 1, 0x10d0, 8, 56},
    {L"Fields15", L"{00001506-0000-0010-8000-00AA006D2EA4}", 4, 11, 0, 1, 0x10d0, 8, 56},
    {L"Field", L"{00001569-0000-0010-8000-00AA006D2EA4}", 4, 29, 0, 1, 0x10d0, 8, 56},
    {L"Field20", L"{0000154C-0000-0010-8000-00AA006D2EA4}", 4, 28, 0, 1, 0x10d0, 8, 56},
    {L"_Parameter", L"{0000150C-0000-0010-8000-00AA006D2EA4}", 4, 25, 0, 1, 0x10d0, 8, 56},
    {L"Parameters", L"{0000150D-0000-0010-8000-00AA006D2EA4}", 4, 13, 0, 1, 0x10c0, 8, 56},
    {L"Command25", L"{0000154E-0000-0010-8000-00AA006D2EA4}", 4, 26, 0, 1, 0x10d0, 8, 56},
    {L"_Command", L"{986761E8-7269-4890-AA65-AD7C03697A6D}", 4, 32, 0, 1, 0x10c0, 8, 56},
    {L"ConnectionEventsVt", L"{00001402-0000-0010-8000-00AA006D2EA4}", 3, 9, 0, 1, 0x10, 8, 96},
    {L"RecordsetEventsVt", L"{00001403-0000-0010-8000-00AA006D2EA4}", 3, 11, 0, 1, 0x10, 8, 112},
    {L"ConnectionEvents", L"{00001400-0000-0010-8000-00AA006D2EA4}", 4, 9, 0, 1, 0x1000, 8, 56},
    {L"RecordsetEvents", L"{00001266-0000-0010-8000-00AA006D2EA4}", 4, 11, 0, 1, 0x1000, 8, 56},
    {L"ADOConnectionConstruction15", L"{00000516-0000-0010-8000-00AA006D2EA4}", 3, 3, 0, 1, 0x200, 8, 48},
    {L"ADOConnectionConstruction", L"{00000551-0000-0010-8000-00AA006D2EA4}", 3, 0, 0, 1, 0x200, 8, 48},
    {L"Connection", L"{00000514-0000-0010-8000-00AA006D2EA4}", 5, 0, 0, 2, 0x6, 8, 0},
    {L"_Record", L"{00001562-0000-0010-8000-00AA006D2EA4}", 4, 27, 0, 1, 0x1050, 8, 56},
    {L"Record", L"{00000560-0000-0010-8000-00AA006D2EA4}", 5, 0, 0, 1, 0x6, 8, 0},
    {L"_Stream", L"{00001565-0000-0010-8000-00AA006D2EA4}", 4, 33, 0, 1, 0x1050, 8, 56},
    {L"Stream", L"{00000566-0000-0010-8000-00AA006D2EA4}", 5, 0, 0, 1, 0x6, 8, 0},
    {L"ADORecordConstruction", L"{00000567-0000-0010-8000-00AA006D2EA4}", 3, 3, 0, 1, 0x1200, 8, 80},
    {L"ADOStreamConstruction", L"{00000568-0000-0010-8000-00AA006D2EA4}", 3, 2, 0, 1, 0x1200, 8, 72},
    {L"ADOCommandConstruction", L"{00000517-0000-0010-8000-00AA006D2EA4}", 3, 2, 0, 1, 0x200, 8, 40},
    {L"Command", L"{00000507-0000-0010-8000-00AA006D2EA4}", 5, 0, 0, 1, 0x6, 8, 0},
    {L"Recordset", L"{00000535-0000-0010-8000-00AA006D2EA4}", 5, 0, 0, 2, 0x6, 8, 0},
    {L"ADORecordsetConstruction", L"{00000283-0000-0010-8000-00AA006D2EA4}", 3, 6, 0, 1, 0x1210, 8, 104},
    {L"ADO_LONGPTR", L"{54D8B4B9-663B-4A9C-95F6-0E749ABD70F1}", 6, 0, 0, 0, 0x10, 8, 0},
    {L"Field15", L"{00001505-0000-0010-8000-00AA006D2EA4}", 4, 21, 0, 1, 0x10d0, 8, 56},
    {L"Parameter", L"{0000050B-0000-0010-8000-00AA006D2EA4}", 5, 0, 0, 1, 0x6, 8, 0},
    {L"ConnectionEventsVt_Deprecated", L"{00000402-0000-0010-8000-00AA006D2EA4}", 3, 9, 0, 1, 0x10, 8, 96},
    {L"_Connection_Deprecated", L"{00000550-0000-0010-8000-00AA006D2EA4}", 4, 37, 0, 1, 0x1050, 8, 56},
    {L"Connection15_Deprecated", L"{00000515-0000-0010-8000-00AA006D2EA4}", 4, 36, 0, 1, 0x1050, 8, 56},
    {L"_Recordset_Deprecated", L"{00000556-0000-0010-8000-00AA006D2EA4}", 4, 85, 0, 1, 0x10d0, 8, 56},
    {L"Recordset21_Deprecated", L"{00000555-0000-0010-8000-00AA006D2EA4}", 4, 84, 0, 1, 0x10d0, 8, 56},
    {L"Recordset20_Deprecated", L"{0000054F-0000-0010-8000-00AA006D2EA4}", 4, 81, 0, 1, 0x10d0, 8, 56},
    {L"Recordset15_Deprecated", L"{0000050E-0000-0010-8000-00AA006D2EA4}", 4, 68, 0, 1, 0x10d0, 8, 56},
    {L"Fields_Deprecated", L"{00000564-0000-0010-8000-00AA006D2EA4}", 4, 17, 0, 1, 0x10d0, 8, 56},
    {L"Fields20_Deprecated", L"{0000054D-0000-0010-8000-00AA006D2EA4}", 4, 13, 0, 1, 0x10d0, 8, 56},
    {L"Fields15_Deprecated", L"{00000506-0000-0010-8000-00AA006D2EA4}", 4, 11, 0, 1, 0x10d0, 8, 56},
    {L"Field_Deprecated", L"{00000569-0000-0010-8000-00AA006D2EA4}", 4, 29, 0, 1, 0x10d0, 8, 56},
    {L"Field20_Deprecated", L"{0000054C-0000-0010-8000-00AA006D2EA4}", 4, 28, 0, 1, 0x10d0, 8, 56},
    {L"_Command_Deprecated", L"{B08400BD-F9D1-4D02-B856-71D5DBA123E9}", 4, 32, 0, 1, 0x10d0, 8, 56},
    {L"Command25_Deprecated", L"{0000054E-0000-0010-8000-00AA006D2EA4}", 4, 26, 0, 1, 0x10d0, 8, 56},
    {L"Command15_Deprecated", L"{00000508-0000-0010-8000-00AA006D2EA4}", 4, 24, 0, 1, 0x10d0, 8, 56},
    {L"_Parameter_Deprecated", L"{0000050C-0000-0010-8000-00AA006D2EA4}", 4, 25, 0, 1, 0x10d0, 8, 56},
    {L"Parameters_Deprecated", L"{0000050D-0000-0010-8000-00AA006D2EA4}", 4, 13, 0, 1, 0x10d0, 8, 56},
    {L"RecordsetEventsVt_Deprecated", L"{00000403-0000-0010-8000-00AA006D2EA4}", 3, 11, 0, 1, 0x10, 8, 112},
    {L"ConnectionEvents_Deprecated", L"{00000400-0000-0010-8000-00AA006D2EA4}", 4, 9, 0, 1, 0x1010, 8, 56},
    {L"RecordsetEvents_Deprecated", L"{00000266-0000-0010-8000-00AA006D2EA4}", 4, 11, 0, 1, 0x1010, 8, 56},
    {L"_Record_Deprecated", L"{00000562-0000-0010-8000-00AA006D2EA4}", 4, 27, 0, 1, 0x1050, 8, 56},
    {L"_Stream_Deprecated", L"{00000565-0000-0010-8000-00AA006D2EA4}", 4, 33, 0, 1, 0x1050, 8, 56},
    {L"Field15_Deprecated", L"{00000505-0000-0010-8000-00AA006D2EA4}", 4, 21, 0, 1, 0x10d0, 8, 56},
};

int main(void)
{
    GUID guid, libid;
    ITypeLib *lib, *owner;
    ITypeInfo *info, *by_guid, *target;
    TYPEATTR *attr;
    FUNCDESC *func;
    VARDESC *var;
    BSTR name;
    HREFTYPE href;
    UINT i, j, index;
    WORD size, vtable;
    IDispatch *object;
    DISPID id;
    LPOLESTR state = L"State";
    DISPPARAMS args = {0};
    VARIANT result;

    REQUIRE(CoInitialize(NULL));
    REQUIRE(CLSIDFromString(L"{B691E011-1797-432E-907A-4D8C69339129}", &libid));
    REQUIRE(LoadRegTypeLib(&libid, 6, 1, 0, &lib));
    CHECK(ITypeLib_GetTypeInfoCount(lib) == sizeof(expected) / sizeof(expected[0]));
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
    {
        REQUIRE(ITypeLib_GetTypeInfo(lib, i, &info));
        REQUIRE(ITypeInfo_GetDocumentation(info, MEMBERID_NIL, &name, NULL, NULL, NULL));
        if (lstrcmpW(name, expected[i].name))
            printf("INDEX %u: expected %ls, got %ls\n", i, expected[i].name, name);
        CHECK(!lstrcmpW(name, expected[i].name));
        SysFreeString(name);
        REQUIRE(CLSIDFromString((LPOLESTR)expected[i].guid, &guid));
        REQUIRE(ITypeInfo_GetTypeAttr(info, &attr));
        CHECK(IsEqualGUID(&attr->guid, &guid));
        CHECK(attr->typekind == expected[i].kind);
        CHECK(attr->cFuncs == expected[i].functions);
        CHECK(attr->cVars == expected[i].variables);
        CHECK(attr->cImplTypes == expected[i].bases);
        CHECK(attr->wTypeFlags == expected[i].flags);
        size = expected[i].size64 == 8 ? sizeof(void *) : expected[i].size64;
        vtable = expected[i].vtable64 * sizeof(void *) / 8;
        CHECK(attr->cbSizeInstance == size);
        CHECK(attr->cbSizeVft == vtable);
        if (!IsEqualGUID(&guid, &GUID_NULL))
        {
            REQUIRE(ITypeLib_GetTypeInfoOfGuid(lib, &guid, &by_guid));
            CHECK(by_guid == info);
            ITypeInfo_Release(by_guid);
        }
        REQUIRE(ITypeInfo_GetContainingTypeLib(info, &owner, &index));
        CHECK(index == i);
        ITypeLib_Release(owner);
        for (j = 0; j < attr->cFuncs; ++j)
        {
            REQUIRE(ITypeInfo_GetFuncDesc(info, j, &func));
            REQUIRE(ITypeInfo_GetDocumentation(info, func->memid, &name, NULL, NULL, NULL));
            CHECK(name && SysStringLen(name));
            SysFreeString(name);
            ITypeInfo_ReleaseFuncDesc(info, func);
        }
        for (j = 0; j < attr->cVars; ++j)
        {
            REQUIRE(ITypeInfo_GetVarDesc(info, j, &var));
            REQUIRE(ITypeInfo_GetDocumentation(info, var->memid, &name, NULL, NULL, NULL));
            CHECK(name && SysStringLen(name));
            SysFreeString(name);
            ITypeInfo_ReleaseVarDesc(info, var);
        }
        for (j = 0; j < attr->cImplTypes; ++j)
        {
            REQUIRE(ITypeInfo_GetRefTypeOfImplType(info, j, &href));
            REQUIRE(ITypeInfo_GetRefTypeInfo(info, href, &target));
            ITypeInfo_Release(target);
        }
        ITypeInfo_ReleaseTypeAttr(info, attr);
        ITypeInfo_Release(info);
    }
    /* Reproduce a compiled client's indexed coclass activation, not a fresh
     * compilation that learns whichever indexes the library happens to have. */
    for (i = 0; i < 2; ++i)
    {
        REQUIRE(ITypeLib_GetTypeInfo(lib, i ? 92 : 83, &info));
        REQUIRE(ITypeInfo_CreateInstance(info, NULL, &IID_IDispatch, (void **)&object));
        REQUIRE(IDispatch_GetIDsOfNames(object, &IID_NULL, &state, 1, 0, &id));
        VariantInit(&result);
        REQUIRE(IDispatch_Invoke(object, id, &IID_NULL, 0, DISPATCH_PROPERTYGET,
                &args, &result, NULL, NULL));
        CHECK(V_VT(&result) == VT_I4 && V_I4(&result) == 0);
        VariantClear(&result);
        IDispatch_Release(object);
        ITypeInfo_Release(info);
    }
    ITypeLib_Release(lib);
    CoUninitialize();
    printf("ADO indexed metadata: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
