#if defined(GPIO_PIN_SPI_VTX_NSS)

#include "devVTXSPI.h"
#include "targets.h"
#include "common.h"
#include "helpers.h"
#include "hwTimer.h"
#include "logging.h"
#include <SPI.h>
#include "PWM.h"

#define SYNTHESIZER_REGISTER_A                  0x00
#define SYNTHESIZER_REGISTER_B                  0x01
#define SYNTHESIZER_REGISTER_C                  0x02
#define RF_VCO_DFC_CONTROL_REGISTER             0x03
#define VCO_CONTROL_REGISTER                    0x04
#define VCO_CONTROL_REGISTER_CONT               0x05
#define AUDIO_MODULATOR_CONTROL_REGISTER        0x06
#define PRE_DRIVER_AND_PA_CONTROL_REGISTER      0x07
#define STATE_REGISTER                          0x0F

#define SYNTH_REG_A_DEFAULT                     0x0190

#define POWER_AMP_ON                            0b00000100111110111111
#define POWER_AMP_OFF                           0x00
// ESP32 DAC pins are 0-255
#define MIN_DAC                                 1 // Testing required.
#define MAX_DAC                                 250 // Absolute max is 255.  But above 250 does nothing.
// PWM is 0-4095
#define MIN_PWM                                 2000 // could be even higher than that, depends on HW.
#define MAX_PWM                                 3700 // Absolute max is 4095. But above 3600 does nothing.

#define VPD_BUFFER                              5

#define READ_BIT                                0x00
#define WRITE_BIT                               0x01

#define RTC6705_BOOT_DELAY                      350
#define RTC6705_PLL_SETTLE_TIME_MS              500 // ms - after changing frequency turn amps back on after this time for clean switching
#define VTX_POWER_INTERVAL_MS                   20

#define BUF_PACKET_SIZE                         4 // 25b packet in 4 bytes

#if defined(PLATFORM_ESP32)
pwm_channel_t rfAmpPwmChannel = -1;
#endif

uint16_t vtxSPIFrequency = 6000;
static uint16_t vtxSPIFrequencyCurrent = 6000;

uint8_t vtxSPIPowerIdx = 0;
static uint8_t vtxSPIPowerIdxCurrent = 0;

uint8_t vtxSPIPitmode = 1;
static uint8_t vtxSPIPitmodeCurrent = 1;

static uint8_t RfAmpVrefState = 0;

static uint16_t vtxSPIPWM = MAX_PWM;
static uint16_t vtxPreviousSPIPWM = 0;

static uint16_t vtxMinPWM = MIN_PWM;
static uint16_t vtxMaxPWM = MAX_PWM;

static uint16_t VpdSetPoint = 0;
static uint16_t Vpd = 0;


static bool stopVtxMonitoring = false;

#define VPD_SETPOINT_0_MW                       VPD_BUFFER // to avoid overflow
#define VPD_SETPOINT_YOLO_MW                    2250
#if defined(TARGET_UNIFIED_RX)
const uint16_t *VpdSetPointArray25mW = nullptr;
const uint16_t *VpdSetPointArray100mW = nullptr;
#else
uint16_t VpdSetPointArray25mW[] = VPD_VALUES_25MW;
uint16_t VpdSetPointArray100mW[] = VPD_VALUES_100MW;
#endif

uint16_t VpdFreqArray[] = {5650, 5750, 5850, 5950};
uint8_t VpdSetPointCount =  ARRAY_SIZE(VpdFreqArray);

static SPIClass *vtxSPI;
static bool vtxSPIIsShared = false;

static void rtc6705WriteRegister(uint32_t regData)
{
    DBGLN("VTX: Register: %d", regData);

    // When sharing the SPI Bus control of the NSS pin is done by us
    if (vtxSPIIsShared)
    {
        vtxSPI->setBitOrder(LSBFIRST);
        digitalWrite(GPIO_PIN_SPI_VTX_NSS, LOW);
    }

    #if defined(PLATFORM_ESP32_S3)
        // On the S3 there's either a library/rom/silicon bug, sometimes when sending 25 bits the last
        // bit is 1, when it should be 0.
        //
        // to reproduce, change from channel A4 to A1, then A1 to A4 (ok), then A4 to A3, then A3 to A4 (fail)
        // 12816, 9286833 appears on the scope when changing from A:1->A:4
        // 12816, 26064049 appears on the scope when changing from A:3->A:4
        // 
        // 9286833  = 0_1000_1101_1011_0100_1011_0001
        // 26064049 = 1_1000_1101_1011_0100_1011_0001
        //
        // so, to workaround this issue, we send 32 bits, and the RTC67005 ignores the last 7 bits.
        regData |= 0b1111111 << 25;
        vtxSPI->transferBits(regData, nullptr, 32);
    #elif defined(PLATFORM_ESP32)
        vtxSPI->transferBits(regData, nullptr, 25);
    #else
        uint8_t buf[BUF_PACKET_SIZE];
        memcpy(buf, (byte *)&regData, BUF_PACKET_SIZE);
        vtxSPI->transfer(buf, BUF_PACKET_SIZE);
    #endif

    if (vtxSPIIsShared)
    {
        digitalWrite(GPIO_PIN_SPI_VTX_NSS, HIGH);
        vtxSPI->setBitOrder(MSBFIRST);
    }
}

static void rtc6705ResetSynthRegA()
{
  uint32_t regData = SYNTHESIZER_REGISTER_A | (WRITE_BIT << 4) | (SYNTH_REG_A_DEFAULT << 5);
  rtc6705WriteRegister(regData);
}

static void rtc6705SetFrequency(uint32_t freq)
{
    DBGLN("VTX: Set frequency: %d", freq);

    rtc6705ResetSynthRegA();

    // RTC6705 seems to need a delay between setting the Synth A and B registers.
    // Observed that on an ESP32S3 a timing of 350us does NOT work reliably.
    // and that on an ESP32PicoD4 a timing of 2ms DOES work.

    // Note that on the ESP32PicoD4 no delay was needed as it took 2ms to run the code without the delay
    // but on the ESP32S3 it only took 350us without a delay.
#if defined(PLATFORM_ESP32_S3)
    delay(2);
#endif

    VTxOutputMinimum(); // Set power to zero for clear channel switching

    uint32_t f = 25 * freq;
    uint32_t SYN_RF_N_REG = f / 64;
    uint32_t SYN_RF_A_REG = f % 64;

    uint32_t regData = SYNTHESIZER_REGISTER_B | (WRITE_BIT << 4) | (SYN_RF_A_REG << 5) | (SYN_RF_N_REG << 12);
    rtc6705WriteRegister(regData);
}

static void rtc6705PowerAmpOn()
{
    DBGLN("VTX: internal PA On");
    uint32_t regData = PRE_DRIVER_AND_PA_CONTROL_REGISTER | (WRITE_BIT << 4) | (POWER_AMP_ON << 5);
    rtc6705WriteRegister(regData);
}

static void RfAmpVrefOn()
{
    if (!RfAmpVrefState) {
        DBGLN("VTX: external PA on");
        digitalWrite(GPIO_PIN_RF_AMP_VREF, HIGH);
    }

    RfAmpVrefState = 1;
}

static void RfAmpVrefOff()
{
    if (RfAmpVrefState) {
        DBGLN("VTX: external PA off");
        digitalWrite(GPIO_PIN_RF_AMP_VREF, LOW);
    }

    RfAmpVrefState = 0;
}

static void setPWM()
{
    if (vtxSPIPWM == vtxPreviousSPIPWM) {
        return;
    }

#if defined(PLATFORM_ESP32_S3)
    DBGLN("VTX: setPWM (pwm), value: %d", vtxSPIPWM * 1000 / 4096);
    PWM.setDuty(rfAmpPwmChannel, vtxSPIPWM * 1000 / 4096);
#elif defined(PLATFORM_ESP32)
    if (GPIO_PIN_RF_AMP_PWM == 25 || GPIO_PIN_RF_AMP_PWM == 26)
    {
        DBGLN("VTX: setPWM (dac), value: %d", vtxSPIPWM >> 4);
        dacWrite(GPIO_PIN_RF_AMP_PWM, vtxSPIPWM >> 4);
    }
    else
    {
        DBGLN("VTX: setPWM (pwm), value: %d", vtxSPIPWM * 1000 / 4096);
        PWM.setDuty(rfAmpPwmChannel, vtxSPIPWM * 1000 / 4096);
    }
#else
    DBGLN("VTX: setPWM (analog), value: %d", vtxSPIPWM);
    analogWrite(GPIO_PIN_RF_AMP_PWM, vtxSPIPWM);
#endif

    vtxPreviousSPIPWM = vtxSPIPWM;
}

void VTxOutputMinimum()
{
    DBGLN("VTX: minimum output power");

    RfAmpVrefOff();

    vtxSPIPWM = vtxMaxPWM;
    setPWM();
}

static void VTxOutputIncrease()
{
    if (vtxSPIPWM > vtxMinPWM) vtxSPIPWM -= 1;
    setPWM();
}

static void VTxOutputDecrease()
{
    if (vtxSPIPWM < vtxMaxPWM) vtxSPIPWM += 1;
    setPWM();
}

static uint16_t LinearInterpVpdSetPointArray(const uint16_t VpdSetPointArray[])
{
    uint16_t newVpd = 0;

    if (vtxSPIFrequencyCurrent <= VpdFreqArray[0])
    {
        newVpd = VpdSetPointArray[0];
    }
    else if (vtxSPIFrequencyCurrent >= VpdFreqArray[VpdSetPointCount - 1])
    {
        newVpd = VpdSetPointArray[VpdSetPointCount - 1];
    }
    else
    {
        for (uint8_t i = 0; i < (VpdSetPointCount - 1); i++)
        {
            if (vtxSPIFrequencyCurrent < VpdFreqArray[i + 1])
            {
                newVpd = VpdSetPointArray[i] + ((VpdSetPointArray[i + 1]-VpdSetPointArray[i])/(VpdFreqArray[i + 1]-VpdFreqArray[i])) * (vtxSPIFrequencyCurrent - VpdFreqArray[i]);
            }
        }
    }

    DBGLN("VTX: linearVpd, value: %d", newVpd);

    return newVpd;
}

static void SetVpdSetPoint()
{
    switch (vtxSPIPowerIdx)
    {
    case 1: // 0 mW
        VpdSetPoint = VPD_SETPOINT_0_MW;
        break;

    case 2: // RCE
    case 3: // 25 mW
        VpdSetPoint = LinearInterpVpdSetPointArray(VpdSetPointArray25mW);
        break;

    case 4: // 100 mW
        VpdSetPoint = LinearInterpVpdSetPointArray(VpdSetPointArray100mW);
        break;

    default: // YOLO mW
        VpdSetPoint = VPD_SETPOINT_YOLO_MW;
        break;
    }

    DBGLN("VTX: new VPD setpoint: %d, power: %d", VpdSetPoint, vtxSPIPowerIdx);
}

static void checkOutputPower()
{
    if (vtxSPIPitmodeCurrent)
    {
        VTxOutputMinimum();
    }
    else
    {
        RfAmpVrefOn();

        uint16_t VpdReading = analogRead(GPIO_PIN_RF_AMP_VPD); // WARNING - Max input 1.0V !!!!

        Vpd = (8 * Vpd + 2 * VpdReading) / 10; // IIR filter

        if (Vpd < (VpdSetPoint - VPD_BUFFER))
        {
            VTxOutputIncrease();
        }
        else if (Vpd > (VpdSetPoint + VPD_BUFFER))
        {
            VTxOutputDecrease();
        }

        DBGLN("VTX: VPD setpoint: %d, raw: %d, filtered: %d, PWM: %d", VpdSetPoint, VpdReading, Vpd, vtxSPIPWM);
    }
}

void disableVTxSpi()
{
    stopVtxMonitoring = true;
    VTxOutputMinimum();
}


static void initialize()
{
    #if defined(TARGET_UNIFIED_RX)
    VpdSetPointArray25mW = VPD_VALUES_25MW;
    VpdSetPointArray100mW = VPD_VALUES_100MW;
    #endif

    if (GPIO_PIN_SPI_VTX_NSS != UNDEF_PIN)
    {
        vtxSPIIsShared = GPIO_PIN_SPI_VTX_SCK == UNDEF_PIN || GPIO_PIN_SPI_VTX_SCK == GPIO_PIN_SCK;

        if (!vtxSPIIsShared)
        {
            DBGLN("VTX: Using dedicated SPI");

            pinMode(GPIO_PIN_SPI_VTX_SCK, OUTPUT);
            pinMode(GPIO_PIN_SPI_VTX_MISO, INPUT);
            pinMode(GPIO_PIN_SPI_VTX_MOSI, OUTPUT);
            digitalWrite(GPIO_PIN_SPI_VTX_SCK, HIGH);

            #if defined(PLATFORM_ESP32_S3)
            vtxSPI = new SPIClass(HSPI);
            #else
            vtxSPI = new SPIClass(VSPI);
            #endif
            vtxSPI->begin(GPIO_PIN_SPI_VTX_SCK, GPIO_PIN_SPI_VTX_MISO, GPIO_PIN_SPI_VTX_MOSI, GPIO_PIN_SPI_VTX_NSS);
            vtxSPI->setFrequency(100000);
            vtxSPI->setHwCs(true);
            vtxSPI->setBitOrder(LSBFIRST);
            //vtxSPI->setDataMode(SPI_MODE0);
        }
        else
        {
            DBGLN("VTX: Using shared SPI");
            vtxSPI = &SPI;
            pinMode(GPIO_PIN_SPI_VTX_NSS, OUTPUT);
            digitalWrite(GPIO_PIN_SPI_VTX_NSS, HIGH);
        }

        pinMode(GPIO_PIN_RF_AMP_VREF, OUTPUT);
        digitalWrite(GPIO_PIN_RF_AMP_VREF, LOW);

        #if defined(PLATFORM_ESP32_S3)
            rfAmpPwmChannel = PWM.allocate(GPIO_PIN_RF_AMP_PWM, 10000);
            DBGLN("VTX: Using PWM, channel: %d", rfAmpPwmChannel);
        #elif defined(PLATFORM_ESP32)
            // If using a DAC pin then adjust min/max and initial value
            if (GPIO_PIN_RF_AMP_PWM == 25 || GPIO_PIN_RF_AMP_PWM == 26)
            {
                vtxMinPWM = MIN_DAC;
                vtxMaxPWM = MAX_DAC;
                vtxSPIPWM = vtxMaxPWM;
                DBGLN("VTX: Using DAC");
            }
            else
            {
                rfAmpPwmChannel = PWM.allocate(GPIO_PIN_RF_AMP_PWM, 10000);
                DBGLN("VTX: Using PWM, channel: %d", rfAmpPwmChannel);
            }
        #else
            DBGLN("VTX: Using analog");
            pinMode(GPIO_PIN_RF_AMP_PWM, OUTPUT);
            analogWriteFreq(10000); // 10kHz
            analogWriteResolution(12); // 0 - 4095
        #endif

        DBGLN("VTX: PWM, min: %d, max: %d, spipwm: %d", vtxMinPWM, vtxMaxPWM, vtxSPIPWM);

        setPWM();

        delay(RTC6705_BOOT_DELAY);
    }
}

static int start()
{
    if (GPIO_PIN_SPI_VTX_NSS == UNDEF_PIN)
    {
        return DURATION_NEVER;
    }

    rtc6705SetFrequency(5999); // Boot with VTx set away from standard frequencies.

    rtc6705PowerAmpOn();

    return VTX_POWER_INTERVAL_MS;
}

static int event()
{
    if (GPIO_PIN_SPI_VTX_NSS == UNDEF_PIN)
    {
        return DURATION_NEVER;
    }

    if (vtxSPIFrequencyCurrent != vtxSPIFrequency || vtxSPIPowerIdxCurrent != vtxSPIPowerIdx || vtxSPIPitmodeCurrent != vtxSPIPitmode)
    {
        return DURATION_IMMEDIATELY;
    }

    return DURATION_IGNORE;
}

static int timeout()
{
    if ((GPIO_PIN_SPI_VTX_NSS == UNDEF_PIN) || stopVtxMonitoring)
    {
        return DURATION_NEVER;
    }

    if (hwTimer::running && !hwTimer::isTick)
    {
        // Dont run spi and analog reads during rx hopping, wifi or updating
        return DURATION_IMMEDIATELY;
    }

    if (vtxSPIFrequencyCurrent != vtxSPIFrequency)
    {
        DBGLN("VTX: Changing frequency, old: %d, new: %d", vtxSPIFrequencyCurrent, vtxSPIFrequency);

        rtc6705SetFrequency(vtxSPIFrequency);
        vtxSPIFrequencyCurrent = vtxSPIFrequency;

        return RTC6705_PLL_SETTLE_TIME_MS;
    }

    if (vtxSPIPowerIdxCurrent != vtxSPIPowerIdx)
    {
        DBGLN("VTX: set power: %d", vtxSPIPowerIdx);
        SetVpdSetPoint();
        vtxSPIPowerIdxCurrent = vtxSPIPowerIdx;
    }

    if (vtxSPIPitmodeCurrent != vtxSPIPitmode)
    {
        DBGLN("VTX: PIT mode: %d", vtxSPIPitmode);
        vtxSPIPitmodeCurrent = vtxSPIPitmode;
    }

    checkOutputPower();

    return VTX_POWER_INTERVAL_MS;
}

device_t VTxSPI_device = {
    .initialize = initialize,
    .start = start,
    .event = event,
    .timeout = timeout
};

#endif
