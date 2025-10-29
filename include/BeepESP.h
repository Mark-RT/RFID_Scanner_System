#ifndef BeepESP_H
#define BeepESP_H

#include <Arduino.h>

enum StateMachine
{
    IDLE,
    ACTIVE,
    FOREVER,
    PAUSE,
    PAUSE_FOREVER
};
StateMachine _beepESP_state = IDLE;

class BeepESP
{
public:
    BeepESP() {}
    void init(uint8_t buzzer_pin, uint8_t pwm_channel, uint8_t pwm_resolution = 8, uint16_t init_freq = 1000)
    {
        _buzzer_pin = buzzer_pin;
        _pwm_channel = pwm_channel;
        _pwm_resolution = pwm_resolution;
        _init_freq = init_freq;
        ledcSetup(_pwm_channel, _init_freq, _pwm_resolution);
        ledcAttachPin(_buzzer_pin, _pwm_channel);
        ledcWrite(_pwm_channel, 0);
    }

    void beep(uint16_t freq, uint16_t count, uint32_t onTime, uint32_t offTime = 0) // Запуск послідовності писків
    {
        _freq = freq;
        _count = count;
        _onTime = onTime;
        _offTime = offTime;
        _current_count = 0;
        _beep_isOn = false;
        _beep_isReady = false;
        _beep_timer = millis();
        _beepESP_state = ACTIVE;
    }

    void beepForever(uint16_t freq, uint32_t onTime, uint32_t offTime = 0) // Запуск послідовності писків
    {
        _freq = freq;
        _onTime = onTime;
        _offTime = offTime;
        _beep_isOn = false;
        _beep_isReady = true;
        _beep_timer = millis();
        _beepESP_state = FOREVER;
    }

    void stop() // Зупинка будь-якого писку
    {
        ledcWrite(_pwm_channel, 0);
        _beep_isOn = false;
        _beep_isReady = true;
        _current_count = 0;
        _beepESP_state = IDLE;
    }

    bool isReady()
    {
        return _beep_isReady;
    }

    void tick() // Функція обробки писку (асинхронна)
    {
        unsigned long now = millis();

        switch (_beepESP_state)
        {
        case IDLE:
            // нічого не робимо
            break;

        case ACTIVE:
            if (!_beep_isOn)
            {
                ledcWriteTone(_pwm_channel, _freq);
                _beep_isOn = true;
                _beep_timer = now;
            }
            else if (now - _beep_timer >= _onTime)
            {
                ledcWrite(_pwm_channel, 0);
                _beep_isOn = false;
                _beep_timer = now;
                _current_count++;
                if (_current_count >= _count)
                {
                    stop();
                }
                else
                {
                    _beepESP_state = PAUSE;
                }
            }
            break;

        case FOREVER:
            if (!_beep_isOn)
            {
                ledcWriteTone(_pwm_channel, _freq);
                _beep_isOn = true;
                _beep_timer = now;
            }
            else if (now - _beep_timer >= _onTime)
            {
                ledcWrite(_pwm_channel, 0);
                _beep_isOn = false;
                _beep_timer = now;
                _beepESP_state = PAUSE_FOREVER;
            }
            break;

        case PAUSE:
            if (now - _beep_timer >= _offTime)
            {
                _beepESP_state = ACTIVE;
            }
            break;

        case PAUSE_FOREVER:
            if (now - _beep_timer >= _offTime)
            {
                _beepESP_state = FOREVER;
            }
            break;

        }
    }

private:
    uint8_t _buzzer_pin, _pwm_channel, _pwm_resolution;
    uint16_t _init_freq, _freq, _count, _current_count;
    uint32_t _onTime, _offTime;
    bool _beep_isOn = false, _beep_isReady = true;
    unsigned long _beep_timer = 0;
};

#endif