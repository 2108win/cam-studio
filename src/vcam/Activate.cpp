#include "Activate.h"

HRESULT Activate::RuntimeClassInitialize()
{
    return MFCreateAttributes(&m_attrs, 4);
}

IFACEMETHODIMP Activate::ActivateObject(REFIID riid, void** ppv)
{
    RETURN_HR_IF_NULL(E_POINTER, ppv);
    *ppv = nullptr;

    // Frame Server may call Shutdown on the source directly and then activate
    // again; never hand out a source that has been shut down.
    if (m_source && m_source->IsShutdown()) m_source.Reset();
    if (!m_source)
    {
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaSource>(&m_source, m_attrs.Get()));
    }
    return m_source->QueryInterface(riid, ppv);
}

IFACEMETHODIMP Activate::ShutdownObject()
{
    if (m_source)
    {
        m_source->Shutdown();
        m_source.Reset();
    }
    return S_OK;
}

IFACEMETHODIMP Activate::DetachObject()
{
    m_source.Reset();
    return S_OK;
}
