#pragma once
#include <Arduino.h>

struct WebhookSettings {
    bool enabled = false;
    String url;
};

class WebhookSettingsStore {
public:
    WebhookSettings load();
    bool save(const WebhookSettings& settings);
};
