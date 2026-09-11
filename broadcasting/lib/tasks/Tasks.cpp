#include "Tasks.h"
#include "SharedState.h"
#include "BleManager.h"
#include "SensorManager/SensorManager.h"
#include "Config.h"
#include "DataTypes.h"
#include "CRC.h"

extern SensorManager sensors;

// ======================================================
// PRODUCER: SENSOR TASK
// ======================================================
void sensorTask(void *pvParameters) {
    SensorData localData;

    while (true) {
        sensors.update();
        sensors.print();

        localData.bodyTemp  = sensors.getBodyTemperature();
        localData.humidity  = sensors.getHumidity();
        localData.timestamp = millis();

        if (xQueueSend(sensorDataQueue, &localData, pdMS_TO_TICKS(10)) != pdPASS) {
            DEBUG_PRINTLN("[WARN] Queue full, data dropped.");
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
    }
}

// ======================================================
// CONSUMER: BLE TASK
// ======================================================
static uint16_t s_seq = 0;

void communicationTask(void *pvParameters) {
    SensorData receivedData;

    while (true) {
        if (xQueueReceive(sensorDataQueue, &receivedData, portMAX_DELAY) != pdPASS) continue;

        uint8_t flags = 0;
        if (isnan(receivedData.bodyTemp) || isnan(receivedData.humidity)) flags |= FLAG_SENSOR_FAIL;

        updateAdvertising(receivedData.bodyTemp,
                          receivedData.humidity,
                          receivedData.timestamp,
                          s_seq,
                          flags);
        s_seq++;

        // Brief LED pulse to signal broadcast
        digitalWrite(LED_GREEN, LOW);
        vTaskDelay(pdMS_TO_TICKS(50));
        digitalWrite(LED_GREEN, HIGH);
    }
}
