#ifndef MAX30205_SENSOR_H
#define MAX30205_SENSOR_H

#include <Arduino.h>
#include <Wire.h>

class MAX30205Sensor {
public:
    bool  begin();
    bool  update();
    float getTemperature() const { return temperature; }

private:
    static constexpr uint8_t ADDRESS = 0x48;
    float temperature = NAN;
};

#endif
