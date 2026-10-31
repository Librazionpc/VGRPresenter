#include "modules/display/HtmlOutputServer.hpp"

#include "modules/rendering/PngCodec.hpp"
#include "platform/ISocket.hpp"
#include "platform/PlatformAccessor.hpp"

#include <format>
#include <string_view>

namespace bps::display {

namespace {
constexpr std::string_view kPage = R"html(<!doctype html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>VGR Presenter</title><style>
html,body{margin:0;width:100%;height:100%;background:#000;overflow:hidden}
img{width:100%;height:100%;object-fit:contain}
</style></head><body><img id="feed" alt="Waiting for video">
<script>const f=document.getElementById('feed');let previous='';async function tick(){
try{const r=await fetch('/frame.png?'+Date.now(),{cache:'no-store'});
if(r.ok){const next=URL.createObjectURL(await r.blob());f.src=next;
if(previous)URL.revokeObjectURL(previous);previous=next;}}catch(e){}
setTimeout(tick,100)}tick();</script></body></html>)html";

Result<void> SendResponse(platform::ISocket& socket, std::string_view status,
                          std::string_view contentType, std::string_view body) {
    const std::string header = std::format(
        "HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, contentType, body.size());
    if (auto sent = socket.Send(header); !sent.ok()) return sent.error();
    if (!body.empty()) {
        if (auto sent = socket.Send(body); !sent.ok()) return sent.error();
    }
    return Ok();
}
} // namespace

HtmlOutputServer::~HtmlOutputServer() { Stop(); }

Result<void> HtmlOutputServer::Start(uint16_t port) {
    if (running_.load(std::memory_order_acquire)) return Ok();
    if (worker_.joinable()) worker_.join();
    auto listener = platform::PlatformAccessor::Get().Sockets().Listen(port);
    if (!listener.ok()) return listener.error();

    listener_ = std::move(listener.value());
    port_.store(listener_->Port(), std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { Serve(); });
    return Ok();
}

void HtmlOutputServer::Stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    if (listener_) listener_->Close();
    listener_.reset();
    port_.store(0, std::memory_order_release);
}

std::string HtmlOutputServer::Url() const {
    const uint16_t port = port_.load(std::memory_order_acquire);
    return port == 0 ? std::string{} : std::format("http://127.0.0.1:{}/", port);
}

void HtmlOutputServer::PublishFrame(const rendering::Frame& frame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.pixels.size() < static_cast<size_t>(frame.width) * frame.height)
        return;
    auto copy = std::make_shared<rendering::Frame>(frame);
    std::lock_guard<std::mutex> lock(frameMutex_);
    latestFrame_ = std::move(copy);
}

void HtmlOutputServer::Serve() {
    while (running_.load(std::memory_order_acquire)) {
        auto accepted = listener_->Accept(200);
        if (!accepted.ok()) {
            if (accepted.error().code == Err::Timeout) continue;
            break;
        }
        ServeClient(std::move(accepted.value()));
    }
    running_.store(false, std::memory_order_release);
}

void HtmlOutputServer::ServeClient(std::unique_ptr<platform::ISocket> socket) {
    if (!socket) return;
    std::string request;
    request.reserve(1024);
    char chunk[1024];
    while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
        auto received = socket->ReceiveTimeout(chunk, sizeof(chunk), 500);
        if (!received.ok() || received.value() == 0) return;
        request.append(chunk, received.value());
    }

    const size_t firstSpace = request.find(' ');
    const size_t secondSpace = firstSpace == std::string::npos
                                   ? std::string::npos : request.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        (void)SendResponse(*socket, "400 Bad Request", "text/plain", "Bad request");
        return;
    }
    const std::string_view path(request.data() + firstSpace + 1, secondSpace - firstSpace - 1);
    if (path == "/" || path.starts_with("/?")) {
        (void)SendResponse(*socket, "200 OK", "text/html; charset=utf-8", kPage);
        return;
    }
    if (path != "/frame.png" && !path.starts_with("/frame.png?")) {
        (void)SendResponse(*socket, "404 Not Found", "text/plain", "Not found");
        return;
    }

    std::shared_ptr<const rendering::Frame> frame;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frame = latestFrame_;
    }
    if (!frame) {
        (void)SendResponse(*socket, "503 Service Unavailable", "text/plain", "No frame yet");
        return;
    }

    std::vector<uint8_t> rgba;
    rgba.resize(frame->pixels.size() * 4);
    for (size_t i = 0; i < frame->pixels.size(); ++i) {
        const uint32_t px = frame->pixels[i];
        rgba[i * 4 + 0] = static_cast<uint8_t>(px & 0xffu);
        rgba[i * 4 + 1] = static_cast<uint8_t>((px >> 8) & 0xffu);
        rgba[i * 4 + 2] = static_cast<uint8_t>((px >> 16) & 0xffu);
        rgba[i * 4 + 3] = static_cast<uint8_t>((px >> 24) & 0xffu);
    }
    auto png = rendering::EncodePngRgba8(rgba.data(), frame->width, frame->height);
    if (!png.ok()) {
        (void)SendResponse(*socket, "503 Service Unavailable", "text/plain",
                           "PNG encoding is unavailable");
        return;
    }
    const std::string body(reinterpret_cast<const char*>(png.value().data()), png.value().size());
    (void)SendResponse(*socket, "200 OK", "image/png", body);
}

} // namespace bps::display
