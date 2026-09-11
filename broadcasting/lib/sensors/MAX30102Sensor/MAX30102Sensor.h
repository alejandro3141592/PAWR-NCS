#ifndef MAX30102_SENSOR_H
#define MAX30102_SENSOR_H

#include <Wire.h>
#include <MAX30105.h>

class MAX30102Sensor {
public:
    bool begin();
    void update();

    float getHeartRate() const;
    float getSpO2()      const;

private:
    MAX30105 particleSensor;

    static const int BUFFER_SIZE = 100;

    uint32_t irBuffer[BUFFER_SIZE];
    uint32_t redBuffer[BUFFER_SIZE];

    int bufferIndex = 0;
    bool bufferFilled = false;

    float heartRate = 0;
    float spo2 = 0;
};

#endif