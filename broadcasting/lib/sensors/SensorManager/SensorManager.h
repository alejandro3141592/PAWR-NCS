#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "MAX30205Sensor/MAX30205Sensor.h"
#include "SHT41Sensor/SHT41Sensor.h"
#include "Config.h"
#include "DataTypes.h"

class SensorManager {
public:
    bool begin();
    void update();
    void print();

    float getBodyTemperature()    const;
    float getAmbientTemperature() const;
    float getHumidity()           const;

private:
    MAX30205Sensor max30205;
    SHT41Sensor    sht41;

    bool max30205Ok = false;
    bool sht41Ok    = false;
};

#endif
