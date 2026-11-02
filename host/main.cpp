#include "capture.h"
#include "convert.h"
#include "encoder.h"
#include "webrtc.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

// ЭТАП 3 (тест видеоканала): экран -> NVENC -> WebRTC -> браузер.
// Сигналинг пока ручной через буфер обмена (потом заменим на Telegram-бота):
//   хост кладёт в буфер "RDOFFER:...", client.html отвечает "RDANS:...".

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64enc(const std::string& in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; valb += 8;
        while (valb >= 0) { out.push_back(B64[(val >> valb) & 0x3F]); valb -= 6; }
    }
    if (valb > -6) out.push_back(B64[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static std::string b64dec(const std::string& in) {
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        const char* p = c ? std::strchr(B64, c) : nullptr;
        if (!p) continue;
        val = (val << 6) + int(p - B64); valb += 6;
        if (valb >= 0) { out.push_back(char((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

static void setClipboard(const std::string& s) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
    if (h) {
        std::memcpy(GlobalLock(h), s.c_str(), s.size() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

static std::string getClipboard() {
    std::string r;
    if (!OpenClipboard(nullptr)) return r;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* p = static_cast<const char*>(GlobalLock(h))) { r = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    return r;
}

int main() {
    using clock = std::chrono::steady_clock;
    constexpr UINT FPS = 60;
    constexpr UINT BITRATE = 8'000'000;
    constexpr int MAX_MINUTES = 30;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DxgiCapture cap;
    if (!cap.init()) { std::printf("Не удалось запустить захват\n"); return 1; }
    Nv12Converter conv;
    if (!conv.init(cap.device(), cap.context(), cap.width(), cap.height())) {
        std::printf("Не удалось создать конвертер NV12\n"); return 2;
    }
    H264Encoder enc;
    if (!enc.init(cap.device(), cap.width(), cap.height(), FPS, BITRATE)) {
        std::printf("Аппаратный H.264 энкодер не запустился\n"); return 3;
    }
    std::printf("Экран %ux%u, энкодер: %s\n", cap.width(), cap.height(), enc.name().c_str());

    WebRtcSender sender;
    if (!sender.init("stun:stun.l.google.com:19302")) return 4;

    std::string offer = sender.createOffer();
    if (offer.empty()) { std::printf("Не удалось создать offer\n"); return 5; }
    setClipboard("RDOFFER:" + b64enc(offer));
    std::printf("\nКод подключения скопирован в буфер обмена.\n"
                "1) Открой client.html в Chrome или Firefox\n"
                "2) Вставь код (Ctrl+V) и нажми «Подключить»\n"
                "Жду ответ (до 5 минут)...\n");

    // Ждём "RDANS:..." в буфере обмена.
    bool gotAnswer = false;
    for (int i = 0; i < 600 && !gotAnswer; ++i) {
        Sleep(500);
        std::string c = getClipboard();
        if (c.rfind("RDANS:", 0) == 0) {
            std::string sdp = b64dec(c.substr(6));
            int cand = 0;
            for (size_t pos = sdp.find("a=candidate"); pos != std::string::npos;
                 pos = sdp.find("a=candidate", pos + 1)) {
                size_t e = sdp.find("\r\n", pos);
                std::printf("кандидат браузера: %s\n", sdp.substr(pos, e - pos).c_str());
                ++cand;
            }
            if (cand == 0) std::printf("В ответе браузера НЕТ кандидатов\n");
            gotAnswer = sender.setAnswer(sdp);
            if (!gotAnswer) std::printf("Ответ не принят, жду другой...\n");
        }
    }
    if (!gotAnswer) { std::printf("Ответ не получен\n"); return 6; }

    std::printf("Ответ принят, устанавливаю соединение...\n");
    for (int i = 0; i < 400 && !sender.ready() && !sender.failed(); ++i) Sleep(100);
    if (!sender.ready()) { std::printf("Соединение не установилось\n"); return 7; }
    std::printf("Соединено! Идёт трансляция (до %d мин, закрой окно чтобы остановить).\n", MAX_MINUTES);

    const auto t0 = clock::now();
    const auto period = std::chrono::nanoseconds(1'000'000'000 / FPS);
    auto next = t0;
    auto lastReport = t0;
    size_t frames = 0, bytes = 0;
    enc.requestKeyFrame();   // браузеру нужен ключевой кадр сразу после подключения

    std::vector<uint8_t> pkt;
    auto drain = [&] {
        LONGLONG pts = 0;
        while (enc.poll(pkt, &pts)) {
            sender.sendFrame(pkt.data(), pkt.size(), double(pts) / 1e7);
            ++frames; bytes += pkt.size();
        }
    };

    while (!sender.failed() && clock::now() - t0 < std::chrono::minutes(MAX_MINUTES)) {
        for (;;) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(next - clock::now()).count();
            UINT wait = (UINT)std::max<long long>(0, left);
            ID3D11Texture2D* tex = nullptr;
            DXGI_OUTDUPL_FRAME_INFO info{};
            auto r = cap.acquire(wait, &tex, &info);
            if (r == DxgiCapture::Result::Frame) {
                if (info.LastPresentTime.QuadPart != 0) conv.update(tex);
                tex->Release();
                cap.release();
            } else if (r == DxgiCapture::Result::Lost) {
                Sleep(300);
                cap.reopen();
            } else if (r == DxgiCapture::Result::Error) {
                Sleep(10);
            }
            if (r == DxgiCapture::Result::Timeout || clock::now() >= next) break;
        }

        const auto tick = next;
        next += period;
        if (clock::now() - next > std::chrono::milliseconds(100)) next = clock::now();

        if (sender.takeKeyframeRequest()) enc.requestKeyFrame();

        const LONGLONG pts = std::chrono::duration_cast<std::chrono::nanoseconds>(tick - t0).count() / 100;
        if (ID3D11Texture2D* nv12 = conv.convert()) enc.submit(nv12, pts);
        drain();

        if (clock::now() - lastReport >= std::chrono::seconds(2)) {
            double sec = std::chrono::duration<double>(clock::now() - lastReport).count();
            std::printf("кадров/с: %.0f | битрейт: %.1f Мбит/с\n", frames / sec, bytes * 8.0 / sec / 1e6);
            frames = 0; bytes = 0;
            lastReport = clock::now();
        }
    }
    return 0;
}
