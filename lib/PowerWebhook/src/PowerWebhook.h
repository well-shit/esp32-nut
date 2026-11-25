#pragma once

#include <string>
#include <cstring>
#include <cctype>

namespace PowerWebhook {
constexpr size_t MAX_URL_LENGTH = 512;

enum class State { UNKNOWN, ONLINE, ON_BATTERY };

inline const char* name(State state) {
    return state == State::ONLINE ? "online" : state == State::ON_BATTERY ? "on_battery" : "unknown";
}

inline bool hasToken(const char* status, const char* token) {
    if (!status) return false;
    const size_t n = std::strlen(token);
    while (*status) {
        while (*status == ' ') ++status;
        const char* start = status;
        while (*status && *status != ' ') ++status;
        if (static_cast<size_t>(status - start) == n && std::strncmp(start, token, n) == 0) return true;
    }
    return false;
}

inline State powerState(const char* status) {
    const bool online = hasToken(status, "OL"), battery = hasToken(status, "OB");
    if (online == battery) return State::UNKNOWN;
    return online ? State::ONLINE : State::ON_BATTERY;
}

struct Change { State previous = State::UNKNOWN; State current = State::UNKNOWN; };

// The first valid sample establishes a baseline. Invalid samples never change it.
class Monitor {
public:
    bool update(bool connected, bool stale, bool waiting, const char* status, Change& change) {
        if (!connected || stale || waiting) return false;
        const State next = powerState(status);
        if (next == State::UNKNOWN) return false;
        const State previous = current_;
        current_ = next;
        if (previous == State::UNKNOWN || previous == next) return false;
        change = {previous, next};
        return true;
    }
    State current() const { return current_; }
private:
    State current_ = State::UNKNOWN;
};

// Plain HTTP(S), no userinfo or fragments. Ports and IPv6 authorities are checked
// here as well as in the UI, before saving or handing a URL to the HTTP client.
inline bool validUrl(const std::string& url) {
    if (url.empty()) return true; // an empty URL disables the feature
    if (url.size() > MAX_URL_LENGTH) return false;
    size_t start;
    if (url.compare(0, 7, "http://") == 0) start = 7;
    else if (url.compare(0, 8, "https://") == 0) start = 8;
    else return false;
    for (unsigned char c : url) if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '#') return false;
    const size_t end = url.find_first_of("/?", start);
    const std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    size_t port = std::string::npos;
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos || close < 3 || authority.substr(1, close - 1).find(':') == std::string::npos) return false;
        for (size_t i = 1; i < close; ++i) {
            const unsigned char c = authority[i];
            if (!std::isxdigit(c) && c != ':' && c != '.') return false;
        }
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port = close + 2;
        }
    } else {
        const size_t colon = authority.find(':');
        const size_t hostEnd = colon == std::string::npos ? authority.size() : colon;
        if (!hostEnd) return false;
        for (size_t i = 0; i < hostEnd; ++i) {
            const unsigned char c = authority[i];
            if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') return false;
        }
        if (colon != std::string::npos) port = colon + 1;
    }
    if (port != std::string::npos) {
        if (port == authority.size() || authority.size() - port > 5) return false;
        unsigned value = 0;
        for (size_t i = port; i < authority.size(); ++i) {
            if (authority[i] < '0' || authority[i] > '9') return false;
            value = value * 10 + (authority[i] - '0');
        }
        if (!value || value > 65535) return false;
    }
    return true;
}
} // namespace PowerWebhook
