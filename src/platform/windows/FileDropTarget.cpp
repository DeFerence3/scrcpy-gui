#include "platform/windows/FileDropTarget.h"
#include <shellapi.h>

namespace app {
namespace {
// Winit's file-drop events are not exposed by the Slint version used here.
// Register a native OLE copy target, enabled only on the tray's Files page.
class FileDropTarget : public IDropTarget {
  ULONG refs_ = 1;
  bool files_ = false;
  std::function<bool()> enabled_;
  std::function<void(bool)> hover_;
  std::function<void(std::vector<std::filesystem::path>)> drop_;
  static FORMATETC format() { return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }
  void effect(DWORD *effect) {
    bool accept = files_ && enabled_() && (*effect & DROPEFFECT_COPY);
    *effect = accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    hover_(accept);
  }
public:
  FileDropTarget(std::function<bool()> enabled, std::function<void(bool)> hover,
                 std::function<void(std::vector<std::filesystem::path>)> drop)
      : enabled_(std::move(enabled)), hover_(std::move(hover)), drop_(std::move(drop)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
    *object = static_cast<IDropTarget *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { auto refs = --refs_; if (!refs) delete this; return refs; }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL, DWORD *result) override {
    auto spec = format(); files_ = SUCCEEDED(data->QueryGetData(&spec)); effect(result); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *result) override { effect(result); return S_OK; }
  HRESULT STDMETHODCALLTYPE DragLeave() override { files_ = false; hover_(false); return S_OK; }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL, DWORD *result) override {
    effect(result);
    hover_(false);
    if (*result == DROPEFFECT_NONE) return S_OK;
    auto spec = format(); STGMEDIUM medium{};
    if (FAILED(data->GetData(&spec, &medium))) { *result = DROPEFFECT_NONE; return S_OK; }
    std::vector<std::filesystem::path> paths;
    auto handle = static_cast<HDROP>(medium.hGlobal);
    UINT count = DragQueryFileW(handle, 0xffffffff, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
      UINT size = DragQueryFileW(handle, i, nullptr, 0);
      std::wstring path(size + 1, L'\0');
      DragQueryFileW(handle, i, path.data(), size + 1);
      path.resize(size); paths.emplace_back(path);
    }
    ReleaseStgMedium(&medium);
    files_ = false;
    if (paths.empty()) { *result = DROPEFFECT_NONE; return S_OK; }
    drop_(std::move(paths));
    return S_OK;
  }
};
}
IDropTarget *create_file_drop_target(std::function<bool()> enabled,
    std::function<void(bool)> hover,
    std::function<void(std::vector<std::filesystem::path>)> drop) {
  return new FileDropTarget(std::move(enabled), std::move(hover), std::move(drop));
}
}
