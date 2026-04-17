#pragma once

#include <cstdint>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"

class Pump
{
public:
    // Constructor
    Pump(gpio_num_t gpio_pin,
         uint8_t gpio_on_level,
         float flow_rate_l_per_hour,
         uint32_t default_auto_off_ms = 10000);

    // Control
    void start();
    void stop();

    // Configuration
    void setTargetVolume(float volume_ml);
    void setAutoOffTimeout(uint32_t timeout_ms);

    // Status
    bool isRunning() const;
    uint32_t getSessionVolumeMl() const;

private:
    /* Configuration */
    gpio_num_t m_gpio_pin;
    uint8_t    m_gpio_on_level;
    float      m_flow_rate_l_per_hour;
    uint32_t   m_default_auto_off_ms;

    /* Runtime state */
    bool        m_running;
    uint32_t    m_pump_start_time_ms;
    uint32_t    m_session_volume_ml;
    uint32_t    m_current_auto_off_timeout;
    float       m_target_volume_ml;

    /* FreeRTOS resources */
    TimerHandle_t m_auto_off_timer;
    TaskHandle_t  m_volume_task_handle;

private:
    // Internal helpers
    static void autoOffTimerCallback(TimerHandle_t timer);
    static void volumeTask(void *arg);

    void turnGpioOn();
    void turnGpioOff();
};
