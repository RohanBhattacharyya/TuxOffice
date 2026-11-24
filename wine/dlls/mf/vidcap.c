/*
 * Video capture devices
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

#define COBJMACROS

#include "mfapi.h"
#include "mfidl.h"
#include "mferror.h"
#include "mf_private.h"
#include "wine/mfinternal.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* winegstreamer lists the cameras, as an activation object for each. */
static HRESULT get_video_capture_devices(IMFCollection **devices)
{
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance(&CLSID_WineVideoCaptureDevices, NULL, CLSCTX_INPROC_SERVER,
            &IID_IMFCollection, (void **)devices)))
        WARN("No video capture devices, hr %#lx.\n", hr);
    return hr;
}

HRESULT enum_video_capture_sources(IMFAttributes *attributes, IMFActivate ***ret_sources, UINT32 *ret_count)
{
    IMFActivate **sources;
    IMFCollection *devices;
    DWORD count, i, found = 0;
    IUnknown *element;
    HRESULT hr;

    *ret_sources = NULL;
    *ret_count = 0;
    if (FAILED(get_video_capture_devices(&devices))) return S_OK;
    if (FAILED(hr = IMFCollection_GetElementCount(devices, &count)) || !count)
    {
        IMFCollection_Release(devices);
        return hr;
    }
    if (!(sources = CoTaskMemAlloc(count * sizeof(*sources))))
    {
        IMFCollection_Release(devices);
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++)
    {
        if (FAILED(IMFCollection_GetElement(devices, i, &element))) continue;
        if (SUCCEEDED(IUnknown_QueryInterface(element, &IID_IMFActivate, (void **)&sources[found]))) found++;
        IUnknown_Release(element);
    }
    IMFCollection_Release(devices);
    if (!found)
    {
        CoTaskMemFree(sources);
        return S_OK;
    }
    *ret_sources = sources;
    *ret_count = found;
    TRACE("%lu video capture devices.\n", found);
    return S_OK;
}

HRESULT create_video_capture_source(IMFAttributes *attributes, IMFMediaSource **source)
{
    WCHAR *link, *other;
    IMFActivate **devices;
    UINT32 count, i, size;
    HRESULT hr;

    if (FAILED(hr = IMFAttributes_GetAllocatedString(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &link, &size)))
        return hr;
    TRACE("link %s.\n", debugstr_w(link));

    hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (SUCCEEDED(enum_video_capture_sources(attributes, &devices, &count)))
    {
        for (i = 0; i < count; i++)
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
                    SUCCEEDED(IMFActivate_GetAllocatedString(devices[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                    &other, &size)))
            {
                if (!wcsicmp(link, other))
                    hr = IMFActivate_ActivateObject(devices[i], &IID_IMFMediaSource, (void **)source);
                CoTaskMemFree(other);
            }
            IMFActivate_Release(devices[i]);
        }
        CoTaskMemFree(devices);
    }
    CoTaskMemFree(link);
    return hr;
}
