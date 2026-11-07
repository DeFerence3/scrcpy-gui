#include "platform/windows/FileDropTarget.h"
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <cstring>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
class FileData : public IDataObject {
public:
  bool supported = true;
  bool broken = false;
  std::vector<std::filesystem::path> paths{L"C:\\My files\\quote ' & 雪.txt", L"C:\\Photos\\photo.jpg"};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
    *out = static_cast<IDataObject *>(this); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    return supported && format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
    if (broken || FAILED(QueryGetData(format))) return DV_E_FORMATETC;
    std::wstring names;
    for (const auto &path : paths) { names += path.wstring(); names += L'\0'; }
    names += L'\0';
    auto bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
    medium->tymed = TYMED_HGLOBAL; medium->pUnkForRelease = nullptr;
    medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!medium->hGlobal) return E_OUTOFMEMORY;
    auto data = static_cast<DROPFILES *>(GlobalLock(medium->hGlobal));
    data->pFiles = sizeof(DROPFILES); data->fWide = TRUE;
    std::memcpy(reinterpret_cast<char *>(data) + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(medium->hGlobal); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }
};
}
int main() {
  HRESULT ole = OleInitialize(nullptr);
  if (FAILED(ole)) return 1;
  int result = 0;
  try {
    bool enabled = true, hover = false;
    std::vector<std::filesystem::path> received;
    auto target = app::create_file_drop_target([&] { return enabled; }, [&](bool value) { hover = value; },
      [&](auto paths) { received = std::move(paths); });
    auto hwnd = CreateWindowExW(0, L"STATIC", L"scrcpy drop test", 0, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(hwnd && SUCCEEDED(RegisterDragDrop(hwnd, target)), "Could not register native OLE target");
    FileData files;
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    target->DragEnter(&files, 0, {}, &effect);
    require(effect == DROPEFFECT_COPY && hover, "File drag was not accepted as copy");
    effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    target->Drop(&files, 0, {}, &effect);
    require(effect == DROPEFFECT_COPY && !hover && received == files.paths, "Native drop corrupted Unicode paths or lost files");
    received.clear(); enabled = false; effect = DROPEFFECT_COPY;
    target->DragEnter(&files, 0, {}, &effect);
    require(effect == DROPEFFECT_NONE && !hover, "Busy explorer accepted a drag");
    effect = DROPEFFECT_COPY; target->Drop(&files, 0, {}, &effect);
    require(effect == DROPEFFECT_NONE && received.empty(), "Busy explorer accepted a drop");
    enabled = true; effect = DROPEFFECT_MOVE;
    target->DragEnter(&files, 0, {}, &effect);
    require(effect == DROPEFFECT_NONE, "Move-only drag could delete local files");
    files.supported = false; effect = DROPEFFECT_COPY;
    target->DragEnter(&files, 0, {}, &effect);
    require(effect == DROPEFFECT_NONE, "Non-file drop was accepted");
    files.supported = true; effect = DROPEFFECT_COPY;
    target->DragEnter(&files, 0, {}, &effect);
    files.broken = true; effect = DROPEFFECT_COPY;
    target->Drop(&files, 0, {}, &effect);
    require(effect == DROPEFFECT_NONE && received.empty(), "Broken drop data was accepted");
    target->DragLeave(); require(!hover, "Drag hover was not cleared");
    RevokeDragDrop(hwnd); DestroyWindow(hwnd); target->Release();
    std::cout << "Native drop registration, multiple Unicode paths, copy semantics and rejection states passed.\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; result = 1; }
  OleUninitialize(); return result;
}
