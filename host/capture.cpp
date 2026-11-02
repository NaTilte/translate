#include "capture.h"
#include <d3d11_4.h>

using Microsoft::WRL::ComPtr;

bool DxgiCapture::init(UINT outputIndex) {
    outputIndex_ = outputIndex;
    dupl_.Reset(); res_.Reset(); ctx_.Reset(); dev_.Reset();
    held_ = false;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev_, &fl, &ctx_);
    if (FAILED(hr)) return false;

    // Энкодер Media Foundation работает со своим устройством из своих потоков.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(dev_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    return reopen();
}

bool DxgiCapture::reopen() {
    dupl_.Reset(); res_.Reset();
    held_ = false;

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput1> output1;

    if (FAILED(dev_.As(&dxgiDev))) return false;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;
    if (FAILED(adapter->EnumOutputs(outputIndex_, &output))) return false;
    if (FAILED(output.As(&output1))) return false;

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    width_  = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
    height_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

    return SUCCEEDED(output1->DuplicateOutput(dev_.Get(), &dupl_));
}

DxgiCapture::Result DxgiCapture::acquire(UINT timeoutMs, ID3D11Texture2D** outTex,
                                         DXGI_OUTDUPL_FRAME_INFO* outInfo) {
    if (!dupl_) return Result::Lost;
    if (held_) release();

    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, outInfo, &res_);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::Timeout;
    if (hr == DXGI_ERROR_ACCESS_LOST)  return Result::Lost;   // UAC, Win+L, смена разрешения
    if (FAILED(hr)) return Result::Error;

    held_ = true;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res_.As(&tex))) { release(); return Result::Error; }
    *outTex = tex.Detach();   // вызывающий делает Release()
    return Result::Frame;
}

void DxgiCapture::release() {
    if (held_ && dupl_) dupl_->ReleaseFrame();
    res_.Reset();
    held_ = false;
}
