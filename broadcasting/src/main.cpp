#include <Arduino.h>
#include <Wire.h>



extern "C" {
    #include "FreeRTOS.h"
    #include "task.h"
    #include "semphr.h"
}

#include "SensorManager/SensorManager.h"
#include "Datatypes.h"
#include "Config.h"
#include "SharedState.h"

#include "Tasks.h"

SensorManager sensors;

// ======================================================
// SETUP
// ======================================================

void setup() {

    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    digitalWrite(LED_GREEN, HIGH);  // HIGH = off (active low)



    Serial.begin(115200);

    Wire.begin();

    delay(2000);

    delay(500);




    DEBUG_PRINTLN("Booting...");

    DEBUG_PRINTLN("Before sensor begin");


    if (!sensors.begin()) {

        DEBUG_PRINTLN("Sensor init failed");

        while (1);
    }

    DEBUG_PRINTLN("After sensor begin");




    setupBLE();

sensorDataQueue = xQueueCreate(5, sizeof(SensorData));


    if (sensorDataQueue == NULL) {
        DEBUG_PRINTLN("Failed to create queue! Halting.");
        while(1);
    }

    // 2. Create Tasks
    xTaskCreate(sensorTask,        "SensorTask", 768,  NULL, 2, NULL);
    xTaskCreate(communicationTask, "CommTask",   1024, NULL, 1, NULL);
    DEBUG_PRINTLN("Setup complete");

            for(int i=0; i<10; i++)
    {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(200);
    }
}

// ======================================================
// LOOP
// ======================================================

void loop() {

    // Keep loop empty for RTOS architecture

    vTaskDelay(pdMS_TO_TICKS(1000));
}