/*
 * Extended Linguistic Services
 *
 * Copyright 2026 The TuxOffice authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_ELSCORE_H
#define __WINE_ELSCORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _MAPPING_SERVICE_INFO
{
    size_t Size;
    LPWSTR pszCopyright;
    WORD wMajorVersion;
    WORD wMinorVersion;
    WORD wBuildVersion;
    WORD wStepVersion;
    DWORD dwInputContentTypesCount;
    LPWSTR *prgInputContentTypes;
    DWORD dwOutputContentTypesCount;
    LPWSTR *prgOutputContentTypes;
    DWORD dwInputLanguagesCount;
    LPWSTR *prgInputLanguages;
    DWORD dwOutputLanguagesCount;
    LPWSTR *prgOutputLanguages;
    DWORD dwInputScriptsCount;
    LPWSTR *prgInputScripts;
    DWORD dwOutputScriptsCount;
    LPWSTR *prgOutputScripts;
    GUID guid;
    LPWSTR pszCategory;
    LPWSTR pszDescription;
    DWORD dwPrivateDataSize;
    LPVOID pPrivateData;
    LPVOID pContext;
    unsigned IsOneToOneLanguageMapping : 1;
    unsigned HasSubservices : 1;
    unsigned OnlineOnly : 1;
    unsigned ServiceType : 2;
} MAPPING_SERVICE_INFO, *PMAPPING_SERVICE_INFO;

typedef struct _MAPPING_ENUM_OPTIONS
{
    size_t Size;
    LPWSTR pszCategory;
    LPWSTR pszInputLanguage;
    LPWSTR pszOutputLanguage;
    LPWSTR pszInputScript;
    LPWSTR pszOutputScript;
    LPWSTR pszInputContentType;
    LPWSTR pszOutputContentType;
    GUID *pGuid;
    unsigned OnlineService : 2;
    unsigned ServiceType : 2;
} MAPPING_ENUM_OPTIONS, *PMAPPING_ENUM_OPTIONS;

typedef struct _MAPPING_DATA_RANGE
{
    DWORD dwStartIndex;
    DWORD dwEndIndex;
    LPWSTR pszDescription;
    DWORD dwDescriptionLength;
    LPVOID pData;
    DWORD dwDataSize;
    LPWSTR pszContentType;
    LPWSTR *prgActionIds;
    DWORD dwActionsCount;
    LPWSTR *prgActionDisplayNames;
} MAPPING_DATA_RANGE, *PMAPPING_DATA_RANGE;

typedef struct _MAPPING_PROPERTY_BAG
{
    size_t Size;
    PMAPPING_DATA_RANGE prgResultRanges;
    DWORD dwRangesCount;
    LPVOID pServiceData;
    DWORD dwServiceDataSize;
    LPVOID pCallerData;
    DWORD dwCallerDataSize;
    LPVOID pContext;
} MAPPING_PROPERTY_BAG, *PMAPPING_PROPERTY_BAG;

typedef void (CALLBACK *PFN_MAPPINGCALLBACKPROC)(MAPPING_PROPERTY_BAG *bag, LPVOID data, DWORD size, HRESULT result);

typedef struct _MAPPING_OPTIONS
{
    size_t Size;
    LPWSTR pszInputLanguage;
    LPWSTR pszOutputLanguage;
    LPWSTR pszInputScript;
    LPWSTR pszOutputScript;
    LPWSTR pszInputContentType;
    LPWSTR pszOutputContentType;
    LPWSTR pszUILanguage;
    PFN_MAPPINGCALLBACKPROC pfnRecognizeCallback;
    LPVOID pRecognizeCallerData;
    DWORD dwRecognizeCallerDataSize;
    PFN_MAPPINGCALLBACKPROC pfnActionCallback;
    LPVOID pActionCallerData;
    DWORD dwActionCallerDataSize;
    DWORD dwServiceFlag;
    unsigned GetActionDisplayName : 1;
} MAPPING_OPTIONS, *PMAPPING_OPTIONS;

HRESULT WINAPI MappingGetServices(PMAPPING_ENUM_OPTIONS options, PMAPPING_SERVICE_INFO *services, DWORD *count);
HRESULT WINAPI MappingFreeServices(PMAPPING_SERVICE_INFO services);
HRESULT WINAPI MappingRecognizeText(PMAPPING_SERVICE_INFO service, LPCWSTR text, DWORD length, DWORD index,
        PMAPPING_OPTIONS options, PMAPPING_PROPERTY_BAG bag);
HRESULT WINAPI MappingDoAction(PMAPPING_PROPERTY_BAG bag, DWORD range, LPCWSTR action);
HRESULT WINAPI MappingFreePropertyBag(PMAPPING_PROPERTY_BAG bag);

#ifdef __cplusplus
}
#endif

#endif /* __WINE_ELSCORE_H */
