#include "encoder.h"
#include <codecapi.h>
#include <chrono>

using Microsoft::WRL::ComPtr;

static void setU32(ICodecAPI* c, const GUID& key, ULONG v) {
    VARIANT var; VariantInit(&var);
    var.vt = VT_UI4; var.ulVal = v;
    c->SetValue(&key, &var);
}
static void setBool(ICodecAPI* c, const GUID& key, bool v) {
    VARIANT var; VariantInit(&var);
    var.vt = VT_BOOL; var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    c->SetValue(&key, &var);
}

H264Encoder::~H264Encoder() {
    codec_.Reset(); gen_.Reset(); mft_.Reset(); mgr_.Reset();
    MFShutdown();
}

bool H264Encoder::init(ID3D11Device* dev, UINT w, UINT h, UINT fps, UINT bitrate) {
    fps_ = fps;
    if (FAILED(MFStartup(MF_VERSION))) return false;

    MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &outInfo, &acts, &n);
    if (FAILED(hr) || n == 0) return false;

    // На ПК с несколькими видеокартами пробуем кодеры по очереди:
    // подойдёт тот, что работает на том же адаптере, что и наше устройство D3D11.
    bool ok = false;
    for (UINT32 i = 0; i < n && !ok; ++i) {
        if (setup(acts[i], dev, w, h, bitrate)) {
            ok = true;
            WCHAR* s = nullptr; UINT32 len = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len))) {
                char buf[256] = {};
                WideCharToMultiByte(CP_ACP, 0, s, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
                name_ = buf;
                CoTaskMemFree(s);
            }
        }
    }
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return ok;
}

bool H264Encoder::setup(IMFActivate* act, ID3D11Device* dev, UINT w, UINT h, UINT bitrate) {
    mft_.Reset(); gen_.Reset(); codec_.Reset(); mgr_.Reset();

    if (FAILED(act->ActivateObject(IID_PPV_ARGS(&mft_)))) return false;

    ComPtr<IMFAttributes> at;
    if (SUCCEEDED(mft_->GetAttributes(&at))) {
        at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);   // аппаратные MFT асинхронные
        at->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &mgr_))) return false;
    if (FAILED(mgr_->ResetDevice(dev, token))) return false;
    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr_.Get())))
        return false;

    // Сначала выходной тип (H.264), потом входной (NV12).
    ComPtr<IMFMediaType> mo;
    MFCreateMediaType(&mo);
    mo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    mo->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(mo.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(mo.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(mo.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mo->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);   // без B-кадров, годится для WebRTC
    if (FAILED(mft_->SetOutputType(0, mo.Get(), 0))) return false;

    ComPtr<IMFMediaType> mi;
    MFCreateMediaType(&mi);
    mi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(mi.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(mi.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(mi.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(mft_->SetInputType(0, mi.Get(), 0))) return false;

    // Настройки качества и задержки (если драйвер что-то не поддерживает - молча пропускаем).
    if (SUCCEEDED(mft_.As(&codec_))) {
        setU32(codec_.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setU32(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
        setBool(codec_.Get(), CODECAPI_AVLowLatencyMode, true);
        setU32(codec_.Get(), CODECAPI_AVEncMPVGOPSize, fps_ * 2);
        setU32(codec_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }

    if (FAILED(mft_.As(&gen_))) return false;   // нужен асинхронный MFT

    MFT_OUTPUT_STREAM_INFO si{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &si))) {
        provides_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                   MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outSize_ = si.cbSize ? si.cbSize : 1 << 20;
    }

    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    need_ = have_ = 0;
    started_ = true;
    return true;
}

// Забираем события энкодера: "готов принять кадр" и "есть готовый пакет".
void H264Encoder::pump() {
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(gen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev))) break;
        MediaEventType t = MEUnknown;
        ev->GetType(&t);
        if (t == METransformNeedInput) ++need_;
        else if (t == METransformHaveOutput) ++have_;
    }
}

bool H264Encoder::submit(ID3D11Texture2D* tex, LONGLONG pts) {
    if (!started_) return false;

    // NVENC выдаёт "готов принять кадр" только после того, как мы забрали
    // предыдущий пакет. Поэтому: забираем пакеты и ждём сигнал до ~8 мс.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
    for (;;) {
        pump();
        collect();
        if (need_ > 0) break;
        if (std::chrono::steady_clock::now() > deadline) return false;
        Sleep(0);
    }

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &buf)))
        return false;
    ComPtr<IMF2DBuffer> b2;
    DWORD len = 0;
    if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len)))
        buf->SetCurrentLength(len);

    ComPtr<IMFSample> smp;
    MFCreateSample(&smp);
    smp->AddBuffer(buf.Get());
    smp->SetSampleTime(pts);
    smp->SetSampleDuration(10000000LL / fps_);

    if (FAILED(mft_->ProcessInput(0, smp.Get(), 0))) return false;
    --need_;
    return true;
}

bool H264Encoder::fetchOne(std::vector<uint8_t>& out, LONGLONG& pts) {
    if (!started_) return false;
    if (have_ <= 0) return false;
    --have_;

    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = 0;
    ComPtr<IMFSample> own;
    if (!provides_) {
        MFCreateSample(&own);
        ComPtr<IMFMediaBuffer> mb;
        MFCreateMemoryBuffer(outSize_, &mb);
        own->AddBuffer(mb.Get());
        ob.pSample = own.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    if (FAILED(hr)) {
        if (provides_ && ob.pSample) ob.pSample->Release();
        return false;
    }

    IMFSample* smp = ob.pSample;
    pts = 0;
    if (smp) smp->GetSampleTime(&pts);
    bool ok = false;
    ComPtr<IMFMediaBuffer> mb;
    if (smp && SUCCEEDED(smp->ConvertToContiguousBuffer(&mb))) {
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(mb->Lock(&p, nullptr, &len))) {
            out.assign(p, p + len);
            mb->Unlock();
            ok = true;
        }
    }
    if (provides_ && smp) smp->Release();
    return ok;
}

void H264Encoder::requestKeyFrame() {
    if (codec_) setU32(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
}

void H264Encoder::collect() {
    std::vector<uint8_t> tmp;
    LONGLONG pts = 0;
    while (fetchOne(tmp, pts)) {
        queue_.push_back(Packet{ std::move(tmp), pts });
        tmp.clear();
        pump();   // после забора пакета может прийти новый NeedInput/HaveOutput
    }
}

bool H264Encoder::poll(std::vector<uint8_t>& out, LONGLONG* pts100ns) {
    if (!started_) return false;
    pump();
    collect();
    if (queue_.empty()) return false;
    out = std::move(queue_.front().data);
    if (pts100ns) *pts100ns = queue_.front().pts;
    queue_.pop_front();
    return true;
}
