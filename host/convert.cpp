#include "convert.h"

using Microsoft::WRL::ComPtr;

bool Nv12Converter::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h) {
    ctx_ = ctx;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vdev_)))) return false;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx_)))) return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = cd.OutputWidth = w;
    cd.InputHeight = cd.OutputHeight = h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &en_))) return false;
    if (FAILED(vdev_->CreateVideoProcessor(en_.Get(), 0, &vp_))) return false;

    // Входная текстура BGRA: сюда копируется каждый новый кадр с экрана.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &src_))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.FourCC = 0;
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    iv.Texture2D.MipSlice = 0;
    iv.Texture2D.ArraySlice = 0;
    if (FAILED(vdev_->CreateVideoProcessorInputView(src_.Get(), en_.Get(), &iv, &inView_)))
        return false;

    // Кольцо выходных NV12-текстур.
    td.Format = DXGI_FORMAT_NV12;
    for (auto& s : ring_) {
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &s.tex))) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET;     // запасной вариант
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &s.tex))) return false;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ov.Texture2D.MipSlice = 0;
        if (FAILED(vdev_->CreateVideoProcessorOutputView(s.tex.Get(), en_.Get(), &ov, &s.view)))
            return false;
    }

    // Цвет: вход - полный диапазон RGB, выход - BT.709 limited (как ждут браузеры).
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.RGB_Range = 0;
    vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.YCbCr_Matrix = 1;   // BT.709
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
    return true;
}

void Nv12Converter::update(ID3D11Texture2D* bgraFrame) {
    ctx_->CopyResource(src_.Get(), bgraFrame);
}

ID3D11Texture2D* Nv12Converter::convert() {
    Slot& s = ring_[idx_];
    idx_ = (idx_ + 1) % kRing;

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = inView_.Get();
    if (FAILED(vctx_->VideoProcessorBlt(vp_.Get(), s.view.Get(), 0, 1, &st)))
        return nullptr;
    return s.tex.Get();
}
