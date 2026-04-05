/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Zigbee HA_on_off_light Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include "esp_zb_light.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_metering.h"
#include "esp_zigbee_type.h"

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

static const char *TAG = "ESP_ZB_ON_OFF_LIGHT";

#define DEFAULT_AUTO_OFF_MS (2000)

/* GPIO configuration for plant watering pump/solenoid */
#define PUMP_GPIO_PIN GPIO_NUM_1  /* Change this to your desired GPIO pin */
#define PUMP_GPIO_LEVEL 0         /* GPIO level for pump ON (1=HIGH, 0=LOW) */

/* Water flow configuration */
#define PUMP_FLOW_RATE_ML_PER_MIN 500  /* Change this to your pump's flow rate in mL/min */

/* Timer for auto-off functionality */
static TimerHandle_t auto_off_timer = NULL;

/* Current auto-off timeout in milliseconds */
static uint32_t current_auto_off_timeout = 0;

/* Water volume tracking */
static uint32_t pump_start_time_ms = 0;  /* When pump was last turned on */

/* Forward declaration */
static void turn_off_light_zb_task(uint8_t param);

/* Helper function to convert volume mL to 48-bit struct for Zigbee */
static esp_zb_uint48_t volume_to_uint48(uint32_t volume_ml)
{
    esp_zb_uint48_t result = {0};
    result.low = volume_ml;
    return result;
}

/* light control function*/
static void set_light(bool power)
{
    light_driver_set_color_RGB(0, 0, power * 255);  // set OFF (0,0,0) or ON BLUE (0,0,255)
}

/* Pump control function */
static void pump_set_power(bool power)
{
    if (power) {
        // Configure as output and set LOW to activate relay
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << PUMP_GPIO_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(PUMP_GPIO_PIN, PUMP_GPIO_LEVEL);  // LOW to activate relay
        pump_start_time_ms = esp_timer_get_time() / 1000;  // Record start time in ms
        ESP_LOGI(TAG, "Pump turned ON (GPIO output LOW) - Start time: %d ms", pump_start_time_ms);
    } else {
        // Calculate volume delivered in this session
        uint32_t session_volume_ml = 0;
        if (pump_start_time_ms > 0) {
            uint32_t runtime_ms = esp_timer_get_time() / 1000 - pump_start_time_ms;
            session_volume_ml = (runtime_ms * PUMP_FLOW_RATE_ML_PER_MIN) / 60000;  // Convert to mL
            ESP_LOGI(TAG, "Pump session: %d ms runtime, %d mL delivered", runtime_ms, session_volume_ml);
            pump_start_time_ms = 0;  // Reset start time
            
            // Report the session volume to HA
            esp_zb_uint48_t volume_attr = volume_to_uint48(session_volume_ml);
            esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT_ENDPOINT, 
                                         ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                         ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                         ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                         &volume_attr,
                                         false);  // Trigger reporting
            ESP_LOGI(TAG, "Volume pulse reported to HA: %d mL", session_volume_ml);
        }
        
        // Configure as input with pullup to deactivate relay
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << PUMP_GPIO_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        ESP_LOGI(TAG, "Pump turned OFF (GPIO input with pullup)");
    }
}

/* Initialize GPIO for pump control */
static void pump_init(void)
{
    // Start with pump OFF (input with pullup)
    pump_set_power(false);
    ESP_LOGI(TAG, "Pump GPIO initialized on pin %d (input with pullup)", PUMP_GPIO_PIN);
}

/* Timer callback to turn off the light */
static void auto_off_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Auto-off timer expired, turning light off");
    // Schedule attribute update in Zigbee task context
    esp_zb_scheduler_alarm((esp_zb_callback_t)turn_off_light_zb_task, 0, 0);
    current_auto_off_timeout = 0;
}

/* Function to report water volume in Zigbee task context */
static void report_water_volume_zb_task(uint8_t param)
{
    (void)param;  // Unused parameter
    ESP_LOGI(TAG, "Water volume reporting enabled");
}

/* Function to turn off light in Zigbee task context */
static void turn_off_light_zb_task(uint8_t param)
{
    (void)param;

    set_light(false);
    pump_set_power(false);
    ESP_LOGI(TAG, "Light and pump turned OFF by auto-off timer");

    // Update the attribute value
    uint8_t attr_value = 0;
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        HA_ESP_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        &attr_value,
        false);  // <-- false: don't mark as "needs reporting", we send manually

    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Failed to update OnOff attribute: %d", status);
        return;
    }

    // Send an unsolicited attribute report to the coordinator
   esp_zb_zcl_report_attr_cmd_t report_cmd = {
    .zcl_basic_cmd = {
        .src_endpoint       = HA_ESP_LIGHT_ENDPOINT,
        .dst_endpoint       = 1,        // coordinator/z2m endpoint
        .dst_addr_u.addr_short = 0x0000, // coordinator always has short addr 0x0000
    },
    .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
    .clusterID    = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
    .direction    = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
    .attributeID  = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
    };
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&report_cmd);
    ESP_LOGI(TAG, "Report sent to coordinator: %s", esp_err_to_name(err));
    }
/********************* Define functions **************************/
static esp_err_t deferred_driver_init(void)
{
    static bool is_inited = false;
    if (!is_inited) {
        light_driver_init(LIGHT_DEFAULT_OFF);
        is_inited = true;
    }
    return is_inited ? ESP_OK : ESP_FAIL;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : " non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status: %s, retrying", esp_zb_zdo_signal_to_string(sig_type),
                     esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            
            // Configure reporting for OnOff attribute after joining network
            esp_zb_zcl_reporting_info_t reporting_info = {
                .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
                .ep = HA_ESP_LIGHT_ENDPOINT,
                .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                .attr_id = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                .flags = 6,  // ZB_ZCL_REPORT_ATTR | ZB_ZCL_REPORT_IS_ALLOWED
                .u.send_info = {
                    .min_interval = 0,  // Minimum reporting interval in seconds
                    .max_interval = 300,  // Maximum reporting interval (5 minutes)
                    .delta = { .u8 = 1 },  // Report on any change
                    .reported_value = { .u8 = 0 }  // Initial value
                },
                .dst = {
                    .short_addr = 0x0000,  // Report to coordinator
                    .endpoint = 1,
                    .profile_id = ESP_ZB_AF_HA_PROFILE_ID
                }
            };
            esp_err_t report_err = esp_zb_zcl_update_reporting_info(&reporting_info);
            ESP_LOGI(TAG, "On/Off reporting configuration updated, result: %s", esp_err_to_name(report_err));

            // Configure reporting for Metering cluster (volume delivered)
            esp_zb_zcl_reporting_info_t metering_reporting_info = {0};
            metering_reporting_info.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
            metering_reporting_info.ep = HA_ESP_LIGHT_ENDPOINT;
            metering_reporting_info.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_METERING;
            metering_reporting_info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
            metering_reporting_info.attr_id = ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID;
            metering_reporting_info.flags = 6;  // ZB_ZCL_REPORT_ATTR | ZB_ZCL_REPORT_IS_ALLOWED
            metering_reporting_info.run_time = 0;
            metering_reporting_info.u.send_info.min_interval = 0;
            metering_reporting_info.u.send_info.max_interval = 300;
            metering_reporting_info.u.send_info.delta.u48.low = 1;
            metering_reporting_info.u.send_info.delta.u48.high = 0;
            metering_reporting_info.u.send_info.reported_value.u48.low = 0;
            metering_reporting_info.u.send_info.reported_value.u48.high = 0;
            metering_reporting_info.u.send_info.def_min_interval = 0;
            metering_reporting_info.u.send_info.def_max_interval = 0;
            metering_reporting_info.dst.short_addr = 0x0000;
            metering_reporting_info.dst.endpoint = 1;
            metering_reporting_info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
            metering_reporting_info.manuf_code = 0;
            esp_err_t metering_report_err = esp_zb_zcl_update_reporting_info(&metering_reporting_info);
            ESP_LOGI(TAG, "Metering reporting configuration updated, result: %s", esp_err_to_name(metering_report_err));

            // Trigger initial volume report if any pump session has occurred
            esp_zb_scheduler_alarm((esp_zb_callback_t)report_water_volume_zb_task, 0, 0);
            ESP_LOGI(TAG, "Water volume pulse reporting enabled");
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_command_handler(const esp_zb_zcl_custom_cluster_command_message_t *message)
{
    esp_err_t ret = ESP_OK;
    
    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->info.command.id == ESP_ZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID) {
                // Handle "On with Timed Off" command
                esp_zb_zcl_on_off_on_with_timed_off_cmd_t *cmd = (esp_zb_zcl_on_off_on_with_timed_off_cmd_t *)message->data.value;
                
                // Turn on the light and pump
                // light_driver_set_power(true);
                set_light (true);
                pump_set_power(true);
                ESP_LOGI(TAG, "Light and pump turned ON with timed off (on_time: %d tenths of second)", cmd->on_time);
                
                // Start timer for auto-off (convert from tenths of second to milliseconds)
                current_auto_off_timeout = cmd->on_time * 100;  // on_time is in 1/10ths second
                
                if (auto_off_timer != NULL) {
                    // Stop any existing timer first
                    xTimerStop(auto_off_timer, 0);
                    
                    // Change timer period and start
                    if (xTimerChangePeriod(auto_off_timer, pdMS_TO_TICKS(current_auto_off_timeout), 0) == pdPASS) {
                        xTimerStart(auto_off_timer, 0);
                        ESP_LOGI(TAG, "Auto-off timer started (%d ms)", current_auto_off_timeout);
                    } else {
                        ESP_LOGE(TAG, "Failed to change timer period");
                    }
                }
            }
        }
    }
    
    return ret;
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;

    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light sets to %s", light_state ? "On" : "Off");
                //light_driver_set_power(light_state);
                set_light(light_state);
                pump_set_power(light_state);  /* Control pump along with light */
                
                // Handle auto-off timer
                if (light_state) {
                    // Light turned ON - start default auto-off timer
                    current_auto_off_timeout = DEFAULT_AUTO_OFF_MS;
                    if (auto_off_timer != NULL) {
                        xTimerStop(auto_off_timer, 0);
                        xTimerChangePeriod(auto_off_timer, pdMS_TO_TICKS(current_auto_off_timeout), 0);
                        xTimerStart(auto_off_timer, 0);
                        ESP_LOGI(TAG, "Auto-off timer started (%d ms, default)", current_auto_off_timeout);
                    }
                } else {
                    // Light turned OFF - stop timer if running
                    if (auto_off_timer != NULL) {
                        xTimerStop(auto_off_timer, 0);
                        ESP_LOGI(TAG, "Auto-off timer stopped");
                    }
                    current_auto_off_timeout = 0;
                }
            }
        }
    }
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID:
        ret = zb_command_handler((esp_zb_zcl_custom_cluster_command_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    
    // Create endpoint with On/Off cluster
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *esp_zb_on_off_light_ep = esp_zb_on_off_light_ep_create(HA_ESP_LIGHT_ENDPOINT, &light_cfg);
    
    // Add Metering cluster for water volume tracking
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(esp_zb_on_off_light_ep, HA_ESP_LIGHT_ENDPOINT);
    
    // Configure metering cluster for water volume
    esp_zb_metering_cluster_cfg_t metering_cfg = {
        .current_summation_delivered = {0},  // Start at 0 - each pulse is a new delivery
        .status = 0x00,                      // No error status
        .uint_of_measure = 0x00,             // Liters (for mL compatibility)
        .summation_formatting = 0x00,        // No special formatting
        .metering_device_type = 0x02,        // Water meter
    };
    
    // Create metering cluster
    esp_zb_attribute_list_t *metering_cluster = esp_zb_metering_cluster_create(&metering_cfg);
    
    // Add cluster to endpoint
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_metering_cluster(cluster_list, metering_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "Metering cluster added to endpoint %d", HA_ESP_LIGHT_ENDPOINT);

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };

    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_on_off_light_ep, HA_ESP_LIGHT_ENDPOINT, &info);
    esp_zb_device_register(esp_zb_on_off_light_ep);
    
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    
    /* Initialize pump GPIO control */
    pump_init();
    
    /* Create auto-off timer (default 5 seconds, but will be changed dynamically) */
    auto_off_timer = xTimerCreate("auto_off_timer", 
                                  pdMS_TO_TICKS(5000),  // Default 5 seconds
                                  pdFALSE,              // One-shot timer
                                  NULL, 
                                  auto_off_timer_callback);
    
    if (auto_off_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create auto-off timer");
    }
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
