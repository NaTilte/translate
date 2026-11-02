#pragma once
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <wrl/client.h>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Аппаратный H.264 энкодер видеокарты (NVENC / AMD AMF / Intel QSV)
// через Media Foundation. Вход - NV12 текстура в видеопамяти.
class H264Encoder {
public:
    ~H264Encoder();

    bool init(ID3D11Device* dev, UINT w, UINT h, UINT fps, UINT bitrateBps);

    // false = энкодер сейчас занят, кадр пропущен (естественное ограничение нагрузки).
    bool submit(ID3D11Texture2D* nv12, LONGLONG pts100ns);

    // true = готов очередной пакет H.264 (Annex B), его можно слать в WebRTC.
    bool poll(std::vector<uint8_t>& out, LONGLONG* pts100ns = nullptr);

    void requestKeyFrame();
    const std::string& name() const { return name_; }

private:
    bool setup(IMFActivate* act, ID3D11Device* dev, UINT w, UINT h, UINT bitrate);
    void pump();
    bool fetchOne(std::vector<uint8_t>& out, LONGLONG& pts);   // забрать один готовый пакет
    void collect();                             // забрать все готовые пакеты в очередь

    struct Packet { std::vector<uint8_t> data; LONGLONG pts; };
    std::deque<Packet> queue_;

    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> gen_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mgr_;
    Microsoft::WRL::ComPtr<ICodecAPI> codec_;
    std::string name_;
    UINT fps_ = 60;
    int need_ = 0, have_ = 0;
    bool provides_ = false;
    DWORD outSize_ = 0;
    bool started_ = false;
};
