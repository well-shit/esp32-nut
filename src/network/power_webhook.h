#pragma once

#include "PowerWebhook.h"
#include "core/webhook_settings_store.h"
#include "network/network_manager.h"
#include "IUpsSource.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// loopTask owns settings, monitoring and statistics. The worker only receives
// fixed-size jobs and returns results through queues; it never touches UPS data.
class PowerWebhookOutput {
public:
    bool begin(AppNetworkManager* network, IUpsSource* ups, const String& upsName, const WebhookSettings& settings);
    void configure(const WebhookSettings& settings);
    const WebhookSettings& settings() const { return settings_; }
    void poll(uint32_t now);
    String statusJson();
    // Returns 0 with a reason on failure; otherwise the queued test's ID.
    uint32_t test(PowerWebhook::State state, String& error);

private:
    struct Job {
        uint32_t id;
        bool test;
        char url[PowerWebhook::MAX_URL_LENGTH + 1];
        char payload[512];
    };
    struct Result {
        uint32_t id = 0;
        bool test = false;
        int http_code = 0;
        char error[96] = {};
    };
    static constexpr size_t QUEUE_SIZE = 4;
    QueueHandle_t jobs_ = nullptr, results_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    AppNetworkManager* network_ = nullptr;
    IUpsSource* ups_ = nullptr;
    String ups_name_;
    WebhookSettings settings_;
    PowerWebhook::Monitor monitor_;
    uint32_t last_poll_ = 0, next_id_ = 0, pending_ = 0, sent_ = 0, failed_ = 0;
    Result last_, last_test_;
    bool enqueue(const PowerWebhook::Change& change, const char* status, bool test, uint32_t& id, String& error);
    void collectResults();
    static void run(void* context);
};
