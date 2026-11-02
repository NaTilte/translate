#pragma once
#include <rtc/rtc.hpp>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>

// Отправка H.264 в браузер по WebRTC (libdatachannel).
// Сигналинг (обмен offer/answer) сюда НЕ входит: класс только создаёт offer
// и принимает answer, а как строки попадут к телефону, решает вызывающий код.
class WebRtcSender {
public:
    bool init(const std::string& stunUrl);

    // Создаёт offer и ждёт сбора ICE-кандидатов (до 5 сек). Возвращает полный SDP.
    std::string createOffer();
    bool setAnswer(const std::string& sdp);

    bool ready() const { return track_ && track_->isOpen(); }
    bool failed() const { return failed_; }

    // Отправить один закодированный кадр (Annex B). seconds - время кадра от начала трансляции.
    void sendFrame(const uint8_t* data, size_t size, double seconds);

    // Браузер просит ключевой кадр (PLI) - вызывающий код должен позвать encoder.requestKeyFrame().
    bool takeKeyframeRequest() { return keyReq_.exchange(false); }

private:
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> track_;
    std::promise<void> gathered_;
    std::atomic<bool> gatheredSet_{ false };
    std::atomic<bool> keyReq_{ false };
    std::atomic<bool> failed_{ false };
};
