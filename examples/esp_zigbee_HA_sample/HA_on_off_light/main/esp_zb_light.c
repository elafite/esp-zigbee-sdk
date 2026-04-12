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

static const char *TAG = "ESP_ZB_ON_OFF_PUMP";

#define DEFAULT_AUTO_OFF_MS (10000)

static float target_volume = 100.0f;  /* Default target volume in mL for watering */

/* GPIO configuration for plant watering pump/solenoid */
#define PUMP_GPIO_PIN GPIO_NUM_1  /* Change this to your desired GPIO pin */
#define PUMP_GPIO_LEVEL 0         /* GPIO level for pump ON (1=HIGH, 0=LOW) */

/* Water flow configuration */
#define PUMP_FLOW_RATE_L_PER_HOUR 50  /* Change this to your pump's flow rate in L/h */

static bool g_pump_running = false;  /* Track pump state for volume calculation */
/* Timer for auto-off functionality */
static TimerHandle_t auto_off_timer = NULL;

static TaskHandle_t volume_task_handle = NULL;
/* Current auto-off timeout in milliseconds */
static uint32_t current_auto_off_timeout = 0;

/* Water volume tracking */
static uint32_t pump_start_time_ms = 0;  /* When pump was last turned on */
static uint32_t session_volume_ml = 0;

/* Forward declaration */
static void turn_off_zb_task(uint8_t param);


/* light control function*/
static void set_light(bool power)
{
    light_driver_set_color_RGB(0, 0, power * 255);  // set OFF (0,0,0) or ON BLUE (0,0,255)
}

static void update_volume_callback(uint8_t param)
{
    // On récupère la valeur statique ou via un pointeur
    // Pour faire simple ici, on va utiliser une variable globale ou lire la session_volume_ml
    float volume_liters = (float)session_volume_ml; 
    
    esp_zb_zcl_set_attribute_val(
        HA_ESP_PUMP_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
        &volume_liters,
        false
    );
}

// Fonction utilitaire pour envoyer le volume
void update_zigbee_volume() {
    // Calculate volume delivered in this session
    uint32_t runtime_ms = esp_timer_get_time() / 1000 - pump_start_time_ms;
    session_volume_ml = (runtime_ms * PUMP_FLOW_RATE_L_PER_HOUR) / 3600;  // Convert to mL
    ESP_LOGI(TAG, "Pump session: %d ms runtime, %d mL delivered", runtime_ms, session_volume_ml);
    esp_zb_scheduler_alarm((esp_zb_callback_t)update_volume_callback, 0, 0);
}

/* Pump control function */
static void pump_set_power(bool power)
{
    g_pump_running = power;
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
        if (volume_task_handle != NULL) {
            xTaskNotifyGive(volume_task_handle);
        }
        ESP_LOGI(TAG, "Pump turned ON (GPIO output LOW) - Start time: %d ms", pump_start_time_ms);
    } else {
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
    set_light(false);
    pump_set_power(false);
    esp_zb_scheduler_alarm((esp_zb_callback_t)turn_off_zb_task, 0, 0);
    current_auto_off_timeout = 0;
}

/* Function to turn off light in Zigbee task context */
static void turn_off_zb_task(uint8_t param)
{
//    (void)param;

    // set_light(false);
    // pump_set_power(false);
    ESP_LOGI(TAG, "Light and pump turned OFF by auto-off timer");

    // Update the attribute value
     uint8_t attr_value = 0;
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        HA_ESP_PUMP_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        &attr_value,
        false);  // <-- false to avoid triggering another report
    
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Failed to update OnOff attribute: %d", status);
        return;
    }

    // Send an unsolicited attribute report to the coordinator
   esp_zb_zcl_report_attr_cmd_t report_cmd = {
    .zcl_basic_cmd = {
        .src_endpoint       = HA_ESP_PUMP_ENDPOINT,
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
                //update_volume_callback(0);  // Update volume attribute on reboot to reflect current state
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
                .ep = HA_ESP_PUMP_ENDPOINT,
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

// static esp_err_t zb_command_handler(const esp_zb_zcl_custom_cluster_command_message_t *message)
// {
//     esp_err_t ret = ESP_OK;
    
//     if (message->info.dst_endpoint == HA_ESP_PUMP_ENDPOINT) {
//         if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
//             if (message->info.command.id == ESP_ZB_ZCL_CMD_ON_OFF_ON_WITH_TIMED_OFF_ID) {
//                 // Handle "On with Timed Off" command
//                 esp_zb_zcl_on_off_on_with_timed_off_cmd_t *cmd = (esp_zb_zcl_on_off_on_with_timed_off_cmd_t *)message->data.value;
                
//                 // Turn on the light and pump
//                 // light_driver_set_power(true);
//                 set_light (true);
//                 pump_set_power(true);
//                 ESP_LOGI(TAG, "Light and pump turned ON with timed off (on_time: %d tenths of second)", cmd->on_time);
                
//                 // Start timer for auto-off (convert from tenths of second to milliseconds)
//                 current_auto_off_timeout = cmd->on_time * 100;  // on_time is in 1/10ths second
                
//                 if (auto_off_timer != NULL) {
//                     // Stop any existing timer first
//                     xTimerStop(auto_off_timer, 0);
                    
//                     // Change timer period and start
//                     if (xTimerChangePeriod(auto_off_timer, pdMS_TO_TICKS(current_auto_off_timeout), 0) == pdPASS) {
//                         xTimerStart(auto_off_timer, 0);
//                         ESP_LOGI(TAG, "Auto-off timer started (%d ms)", current_auto_off_timeout);
//                     } else {
//                         ESP_LOGE(TAG, "Failed to change timer period");
//                     }
//                 }
//             }
//         }
//     }
    
//     return ret;
// }

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool pump_light_state = 0;

    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT) {
        return ESP_OK; 
    }
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    if (message->info.dst_endpoint == HA_ESP_PUMP_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                bool new_state = *(bool *)message->attribute.data.value;

                // SI L'ÉTAT EST DÉJÀ LE BON, ON NE FAIT RIEN
                if (new_state == g_pump_running) {
                    ESP_LOGD(TAG, "State already %s, ignoring redundant command", new_state ? "On" : "Off");
                    return ESP_OK; 
                }

                pump_light_state = new_state; // Mettre à jour l'état actuel                
                //pump_light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : pump_light_state;
                ESP_LOGI(TAG, "Light sets to %s", pump_light_state ? "On" : "Off");
                //light_driver_set_power(light_state);
                set_light(pump_light_state);
                pump_set_power(pump_light_state);  /* Control pump along with light */
                
                // Handle auto-off timer
                if (pump_light_state) {
                    // Light turned ON - start default auto-off timer
                    current_auto_off_timeout = 3600.0f * target_volume / PUMP_FLOW_RATE_L_PER_HOUR; //DEFAULT_AUTO_OFF_MS;
                    if (auto_off_timer != NULL) {
                        xTimerStop(auto_off_timer, 0);
                        xTimerChangePeriod(auto_off_timer, pdMS_TO_TICKS(current_auto_off_timeout), 0);
                        xTimerStart(auto_off_timer, 0);
                        ESP_LOGI(TAG, "Auto-off timer started (%d ms for %f mL)", current_auto_off_timeout, target_volume);
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
        } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_VALUE) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID) {
                // On récupère la valeur envoyée par HA (en float)
                target_volume = *(float *)message->attribute.data.value;
                
                // Conversion en millisecondes pour le timer FreeRTOS
                // Securité : on limite entre 1 et 60 minutes par exemple
                if (target_volume < 10.0f) target_volume = 10.0f;
                if (target_volume > 500.0f) target_volume = 500.0f;

                ESP_LOGI(TAG, "Volume d'arrosage mis a jour : %f mL", target_volume);
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
         //ret = zb_command_handler((esp_zb_zcl_custom_cluster_command_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void volume_reporting_task(void *pvParameters){
    
    static uint32_t last_reporting_ms; 
    while (1) {
        // La tâche s'endort ici et attend un "signal" pour commencer
        // PortMaxDelay = elle ne consomme rien tant qu'elle ne reçoit pas de notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Début du reporting temps réel...");
        
        last_reporting_ms = esp_timer_get_time() / 1000; // Initialiser le temps de référence

        while (g_pump_running) {
            uint32_t elapsed_since_last_reporting = esp_timer_get_time() / 1000 - last_reporting_ms;
            if (elapsed_since_last_reporting >= 2000) { // Report every 2000 ms
                last_reporting_ms = esp_timer_get_time() / 1000; // Reset reference time
                ESP_LOGI(TAG, "Reporting volume..."); // Log pour debug
                update_zigbee_volume();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Envoi final une fois que la pompe s'arrête
        update_zigbee_volume();
        pump_start_time_ms = 0;  // Reset start time
        ESP_LOGI(TAG, "Fin du reporting. Retour en veille.");
    }
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    
    // 1. Préparer les données (Format Zigbee : longueur en premier octet)
    uint8_t mfr_name[] = {7, 'E', 'm', 'l', ' ', 'I', 'o', 'T'};
    uint8_t model_id[] = {14, 'p', 'l', 'a', 'n', 't', '-', 'w', 'a', 't', 'e', 'r', 'i', 'n', 'g'};

    // 2. Créer la configuration de base
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };

    // 3. Créer la liste d'attributs et AJOUTER tes infos
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);

    // Pour le Fabricant (Eml IoT)
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, mfr_name);

    // Pour le Modèle (plant-watering)
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_id);    


    // 1. Création du cluster On/Off (pour la pompe)
    esp_zb_on_off_cluster_cfg_t on_off_cfg = { .on_off = false };
    esp_zb_attribute_list_t *on_off_attr_list = esp_zb_on_off_cluster_create(&on_off_cfg);

    uint8_t read_write_access = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
    esp_zb_cluster_update_attr(on_off_attr_list, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &read_write_access);
    // 2. Création du cluster Analog Input (pour le volume d'eau)
    esp_zb_analog_input_cluster_cfg_t analog_input_cfg = {
        .present_value = 0.0f, // Volume initial
        .out_of_service = false,
        .status_flags = 0
    };
    esp_zb_attribute_list_t *analog_input_attr_list = esp_zb_analog_input_cluster_create(&analog_input_cfg);

    // Configurer l'attribut de volume (en mL)
    float default_volume = 100.0f; // 100 mL par défaut
    esp_zb_attribute_list_t *analog_val_attr_list = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_VALUE);

    esp_zb_analog_value_cluster_add_attr(analog_val_attr_list, 
        ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID, 
        &default_volume);

    // 1. Déclarer le mode d'accès dans une variable
    uint8_t access_mode = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;

    // 2. Passer l'ADRESSE de cette variable (&access_mode)
    esp_zb_cluster_update_attr(analog_val_attr_list, 
        ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID, 
        &access_mode); // Le '&' est crucial ici


    // 3. Création de la liste des clusters pour l'Endpoint
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_on_off_cluster(cluster_list, on_off_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_analog_input_cluster(cluster_list, analog_input_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_analog_value_cluster(cluster_list, analog_val_attr_list, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // 4. Enregistrement de l'Endpoint
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HA_ESP_PUMP_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID, // On utilise "Output" pour un actionneur
    };
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);

    // Enregistrement final du device
    esp_zb_device_register(ep_list);

    esp_zb_zcl_reporting_info_t reporting_info = {
        .direction = 0,               // 0 = Send
        .ep = HA_ESP_PUMP_ENDPOINT,   // Le champ s'appelle .ep
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .attr_id = ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
        .u.send_info = {
            .min_interval = 1,
            .max_interval = 0,        // 0 = Pas de rapport périodique, seulement sur changement
            .def_min_interval = 1,
            .def_max_interval = 0,
        },
        .dst = {
            .short_addr = 0x0000,     // Destination : Coordinateur (Home Assistant)
            .endpoint = HA_ESP_PUMP_ENDPOINT,
            .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        }
    };

    // Enregistrement de la configuration
    esp_zb_zcl_update_reporting_info(&reporting_info);

    
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
    xTaskCreate(volume_reporting_task, "report_volume", 4096, NULL, 3, &volume_task_handle);
}
