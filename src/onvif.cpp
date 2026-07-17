#include "vibe_motion/onvif.hpp"

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/random.h>

extern "C" {
#include <libavutil/base64.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vibe_motion {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t max_soap_response = 4U * 1024U * 1024U;

std::string trim(std::string value) {
    const auto non_space = [](unsigned char character) { return !std::isspace(character); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), non_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), non_space).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string xml_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '\"':
            result += "&quot;";
            break;
        case '\'':
            result += "&apos;";
            break;
        default:
            result.push_back(character);
        }
    }
    return result;
}

struct ShaDeleter {
    void operator()(AVSHA* value) const noexcept {
        av_free(value);
    }
};

void sha_update(AVSHA* sha, const std::uint8_t* data, std::size_t size) {
    if (size > std::numeric_limits<unsigned int>::max()) {
        throw std::runtime_error("ONVIF WS-Security digest input is too large");
    }
    av_sha_update(sha, data, static_cast<unsigned int>(size));
}

std::string base64(const std::uint8_t* data, std::size_t size) {
    std::vector<char> encoded(static_cast<std::size_t>(AV_BASE64_SIZE(size)));
    if (av_base64_encode(encoded.data(), static_cast<int>(encoded.size()), data,
                         static_cast<int>(size)) == nullptr) {
        throw std::runtime_error("cannot encode ONVIF WS-Security value");
    }
    return encoded.data();
}

std::array<std::uint8_t, 16> secure_nonce() {
    std::array<std::uint8_t, 16> nonce{};
    std::size_t offset = 0;
    while (offset < nonce.size()) {
        const ssize_t count = ::getrandom(nonce.data() + offset, nonce.size() - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("cannot generate ONVIF WS-Security nonce");
        }
        offset += static_cast<std::size_t>(count);
    }
    return nonce;
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t instant = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&instant, &utc);
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        throw std::runtime_error("cannot format ONVIF WS-Security timestamp");
    }
    return buffer;
}

std::string wsse_security(const std::string& userpass) {
    const auto colon = userpass.find(':');
    const std::string username = userpass.substr(0, colon);
    const std::string password =
        colon == std::string::npos ? std::string{} : userpass.substr(colon + 1);
    const auto nonce = secure_nonce();
    const std::string created = utc_now();

    std::unique_ptr<AVSHA, ShaDeleter> sha(av_sha_alloc());
    if (!sha || av_sha_init(sha.get(), 160) < 0) {
        throw std::runtime_error("cannot initialize ONVIF WS-Security digest");
    }
    sha_update(sha.get(), nonce.data(), nonce.size());
    sha_update(sha.get(), reinterpret_cast<const std::uint8_t*>(created.data()), created.size());
    sha_update(sha.get(), reinterpret_cast<const std::uint8_t*>(password.data()), password.size());
    std::array<std::uint8_t, 20> digest{};
    av_sha_final(sha.get(), digest.data());

    return "<wsse:Security s:mustUnderstand=\"1\" "
           "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
           "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
           "<wsse:UsernameToken><wsse:Username>" +
           xml_escape(username) +
           "</wsse:Username><wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
           base64(digest.data(), digest.size()) +
           "</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
           base64(nonce.data(), nonce.size()) + "</wsse:Nonce><wsu:Created>" + created +
           "</wsu:Created></wsse:UsernameToken></wsse:Security>";
}

std::string soap_envelope(const std::string& body, const std::string& action,
                          const std::string& destination, const std::string& security) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
           "xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" "
           "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
           "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
           "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
           "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
           "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
           "<s:Header><wsa:Action s:mustUnderstand=\"1\">" +
           xml_escape(action) + "</wsa:Action><wsa:To s:mustUnderstand=\"1\">" +
           xml_escape(destination) + "</wsa:To>" + security + "</s:Header><s:Body>" + body +
           "</s:Body></s:Envelope>";
}

struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("cannot initialize libcurl");
        }
    }
    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

void ensure_curl() {
    static CurlGlobal global;
    (void)global;
}

struct CurlDeleter {
    void operator()(CURL* handle) const noexcept {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }
};

struct HeaderDeleter {
    void operator()(curl_slist* headers) const noexcept {
        curl_slist_free_all(headers);
    }
};

using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;
using HeaderPtr = std::unique_ptr<curl_slist, HeaderDeleter>;

struct ResponseBuffer {
    std::string value;
    bool exceeded = false;
};

std::size_t receive_response(char* data, std::size_t size, std::size_t count, void* opaque) {
    const auto bytes = size * count;
    auto& response = *static_cast<ResponseBuffer*>(opaque);
    if (bytes > max_soap_response - std::min(response.value.size(), max_soap_response)) {
        response.exceeded = true;
        return 0;
    }
    response.value.append(data, bytes);
    return bytes;
}

int stop_transfer(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* stop = static_cast<const std::atomic<bool>*>(opaque);
    return stop != nullptr && stop->load() ? 1 : 0;
}

struct SoapResponse {
    long status = 0;
    std::string body;
};

enum class SoapAuth { none, digest, wsse };

SoapResponse post_soap_once(const OnvifClientConfig& config, const std::string& url,
                            const std::string& action, const std::string& body, SoapAuth auth,
                            const std::atomic<bool>* stop, std::chrono::seconds timeout) {
    ensure_curl();
    CurlPtr curl(curl_easy_init());
    if (!curl) {
        throw std::runtime_error("cannot create ONVIF HTTP client");
    }

    const std::string security =
        auth == SoapAuth::wsse ? wsse_security(config.userpass) : std::string{};
    const std::string document = soap_envelope(body, action, url, security);
    const std::string content_type =
        "Content-Type: application/soap+xml; charset=utf-8; action=\"" + action + "\"";
    HeaderPtr headers(curl_slist_append(nullptr, content_type.c_str()));
    if (!headers) {
        throw std::runtime_error("cannot allocate ONVIF HTTP headers");
    }

    ResponseBuffer response;
    char error_buffer[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, document.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(document.size()));
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_response);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(config.request_timeout.count() * 1000));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count() * 1000));
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, config.tls_verify ? 1L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, config.tls_verify ? 2L : 0L);
    if (auth == SoapAuth::digest && !config.userpass.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(curl.get(), CURLOPT_USERPWD, config.userpass.c_str());
    }
    if (stop != nullptr) {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, stop_transfer);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, stop);
    }

    const CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        if (stop != nullptr && stop->load() && result == CURLE_ABORTED_BY_CALLBACK) {
            throw std::runtime_error("ONVIF request stopped");
        }
        const std::string detail =
            error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(result);
        throw std::runtime_error(response.exceeded ? "ONVIF SOAP response exceeds 4 MiB"
                                                   : "ONVIF HTTP request failed: " + detail);
    }
    SoapResponse result_value;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result_value.status);
    result_value.body = std::move(response.value);
    return result_value;
}

bool http_success(const SoapResponse& response) {
    return response.status >= 200 && response.status < 300;
}

std::runtime_error http_status_error(const SoapResponse& response) {
    return std::runtime_error("ONVIF HTTP status " + std::to_string(response.status));
}

SoapResponse post_soap(OnvifClientConfig& config, const std::string& url, const std::string& action,
                       const std::string& body, const std::atomic<bool>* stop = nullptr,
                       std::chrono::seconds timeout = 15s) {
    if (config.userpass.empty()) {
        SoapResponse response =
            post_soap_once(config, url, action, body, SoapAuth::none, stop, timeout);
        if (!http_success(response)) {
            throw http_status_error(response);
        }
        return response;
    }

    const std::string auth = lower(trim(config.auth));
    if (auth == "digest" || auth == "auto") {
        SoapResponse response =
            post_soap_once(config, url, action, body, SoapAuth::digest, stop, timeout);
        if (http_success(response)) {
            if (auth == "auto") {
                config.auth = "digest";
            }
            return response;
        }
        if (auth == "digest") {
            throw http_status_error(response);
        }
    }
    SoapResponse response =
        post_soap_once(config, url, action, body, SoapAuth::wsse, stop, timeout);
    if (!http_success(response)) {
        throw http_status_error(response);
    }
    if (auth == "auto") {
        config.auth = "wsse";
    }
    return response;
}

struct XmlDocDeleter {
    void operator()(xmlDoc* document) const noexcept {
        xmlFreeDoc(document);
    }
};

using XmlDocPtr = std::unique_ptr<xmlDoc, XmlDocDeleter>;

struct XmlCharDeleter {
    void operator()(xmlChar* value) const noexcept {
        xmlFree(value);
    }
};

struct XmlBufferDeleter {
    void operator()(xmlBuffer* value) const noexcept {
        xmlBufferFree(value);
    }
};

bool named(const xmlNode* node, const char* name) {
    return node != nullptr && node->type == XML_ELEMENT_NODE &&
           xmlStrEqual(node->name, reinterpret_cast<const xmlChar*>(name)) != 0;
}

xmlNode* first_descendant(xmlNode* node, const char* name) {
    for (xmlNode* current = node; current != nullptr; current = current->next) {
        if (named(current, name)) {
            return current;
        }
        if (xmlNode* child = first_descendant(current->children, name); child != nullptr) {
            return child;
        }
    }
    return nullptr;
}

void descendants(xmlNode* node, const char* name, std::vector<xmlNode*>& result) {
    for (xmlNode* current = node; current != nullptr; current = current->next) {
        if (named(current, name)) {
            result.push_back(current);
        }
        descendants(current->children, name, result);
    }
}

std::string content(xmlNode* node) {
    if (node == nullptr) {
        return {};
    }
    std::unique_ptr<xmlChar, XmlCharDeleter> value(xmlNodeGetContent(node));
    return value ? trim(reinterpret_cast<const char*>(value.get())) : std::string{};
}

std::string property(xmlNode* node, const char* name) {
    if (node == nullptr) {
        return {};
    }
    std::unique_ptr<xmlChar, XmlCharDeleter> value(
        xmlGetProp(node, reinterpret_cast<const xmlChar*>(name)));
    return value ? reinterpret_cast<const char*>(value.get()) : std::string{};
}

std::string serialize_node(xmlDoc* document, xmlNode* node) {
    std::unique_ptr<xmlBuffer, XmlBufferDeleter> buffer(xmlBufferCreate());
    if (!buffer || xmlNodeDump(buffer.get(), document, node, 0, 0) < 0) {
        throw std::runtime_error("cannot serialize ONVIF notification XML");
    }
    const xmlChar* value = xmlBufferContent(buffer.get());
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

XmlDocPtr parse_xml(const std::string& xml) {
    XmlDocPtr document(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "onvif.xml", nullptr,
                                     XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
    if (!document) {
        throw std::runtime_error("camera returned invalid ONVIF XML");
    }
    if (xmlNode* fault = first_descendant(xmlDocGetRootElement(document.get()), "Fault");
        fault != nullptr) {
        std::string reason = content(first_descendant(fault->children, "Text"));
        if (reason.empty()) {
            reason = content(first_descendant(fault->children, "Reason"));
        }
        throw std::runtime_error("ONVIF SOAP fault" +
                                 (reason.empty() ? std::string{} : ": " + reason));
    }
    return document;
}

int integer_content(xmlNode* node) {
    try {
        return std::stoi(content(node));
    } catch (...) {
        return 0;
    }
}

struct Services {
    std::string media;
    std::string events;
};

Services get_services(OnvifClientConfig& config, const std::atomic<bool>* stop = nullptr) {
    constexpr auto action = "http://www.onvif.org/ver10/device/wsdl/GetCapabilities";
    const auto response =
        post_soap(config, config.device_url, action,
                  "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>",
                  stop, config.request_timeout);
    auto document = parse_xml(response.body);
    xmlNode* root = xmlDocGetRootElement(document.get());
    Services services;
    if (xmlNode* media = first_descendant(root, "Media"); media != nullptr) {
        services.media = content(first_descendant(media->children, "XAddr"));
    }
    if (xmlNode* events = first_descendant(root, "Events"); events != nullptr) {
        services.events = content(first_descendant(events->children, "XAddr"));
    }
    return services;
}

struct Profile {
    std::string token;
    std::string name;
    int width = 0;
    int height = 0;
};

std::vector<Profile> get_profiles(OnvifClientConfig& config, const std::string& media_url) {
    constexpr auto action = "http://www.onvif.org/ver10/media/wsdl/GetProfiles";
    const auto response =
        post_soap(config, media_url, action, "<trt:GetProfiles/>", nullptr, config.request_timeout);
    auto document = parse_xml(response.body);
    std::vector<xmlNode*> nodes;
    descendants(xmlDocGetRootElement(document.get()), "Profiles", nodes);
    std::vector<Profile> profiles;
    for (xmlNode* node : nodes) {
        Profile profile;
        profile.token = property(node, "token");
        profile.name = content(first_descendant(node->children, "Name"));
        if (xmlNode* encoder = first_descendant(node->children, "VideoEncoderConfiguration");
            encoder != nullptr) {
            if (xmlNode* resolution = first_descendant(encoder->children, "Resolution");
                resolution != nullptr) {
                profile.width = integer_content(first_descendant(resolution->children, "Width"));
                profile.height = integer_content(first_descendant(resolution->children, "Height"));
            }
        }
        if (!profile.token.empty()) {
            profiles.push_back(std::move(profile));
        }
    }
    return profiles;
}

std::string get_stream_uri(OnvifClientConfig& config, const std::string& media_url,
                           const std::string& profile_token) {
    constexpr auto action = "http://www.onvif.org/ver10/media/wsdl/GetStreamUri";
    const std::string request =
        "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
        "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
        "</trt:StreamSetup><trt:ProfileToken>" +
        xml_escape(profile_token) + "</trt:ProfileToken></trt:GetStreamUri>";
    const auto response =
        post_soap(config, media_url, action, request, nullptr, config.request_timeout);
    auto document = parse_xml(response.body);
    return content(first_descendant(xmlDocGetRootElement(document.get()), "Uri"));
}

bool parse_boolean(std::string value, bool& result) {
    value = lower(trim(std::move(value)));
    if (value == "true" || value == "1" || value == "on" || value == "active") {
        result = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "off" || value == "inactive") {
        result = false;
        return true;
    }
    return false;
}

void collect_simple_items(xmlNode* scope, std::map<std::string, std::string>& result) {
    if (scope == nullptr) {
        return;
    }
    std::vector<xmlNode*> items;
    descendants(scope->children, "SimpleItem", items);
    for (xmlNode* item : items) {
        const std::string name = property(item, "Name");
        if (!name.empty()) {
            result[name] = property(item, "Value");
        }
    }
}

bool topic_matches(const std::string& topic, const std::vector<std::string>& patterns) {
    const std::string haystack = lower(topic);
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
        return !pattern.empty() && haystack.find(lower(pattern)) != std::string::npos;
    });
}

std::vector<OnvifEvent> parse_events(const std::string& xml,
                                     const std::vector<std::string>& topics) {
    auto document = parse_xml(xml);
    std::vector<xmlNode*> notifications;
    descendants(xmlDocGetRootElement(document.get()), "NotificationMessage", notifications);
    std::vector<OnvifEvent> result;
    for (xmlNode* notification : notifications) {
        OnvifEvent event;
        event.raw_xml = serialize_node(document.get(), notification);
        event.topic = content(first_descendant(notification->children, "Topic"));
        event.motion_topic = topic_matches(event.topic, topics);
        xmlNode* message = first_descendant(notification->children, "Message");
        if (message != nullptr) {
            // NotificationMessage contains a wsnt:Message wrapper and, inside it,
            // the tt:Message payload carrying UtcTime and PropertyOperation.
            if (xmlNode* payload = first_descendant(message->children, "Message");
                payload != nullptr) {
                message = payload;
            }
            event.utc_time = property(message, "UtcTime");
            event.property_operation = property(message, "PropertyOperation");
            collect_simple_items(first_descendant(message->children, "Source"), event.source);
            collect_simple_items(first_descendant(message->children, "Key"), event.key_items);
            collect_simple_items(first_descendant(message->children, "Data"), event.data);
        }

        if (lower(event.property_operation) == "deleted") {
            event.active = false;
            event.has_state = true;
        }
        if (!event.has_state) {
            for (const std::string name :
                 {"IsMotion", "Motion", "State", "Alarm", "LogicalState"}) {
                const auto found = event.data.find(name);
                if (found != event.data.end() && parse_boolean(found->second, event.active)) {
                    event.has_state = true;
                    break;
                }
            }
        }
        if (!event.has_state) {
            for (const auto& [name, value] : event.data) {
                (void)name;
                if (parse_boolean(value, event.active)) {
                    event.has_state = true;
                    break;
                }
            }
        }
        std::ostringstream key;
        key << event.topic;
        for (const auto& [name, value] : event.source) {
            key << '|' << name << '=' << value;
        }
        for (const auto& [name, value] : event.key_items) {
            key << '|' << name << '=' << value;
        }
        event.key = key.str();
        result.push_back(std::move(event));
    }
    return result;
}

std::string create_subscription(OnvifClientConfig& config, const std::string& events_url,
                                const std::atomic<bool>& stop) {
    constexpr auto action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/"
                            "CreatePullPointSubscriptionRequest";
    const auto response =
        post_soap(config, events_url, action,
                  "<tev:CreatePullPointSubscription><tev:InitialTerminationTime>PT1H"
                  "</tev:InitialTerminationTime></tev:CreatePullPointSubscription>",
                  &stop, config.request_timeout);
    auto document = parse_xml(response.body);
    xmlNode* reference =
        first_descendant(xmlDocGetRootElement(document.get()), "SubscriptionReference");
    return content(
        first_descendant(reference == nullptr ? nullptr : reference->children, "Address"));
}

void renew_subscription(OnvifClientConfig& config, const std::string& subscription_url,
                        const std::atomic<bool>& stop) {
    constexpr auto action = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";
    (void)post_soap(config, subscription_url, action,
                    "<wsnt:Renew><wsnt:TerminationTime>PT1H</wsnt:TerminationTime>"
                    "</wsnt:Renew>",
                    &stop, config.request_timeout);
}

void unsubscribe(OnvifClientConfig& config, const std::string& subscription_url) noexcept {
    try {
        constexpr auto action =
            "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest";
        (void)post_soap(config, subscription_url, action, "<wsnt:Unsubscribe/>", nullptr, 2s);
    } catch (...) {
        // Subscription resources also expire camera-side. Shutdown and the
        // original pull failure must not be hidden by cleanup diagnostics.
    }
}

std::vector<OnvifEvent> pull_messages(OnvifClientConfig& config,
                                      const std::string& subscription_url,
                                      const std::atomic<bool>& stop) {
    constexpr auto action = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
                            "PullMessagesRequest";
    const std::string body =
        "<tev:PullMessages><tev:Timeout>PT" + std::to_string(config.pull_timeout.count()) +
        "S</tev:Timeout><tev:MessageLimit>64</tev:MessageLimit></tev:PullMessages>";
    // Some cameras (including Reolink firmware) ignore the requested PullMessages
    // Timeout and hold an empty request for about a minute. The progress callback
    // still makes shutdown immediate, so allow that vendor long-poll to finish.
    const auto transport_timeout = std::max(config.pull_timeout + config.request_timeout, 75s);
    const auto response =
        post_soap(config, subscription_url, action, body, &stop, transport_timeout);
    return parse_events(response.body, config.motion_topics);
}

bool wait_retry(const std::atomic<bool>& stop) {
    for (int step = 0; step < 10 && !stop.load(); ++step) {
        std::this_thread::sleep_for(100ms);
    }
    return !stop.load();
}

} // namespace

std::vector<std::string> parse_onvif_topic_list(const std::string& value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        std::string item =
            trim(value.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!item.empty()) {
            result.push_back(std::move(item));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

OnvifClient::OnvifClient(OnvifClientConfig config) : config_(std::move(config)) {
    if (config_.device_url.empty()) {
        throw std::invalid_argument("ONVIF device URL is empty");
    }
    if (config_.motion_topics.empty()) {
        config_.motion_topics = {"Motion",       "MotionAlarm",   "CellMotionDetector",
                                 "PeopleDetect", "VehicleDetect", "DogCatDetect",
                                 "FaceDetect"};
    }
    if (config_.request_timeout <= 0s || config_.pull_timeout <= 0s) {
        throw std::invalid_argument("invalid ONVIF timeout");
    }
    config_.auth = lower(trim(config_.auth));
    if (config_.auth != "auto" && config_.auth != "digest" && config_.auth != "wsse") {
        throw std::invalid_argument("ONVIF auth must be auto, digest, or wsse");
    }
}

OnvifStream OnvifClient::resolve_stream() {
    const Services services = get_services(config_);
    if (services.media.empty()) {
        throw std::runtime_error("camera did not advertise an ONVIF Media service");
    }
    const auto profiles = get_profiles(config_, services.media);
    if (profiles.empty()) {
        throw std::runtime_error("camera returned no ONVIF media profiles");
    }
    auto selected = std::max_element(
        profiles.begin(), profiles.end(), [](const Profile& left, const Profile& right) {
            const auto left_area = static_cast<std::uint64_t>(std::max(left.width, 0)) *
                                   static_cast<std::uint64_t>(std::max(left.height, 0));
            const auto right_area = static_cast<std::uint64_t>(std::max(right.width, 0)) *
                                    static_cast<std::uint64_t>(std::max(right.height, 0));
            return left_area < right_area;
        });
    if (!config_.profile.empty()) {
        selected = std::find_if(profiles.begin(), profiles.end(), [&](const Profile& profile) {
            return profile.name == config_.profile || profile.token == config_.profile;
        });
        if (selected == profiles.end()) {
            throw std::runtime_error("ONVIF media profile name/token not found: " +
                                     config_.profile);
        }
    }
    OnvifStream stream;
    stream.uri = get_stream_uri(config_, services.media, selected->token);
    stream.profile_token = selected->token;
    stream.profile_name = selected->name;
    stream.width = selected->width;
    stream.height = selected->height;
    if (stream.uri.empty()) {
        throw std::runtime_error("camera returned an empty ONVIF stream URI");
    }
    return stream;
}

void OnvifClient::receive_motion_events(const std::atomic<bool>& stop, OnvifEventCallback on_event,
                                        OnvifConnectionCallback on_connection) {
    while (!stop.load()) {
        std::string subscription;
        try {
            const Services services = get_services(config_, &stop);
            if (services.events.empty()) {
                throw std::runtime_error("camera did not advertise an ONVIF Events service");
            }
            subscription = create_subscription(config_, services.events, stop);
            if (subscription.empty()) {
                throw std::runtime_error("camera returned an empty ONVIF subscription address");
            }
            if (on_connection) {
                on_connection(true, {});
            }
            auto next_renewal = std::chrono::steady_clock::now() + 30min;
            while (!stop.load()) {
                for (const auto& event : pull_messages(config_, subscription, stop)) {
                    if (on_event) {
                        on_event(event);
                    }
                }
                if (std::chrono::steady_clock::now() >= next_renewal) {
                    renew_subscription(config_, subscription, stop);
                    next_renewal = std::chrono::steady_clock::now() + 30min;
                }
            }
            unsubscribe(config_, subscription);
        } catch (const std::exception& error) {
            if (!subscription.empty()) {
                unsubscribe(config_, subscription);
            }
            if (stop.load()) {
                break;
            }
            if (on_connection) {
                on_connection(false, error.what());
            }
            if (!wait_retry(stop)) {
                break;
            }
        }
    }
    if (on_connection) {
        on_connection(false, {});
    }
}

} // namespace vibe_motion
