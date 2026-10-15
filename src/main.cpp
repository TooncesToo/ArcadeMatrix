#include <Arduino.h>
#include "core/AppRuntime.h"
#include "hal/BoardProfile.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#if !defined(PIO_UNIT_TESTING) && !defined(UNIT_TEST)
extern volatile uint32_t g_stallProbeBeat;
void startStallProbe();

void setup() {
#if defined(HARDWARE_PROFILE_ESP32_DEV)
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
    BoardProfile::current().applyPowerQuirks();
    app.initialize();
    startStallProbe();
}

void loop() {
    g_stallProbeBeat++;
    app.update();
}
#endif

