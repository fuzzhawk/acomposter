#include "DragOut.h"

#include "../core/Utf.h"

#include <windows.h>
#include <objidl.h>
#include <shlobj.h>

#include <cstring>
#include <new>

namespace acm::platform {
namespace {

// The one format offered: a DROPFILES header followed by a double-null
// terminated list of wide paths, which is what every Windows drop target
// understands.
FORMATETC hdropFormat() {
    FORMATETC format{};
    format.cfFormat = CF_HDROP;
    format.ptd = nullptr;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;
    return format;
}

// Builds the CF_HDROP block. Returns null if the allocation fails, which is the
// only way this can go wrong.
HGLOBAL buildDropFiles(const std::vector<std::string>& utf8Paths) {
    const std::wstring list = dropFileList(utf8Paths);

    const SIZE_T bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return nullptr;

    auto* header = static_cast<DROPFILES*>(::GlobalLock(memory));
    if (!header) {
        ::GlobalFree(memory);
        return nullptr;
    }

    header->pFiles = sizeof(DROPFILES);
    header->pt = POINT{ 0, 0 };
    header->fNC = FALSE;
    header->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(header) + sizeof(DROPFILES), list.data(),
                list.size() * sizeof(wchar_t));

    ::GlobalUnlock(memory);
    return memory;
}

// -- IEnumFORMATETC ---------------------------------------------------------
// Required by the interface even though there is only ever one format to
// enumerate. Drop targets do call it: Explorer asks what is on offer before
// deciding whether to light up.
class FormatEnumerator final : public IEnumFORMATETC {
public:
    explicit FormatEnumerator(ULONG index) : index_(index) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_INVALIDARG;
        if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC) {
            *out = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* out, ULONG* fetched) override {
        ULONG written = 0;
        while (written < count && index_ < 1) {
            out[written] = hdropFormat();
            ++written;
            ++index_;
        }
        if (fetched) *fetched = written;
        return written == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        index_ += count;
        return index_ <= 1 ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        index_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override {
        if (!out) return E_INVALIDARG;
        *out = new (std::nothrow) FormatEnumerator(index_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    ULONG references_ = 1;
    ULONG index_ = 0;
};

// -- IDataObject ------------------------------------------------------------
// Holds the CF_HDROP block and hands out copies of it. The medium is rebuilt
// per GetData call because the caller owns and frees whatever it is given, and
// a target is entitled to ask more than once.
class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(std::vector<std::string> paths) : paths_(std::move(paths)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_INVALIDARG;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request, STGMEDIUM* out) override {
        if (!request || !out) return E_INVALIDARG;
        if (QueryGetData(request) != S_OK) return DV_E_FORMATETC;

        HGLOBAL memory = buildDropFiles(paths_);
        if (!memory) return E_OUTOFMEMORY;

        *out = STGMEDIUM{};
        out->tymed = TYMED_HGLOBAL;
        out->hGlobal = memory;
        out->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        // Writing into a medium the caller allocated. Nothing that accepts a
        // file drag asks for this, and getting it subtly wrong is worse than
        // saying plainly that it is not supported.
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
        if (!request) return E_INVALIDARG;
        if (request->cfFormat != CF_HDROP) return DV_E_FORMATETC;
        if (!(request->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (request->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;   // the object is the source; nothing writes into it
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_INVALIDARG;
        if (direction != DATADIR_GET) {
            *out = nullptr;
            return E_NOTIMPL;
        }
        *out = new (std::nothrow) FormatEnumerator(0);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ULONG references_ = 1;
    std::vector<std::string> paths_;
};

// -- IDropSource ------------------------------------------------------------
class DropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_INVALIDARG;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        // The drag ends when the button that started it comes up. Testing only
        // the left button would strand a drag begun with either of the others.
        if (!(keyState & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        // DRAGDROP_S_USEDEFAULTCURSORS asks the shell for the standard copy and
        // no-drop cursors, which is what a user reads as "this will work here".
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ULONG references_ = 1;
};

} // namespace

std::wstring dropFileList(const std::vector<std::string>& utf8Paths) {
    std::wstring list;
    for (const std::string& path : utf8Paths) {
        if (path.empty()) continue;   // an empty path would end the list early
        list += utf8ToWide(path);
        list.push_back(L'\0');
    }
    list.push_back(L'\0');   // the second terminator ends the list
    return list;
}

bool dragOutFiles(const std::vector<std::string>& utf8Paths) {
    if (utf8Paths.empty()) return false;

    // The window holds the mouse capture for the length of a press, and
    // DoDragDrop takes it for itself. Handing it over here rather than fighting
    // over it is what makes the drop targets on the other side see the pointer
    // at all.
    ::ReleaseCapture();

    auto* data = new (std::nothrow) FileDataObject(utf8Paths);
    auto* source = new (std::nothrow) DropSource();

    bool dropped = false;
    if (data && source) {
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT result = ::DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
        dropped = result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
    }

    if (data) data->Release();
    if (source) source->Release();
    return dropped;
}

} // namespace acm::platform
