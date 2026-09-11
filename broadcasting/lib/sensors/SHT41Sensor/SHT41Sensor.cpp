#include "SHT41Sensor.h"
#include <Arduino.h>

bool SHT41Sensor::begin() {
    sht4x.begin(Wire, SHT40_I2C_ADDR_44);
    sht4x.softReset();
    delay(10);
    return true;
}

bool SHT41Sensor::update() {
    int16_t err = sht4x.measureHighPrecision(temperature, humidity);
    return (err == 0);
}
