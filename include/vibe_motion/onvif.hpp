#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vibe_motion {

struct OnvifClientConfig {
    std::string device_url;
    std::string userpass;
    // Exact ONVIF profile Name or token. Empty selects the largest profile.
    std::string profile;
    std::string auth = "auto";
    std::vector<std::string> motion_topics;
    bool tls_verify = true;
    std::chrono::seconds request_timeout{15};
    std::chrono::seconds pull_timeout{5};
};

struct OnvifStream {
    std::string uri;
    std::string profile_token;
    std::string profile_name;
    int width = 0;
    int height = 0;
};

struct OnvifDeviceInformation {
    std::string manufacturer;
    std::string model;
    std::string firmware_version;
    std::string serial_number;
    std::string hardware_id;
};

struct OnvifEvent {
    std::string key;
    std::string topic;
    bool motion_topic = false;
    bool has_state = false;
    bool active = false;
    std::string utc_time;
    std::string property_operation;
    std::map<std::string, std::string> source;
    std::map<std::string, std::string> key_items;
    std::map<std::string, std::string> data;
    // Serialized NotificationMessage subtree for temporary vendor diagnostics.
    // It contains the event payload, not the SOAP/WS-Security request header.
    std::string raw_xml;
};

using OnvifEventCallback = std::function<void(const OnvifEvent&)>;
using OnvifConnectionCallback = std::function<void(bool, const std::string&)>;

class OnvifClient {
  public:
    explicit OnvifClient(OnvifClientConfig config);

    OnvifDeviceInformation device_information();
    OnvifStream resolve_stream();

    // Runs until stop becomes true. Pull requests are interruptible, so shutdown
    // does not wait for the camera-side PullMessages timeout to elapse.
    void receive_motion_events(const std::atomic<bool>& stop, OnvifEventCallback on_event,
                               OnvifConnectionCallback on_connection = {});

  private:
    OnvifClientConfig config_;
};

std::vector<std::string> parse_onvif_topic_list(const std::string& value);

} // namespace vibe_motion
