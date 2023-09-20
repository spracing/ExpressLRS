#if defined(GPIO_PIN_PWM_OUTPUTS)

#include "devServoOutput.h"
#include "PWM.h"
#include "CRSF.h"
#include "config.h"
#include "helpers.h"
#include "rxtx_intf.h"

static int8_t SERVO_PINS[PWM_MAX_CHANNELS] = {-1};
static pwm_channel_t PWM_CHANNELS[PWM_MAX_CHANNELS] = {-1};
// true when the RX has a new channels packet
static bool newChannelsAvailable;
// Absolute max failsafe time if no update is received, regardless of LQ
static constexpr uint32_t FAILSAFE_ABS_TIMEOUT_MS = 1000U;

void ICACHE_RAM_ATTR servoNewChannelsAvaliable()
{
    newChannelsAvailable = true;
}

uint16_t servoOutputModeToFrequency(eServoOutputMode mode)
{
    switch (mode)
    {
    case som50Hz:
        return 50U;
    case som60Hz:
        return 60U;
    case som100Hz:
        return 100U;
    case som160Hz:
        return 160U;
    case som333Hz:
        return 333U;
    case som400Hz:
        return 400U;
    case som10KHzDuty:
        return 10000U;
    default:
        return 0;
    }
}

static void servoWrite(uint8_t ch, uint16_t us)
{
    const rx_config_pwm_t *chConfig = config.GetPwmChannel(ch);
    if ((eServoOutputMode)chConfig->val.mode == somOnOff)
    {
        digitalWrite(SERVO_PINS[ch], us > 1500);
    }
    else
    {
        if ((eServoOutputMode)chConfig->val.mode == som10KHzDuty)
        {
            PWM.setDuty(PWM_CHANNELS[ch], constrain(us, 1000, 2000) - 1000);
        }
        else
        {
            PWM.setMicroseconds(PWM_CHANNELS[ch], us / (chConfig->val.narrow + 1));
        }
    }
}

static void servosFailsafe()
{
    constexpr unsigned SERVO_FAILSAFE_MIN = 988U;
    for (unsigned ch = 0 ; ch < GPIO_PIN_PWM_OUTPUTS_COUNT ; ++ch)
    {
        const rx_config_pwm_t *chConfig = config.GetPwmChannel(ch);
        // Note: Failsafe values do not respect the inverted flag, failsafes are absolute
        uint16_t us = chConfig->val.failsafe + SERVO_FAILSAFE_MIN;
        // Always write the failsafe position even if the servo never has been started,
        // so all the servos go to their expected position
        servoWrite(ch, us);
    }
}

static int servosUpdate(unsigned long now)
{
    static uint32_t lastUpdate;
    if (newChannelsAvailable)
    {
        newChannelsAvailable = false;
        lastUpdate = now;
        for (unsigned ch = 0 ; ch < GPIO_PIN_PWM_OUTPUTS_COUNT ; ++ch)
        {
            const rx_config_pwm_t *chConfig = config.GetPwmChannel(ch);
            const unsigned crsfVal = ChannelData[chConfig->val.inputChannel];
            // crsfVal might 0 if this is a switch channel and it has not been
            // received yet. Delay initializing the servo until the channel is valid
            if (crsfVal == 0)
            {
                continue;
            }

            uint16_t us = CRSF_to_US(crsfVal);
            // Flip the output around the mid value if inverted
            // (1500 - usOutput) + 1500
            if (chConfig->val.inverted)
            {
                us = 3000U - us;
            }
            servoWrite(ch, us);
        } /* for each servo */
    }     /* if newChannelsAvailable */

    // LQ goes to 0 (100 packets missed in a row)
    // OR last update older than FAILSAFE_ABS_TIMEOUT_MS
    // go to failsafe
    else if (lastUpdate && ((getLq() == 0) || (now - lastUpdate > FAILSAFE_ABS_TIMEOUT_MS)))
    {
        servosFailsafe();
        lastUpdate = 0;
    }

    return DURATION_IMMEDIATELY;
}

static void initialize()
{
    if (!OPT_HAS_SERVO_OUTPUT)
    {
        return;
    }

    for (int ch = 0; ch < GPIO_PIN_PWM_OUTPUTS_COUNT; ++ch)
    {
        uint8_t pin = GPIO_PIN_PWM_OUTPUTS[ch];
#if (defined(DEBUG_LOG) || defined(DEBUG_RCVR_LINKSTATS)) && (defined(PLATFORM_ESP8266) || defined(PLATFORM_ESP32))
        // Disconnect the debug UART pins if DEBUG_LOG
#if defined(PLATFORM_ESP32_S3)
        if (pin == 43 || pin == 44)
#else
        if (pin == 1 || pin == 3)
#endif
        {
            pin = UNDEF_PIN;
        }
#endif
        // Mark servo pins that are being used for serial as disconnected
        eServoOutputMode mode = (eServoOutputMode)config.GetPwmChannel(ch)->val.mode;
        if (mode == somSerial)
        {
            pin = UNDEF_PIN;
        }
        SERVO_PINS[ch] = pin;
        // Initialize all servos to low ASAP
        if (pin != UNDEF_PIN)
        {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, LOW);
        }
    }
}

static int start()
{
    for (unsigned ch = 0; ch < GPIO_PIN_PWM_OUTPUTS_COUNT; ++ch)
    {
        const rx_config_pwm_t *chConfig = config.GetPwmChannel(ch);
        auto frequency = servoOutputModeToFrequency((eServoOutputMode)chConfig->val.mode);
        if (frequency)
        {
            PWM_CHANNELS[ch] = PWM.allocate(SERVO_PINS[ch], frequency);
        }
    }

    return DURATION_NEVER;
}

static int event(bool timeout_expired)
{
    if (!OPT_HAS_SERVO_OUTPUT || connectionState == disconnected)
    {
        // Disconnected should come after failsafe on the RX
        // so it is safe to shut down when disconnected
        return DURATION_NEVER;
    }
    else if (connectionState == wifiUpdate)
    {
        for (unsigned ch = 0; ch < GPIO_PIN_PWM_OUTPUTS_COUNT; ++ch)
        {
            if (PWM_CHANNELS[ch] != -1)
            {
                PWM.release(PWM_CHANNELS[ch]);
                PWM_CHANNELS[ch] = -1;
            }
            SERVO_PINS[ch] = -1;
        }
        return DURATION_NEVER;
    }
    return DURATION_IMMEDIATELY;
}

static int timeout()
{
    return servosUpdate(millis());
}

device_t ServoOut_device = {
    .initialize = initialize,
    .start = start,
    .event = event,
    .timeout = timeout,
};

#endif
