#include "webrtc.h"
#include <chrono>
#include <iostream>

bool WebRtcSender::init(const std::string& stunUrl) {
    try {
        rtc::InitLogger(rtc::LogLevel::Info);

        rtc::Configuration cfg;
        cfg.iceServers.emplace_back(stunUrl);
        pc_ = std::make_shared<rtc::PeerConnection>(cfg);

        pc_->onStateChange([this](rtc::PeerConnection::State s) {
            std::cout << "[webrtc] состояние: " << s << std::endl;
            if (s == rtc::PeerConnection::State::Failed) failed_ = true;
        });
        pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
            if (s == rtc::PeerConnection::GatheringState::Complete && !gatheredSet_.exchange(true))
                gathered_.set_value();
        });

        pc_->onIceStateChange([](rtc::PeerConnection::IceState s) {
            std::cout << "[webrtc] ICE: " << s << std::endl;
        });
        pc_->onLocalCandidate([](rtc::Candidate c) {
            std::cout << "[webrtc] свой кандидат: " << std::string(c) << std::endl;
        });

        const uint32_t ssrc = 42;
        const int payloadType = 96;

        // Видеотрек только на отправку. Профиль - constrained baseline, level 4.2 (1080p60).
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(payloadType,
            "profile-level-id=42e02a;packetization-mode=1;level-asymmetry-allowed=1");
        video.addSSRC(ssrc, "screen");
        track_ = pc_->addTrack(video);

        // Цепочка: H.264 -> RTP пакеты, отчёты отправителя, повторная отправка по NACK, реакция на PLI.
        auto rtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "screen", payloadType, 90000);
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpCfg);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpCfg));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([this]() { keyReq_ = true; }));
        track_->setMediaHandler(packetizer);
        return true;
    } catch (const std::exception& e) {
        std::cout << "[webrtc] ошибка инициализации: " << e.what() << std::endl;
        return false;
    }
}

std::string WebRtcSender::createOffer() {
    try {
        pc_->setLocalDescription();   // без удалённого описания это offer
        gathered_.get_future().wait_for(std::chrono::seconds(5));
        auto d = pc_->localDescription();
        return d ? std::string(*d) : std::string();
    } catch (const std::exception& e) {
        std::cout << "[webrtc] ошибка offer: " << e.what() << std::endl;
        return {};
    }
}

bool WebRtcSender::setAnswer(const std::string& sdp) {
    try {
        pc_->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
        return true;
    } catch (const std::exception& e) {
        std::cout << "[webrtc] ошибка answer: " << e.what() << std::endl;
        return false;
    }
}

void WebRtcSender::sendFrame(const uint8_t* data, size_t size, double seconds) {
    if (!ready()) return;
    try {
        rtc::binary frame(reinterpret_cast<const std::byte*>(data),
                          reinterpret_cast<const std::byte*>(data) + size);
        track_->sendFrame(std::move(frame),
                          rtc::FrameInfo(std::chrono::duration<double>(seconds)));
    } catch (const std::exception&) {
        // канал мог закрыться - пропускаем кадр
    }
}
