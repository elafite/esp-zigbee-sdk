#include "pump.hpp"
#include "esp_timer.h"

Pump::Pump(gpio_num_t gpio_pin,
           uint8_t gpio_on_level,
           float flow_rate_l_per_hour,
           uint32_t default_auto_off_ms)
    : m_gpio_pin(gpio_pin),
      m_gpio_on_level(gpio_on_level),
      m_flow_rate_l_per_hour(flow_rate_l_per_hour),
      m_default_auto_off_ms(default_auto_off_ms),
      m_running(false),
      m_pump_start_time_ms(0),
      m_session_volume_ml(0),
      m_current_auto_off_timeout(default_auto_off_ms),
      m_target_volume_ml(100.0f),
      m_auto_off_timer(nullptr),
      m_volume_task_handle(nullptr)
{
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << m_gpio_pin);
    gpio_config(&io_conf);

    turnGpioOff();

    m_auto_off_timer = xTimerCreate(
        "pump_auto_off",
        pdMS_TO_TICKS(m_default_auto_off_ms),
        pdFALSE,
        this,
        autoOffTimerCallback
    );
}

void Pump::start()
{
    if (m_running) {
        return;
    }

    m_running = true;
    m_session_volume_ml = 0;
    m_pump_start_time_ms = esp_timer_get_time() / 1000;

    turnGpioOn();

    xTimerChangePeriod(
        m_auto_off_timer,
        pdMS_TO_TICKS(m_current_auto_off_timeout),
        0);
    xTimerStart(m_auto_off_timer, 0);

    xTaskCreate(
        volumeTask,
        "pump_volume_task",
        2048,
        this,
        5,
        &m_volume_task_handle);
}

void Pump::stop()
{
    if (!m_running) {
        return;
    }

    m_running = false;

    turnGpioOff();
    xTimerStop(m_auto_off_timer, 0);

    if (m_volume_task_handle) {
        vTaskDelete(m_volume_task_handle);
        m_volume_task_handle = nullptr;
    }
}

void Pump::setTargetVolume(float volume_ml)
{
    m_target_volume_ml = volume_ml;
}

void Pump::setAutoOffTimeout(uint32_t timeout_ms)
{
    m_current_auto_off_timeout = timeout_ms;
}

bool Pump::isRunning() const
{
    return m_running;
}

uint32_t Pump::getSessionVolumeMl() const
{
    return m_session_volume_ml;
}

/* ===== Static callbacks ===== */

void Pump::autoOffTimerCallback(TimerHandle_t timer)
{
    auto *pump = static_cast<Pump *>(pvTimerGetTimerID(timer));
    pump->stop();
}

void Pump::volumeTask(void *arg)
{
    auto *pump = static_cast<Pump *>(arg);

    while (pump->m_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        float ml_per_second =
            (pump->m_flow_rate_l_per_hour * 1000.0f) / 3600.0f;

        pump->m_session_volume_ml += static_cast<uint32_t>(ml_per_second);

        if (pump->m_session_volume_ml >= pump->m_target_volume_ml) {
            pump->stop();
        }
    }

    vTaskDelete(nullptr);
}

/* ===== GPIO helpers ===== */

void Pump::turnGpioOn()
{
    gpio_set_level(m_gpio_pin, m_gpio_on_level);
}

void Pump::turnGpioOff()
{
    gpio_set_level(m_gpio_pin, !m_gpio_on_level);
}