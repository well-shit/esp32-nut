#include "core/webhook_settings_store.h"
#include "core/nvs_namespace.h"
#include "PowerWebhook.h"
#include <ArduinoJson.h>

WebhookSettings WebhookSettingsStore::load() {
    WebhookSettings settings;
    Preferences prefs;
    if (!NvsNamespace::beginRead(prefs, "webhook")) return settings;
    String saved = prefs.getString("settings", "");
    prefs.end();
    JsonDocument doc;
    if (deserializeJson(doc, saved) || !doc["enabled"].is<bool>() || !doc["url"].is<String>()) return settings;
    String url = doc["url"].as<String>();
    if (!PowerWebhook::validUrl(std::string(url.c_str(), url.length()))) return settings;
    settings.url = url;
    settings.enabled = doc["enabled"].as<bool>() && !url.isEmpty();
    return settings;
}

bool WebhookSettingsStore::save(const WebhookSettings& settings) {
    JsonDocument doc;
    doc["enabled"] = settings.enabled;
    doc["url"] = settings.url;
    String saved;
    serializeJson(doc, saved);
    Preferences prefs;
    if (!prefs.begin("webhook", false)) return false;
    // A single NVS value keeps the enabled flag and URL together across power loss.
    const bool ok = prefs.putString("settings", saved) == saved.length();
    prefs.end();
    return ok;
}
