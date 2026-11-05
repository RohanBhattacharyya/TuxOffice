/*
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


/* Extended Linguistic Services, with the Microsoft Script Detection service that Office's
 * RichEdit and Outlook ask for: it splits text into runs of one Unicode script. */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#include "elscore.h"
#include "elssrvc.h"

#include "wine/debug.h"
#include "wine/unicode_scripts.h"

WINE_DEFAULT_DEBUG_CHANNEL(elscore);

static WCHAR text_plain[] = L"text/plain";
static WCHAR *content_types[] = { text_plain };

struct service
{
    const GUID *guid;
    const WCHAR *category;
    const WCHAR *description;
    HRESULT (*recognize)(const WCHAR *text, DWORD length, DWORD index, MAPPING_PROPERTY_BAG *bag);
};

static HRESULT recognize_scripts(const WCHAR *text, DWORD length, DWORD index, MAPPING_PROPERTY_BAG *bag);

static const struct service services[] =
{
    { &ELS_GUID_SCRIPT_DETECTION, L"Script Detection", L"Microsoft Script Detection", recognize_scripts },
};

/* Prefixes the array MappingGetServices returns, so MappingFreeServices can tell it from others. */
struct service_list
{
    DWORD magic;
    DWORD count;
    MAPPING_SERVICE_INFO info[];
};
#define SERVICE_LIST_MAGIC 0x534c4553 /* "ELSS" */

static const struct service *find_service(const MAPPING_SERVICE_INFO *info)
{
    unsigned int i;

    if (!info || info->Size != sizeof(*info)) return NULL;
    for (i = 0; i < ARRAY_SIZE(services); i++)
        if (IsEqualGUID(&info->guid, services[i].guid)) return &services[i];
    return NULL;
}

static BOOL is_text_plain(const WCHAR *type)
{
    return !type || !wcsicmp(type, text_plain);
}

static BOOL service_matches(const struct service *service, const MAPPING_ENUM_OPTIONS *options)
{
    if (!options) return TRUE;
    if (options->pGuid && !IsEqualGUID(options->pGuid, service->guid)) return FALSE;
    if (options->pszCategory && wcsicmp(options->pszCategory, service->category)) return FALSE;
    /* Script detection takes text in any language and script; it reports scripts, not text,
     * so it has no output language or script to match. */
    if (options->pszOutputLanguage || options->pszOutputScript) return FALSE;
    return is_text_plain(options->pszInputContentType) && is_text_plain(options->pszOutputContentType);
}

HRESULT WINAPI MappingGetServices(MAPPING_ENUM_OPTIONS *options, MAPPING_SERVICE_INFO **result, DWORD *count)
{
    struct service_list *list;
    unsigned int i, found = 0;

    TRACE("%p, %p, %p.\n", options, result, count);

    if (!result || !count) return E_INVALIDARG;
    *result = NULL;
    *count = 0;
    if (options)
    {
        if (options->Size != sizeof(*options)) return E_INVALIDARG;
        TRACE("guid %s, category %s, input %s/%s/%s, output %s/%s/%s.\n", debugstr_guid(options->pGuid),
                debugstr_w(options->pszCategory), debugstr_w(options->pszInputLanguage),
                debugstr_w(options->pszInputScript), debugstr_w(options->pszInputContentType),
                debugstr_w(options->pszOutputLanguage), debugstr_w(options->pszOutputScript),
                debugstr_w(options->pszOutputContentType));
    }

    if (!(list = calloc(1, offsetof(struct service_list, info[ARRAY_SIZE(services)]))))
        return E_OUTOFMEMORY;
    list->magic = SERVICE_LIST_MAGIC;
    for (i = 0; i < ARRAY_SIZE(services); i++)
    {
        MAPPING_SERVICE_INFO *info = &list->info[found];

        if (!service_matches(&services[i], options)) continue;
        info->Size = sizeof(*info);
        info->pszCopyright = (WCHAR *)L"Copyright 2026 The TuxOffice authors";
        info->wMajorVersion = 1;
        info->dwInputContentTypesCount = ARRAY_SIZE(content_types);
        info->prgInputContentTypes = content_types;
        info->dwOutputContentTypesCount = ARRAY_SIZE(content_types);
        info->prgOutputContentTypes = content_types;
        info->guid = *services[i].guid;
        info->pszCategory = (WCHAR *)services[i].category;
        info->pszDescription = (WCHAR *)services[i].description;
        info->pContext = (void *)&services[i];
        found++;
    }
    if (!found)
    {
        free(list);
        return S_OK;
    }
    list->count = found;
    *result = list->info;
    *count = found;
    return S_OK;
}

HRESULT WINAPI MappingFreeServices(MAPPING_SERVICE_INFO *info)
{
    struct service_list *list;

    TRACE("%p.\n", info);

    if (!info) return E_INVALIDARG;
    list = CONTAINING_RECORD(info, struct service_list, info);
    if (list->magic != SERVICE_LIST_MAGIC) return E_INVALIDARG;
    list->magic = 0;
    free(list);
    return S_OK;
}

/* The results of one recognition: the ranges and the script names they point to. */
struct results
{
    DWORD count;
    MAPPING_DATA_RANGE ranges[];
};

struct script_run
{
    DWORD start, end;
    const char *script;
};

static HRESULT store_runs(const struct script_run *runs, DWORD count, MAPPING_PROPERTY_BAG *bag)
{
    struct results *results;
    WCHAR *names;
    DWORD i, j;

    if (!(results = calloc(1, offsetof(struct results, ranges[count]) + count * 5 * sizeof(WCHAR))))
        return E_OUTOFMEMORY;
    names = (WCHAR *)&results->ranges[count];
    results->count = count;
    for (i = 0; i < count; i++)
    {
        MAPPING_DATA_RANGE *range = &results->ranges[i];
        WCHAR *name = names + i * 5;

        for (j = 0; runs[i].script[j]; j++) name[j] = runs[i].script[j];
        name[j] = 0;
        range->dwStartIndex = runs[i].start;
        range->dwEndIndex = runs[i].end;
        range->pData = name;
        range->dwDataSize = (j + 1) * sizeof(WCHAR);
        range->pszContentType = text_plain;
        TRACE("%lu-%lu %s.\n", runs[i].start, runs[i].end, debugstr_w(name));
    }
    bag->prgResultRanges = results->ranges;
    bag->dwRangesCount = count;
    bag->pContext = results;
    return S_OK;
}

static BOOL is_common(const char *script)
{
    return !strcmp(script, "Zyyy") || !strcmp(script, "Zinh") || !strcmp(script, "Zzzz");
}

/* Runs of one script. Common and inherited characters belong to the run before them, or to the
 * first run when they start the text; text of nothing else is one run with an empty name. */
static HRESULT recognize_scripts(const WCHAR *text, DWORD length, DWORD index, MAPPING_PROPERTY_BAG *bag)
{
    struct script_run *runs;
    DWORD count = 0, pos = index, start;
    HRESULT hr;

    if (!(runs = malloc((length - index) * sizeof(*runs)))) return E_OUTOFMEMORY;
    while (pos < length)
    {
        unsigned int ch;
        const char *script;

        start = pos;
        ch = text[pos++];
        if (IS_HIGH_SURROGATE(ch) && pos < length && IS_LOW_SURROGATE(text[pos]))
            ch = 0x10000 + ((ch - 0xd800) << 10) + (text[pos++] - 0xdc00);
        script = unicode_script_code(ch);
        if (is_common(script)) continue;
        if (count && !strcmp(runs[count - 1].script, script)) continue;
        if (count) runs[count - 1].end = start - 1;
        runs[count].start = count ? start : index;
        runs[count].script = script;
        count++;
    }
    if (!count)
    {
        runs[0].start = index;
        runs[0].script = "";
        count = 1;
    }
    runs[count - 1].end = length - 1;
    hr = store_runs(runs, count, bag);
    free(runs);
    return hr;
}

struct recognition
{
    const struct service *service;
    const WCHAR *text;
    DWORD length, index;
    MAPPING_OPTIONS *options;
    MAPPING_PROPERTY_BAG *bag;
};

static void CALLBACK recognize_async(TP_CALLBACK_INSTANCE *instance, void *context)
{
    struct recognition *recognition = context;
    MAPPING_OPTIONS *options = recognition->options;
    HRESULT hr;

    hr = recognition->service->recognize(recognition->text, recognition->length, recognition->index,
            recognition->bag);
    options->pfnRecognizeCallback(recognition->bag, options->pRecognizeCallerData,
            options->dwRecognizeCallerDataSize, hr);
    free(recognition);
}

HRESULT WINAPI MappingRecognizeText(MAPPING_SERVICE_INFO *info, const WCHAR *text, DWORD length, DWORD index,
        MAPPING_OPTIONS *options, MAPPING_PROPERTY_BAG *bag)
{
    const struct service *service;
    struct recognition *recognition;

    TRACE("%p, %s, %lu, %lu, %p, %p.\n", info, debugstr_wn(text, length), length, index, options, bag);

    if (!(service = find_service(info)) || !text || !bag || bag->Size != sizeof(*bag)) return E_INVALIDARG;
    if (index >= length) return E_INVALIDARG;
    if (options)
    {
        if (options->Size != sizeof(*options)) return E_INVALIDARG;
        if (!is_text_plain(options->pszInputContentType) || !is_text_plain(options->pszOutputContentType))
            return E_INVALIDARG;
    }

    bag->prgResultRanges = NULL;
    bag->dwRangesCount = 0;
    bag->pServiceData = NULL;
    bag->dwServiceDataSize = 0;
    bag->pContext = NULL;
    if (options)
    {
        bag->pCallerData = options->pRecognizeCallerData;
        bag->dwCallerDataSize = options->dwRecognizeCallerDataSize;
    }

    if (!options || !options->pfnRecognizeCallback)
        return service->recognize(text, length, index, bag);

    if (!(recognition = malloc(sizeof(*recognition)))) return E_OUTOFMEMORY;
    recognition->service = service;
    recognition->text = text;
    recognition->length = length;
    recognition->index = index;
    recognition->options = options;
    recognition->bag = bag;
    if (!TrySubmitThreadpoolCallback(recognize_async, recognition, NULL))
    {
        free(recognition);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

HRESULT WINAPI MappingDoAction(MAPPING_PROPERTY_BAG *bag, DWORD range, const WCHAR *action)
{
    TRACE("%p, %lu, %s.\n", bag, range, debugstr_w(action));

    if (!bag || bag->Size != sizeof(*bag) || !action || range >= bag->dwRangesCount) return E_INVALIDARG;
    /* No range offers actions, so no action id is one of its own. */
    if (!bag->prgResultRanges[range].dwActionsCount) return E_INVALIDARG;
    return E_NOTIMPL;
}

HRESULT WINAPI MappingFreePropertyBag(MAPPING_PROPERTY_BAG *bag)
{
    TRACE("%p.\n", bag);

    if (!bag || bag->Size != sizeof(*bag)) return E_INVALIDARG;
    free(bag->pContext);
    bag->prgResultRanges = NULL;
    bag->dwRangesCount = 0;
    bag->pServiceData = NULL;
    bag->dwServiceDataSize = 0;
    bag->pContext = NULL;
    return S_OK;
}
