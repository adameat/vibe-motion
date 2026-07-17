#include "vibe_motion/onvif.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace vibe_motion;

namespace {

class SoapCamera {
  public:
    SoapCamera() {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) {
            throw std::runtime_error("socket failed");
        }
        int enabled = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(socket_, 8) != 0) {
            throw std::runtime_error("bind/listen failed");
        }
        socklen_t size = sizeof(address);
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { run(); });
    }

    ~SoapCamera() {
        stopping_.store(true);
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string url(const std::string& path = "/onvif/device_service") const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    int digest_attempts() const noexcept {
        return digest_attempts_.load();
    }

  private:
    static std::string receive_request(int client) {
        std::string request;
        char buffer[4096];
        std::size_t expected = 0;
        while (true) {
            const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                break;
            }
            request.append(buffer, static_cast<std::size_t>(count));
            const auto headers_end = request.find("\r\n\r\n");
            if (headers_end != std::string::npos && expected == 0) {
                const auto content_length = request.find("Content-Length:");
                if (content_length != std::string::npos) {
                    const auto value = content_length + std::strlen("Content-Length:");
                    expected = headers_end + 4 +
                               static_cast<std::size_t>(std::stoul(request.substr(value)));
                }
            }
            if (expected > 0 && request.size() >= expected) {
                break;
            }
        }
        return request;
    }

    static void send_all(int client, const std::string& value) {
        std::size_t sent = 0;
        while (sent < value.size()) {
            const ssize_t count = ::send(client, value.data() + sent, value.size() - sent, 0);
            if (count <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    std::string envelope(const std::string& body) const {
        return "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
               "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
               "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
               "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
               "xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" "
               "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
               "xmlns:tns1=\"http://www.onvif.org/ver10/topics\">"
               "<s:Body>" +
               body + "</s:Body></s:Envelope>";
    }

    std::string response_for(const std::string& request) const {
        assert(request.find("wsse:UsernameToken") != std::string::npos);
        assert(request.find("PasswordDigest") != std::string::npos);
        assert(request.find("admin") != std::string::npos);
        assert(request.find("secret") == std::string::npos);
        if (request.find("GetCapabilities") != std::string::npos) {
            return envelope(
                "<tds:GetCapabilitiesResponse><tds:Capabilities>"
                "<tt:Media><tt:XAddr>" +
                url("/onvif/media") + "</tt:XAddr></tt:Media><tt:Events><tt:XAddr>" +
                url("/onvif/events") +
                "</tt:XAddr></tt:Events></tds:Capabilities></tds:GetCapabilitiesResponse>");
        }
        if (request.find("GetProfiles") != std::string::npos) {
            return envelope("<trt:GetProfilesResponse><trt:Profiles token=\"main-token\">"
                            "<tt:Name>Main stream</tt:Name><tt:VideoEncoderConfiguration>"
                            "<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height>"
                            "</tt:Resolution></tt:VideoEncoderConfiguration></trt:Profiles>"
                            "<trt:Profiles token=\"sub-token\"><tt:Name>Sub stream</tt:Name>"
                            "<tt:VideoEncoderConfiguration><tt:Resolution>"
                            "<tt:Width>640</tt:Width><tt:Height>360</tt:Height>"
                            "</tt:Resolution></tt:VideoEncoderConfiguration>"
                            "</trt:Profiles></trt:GetProfilesResponse>");
        }
        if (request.find("GetStreamUri") != std::string::npos) {
            const bool sub = request.find("sub-token") != std::string::npos;
            assert(sub || request.find("main-token") != std::string::npos);
            return envelope(std::string("<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>") +
                            (sub ? "rtsp://camera.test/live/sub" : "rtsp://camera.test/live/main") +
                            "</tt:Uri></trt:MediaUri></trt:GetStreamUriResponse>");
        }
        if (request.find("CreatePullPointSubscription") != std::string::npos) {
            return envelope("<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>"
                            "<wsa:Address>" +
                            url("/onvif/subscription/1") +
                            "</wsa:Address></tev:SubscriptionReference>"
                            "</tev:CreatePullPointSubscriptionResponse>");
        }
        if (request.find("PullMessages") != std::string::npos) {
            const auto notification = [](const char* state, const char* token) {
                return std::string(
                           "<wsnt:NotificationMessage><wsnt:Topic>"
                           "tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic>"
                           "<wsnt:Message><tt:Message UtcTime=\"2026-07-16T00:00:00Z\" "
                           "PropertyOperation=\"Changed\"><tt:Source>"
                           "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"") +
                       token +
                       "\"/></tt:Source><tt:Data><tt:SimpleItem Name=\"IsMotion\" Value=\"" +
                       state +
                       "\"/></tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
            };
            const std::string deleted =
                "<wsnt:NotificationMessage><wsnt:Topic>"
                "tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic><wsnt:Message>"
                "<tt:Message UtcTime=\"2026-07-16T00:00:01Z\" PropertyOperation=\"Deleted\">"
                "<tt:Source><tt:SimpleItem Name=\"VideoSourceConfigurationToken\" "
                "Value=\"source-2\"/></tt:Source></tt:Message></wsnt:Message>"
                "</wsnt:NotificationMessage>";
            const std::string diagnostic =
                "<wsnt:NotificationMessage><wsnt:Topic>"
                "tns1:VideoAnalytics/LineDetector/Crossed</wsnt:Topic><wsnt:Message>"
                "<tt:Message UtcTime=\"2026-07-16T00:00:02Z\" PropertyOperation=\"Changed\">"
                "<tt:Key><tt:SimpleItem Name=\"Rule\" Value=\"line-1\"/></tt:Key>"
                "<tt:Data><tt:SimpleItem Name=\"ObjectId\" Value=\"7\"/>"
                "<tt:ElementItem Name=\"VendorOnly\"><tt:Extension>opaque-value"
                "</tt:Extension></tt:ElementItem></tt:Data>"
                "</tt:Message></wsnt:Message></wsnt:NotificationMessage>";
            return envelope("<tev:PullMessagesResponse>" + notification("true", "source-1") +
                            notification("false", "source-1") + deleted + diagnostic +
                            "</tev:PullMessagesResponse>");
        }
        return envelope("<s:Fault><s:Reason><s:Text>unexpected request</s:Text>"
                        "</s:Reason></s:Fault>");
    }

    void run() {
        while (!stopping_.load()) {
            const int client = ::accept(socket_, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            const std::string request = receive_request(client);
            const bool has_wsse = request.find("wsse:UsernameToken") != std::string::npos;
            if (!has_wsse) {
                ++digest_attempts_;
            }
            const std::string body =
                has_wsse ? response_for(request)
                         : envelope("<s:Fault><s:Reason><s:Text>WSSE required</s:Text>"
                                    "</s:Reason></s:Fault>");
            const std::string response = std::string("HTTP/1.1 ") +
                                         (has_wsse ? "200 OK" : "400 Bad Request") +
                                         "\r\nConnection: close\r\nContent-Type: "
                                         "application/soap+xml\r\nContent-Length: " +
                                         std::to_string(body.size()) + "\r\n\r\n" + body;
            send_all(client, response);
            ::close(client);
        }
    }

    int socket_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> digest_attempts_{0};
    std::thread thread_;
};

} // namespace

int main() {
    const auto topics = parse_onvif_topic_list(" Motion, MotionAlarm ,, CellMotionDetector ");
    assert(topics.size() == 3);
    assert(topics.front() == "Motion");

    SoapCamera camera;
    OnvifClientConfig config;
    config.device_url = camera.url();
    config.userpass = "admin:secret";
    config.auth = "auto";
    config.motion_topics = topics;
    config.request_timeout = std::chrono::seconds(2);
    config.pull_timeout = std::chrono::seconds(1);
    OnvifClient client(config);

    const OnvifStream stream = client.resolve_stream();
    assert(stream.uri == "rtsp://camera.test/live/main");
    assert(stream.profile_token == "main-token");
    assert(stream.profile_name == "Main stream");
    assert(stream.width == 1920 && stream.height == 1080);

    OnvifClientConfig named_config = config;
    named_config.auth = "wsse";
    named_config.profile = "Sub stream";
    const OnvifStream named_stream = OnvifClient(named_config).resolve_stream();
    assert(named_stream.uri == "rtsp://camera.test/live/sub");
    assert(named_stream.profile_token == "sub-token");
    assert(named_stream.width == 640 && named_stream.height == 360);

    std::atomic<bool> stop{false};
    std::vector<OnvifEvent> events;
    bool connected = false;
    client.receive_motion_events(
        stop,
        [&](const OnvifEvent& event) {
            events.push_back(event);
            if (events.size() == 4) {
                stop.store(true);
            }
        },
        [&](bool state, const std::string&) {
            if (state) {
                connected = true;
            }
        });
    assert(connected);
    assert(events.size() == 4);
    assert(events.at(0).active);
    assert(events.at(0).motion_topic);
    assert(events.at(0).has_state);
    assert(!events.at(1).active);
    assert(events.at(0).source.at("VideoSourceConfigurationToken") == "source-1");
    assert(events.at(0).data.at("IsMotion") == "true");
    assert(events.at(0).key.find("source-1") != std::string::npos);
    assert(events.at(0).utc_time == "2026-07-16T00:00:00Z");
    assert(events.at(0).property_operation == "Changed");
    assert(!events.at(2).active);
    assert(events.at(2).property_operation == "Deleted");
    assert(events.at(2).motion_topic);
    assert(events.at(2).has_state);
    assert(!events.at(3).motion_topic);
    assert(!events.at(3).has_state);
    assert(events.at(3).key_items.at("Rule") == "line-1");
    assert(events.at(3).data.at("ObjectId") == "7");
    assert(events.at(3).raw_xml.find("NotificationMessage") != std::string::npos);
    assert(events.at(3).raw_xml.find("VendorOnly") != std::string::npos);
    assert(events.at(3).raw_xml.find("opaque-value") != std::string::npos);
    assert(camera.digest_attempts() == 1);

    std::cout << "onvif tests passed\n";
}
