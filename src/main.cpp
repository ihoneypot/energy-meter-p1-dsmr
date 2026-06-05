#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#if CONFIG_ENERGYMETER_MQTT_USE_TLS
#include "esp_crt_bundle.h"
#endif
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace {

constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t MQTT_CONNECTED_BIT = BIT1;

constexpr int kTopicBufferSize = 160;
constexpr int kFieldBufferSize = 64;
constexpr int kLineBufferSize = 192;
constexpr int kPreviewBufferSize = 256;
constexpr TickType_t kNoDataWarnPeriod = pdMS_TO_TICKS(10000);
constexpr TickType_t kStatusLogPeriod = pdMS_TO_TICKS(5000);

static const char *TAG = "energy_meter";

static EventGroupHandle_t s_event_group = nullptr;
static esp_mqtt_client_handle_t s_mqtt_client = nullptr;
static bool s_mqtt_started = false;
static int s_wifi_retry_count = 0;
static bool s_seen_p1_data = false;

static char s_state_topic[kTopicBufferSize];
static char s_raw_topic[kTopicBufferSize];
static char s_availability_topic[kTopicBufferSize];

struct MeterReading {
    char header[kFieldBufferSize];
    char meter_id[kFieldBufferSize];
    char timestamp[kFieldBufferSize];
    char gas_timestamp[kFieldBufferSize];
    bool crc_ok;
    int tariff;
    double import_t1_kwh;
    double import_t2_kwh;
    double export_t1_kwh;
    double export_t2_kwh;
    double power_delivered_kw;
    double power_returned_kw;
    double voltage_l1_v;
    double voltage_l2_v;
    double voltage_l3_v;
    double current_l1_a;
    double current_l2_a;
    double current_l3_a;
    double power_delivered_l1_kw;
    double power_delivered_l2_kw;
    double power_delivered_l3_kw;
    double power_returned_l1_kw;
    double power_returned_l2_kw;
    double power_returned_l3_kw;
    double gas_m3;
    int power_failures;
    int long_power_failures;
};

void reset_reading(MeterReading &reading) {
    std::memset(&reading, 0, sizeof(reading));
    reading.crc_ok = false;
    reading.tariff = -1;
    reading.import_t1_kwh = NAN;
    reading.import_t2_kwh = NAN;
    reading.export_t1_kwh = NAN;
    reading.export_t2_kwh = NAN;
    reading.power_delivered_kw = NAN;
    reading.power_returned_kw = NAN;
    reading.voltage_l1_v = NAN;
    reading.voltage_l2_v = NAN;
    reading.voltage_l3_v = NAN;
    reading.current_l1_a = NAN;
    reading.current_l2_a = NAN;
    reading.current_l3_a = NAN;
    reading.power_delivered_l1_kw = NAN;
    reading.power_delivered_l2_kw = NAN;
    reading.power_delivered_l3_kw = NAN;
    reading.power_returned_l1_kw = NAN;
    reading.power_returned_l2_kw = NAN;
    reading.power_returned_l3_kw = NAN;
    reading.gas_m3 = NAN;
    reading.power_failures = -1;
    reading.long_power_failures = -1;
}

bool value_is_set(double value) {
    return !std::isnan(value);
}

uint64_t gpio_mask_for_pin(int pin) {
    if (pin < 0 || pin >= 64) {
        return 0;
    }
    return UINT64_C(1) << pin;
}

void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    std::snprintf(dst, dst_size, "%s", src ? src : "");
}

void append_escaped_char(char *out, size_t out_size, size_t &offset, unsigned char ch) {
    if (offset >= out_size - 1) {
        return;
    }

    if (std::isprint(ch) != 0 && ch != '\\') {
        out[offset++] = static_cast<char>(ch);
        out[offset] = '\0';
        return;
    }

    const char *replacement = nullptr;
    char hex_buf[5] = {};
    switch (ch) {
        case '\r':
            replacement = "\\r";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\t':
            replacement = "\\t";
            break;
        case '\\':
            replacement = "\\\\";
            break;
        default:
            std::snprintf(hex_buf, sizeof(hex_buf), "\\x%02X", ch);
            replacement = hex_buf;
            break;
    }

    const size_t len = std::strlen(replacement);
    if (offset + len >= out_size) {
        return;
    }

    std::memcpy(out + offset, replacement, len);
    offset += len;
    out[offset] = '\0';
}

void log_telegram_preview(const char *reason, const char *telegram, size_t len) {
    char preview[kPreviewBufferSize] = {};
    size_t offset = 0;

    const size_t head_len = len < 80 ? len : 80;
    for (size_t i = 0; i < head_len; ++i) {
        append_escaped_char(preview, sizeof(preview), offset, static_cast<unsigned char>(telegram[i]));
    }

    if (len > head_len) {
        const char ellipsis[] = "...";
        if (offset + sizeof(ellipsis) - 1 < sizeof(preview)) {
            std::memcpy(preview + offset, ellipsis, sizeof(ellipsis) - 1);
            offset += sizeof(ellipsis) - 1;
            preview[offset] = '\0';
        }

        const size_t tail_len = len > 40 ? 40 : len;
        for (size_t i = len - tail_len; i < len; ++i) {
            append_escaped_char(preview, sizeof(preview), offset, static_cast<unsigned char>(telegram[i]));
        }
    }

    ESP_LOGW(TAG, "%s len=%zu preview=%s", reason, len, preview);
}

uint16_t dsmr_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x0001U) != 0U) {
                crc = static_cast<uint16_t>((crc >> 1U) ^ 0xA001U);
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

bool verify_crc(const char *telegram) {
    const char *bang = std::strchr(telegram, '!');
    if (bang == nullptr) {
        return false;
    }

    char expected_hex[5] = {};
    size_t copied = 0;
    const char *cursor = bang + 1;
    while (*cursor != '\0' && copied < 4) {
        if (std::isxdigit(static_cast<unsigned char>(*cursor)) == 0) {
            break;
        }
        expected_hex[copied++] = *cursor++;
    }

    if (copied != 4) {
        return false;
    }

    char *end = nullptr;
    const long expected = std::strtol(expected_hex, &end, 16);
    if (end == nullptr || *end != '\0' || expected < 0 || expected > 0xFFFF) {
        return false;
    }

    const auto actual = static_cast<long>(dsmr_crc16(reinterpret_cast<const uint8_t *>(telegram),
                                                     static_cast<size_t>((bang - telegram) + 1)));
    return actual == expected;
}

void trim_line(char *line) {
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
}

bool extract_group(const char *line, int group_index, char *out, size_t out_size) {
    if (group_index < 1 || out_size == 0) {
        return false;
    }

    const char *cursor = line;
    for (int current_group = 1; current_group <= group_index; ++current_group) {
        const char *start = std::strchr(cursor, '(');
        if (start == nullptr) {
            return false;
        }

        const char *end = std::strchr(start + 1, ')');
        if (end == nullptr) {
            return false;
        }

        if (current_group == group_index) {
            const size_t len = static_cast<size_t>(end - (start + 1));
            const size_t copy_len = len < (out_size - 1) ? len : (out_size - 1);
            std::memcpy(out, start + 1, copy_len);
            out[copy_len] = '\0';
            return true;
        }

        cursor = end + 1;
    }

    return false;
}

bool line_has_prefix(const char *line, const char *prefix) {
    return std::strncmp(line, prefix, std::strlen(prefix)) == 0;
}

bool extract_string_after_prefix(const char *line, const char *prefix, int group_index, char *out, size_t out_size) {
    if (!line_has_prefix(line, prefix)) {
        return false;
    }
    return extract_group(line, group_index, out, out_size);
}

bool parse_double_group(const char *line, const char *prefix, int group_index, double &value) {
    char field[kFieldBufferSize];
    if (!extract_string_after_prefix(line, prefix, group_index, field, sizeof(field))) {
        return false;
    }

    char number[kFieldBufferSize];
    size_t idx = 0;
    while (field[idx] != '\0' && field[idx] != '*' && idx < sizeof(number) - 1) {
        number[idx] = field[idx];
        ++idx;
    }
    number[idx] = '\0';

    errno = 0;
    char *end = nullptr;
    const double parsed = std::strtod(number, &end);
    if (end == number || errno != 0) {
        return false;
    }

    value = parsed;
    return true;
}

bool parse_int_group(const char *line, const char *prefix, int group_index, int &value) {
    char field[kFieldBufferSize];
    if (!extract_string_after_prefix(line, prefix, group_index, field, sizeof(field))) {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(field, &end, 10);
    if (end == field || errno != 0) {
        return false;
    }

    value = static_cast<int>(parsed);
    return true;
}

void parse_line(const char *line, MeterReading &reading) {
    if (line[0] == '/') {
        copy_string(reading.header, sizeof(reading.header), line + 1);
        return;
    }

    extract_string_after_prefix(line, "0-0:1.0.0", 1, reading.timestamp, sizeof(reading.timestamp));
    extract_string_after_prefix(line, "0-0:96.1.1", 1, reading.meter_id, sizeof(reading.meter_id));

    if (reading.meter_id[0] == '\0') {
        extract_string_after_prefix(line, "0-0:96.1.0", 1, reading.meter_id, sizeof(reading.meter_id));
    }

    parse_int_group(line, "0-0:96.14.0", 1, reading.tariff);
    parse_int_group(line, "0-0:96.7.21", 1, reading.power_failures);
    parse_int_group(line, "0-0:96.7.9", 1, reading.long_power_failures);

    parse_double_group(line, "1-0:1.8.1", 1, reading.import_t1_kwh);
    parse_double_group(line, "1-0:1.8.2", 1, reading.import_t2_kwh);
    parse_double_group(line, "1-0:2.8.1", 1, reading.export_t1_kwh);
    parse_double_group(line, "1-0:2.8.2", 1, reading.export_t2_kwh);
    parse_double_group(line, "1-0:1.7.0", 1, reading.power_delivered_kw);
    parse_double_group(line, "1-0:2.7.0", 1, reading.power_returned_kw);

    parse_double_group(line, "1-0:32.7.0", 1, reading.voltage_l1_v);
    parse_double_group(line, "1-0:52.7.0", 1, reading.voltage_l2_v);
    parse_double_group(line, "1-0:72.7.0", 1, reading.voltage_l3_v);
    parse_double_group(line, "1-0:31.7.0", 1, reading.current_l1_a);
    parse_double_group(line, "1-0:51.7.0", 1, reading.current_l2_a);
    parse_double_group(line, "1-0:71.7.0", 1, reading.current_l3_a);
    parse_double_group(line, "1-0:21.7.0", 1, reading.power_delivered_l1_kw);
    parse_double_group(line, "1-0:41.7.0", 1, reading.power_delivered_l2_kw);
    parse_double_group(line, "1-0:61.7.0", 1, reading.power_delivered_l3_kw);
    parse_double_group(line, "1-0:22.7.0", 1, reading.power_returned_l1_kw);
    parse_double_group(line, "1-0:42.7.0", 1, reading.power_returned_l2_kw);
    parse_double_group(line, "1-0:62.7.0", 1, reading.power_returned_l3_kw);

    if (extract_string_after_prefix(line, "0-1:24.2.1", 1, reading.gas_timestamp, sizeof(reading.gas_timestamp))) {
        parse_double_group(line, "0-1:24.2.1", 2, reading.gas_m3);
    }
}

bool parse_telegram(const char *telegram, MeterReading &reading) {
    reset_reading(reading);

    const char *cursor = telegram;
    while (*cursor != '\0') {
        const char *line_end = std::strchr(cursor, '\n');
        const size_t line_len = line_end == nullptr
                                    ? std::strlen(cursor)
                                    : static_cast<size_t>(line_end - cursor);

        if (line_len > 0) {
            char line[kLineBufferSize];
            const size_t copy_len = line_len < (sizeof(line) - 1) ? line_len : (sizeof(line) - 1);
            std::memcpy(line, cursor, copy_len);
            line[copy_len] = '\0';
            trim_line(line);
            parse_line(line, reading);
        }

        if (line_end == nullptr) {
            break;
        }
        cursor = line_end + 1;
    }

    reading.crc_ok = verify_crc(telegram);
    return reading.header[0] != '\0' && (reading.meter_id[0] != '\0' || value_is_set(reading.power_delivered_kw));
}

void json_add_number_if_set(cJSON *root, const char *key, double value) {
    if (value_is_set(value)) {
        cJSON_AddNumberToObject(root, key, value);
    }
}

void json_add_int_if_set(cJSON *root, const char *key, int value) {
    if (value >= 0) {
        cJSON_AddNumberToObject(root, key, value);
    }
}

bool mqtt_is_connected() {
    return (xEventGroupGetBits(s_event_group) & MQTT_CONNECTED_BIT) != 0;
}

void mqtt_publish_text(const char *topic, const char *payload, int qos, int retain) {
    if (s_mqtt_client == nullptr || topic == nullptr || payload == nullptr) {
        return;
    }

    const int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, qos, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT publish failed for topic %s", topic);
    }
}

void publish_reading(const MeterReading &reading, const char *raw_telegram) {
    if (!mqtt_is_connected()) {
        ESP_LOGW(TAG, "Skipping publish because MQTT is offline");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate JSON object");
        return;
    }

    cJSON_AddStringToObject(root, "header", reading.header);
    cJSON_AddBoolToObject(root, "crc_ok", reading.crc_ok);

    if (reading.meter_id[0] != '\0') {
        cJSON_AddStringToObject(root, "meter_id", reading.meter_id);
    }
    if (reading.timestamp[0] != '\0') {
        cJSON_AddStringToObject(root, "timestamp", reading.timestamp);
    }
    if (reading.gas_timestamp[0] != '\0') {
        cJSON_AddStringToObject(root, "gas_timestamp", reading.gas_timestamp);
    }

    json_add_int_if_set(root, "tariff", reading.tariff);
    json_add_int_if_set(root, "power_failures", reading.power_failures);
    json_add_int_if_set(root, "long_power_failures", reading.long_power_failures);

    json_add_number_if_set(root, "import_t1_kwh", reading.import_t1_kwh);
    json_add_number_if_set(root, "import_t2_kwh", reading.import_t2_kwh);
    json_add_number_if_set(root, "export_t1_kwh", reading.export_t1_kwh);
    json_add_number_if_set(root, "export_t2_kwh", reading.export_t2_kwh);
    json_add_number_if_set(root, "power_delivered_kw", reading.power_delivered_kw);
    json_add_number_if_set(root, "power_returned_kw", reading.power_returned_kw);

    json_add_number_if_set(root, "voltage_l1_v", reading.voltage_l1_v);
    json_add_number_if_set(root, "voltage_l2_v", reading.voltage_l2_v);
    json_add_number_if_set(root, "voltage_l3_v", reading.voltage_l3_v);
    json_add_number_if_set(root, "current_l1_a", reading.current_l1_a);
    json_add_number_if_set(root, "current_l2_a", reading.current_l2_a);
    json_add_number_if_set(root, "current_l3_a", reading.current_l3_a);
    json_add_number_if_set(root, "power_delivered_l1_kw", reading.power_delivered_l1_kw);
    json_add_number_if_set(root, "power_delivered_l2_kw", reading.power_delivered_l2_kw);
    json_add_number_if_set(root, "power_delivered_l3_kw", reading.power_delivered_l3_kw);
    json_add_number_if_set(root, "power_returned_l1_kw", reading.power_returned_l1_kw);
    json_add_number_if_set(root, "power_returned_l2_kw", reading.power_returned_l2_kw);
    json_add_number_if_set(root, "power_returned_l3_kw", reading.power_returned_l3_kw);
    json_add_number_if_set(root, "gas_m3", reading.gas_m3);

    char *json_payload = cJSON_PrintUnformatted(root);
    if (json_payload == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize JSON payload");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Publishing meter reading to MQTT");
    mqtt_publish_text(s_state_topic, json_payload, CONFIG_ENERGYMETER_MQTT_QOS, 0);

#if CONFIG_ENERGYMETER_PUBLISH_RAW_TELEGRAM
    if (raw_telegram != nullptr) {
        mqtt_publish_text(s_raw_topic, raw_telegram, 0, 0);
    }
#else
    (void)raw_telegram;
#endif

    cJSON_free(json_payload);
    cJSON_Delete(root);
}

void prepare_topics() {
    std::snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", CONFIG_ENERGYMETER_MQTT_BASE_TOPIC);
    std::snprintf(s_raw_topic, sizeof(s_raw_topic), "%s/raw", CONFIG_ENERGYMETER_MQTT_BASE_TOPIC);
    std::snprintf(s_availability_topic, sizeof(s_availability_topic), "%s/availability", CONFIG_ENERGYMETER_MQTT_BASE_TOPIC);
}

void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args;
    (void)base;
    auto event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
            ESP_LOGI(TAG, "MQTT connected");
            mqtt_publish_text(s_availability_topic, "online", 1, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT transport error type=%d", event->error_handle ? event->error_handle->error_type : -1);
            break;
        default:
            break;
    }
}

void start_mqtt() {
    if (s_mqtt_started) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = CONFIG_ENERGYMETER_MQTT_URI;
#if CONFIG_ENERGYMETER_MQTT_USE_TLS
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    mqtt_cfg.credentials.username = std::strlen(CONFIG_ENERGYMETER_MQTT_USERNAME) > 0
                                        ? CONFIG_ENERGYMETER_MQTT_USERNAME
                                        : nullptr;
    mqtt_cfg.credentials.authentication.password = std::strlen(CONFIG_ENERGYMETER_MQTT_PASSWORD) > 0
                                                       ? CONFIG_ENERGYMETER_MQTT_PASSWORD
                                                       : nullptr;
    mqtt_cfg.credentials.client_id = std::strlen(CONFIG_ENERGYMETER_MQTT_CLIENT_ID) > 0
                                         ? CONFIG_ENERGYMETER_MQTT_CLIENT_ID
                                         : nullptr;
    mqtt_cfg.session.keepalive = 30;
    mqtt_cfg.session.last_will.topic = s_availability_topic;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;
    mqtt_cfg.network.timeout_ms = 10000;
    mqtt_cfg.buffer.size = 2048;
    mqtt_cfg.task.stack_size = 6144;

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    ESP_LOGI(TAG, "Starting MQTT client topic=%s client_id=%s tls=%s",
             CONFIG_ENERGYMETER_MQTT_BASE_TOPIC,
             std::strlen(CONFIG_ENERGYMETER_MQTT_CLIENT_ID) > 0 ? CONFIG_ENERGYMETER_MQTT_CLIENT_ID : "auto",
#if CONFIG_ENERGYMETER_MQTT_USE_TLS
             "on"
#else
             "off"
#endif
    );
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    s_mqtt_started = true;
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi station started, connecting to SSID \"%s\"", CONFIG_ENERGYMETER_WIFI_SSID);
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);

        if (s_wifi_retry_count < CONFIG_ENERGYMETER_WIFI_MAXIMUM_RETRY) {
            ++s_wifi_retry_count;
        }
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi disconnected, retry=%d reason=%d",
                 s_wifi_retry_count,
                 event != nullptr ? event->reason : -1);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi connected to \"%s\" RSSI=%d channel=%u IP " IPSTR,
                     reinterpret_cast<const char *>(ap_info.ssid),
                     ap_info.rssi,
                     ap_info.primary,
                     IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "Wi-Fi connected with IP " IPSTR, IP2STR(&event->ip_info.ip));
        }
        start_mqtt();
    }
}

void wifi_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));

    wifi_config_t wifi_cfg = {};
    std::snprintf(reinterpret_cast<char *>(wifi_cfg.sta.ssid), sizeof(wifi_cfg.sta.ssid), "%s",
                  CONFIG_ENERGYMETER_WIFI_SSID);
    std::snprintf(reinterpret_cast<char *>(wifi_cfg.sta.password), sizeof(wifi_cfg.sta.password), "%s",
                  CONFIG_ENERGYMETER_WIFI_PASSWORD);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void init_p1_request_gpio() {
    const int request_gpio = CONFIG_ENERGYMETER_P1_REQUEST_GPIO;
#if CONFIG_ENERGYMETER_P1_REQUEST_ACTIVE_HIGH
    constexpr int request_level = 1;
    const char *request_polarity = "high";
#else
    constexpr int request_level = 0;
    const char *request_polarity = "low";
#endif

    if (request_gpio < 0) {
        ESP_LOGI(TAG, "P1 request pin disabled");
        return;
    }

    gpio_config_t gpio_cfg = {};
    gpio_cfg.pin_bit_mask = gpio_mask_for_pin(request_gpio);
    gpio_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_cfg.intr_type = GPIO_INTR_DISABLE;

    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
    ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(request_gpio), request_level));
    ESP_LOGI(TAG, "P1 request pin set to GPIO%d active_%s",
             request_gpio,
             request_polarity);
}

void init_p1_uart() {
    const uart_port_t uart_port = static_cast<uart_port_t>(CONFIG_ENERGYMETER_P1_UART_PORT);

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = CONFIG_ENERGYMETER_P1_BAUDRATE;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(uart_port, CONFIG_ENERGYMETER_UART_BUFFER_SIZE, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_port, &uart_cfg));
#if CONFIG_ENERGYMETER_P1_RX_INTERNAL_PULLUP
    ESP_ERROR_CHECK(gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_ENERGYMETER_P1_RX_GPIO), GPIO_PULLUP_ONLY));
#endif
    ESP_ERROR_CHECK(uart_set_pin(uart_port, UART_PIN_NO_CHANGE, CONFIG_ENERGYMETER_P1_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

#if CONFIG_ENERGYMETER_P1_UART_INVERT_RX
    ESP_ERROR_CHECK(uart_set_line_inverse(uart_port, UART_SIGNAL_RXD_INV));
#endif

    ESP_ERROR_CHECK(uart_flush_input(uart_port));
    ESP_LOGI(TAG, "P1 UART ready port=%d rx_gpio=%d baud=%d invert_rx=%s",
             static_cast<int>(uart_port),
             CONFIG_ENERGYMETER_P1_RX_GPIO,
             CONFIG_ENERGYMETER_P1_BAUDRATE,
#if CONFIG_ENERGYMETER_P1_UART_INVERT_RX
             "yes"
#else
             "no"
#endif
    );
#if CONFIG_ENERGYMETER_P1_RX_INTERNAL_PULLUP
    ESP_LOGI(TAG, "P1 RX internal pull-up enabled on GPIO%d", CONFIG_ENERGYMETER_P1_RX_GPIO);
#endif
}

bool telegram_complete(const char *buffer, size_t len) {
    if (len < 7 || buffer[len - 1] != '\n') {
        return false;
    }

    if (buffer[len - 2] == '\r') {
        return std::isxdigit(static_cast<unsigned char>(buffer[len - 3])) != 0 &&
               std::isxdigit(static_cast<unsigned char>(buffer[len - 4])) != 0 &&
               std::isxdigit(static_cast<unsigned char>(buffer[len - 5])) != 0 &&
               std::isxdigit(static_cast<unsigned char>(buffer[len - 6])) != 0 &&
               buffer[len - 7] == '!';
    }

    return std::isxdigit(static_cast<unsigned char>(buffer[len - 2])) != 0 &&
           std::isxdigit(static_cast<unsigned char>(buffer[len - 3])) != 0 &&
           std::isxdigit(static_cast<unsigned char>(buffer[len - 4])) != 0 &&
           std::isxdigit(static_cast<unsigned char>(buffer[len - 5])) != 0 &&
           buffer[len - 6] == '!';
}

void handle_complete_telegram(const char *telegram) {
    MeterReading reading;
    if (!parse_telegram(telegram, reading)) {
        ESP_LOGW(TAG, "Failed to parse telegram");
        return;
    }

#if CONFIG_ENERGYMETER_P1_REQUIRE_CRC
    if (!reading.crc_ok) {
        ESP_LOGW(TAG, "Ignoring telegram because CRC check failed");
        return;
    }
#endif

    ESP_LOGI(TAG, "Telegram ok meter=%s power=%.3f kW gas=%.3f m3 crc=%s",
             reading.meter_id[0] != '\0' ? reading.meter_id : "unknown",
             value_is_set(reading.power_delivered_kw) ? reading.power_delivered_kw : -1.0,
             value_is_set(reading.gas_m3) ? reading.gas_m3 : -1.0,
             reading.crc_ok ? "ok" : "bad");

    publish_reading(reading, telegram);
}

void status_task(void *arg) {
    (void)arg;

    while (true) {
        const EventBits_t bits = xEventGroupGetBits(s_event_group);
        const bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;
        const bool mqtt_connected = (bits & MQTT_CONNECTED_BIT) != 0;

        if (wifi_connected) {
            wifi_ap_record_t ap_info = {};
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                ESP_LOGI(TAG, "Status wifi=up ssid=\"%s\" rssi=%d ch=%u mqtt=%s p1_data=%s",
                         reinterpret_cast<const char *>(ap_info.ssid),
                         ap_info.rssi,
                         ap_info.primary,
                         mqtt_connected ? "up" : "down",
                         s_seen_p1_data ? "yes" : "no");
            } else {
                ESP_LOGI(TAG, "Status wifi=up mqtt=%s p1_data=%s",
                         mqtt_connected ? "up" : "down",
                         s_seen_p1_data ? "yes" : "no");
            }
        } else {
            ESP_LOGW(TAG, "Status wifi=down mqtt=%s p1_data=%s retry=%d",
                     mqtt_connected ? "up" : "down",
                     s_seen_p1_data ? "yes" : "no",
                     s_wifi_retry_count);
        }

        vTaskDelay(kStatusLogPeriod);
    }
}

void p1_reader_task(void *arg) {
    (void)arg;

    init_p1_request_gpio();
    init_p1_uart();

    const uart_port_t uart_port = static_cast<uart_port_t>(CONFIG_ENERGYMETER_P1_UART_PORT);
    char telegram[CONFIG_ENERGYMETER_TELEGRAM_BUFFER_SIZE];
    uint8_t rx_chunk[128];

    bool collecting = false;
    bool seen_uart_data = false;
    size_t telegram_len = 0;
    TickType_t last_rx_tick = xTaskGetTickCount();
    TickType_t last_no_data_log_tick = last_rx_tick;

    ESP_LOGI(TAG, "P1 reader task started");

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const int bytes_read = uart_read_bytes(uart_port, rx_chunk, sizeof(rx_chunk), pdMS_TO_TICKS(250));
        if (bytes_read <= 0) {
            if ((now - last_rx_tick) >= kNoDataWarnPeriod && (now - last_no_data_log_tick) >= kNoDataWarnPeriod) {
                ESP_LOGW(TAG, "No P1 data seen on UART%d RX GPIO%d for %lu ms",
                         static_cast<int>(uart_port),
                         CONFIG_ENERGYMETER_P1_RX_GPIO,
                         static_cast<unsigned long>(pdTICKS_TO_MS(now - last_rx_tick)));
                last_no_data_log_tick = now;
            }
            continue;
        }

        if (!seen_uart_data) {
            ESP_LOGI(TAG, "First P1 UART bytes received (%d bytes)", bytes_read);
            seen_uart_data = true;
            s_seen_p1_data = true;
        } else if ((now - last_rx_tick) >= kNoDataWarnPeriod) {
            ESP_LOGI(TAG, "P1 UART traffic resumed (%d bytes)", bytes_read);
        }
        last_rx_tick = now;

        for (int i = 0; i < bytes_read; ++i) {
            const char ch = static_cast<char>(rx_chunk[i]);

            if (!collecting) {
                if (ch == '/') {
                    collecting = true;
                    telegram_len = 0;
                    telegram[telegram_len++] = ch;
                    ESP_LOGI(TAG, "P1 telegram start detected");
                }
                continue;
            }

            if (ch == '/') {
                log_telegram_preview("Restarting P1 frame capture after incomplete telegram", telegram, telegram_len);
                telegram_len = 0;
                telegram[telegram_len++] = ch;
                continue;
            }

            if (telegram_len >= sizeof(telegram) - 1) {
                log_telegram_preview("Telegram buffer overflow, dropping frame", telegram, telegram_len);
                collecting = false;
                telegram_len = 0;
                continue;
            }

            telegram[telegram_len++] = ch;
            telegram[telegram_len] = '\0';

            if (telegram_complete(telegram, telegram_len)) {
                handle_complete_telegram(telegram);
                collecting = false;
                telegram_len = 0;
            }
        }
    }
}

void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

}  // namespace

extern "C" void app_main() {
    s_event_group = xEventGroupCreate();
    prepare_topics();
    init_nvs();

    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "P1 RX GPIO=%d request GPIO=%d MQTT topic=%s",
             CONFIG_ENERGYMETER_P1_RX_GPIO,
             CONFIG_ENERGYMETER_P1_REQUEST_GPIO,
             CONFIG_ENERGYMETER_MQTT_BASE_TOPIC);

    if (std::strlen(CONFIG_ENERGYMETER_WIFI_SSID) == 0 || std::strlen(CONFIG_ENERGYMETER_MQTT_URI) == 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID or MQTT URI is empty; configure project settings before deployment");
    }

    wifi_init();

    xTaskCreate(p1_reader_task,
                "p1_reader",
                CONFIG_ENERGYMETER_READER_TASK_STACK,
                nullptr,
                5,
                nullptr);
    xTaskCreate(status_task,
                "status_task",
                4096,
                nullptr,
                4,
                nullptr);
}
