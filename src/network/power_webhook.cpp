#include "network/power_webhook.h"
#include "core/app_logger.h"
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cstdio>

bool PowerWebhookOutput::begin(AppNetworkManager* network, IUpsSource* ups, const String& upsName,
                               const WebhookSettings& settings) {
    network_ = network;
    ups_ = ups;
    ups_name_ = upsName;
    settings_ = settings;
    jobs_ = xQueueCreate(QUEUE_SIZE, sizeof(Job));
    results_ = xQueueCreate(QUEUE_SIZE + 1, sizeof(Result));
    if (jobs_ && results_ && xTaskCreate(run, "power_webhook", 8192, this, 1, &worker_) == pdPASS) return true;
    if (jobs_) vQueueDelete(jobs_);
    if (results_) vQueueDelete(results_);
    jobs_ = results_ = nullptr;
    worker_ = nullptr;
    AppLogger::log("ERROR", "[WEBHOOK] Could not start notification worker");
    return false;
}

void PowerWebhookOutput::configure(const WebhookSettings& settings) {
    settings_ = settings;
    // Observe a fresh baseline after changing the destination or enabling it.
    monitor_ = PowerWebhook::Monitor();
    AppLogger::log("INFO", "[WEBHOOK] Power notifications %s", settings.enabled ? "enabled" : "disabled");
}

bool PowerWebhookOutput::enqueue(const PowerWebhook::Change& change, const char* status, bool test,
                                 uint32_t& id, String& error) {
    if (!worker_) { error = "Notification worker unavailable"; return false; }
    if (settings_.url.isEmpty()) { error = "Save an API URL first"; return false; }
    if (!network_ || !network_->isConnected() || network_->isApMode()) {
        error = "No Wi-Fi or Ethernet connection";
        return false;
    }
    Job job = {};
    job.id = ++next_id_;
    if (!job.id) job.id = ++next_id_;
    job.test = test;
    snprintf(job.url, sizeof(job.url), "%s", settings_.url.c_str());
    JsonDocument payload;
    payload["event"] = "power_status_changed";
    payload["event_id"] = job.id;
    payload["hostname"] = network_->hostname();
    payload["ups_name"] = ups_name_;
    payload["previous_status"] = PowerWebhook::name(change.previous);
    payload["status"] = PowerWebhook::name(change.current);
    payload["ups_status"] = status;
    payload["test"] = test;
    payload["uptime_ms"] = millis();
    if (measureJson(payload) >= sizeof(job.payload)) { error = "Notification payload too large"; return false; }
    serializeJson(payload, job.payload, sizeof(job.payload));
    if (xQueueSend(jobs_, &job, 0) != pdTRUE) { error = "Notification queue full"; return false; }
    ++pending_;
    id = job.id;
    return true;
}

uint32_t PowerWebhookOutput::test(PowerWebhook::State state, String& error) {
    const PowerWebhook::State previous = state == PowerWebhook::State::ONLINE
        ? PowerWebhook::State::ON_BATTERY : PowerWebhook::State::ONLINE;
    uint32_t id = 0;
    enqueue({previous, state}, state == PowerWebhook::State::ONLINE ? "OL" : "OB", true, id, error);
    return id;
}

void PowerWebhookOutput::collectResults() {
    if (!results_) return;
    Result result;
    while (xQueueReceive(results_, &result, 0) == pdTRUE) {
        if (pending_) --pending_;
        last_ = result;
        if (result.test) last_test_ = result;
        if (result.http_code >= 200 && result.http_code < 300) {
            ++sent_;
            AppLogger::log("INFO", "[WEBHOOK] %snotification %u delivered (HTTP %d)",
                           result.test ? "Test " : "Power ", (unsigned)result.id, result.http_code);
        } else {
            ++failed_;
            AppLogger::log("WARN", "[WEBHOOK] %snotification %u failed: %s", result.test ? "Test " : "Power ",
                           (unsigned)result.id, result.error);
        }
    }
}

void PowerWebhookOutput::poll(uint32_t now) {
    collectResults();
    if (!settings_.enabled || !ups_ || now - last_poll_ < 250) return;
    last_poll_ = now;
    const String status = ups_->getUPSStatusString();
    PowerWebhook::Change change;
    if (!monitor_.update(ups_->isConnected(), ups_->isDataStale(), ups_->isWaitingFirstData(), status.c_str(), change)) return;
    uint32_t id = 0;
    String error;
    if (!enqueue(change, status.c_str(), false, id, error)) {
        ++failed_;
        last_ = Result();
        snprintf(last_.error, sizeof(last_.error), "%s", error.c_str());
        AppLogger::log("WARN", "[WEBHOOK] Power notification not queued: %s", error.c_str());
    }
}

String PowerWebhookOutput::statusJson() {
    collectResults();
    JsonDocument doc;
    doc["enabled"] = settings_.enabled;
    doc["url"] = settings_.url;
    doc["available"] = worker_ != nullptr;
    doc["power_status"] = PowerWebhook::name(monitor_.current());
    doc["pending"] = pending_;
    doc["sent"] = sent_;
    doc["failed"] = failed_;
    auto writeResult = [](JsonObject object, const Result& r) {
        object["id"] = r.id;
        object["test"] = r.test;
        object["http_code"] = r.http_code;
        object["error"] = r.error;
        object["success"] = r.http_code >= 200 && r.http_code < 300;
    };
    writeResult(doc["last_result"].to<JsonObject>(), last_);
    writeResult(doc["last_test"].to<JsonObject>(), last_test_);
    String json;
    serializeJson(doc, json);
    return json;
}

void PowerWebhookOutput::run(void* context) {
    auto* self = static_cast<PowerWebhookOutput*>(context);
    Job job;
    while (true) {
        if (xQueueReceive(self->jobs_, &job, portMAX_DELAY) != pdTRUE) continue;
        Result result;
        result.id = job.id;
        result.test = job.test;
        esp_http_client_config_t config = {};
        config.url = job.url;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = 5000;
        config.disable_auto_redirect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#else
        // Do not fall back to unverified TLS when a build has no trust bundle.
        if (strncmp(job.url, "https://", 8) == 0) {
            snprintf(result.error, sizeof(result.error), "HTTPS requires a firmware certificate bundle");
            xQueueSend(self->results_, &result, portMAX_DELAY);
            continue;
        }
#endif
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_header(client, "Accept", "application/json");
            esp_http_client_set_post_field(client, job.payload, strlen(job.payload));
            const esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                result.http_code = esp_http_client_get_status_code(client);
                if (result.http_code < 200 || result.http_code >= 300)
                    snprintf(result.error, sizeof(result.error), "Endpoint returned HTTP %d", result.http_code);
            } else {
                snprintf(result.error, sizeof(result.error), "Request failed: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        } else {
            snprintf(result.error, sizeof(result.error), "Could not initialize HTTP client");
        }
        xQueueSend(self->results_, &result, portMAX_DELAY);
    }
}
