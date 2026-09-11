#ifndef SHT41_SENSOR_H
#define SHT41_SENSOR_H

#include <Wire.h>
#include <SensirionI2cSht4x.h>

class SHT41Sensor {
public:
    bool  begin();
    bool  update();
    float getTemperature() const { return temperature; }
    float getHumidity()    const { return humidity; }

private:
    SensirionI2cSht4x sht4x;
    float temperature = NAN;
    float humidity    = NAN;
};

#endif
