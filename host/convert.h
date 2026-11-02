#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

// Конвертация BGRA -> NV12 целиком на GPU (D3D11 Video Processor).
// Энкодеры видеокарт принимают NV12, а экран захватывается в BGRA.
class Nv12Converter {
public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h);

    // Копирует новый кадр с экрана во внутреннюю текстуру (GPU->GPU).
    void update(ID3D11Texture2D* bgraFrame);

    // Конвертирует последний кадр в NV12. Возвращает текстуру из кольца
    // (кольцо нужно, чтобы энкодер ещё читал прошлую, пока пишем следующую).
    ID3D11Texture2D* convert();

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
    };
    static constexpr int kRing = 4;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vdev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> vctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> en_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> src_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView_;
    Slot ring_[kRing];
    int idx_ = 0;
};
