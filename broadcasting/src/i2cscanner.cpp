// #include <Arduino.h>
// #include <Wire.h>

// void setup() {
//     Serial.begin(115200);
//     delay(2000);

//     Wire.begin();
//     Wire.setClock(400000);  // optional: fast mode

//     Serial.println("I2C Scanner starting...");
// }

// void loop() {
//     byte error, address;
//     int devices = 0;

//     Serial.println("Scanning...");

//     for (address = 1; address < 127; address++) {
//         Wire.beginTransmission(address);

//         error = Wire.endTransmission();

//         if (error == 0) {
//             Serial.print("Found device at 0x");
//             if (address < 16) Serial.print("0");
//             Serial.println(address, HEX);
//             devices++;
//         } 
//         else if (error == 4) {
//             Serial.print("Unknown error at 0x");
//             Serial.println(address, HEX);
//         }
//     }

//     if (devices == 0) {
//         Serial.println("No I2C devices found\n");
//     } else {
//         Serial.println("Done\n");
//     }

//     delay(3000);
// }