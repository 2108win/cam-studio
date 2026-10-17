#pragma once

#include "Common.h"
#include "MediaSource.h"

// The COM object Frame Server creates from our CLSID. Frame Server copies
// the attributes the host set on IMFVirtualCamera into it, then calls
// ActivateObject to get the media source.
class Activate
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>>
{
public:
    HRESULT RuntimeClassInitialize();

    // IMFActivate
    IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    IFACEMETHODIMP ShutdownObject() override;
    IFACEMETHODIMP DetachObject() override;

    // IMFAttributes: delegated to an internal store.
    IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return m_attrs->GetItem(k, v); }
    IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return m_attrs->GetItemType(k, t); }
    IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return m_attrs->CompareItem(k, v, r); }
    IFACEMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return m_attrs->Compare(o, m, r); }
    IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return m_attrs->GetUINT32(k, v); }
    IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return m_attrs->GetUINT64(k, v); }
    IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return m_attrs->GetDouble(k, v); }
    IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return m_attrs->GetGUID(k, v); }
    IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return m_attrs->GetStringLength(k, l); }
    IFACEMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override { return m_attrs->GetString(k, v, n, l); }
    IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return m_attrs->GetAllocatedString(k, v, l); }
    IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return m_attrs->GetBlobSize(k, s); }
    IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* s) override { return m_attrs->GetBlob(k, b, n, s); }
    IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return m_attrs->GetAllocatedBlob(k, b, s); }
    IFACEMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* p) override { return m_attrs->GetUnknown(k, r, p); }
    IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return m_attrs->SetItem(k, v); }
    IFACEMETHODIMP DeleteItem(REFGUID k) override { return m_attrs->DeleteItem(k); }
    IFACEMETHODIMP DeleteAllItems() override { return m_attrs->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return m_attrs->SetUINT32(k, v); }
    IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return m_attrs->SetUINT64(k, v); }
    IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return m_attrs->SetDouble(k, v); }
    IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return m_attrs->SetGUID(k, v); }
    IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return m_attrs->SetString(k, v); }
    IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 s) override { return m_attrs->SetBlob(k, b, s); }
    IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* p) override { return m_attrs->SetUnknown(k, p); }
    IFACEMETHODIMP LockStore() override { return m_attrs->LockStore(); }
    IFACEMETHODIMP UnlockStore() override { return m_attrs->UnlockStore(); }
    IFACEMETHODIMP GetCount(UINT32* c) override { return m_attrs->GetCount(c); }
    IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return m_attrs->GetItemByIndex(i, k, v); }
    IFACEMETHODIMP CopyAllItems(IMFAttributes* d) override { return m_attrs->CopyAllItems(d); }

private:
    ComPtr<IMFAttributes> m_attrs;
    ComPtr<MediaSource> m_source;
};
