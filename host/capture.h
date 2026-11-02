#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// Захват экрана через DXGI Desktop Duplication.
// Устройство D3D11 создаётся один раз и делится с конвертером и энкодером,
// поэтому кадр ни разу не покидает видеопамять.
class DxgiCapture {
public:
    enum class Result { Frame, Timeout, Lost, Error };

    bool init(UINT outputIndex = 0);   // создаёт устройство + дубликацию
    bool reopen();                     // только пересоздаёт дубликацию (устройство остаётся)

    Result acquire(UINT timeoutMs, ID3D11Texture2D** outTex,
                   DXGI_OUTDUPL_FRAME_INFO* outInfo);
    void release();

    UINT width() const { return width_; }
    UINT height() const { return height_; }
    ID3D11Device* device() const { return dev_.Get(); }
    ID3D11DeviceContext* context() const { return ctx_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dupl_;
    Microsoft::WRL::ComPtr<IDXGIResource> res_;
    UINT outputIndex_ = 0;
    UINT width_ = 0, height_ = 0;
    bool held_ = false;
};
