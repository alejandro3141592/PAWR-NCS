#include "MAX30205Sensor.h"
#include <Arduino.h>

bool MAX30205Sensor::begin() {
    Wire.beginTransmission(ADDRESS);
    return (Wire.endTransmission() == 0);
}

bool MAX30205Sensor::update() {
    Wire.beginTransmission(ADDRESS);
    Wire.write(0x00);
    Wire.endTransmission(false);

    Wire.requestFrom(ADDRESS, (uint8_t)2);
    if (Wire.available() < 2) return false;

    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    int16_t raw = (int16_t)((msb << 8) | lsb);
    temperature = raw * 0.00390625f;
    return true;
}
