/*********************************************************************
 BLE cycling power service
 
 Reads the data stream from a Peloton bike and broadcasts it as a cycling power service (for connection 
 to wearable devices) or a fitness machine (for connection to apps like Zwift).
 Adapted from Adafruit's heart rate example service for nRF52 based Bluefruit LE modules.

Copyright 2021 Intelligenate LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*********************************************************************/
#include <bluefruit.h>
#include <Arduino.h>
#include <stdint.h>


#define UUID16_SVC_FITNESS_MACHINE                      0x1826
//#define UUID16_CHR_FITNESS_MACHINE_STATUS               0x2ADA // not used
#define UUID16_CHR_FITNESS_MACHINE_FEATURE              0x2ACC
#define UUID16_CHR_INDOOR_BIKE_DATA                     0x2AD2
#define HEADER        0xF1
#define FOOTER        0xF6
#define CAD_ID        0x41
#define RES_ID        0x4A
#define POW_ID        0x44 
#define DEBUG         false
#define DEBUG_CAD     false
#define POW_CAD       false

// ── Legacy UART (non-DMA) for Peloton serial ──────────────────────
// The DFC original firmware uses legacy UART (ENABLE=4) on P0.26/P0.27.
// Arduino Serial1 forces UARTE/DMA (ENABLE=8) which doesn't work.
// These functions bypass Serial1 entirely.
#define PELOTON_RX_PIN  26  // P0.26
#define PELOTON_TX_PIN  27  // P0.27
#define PELOTON_BAUD    0x004EA000  // 19200 baud register value

void peloton_uart_init() {
  // === GPIO setup — verified from hardware analysis ===

  // P0.30 LOW — original firmware does this at boot
  NRF_P0->OUTCLR = (1UL << 30);
  NRF_P0->PIN_CNF[30] = 1;  // Output

  // RTS (P0.05) LOW — enables SP3222 RS-232 transceiver.
  // SP3222 FORCEOFF is tied to RTS: LOW = transceiver ON, HIGH = OFF.
  NRF_P0->OUTCLR = (1UL << 5);
  NRF_P0->PIN_CNF[5] = 1;  // Output

  // CTS (P0.07) LOW — not strictly needed without HWFC but keep consistent
  NRF_P0->OUTCLR = (1UL << 7);
  NRF_P0->PIN_CNF[7] = 1;  // Output

  // === UART peripheral setup ===

  // Disable any existing UART/UARTE on this peripheral
  NRF_UARTE0->ENABLE = 0;
  NRF_UART0->ENABLE = 0;

  // Configure pins — RTS/CTS disconnected (driven by GPIO above)
  NRF_UART0->PSEL.TXD = PELOTON_TX_PIN;
  NRF_UART0->PSEL.RXD = PELOTON_RX_PIN;
  NRF_UART0->PSEL.RTS = 0xFFFFFFFF;  // disconnected
  NRF_UART0->PSEL.CTS = 0xFFFFFFFF;  // disconnected

  // Configure UART: 19200 baud, no parity, no flow control
  NRF_UART0->BAUDRATE = PELOTON_BAUD;
  NRF_UART0->CONFIG = 0;

  // Enable in legacy UART mode (not UARTE/DMA)
  NRF_UART0->ENABLE = 4;

  // Clear any pending events
  NRF_UART0->EVENTS_RXDRDY = 0;
  NRF_UART0->EVENTS_ERROR = 0;
  NRF_UART0->ERRORSRC = 0xF;  // Clear all error flags

  // Start receiving
  NRF_UART0->TASKS_STARTRX = 1;
}

bool peloton_uart_available() {
  // Check for errors and recover
  if (NRF_UART0->EVENTS_ERROR) {
    NRF_UART0->EVENTS_ERROR = 0;
    NRF_UART0->ERRORSRC = NRF_UART0->ERRORSRC;
    NRF_UART0->TASKS_STARTRX = 1;  // Restart receiver after error
  }
  // Check if UART was disabled (e.g., by USB/Serial init)
  if (NRF_UART0->ENABLE != 4) {
    peloton_uart_init();  // Re-initialize
  }
  return NRF_UART0->EVENTS_RXDRDY != 0;
}

uint8_t peloton_uart_read() {
  NRF_UART0->EVENTS_RXDRDY = 0;
  return (uint8_t)NRF_UART0->RXD;
}

// Speed computation constants (indoor bike assumptions)
// 700x25c tire circumference in meters
#define WHEEL_CIRCUMFERENCE  2.105
// Typical indoor bike gear ratio (chainring/cog)
#define GEAR_RATIO           3.5

// DFC board LED on P0.28
#define DFC_LED_PIN  28

/* Service Definitions from BLEUuid.h
 * Cycling Power Service:       0x1818
 */
BLEService        powerService = BLEService(UUID16_SVC_CYCLING_POWER);
BLECharacteristic powerMeasCharacteristic = BLECharacteristic(UUID16_CHR_CYCLING_POWER_MEASUREMENT);
BLECharacteristic powerFeatureCharacteristic = BLECharacteristic(UUID16_CHR_CYCLING_POWER_FEATURE);
BLECharacteristic sensorLocation = BLECharacteristic(UUID16_CHR_SENSOR_LOCATION);

BLEService        fitnessService = BLEService(UUID16_SVC_FITNESS_MACHINE);
BLECharacteristic fitnessFeatureCharacteristic = BLECharacteristic(UUID16_CHR_FITNESS_MACHINE_FEATURE);
BLECharacteristic indoorBikeDataCharacteristic = BLECharacteristic(UUID16_CHR_INDOOR_BIKE_DATA);

BLEService        cyclingSpeedCadService = BLEService(UUID16_SVC_CYCLING_SPEED_AND_CADENCE);
BLECharacteristic cyclingSpeedCadFeature = BLECharacteristic(UUID16_CHR_CSC_FEATURE);
BLECharacteristic cyclingSpeedCadMeasurement = BLECharacteristic(UUID16_CHR_CSC_MEASUREMENT);

BLEDis bledis;    // DIS (Device Information Service) helper class instance
BLEBas blebas;    // BAS (Battery Service) helper class instance

int16_t prevPower = 100;  // Used for randomWalk
float revCount = 0;
uint16_t lastRevTime = 0;
// wheelRevs removed — CSC service not active
long lastNotify = millis();

// Struct to hold and return power and cadence measurements
struct powerAndCadence {
  int16_t power;
  int16_t cadence;
  int16_t resistance;
  bool powerValid;
  bool cadenceValid;
  bool resistanceValid;
}; 

struct powerAndCadence pAndC = {
  0,0,0,false,false,false
};

void setup()
{
  // DFC board LED setup (P0.28)
  NRF_P0->PIN_CNF[DFC_LED_PIN] = 1;  // Output
  NRF_P0->OUTSET = (1UL << DFC_LED_PIN);  // LED on

  if(DEBUG){
    Serial.begin(115200);
    for (int i = 0; i < 200 && !Serial; i++) delay(10);
    Serial.println("Initializing the Coffee Cycling Meter");
    Serial.println("-----------------------\n");
  }

  // Initialize the Bluefruit module with Peripheral = 2, Central = 0
//  Bluefruit.begin();
  Bluefruit.begin(3,0);

  Bluefruit.setName("Coffee");

  // Set the connect/disconnect callback handlers
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  // Configure and Start the Device Information Service
  bledis.setManufacturer("Intelligenate Industries");
  bledis.setModel("Model 1");
  bledis.begin();

  // Start the BLE Battery Service and set it to 100%
  blebas.begin();
  blebas.write(100);

  // Setup the Cycling Power Meter service using
  // BLEService and BLECharacteristic classes
  setupCPM();

  // Setup the fitness machine service
  setupFTMS();

  // Setup the speed and cadence service
//  setupCSC();

  // Setup the advertising packet(s)
  startAdv();
  if(DEBUG){
    Serial.println("\nAdvertising");  
  }  

  // Delay to let Bluefruit FreeRTOS tasks finish initialization
  delay(500);
  peloton_uart_init();  // Legacy UART on P0.26/P0.27 at 19200 baud
  if(DEBUG){
    Serial.print("UART0 ENABLE=");
    Serial.println(NRF_UART0->ENABLE);
    Serial.print("UART0 RXD pin=");
    Serial.println(NRF_UART0->PSEL.RXD);
    Serial.print("UART0 TXD pin=");
    Serial.println(NRF_UART0->PSEL.TXD);
  }
}

void startAdv(void)
{
  // Advertising packet
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  
  
// Create service data bytes
//Service Data AD Type            UINT8   1  BLE_GAP_AD_TYPE_SERVICE_DATA 
//Fitness Machine Service UUID    UINT16  2  UUID16_SVC_FITNESS_MACHINE
//Flags                           UINT8   1  0x01 == Fitness Machine Available
//Fitness Machine Type            UINT16  2  0x20 == bit5 == 1, Indoor bike supported

const uint8_t serviceData[5] = {highByte(UUID16_SVC_FITNESS_MACHINE),lowByte(UUID16_SVC_FITNESS_MACHINE),0x01,0x00,0x20};
//const uint8_t serviceData[5] = {0x20,0x00,0x01,highByte(UUID16_SVC_FITNESS_MACHINE),lowByte(UUID16_SVC_FITNESS_MACHINE)};
//const uint8_t serviceData[5] = {lowByte(UUID16_SVC_FITNESS_MACHINE),highByte(UUID16_SVC_FITNESS_MACHINE),0x01,0x20,0x00}; // doesn't work
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, &serviceData, 5); // sizeof(fitnessMachineType) //Indoor bike supported
  
  Bluefruit.Advertising.addTxPower();

  
  Bluefruit.Advertising.addService(fitnessService);
//  Bluefruit.Advertising.addService(cyclingSpeedCadService);
  Bluefruit.Advertising.addService(powerService);

  // Include Name
  Bluefruit.Advertising.addName();
  
  /* Start Advertising
   * - Enable auto advertising if disconnected
   * - Interval:  fast mode = 20 ms, slow mode = 152.5 ms
   * - Timeout for fast mode is 30 seconds
   * - Start(timeout) with timeout = 0 will advertise forever (until connected)
   * 
   * For recommended advertising interval
   * https://developer.apple.com/library/content/qa/qa1931/_index.html   
   */
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);    // in unit of 0.625 ms
  Bluefruit.Advertising.setFastTimeout(30);      // number of seconds in fast mode
  Bluefruit.Advertising.start(0);                // 0 = Don't stop advertising after n seconds  
}

void setupCPM(void)
{
  // Configure the Cycling Power service
  powerService.begin();

  // Note: You must call .begin() on the BLEService before calling .begin() on
  // any characteristic(s) within that service definition.. Calling .begin() on
  // a BLECharacteristic will cause it to be added to the last BLEService that
  // was 'begin()'ed!

  // Configure the Cycling Power Measurement characteristic
  // Properties = Notify
  // Min Len    = 1
  // Max Len    = 8
  //    B0:1    = UINT16  - Flag (MANDATORY)
  //       b5   = Crank Revolution Data Present
  //    B2:3      = SINT16  - 16-bit Instantaneous Power
  powerMeasCharacteristic.setProperties(CHR_PROPS_NOTIFY);
  powerMeasCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  if(POW_CAD){
    powerMeasCharacteristic.setFixedLen(8);  // for power and cadence
  } else{
    powerMeasCharacteristic.setFixedLen(4);  // for power only
  }
  powerMeasCharacteristic.setCccdWriteCallback(cccd_callback);  // Optionally capture CCCD updates
  powerMeasCharacteristic.begin();


//    uint16_t powerData[4]; // for power and cadence
//    powerData[0] = 32; // Set the flags, 00100000 - 32  - 0x020 - Crank Revolution Data Present

  uint16_t powerData[2]; // for power only
  powerData[0] = 0; // Set the flags, power only

  powerData[1] = 0; // Power
  powerData[2] = 0; // Crank revs
  powerData[3] = 0; // time
  powerMeasCharacteristic.write(powerData, sizeof(powerData));  //data,length

  // Configure the cycling power feature characteristic
  powerFeatureCharacteristic.setProperties(CHR_PROPS_READ);
  powerFeatureCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  powerFeatureCharacteristic.setFixedLen(4);
  powerFeatureCharacteristic.begin();
  
//  uint8_t featureFlag[4] = {0,0,0,8}; // for power and cadence
  
  uint8_t featureFlag[4] = {0,0,0,0};  // works for just power
  
  powerFeatureCharacteristic.write(featureFlag,4);  //(data,length) data is 32bit flag with bit 3 on "index="3" size="1" name="Crank Revolution Data Supported""

  // Configure the Sensor Location characteristic
  // Properties = Read
  // Min Len    = 1
  // Max Len    = 1
  //    B0      = UINT8 - Sensor Location
  //      9     = Front Hub location - doesn't really matter
  sensorLocation.setProperties(CHR_PROPS_READ);
  sensorLocation.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  sensorLocation.setFixedLen(1);
  sensorLocation.begin();
  sensorLocation.write8(9);    // Set the characteristic to 'Front Hub' (9)

  
}

/**
 * <Field name="CSC Feature">
            <Requirement>Mandatory</Requirement>
            <Format>16bit</Format>
            <BitField>
                <Bit index="0" size="1" name="Wheel Revolution Data Supported">
                    <Enumerations>
                        <Enumeration key="0" value="False" />
                        <Enumeration key="1" value="True" />
                    </Enumerations>
                </Bit>
                <Bit index="1" size="1" name="Crank Revolution Data Supported">
                    <Enumerations>
                        <Enumeration key="0" value="False" />
                        <Enumeration key="1" value="True" />
                    </Enumerations>
                </Bit>
                <Bit index="2" size="1" name="Multiple Sensor Locations Supported">
                    <Enumerations>
                        <Enumeration key="0" value="False" />
                        <Enumeration key="1" value="True" />
                    </Enumerations>
                </Bit>
                <ReservedForFutureUse index="3" size="13" />
            </BitField>
        </Field>
 */
void setupCSC(void)
{
  // Configure the Cycling Power service
  cyclingSpeedCadService.begin();

  // Note: You must call .begin() on the BLEService before calling .begin() on
  // any characteristic(s) within that service definition.. Calling .begin() on
  // a BLECharacteristic will cause it to be added to the last BLEService that
  // was 'begin()'ed!

  // Configure the Cycling Speed and Cadence Measurement characteristic
  cyclingSpeedCadMeasurement.setProperties(CHR_PROPS_NOTIFY);
  cyclingSpeedCadMeasurement.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  cyclingSpeedCadMeasurement.setFixedLen(7);  // for speed only
  cyclingSpeedCadMeasurement.setCccdWriteCallback(cccd_callback);  // Optionally capture CCCD updates
  cyclingSpeedCadMeasurement.begin();

  uint8_t speedData[7]; // for speed only
  speedData[0] = 0x01; // Set the flags, speed only
  speedData[1] = 0; // Wheel Revolutions
  speedData[2] = 0; // Wheel Revolutions
  speedData[3] = 0; // Wheel Revolutions
  speedData[4] = 0; // Wheel Revolutions
  speedData[5] = 0; // Last Wheel Event Time
  speedData[6] = 0; // Last Wheel Event Time
  cyclingSpeedCadMeasurement.write(speedData, sizeof(speedData));  //data,length

  // Configure the cycling power feature characteristic
  cyclingSpeedCadFeature.setProperties(CHR_PROPS_READ);
  cyclingSpeedCadFeature.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  cyclingSpeedCadFeature.setFixedLen(2);
  cyclingSpeedCadFeature.begin();
  
  uint8_t featureFlag[2] = {0,0x01};  
  
  cyclingSpeedCadFeature.write(featureFlag,2);  //(data,length) data is 16bit flag with bit 1 on name="Wheel Revolution Data Supported"
}

/**
   * Fitness machine flags (16 bits):
   * Instantaneous Speed (bit 0 = 0)
   * Instantaneous Cadence (bit 2)
   * Resistance Level Present (bit 5)
   * Instantaneous Power Present (bit 6)
   * 
   * Fitness Machine Features Field (32 bits)
   * Cadence Supported (bit 1)
   * Resistance Level Supported (bit 7)
   * Power Measurement Supported (bit 14)
   * 
   * Fitness Machine Type Field (16 bits)
   * Indoor Bike Supported (bit 5)
   * 
   
 */
void setupFTMS(void){
   fitnessService.begin();

     // Configure the indoor bike data characteristic
  // See: TODO
  // Properties = Notify
  // Min Len    = 1
  // Max Len    = 8
  //    B0:1    = UINT16  - Flag (MANDATORY)
  //       b5   = Crank Revolution Data Present
  //    B2:3      = SINT16  - 16-bit Instantaneous Power
  indoorBikeDataCharacteristic.setProperties(CHR_PROPS_NOTIFY);
  indoorBikeDataCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  indoorBikeDataCharacteristic.setFixedLen(10);  // 5 x uint16_t = 10 bytes
  indoorBikeDataCharacteristic.setCccdWriteCallback(cccd_callback);  // Optionally capture CCCD updates
  indoorBikeDataCharacteristic.begin();

  // Indoor Bike Data: flags(2) + speed(2) + cadence(2) + resistance(2) + power(2) = 10 bytes
  uint16_t indoorBikeData[5];
  indoorBikeData[0] = 0x64; // flags: instantaneous speed (bit 0=0), cadence (bit 2), resistance (bit 5), power (bit 6)
  indoorBikeData[1] = 0; // Speed, uint16, km/h with resolution of 0.01
  indoorBikeData[2] = 0; // Cadence, uint16, 1/min with resolution of 0.5
  indoorBikeData[3] = 0; // Resistance, sint16, unitless
  indoorBikeData[4] = 0; // Power, sint16, Watts
  indoorBikeDataCharacteristic.write(indoorBikeData, sizeof(indoorBikeData));  //data,length


  // Configure the fitness feature characteristic
  // From 4.3.1 Characteristic Behavior
  // fitness machine features field is 32 bits and are set below
  // target settings field is 32 bits but all are set to 0
  fitnessFeatureCharacteristic.setProperties(CHR_PROPS_READ);
  fitnessFeatureCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  fitnessFeatureCharacteristic.setFixedLen(8);  // FTMS spec: 4-byte Machine Features + 4-byte Target Setting Features
  fitnessFeatureCharacteristic.begin();
  // Fitness Machine Features (bits): 1=Cadence, 7=Resistance Level, 14=Power Measurement
  // Target Setting Features: none supported (all zeros)
  uint8_t featureFlag[8];
  featureFlag[0] = 0x82;  // bits 1 (Cadence) + 7 (Resistance Level)
  featureFlag[1] = 0x40;  // bit 14 (Power Measurement)
  featureFlag[2] = 0x00;
  featureFlag[3] = 0x00;
  featureFlag[4] = 0x00;  // Target Setting Features (none)
  featureFlag[5] = 0x00;
  featureFlag[6] = 0x00;
  featureFlag[7] = 0x00;
  fitnessFeatureCharacteristic.write(featureFlag, sizeof(featureFlag));
}


void connect_callback(uint16_t conn_handle)
{
  // Get the reference to current connection
  BLEConnection* connection = Bluefruit.Connection(conn_handle);

  char central_name[32] = { 0 };
  connection->getPeerName(central_name, sizeof(central_name));

  if(DEBUG){
    Serial.print("Connected to ");
    Serial.println(central_name);  
  }
}

/**
 * Callback invoked when a connection is dropped
 * @param conn_handle connection where this event happens
 * @param reason is a BLE_HCI_STATUS_CODE which can be found in ble_hci.h
 */
void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
  (void) conn_handle;
  (void) reason;

  if(DEBUG){
    Serial.print("Disconnected, reason = 0x"); Serial.println(reason, HEX);
    Serial.println("Advertising!");  
  }
}


void cccd_callback(uint16_t conn_hdl, BLECharacteristic* chr, uint16_t cccd_value)
{
    if(DEBUG){
      // Display the raw request packet
      Serial.print("CCCD Updated: ");
      //Serial.printBuffer(request->data, request->len);
      Serial.print(cccd_value);
      Serial.println("");
    }

    // Check the characteristic this CCCD update is associated with in case
    // this handler is used for multiple CCCD records.
    if (chr->uuid == powerMeasCharacteristic.uuid) {
        if (chr->notifyEnabled(conn_hdl)) {
          if(DEBUG){
            Serial.println("Cycling Power Measurement 'Notify' enabled");
          }
        } else {
          if(DEBUG){
            Serial.println("Cycling Power Measurement 'Notify' disabled");
          }
        }
    }

    if (chr->uuid == fitnessFeatureCharacteristic.uuid) {
        if (chr->notifyEnabled(conn_hdl)) {
          if(DEBUG){
            Serial.println("Cycling Power Measurement 'Notify' enabled");
          }
        } else {
          if(DEBUG){
            Serial.println("Cycling Power Measurement 'Notify' disabled");
          }
        }
    }
}

/**
 * Used to generate test values when not connected to a measurement source
 */
int16_t randomWalk(){
  if(random(0,2) == 1){
    prevPower = prevPower + 1;
  } else {
    prevPower = prevPower - 1;
  }
  if (prevPower < 0){
    prevPower = 0;
  }
  if(prevPower > 400){
    prevPower = 400;
  }
  return prevPower;
}

/**
 * Map the power level to a power zone and put out a different color for each zone.
 */
 void powerToZoneColor(){
  // NeoPixel removed — DFC board has no NeoPixel
  // Toggle DFC LED (P0.28) instead
  NRF_P0->OUT ^= (1UL << DFC_LED_PIN);
 }

struct powerAndCadence readPower(){
  if ( peloton_uart_available())// >= MESSAGE_BYTES)
  {
    if( peloton_uart_read() == HEADER)
    {
      int devID = peloton_uart_read();
      int measDigits = peloton_uart_read(); // not using this for now
      if(devID == CAD_ID)
      {
        char b3 = peloton_uart_read();
        int i3 = b3 - '0';
        char b2 = peloton_uart_read();
        int i2 = b2 - '0';
        char b1 = peloton_uart_read();
        int i1 = b1 - '0';
        int val1 = 100*i1 + 10*i2 + i3;
        byte checkSum = peloton_uart_read();
        byte sum = b3 + b2 + b1 + measDigits + devID + HEADER;
        if(sum != checkSum) {
//          pAndC.cadence = -1;  // measurement is invalid
          pAndC.cadenceValid = false;  // measurement is invalid
        } else{
          pAndC.cadence = val1;
          pAndC.cadenceValid = true; 
        }

        if(DEBUG){
//        Serial.print("b3 = ");
//        Serial.print(b3);
//        Serial.print(", b2 = ");
//        Serial.print(b2);
//        Serial.print(", b1 = ");
//        Serial.println(b1);

          Serial.print("Cadence sum: ");
          Serial.print(sum,HEX);
          Serial.print(", checksum: ");
          Serial.println(checkSum,HEX);
          Serial.print("Cadence: ");
          Serial.println(pAndC.cadence);
        }
        
        int footer = peloton_uart_read();

        if(footer != FOOTER){
//          pAndC.cadence = -1;  // measurement is invalid
          pAndC.cadenceValid = false;  // measurement is invalid
        }
      }
      else if(devID == POW_ID){
        char b5 = peloton_uart_read();
        int i5 = b5 - '0';
        char b4 = peloton_uart_read();
        int i4 = b4 - '0';
        char b3 = peloton_uart_read();
        int i3 = b3 - '0';
        char b2 = peloton_uart_read();
        int i2 = b2 - '0';
        char b1 = peloton_uart_read();
        int i1 = b1 - '0';
        float val = 1000*i1 + 100*i2 + 10*i3 + i4 + 0.1*i5;  // Use a float because the last value is a tenth for some reason
        byte checkSum = peloton_uart_read();
        byte sum = b5 + b4 + b3 + b2 + b1 + measDigits + devID + HEADER;
        if(sum != checkSum) {
//          pAndC.power = -1;  // measurement is invalid
          pAndC.powerValid = false;  // measurement is invalid
          // Power checksum fail — no visual indicator
        } else{
          pAndC.power = (int16_t) val;  // Convert it to an int
          pAndC.powerValid = true;
          powerToZoneColor();
        }

        if(DEBUG){
//        Serial.print("b5 = ");
//        Serial.print(b5);
//        Serial.print(", b4 = ");
//        Serial.print(b4);
//        Serial.print(", b3 = ");
//        Serial.print(b3);
//        Serial.print(", b2 = ");
//        Serial.print(b2);
//        Serial.print(", b1 = ");
//        Serial.println(b1);

          Serial.print("Power sum: ");
          Serial.print(sum,HEX);
          Serial.print(", checksum: ");
          Serial.print(checkSum,HEX);
          Serial.print(" Power: ");
          Serial.println(pAndC.power);
        }        
        
        int footer = peloton_uart_read();
        
        if(footer != FOOTER){
          pAndC.powerValid = false;
//          pAndC.power = -1;  // measurement is invalid
        }
      }
      else if(devID == RES_ID){ //Read the resistance
        char b4 = peloton_uart_read();
        int i4 = b4 - '0';
        char b3 = peloton_uart_read();
        int i3 = b3 - '0';
        char b2 = peloton_uart_read();
        int i2 = b2 - '0';
        char b1 = peloton_uart_read();
        int i1 = b1 - '0';
        int val1 = 1000*i1 + 100*i2 + 10*i3 + i4;
        byte checkSum = peloton_uart_read();
        byte sum = b4 + b3 + b2 + b1 + measDigits + devID + HEADER;
        
        if(DEBUG){
//        Serial.print("b4 = ");
//        Serial.print(b4);
//        Serial.print(", b3 = ");
//        Serial.print(b3);
//        Serial.print(", b2 = ");
//        Serial.print(b2);
//        Serial.print(", b1 = ");
//        Serial.println(b1);

          Serial.print("Resistance: ");
          Serial.println(val1);
          Serial.print("Resistance sum: ");
          Serial.print(sum,HEX);
          Serial.print(", checksum: ");
          Serial.println(checkSum,HEX);
        }

        if(sum != checkSum) {
          pAndC.resistanceValid = false;  // measurement is invalid
        } else{
          pAndC.resistance = val1;
          pAndC.resistanceValid = true;
        }

        int footer = peloton_uart_read();

        if(footer != FOOTER){
          pAndC.resistanceValid = false;  // measurement is invalid
        }
      }
      else
      {
        if(DEBUG){
          Serial.print("Got message with unknown device ID ");
          Serial.println(devID);  
        }
        // TODO - this may cause an infinite loop. Log when this happens and capture the bytes.
      }

      return pAndC;
    }
    else{
      // Non-header byte — skip
      delay(random(10,30));  // jitter the delay to prevent locks
      return pAndC;
    }
  }
  else{
    if(DEBUG){
      Serial.println("UART: no data");
    }
    // No UART data available
    delay(random(90,110));  // jitter the delay to prevent locks
    return pAndC;
  }
}

void loop()
{
  NRF_P0->OUT ^= (1UL << DFC_LED_PIN);  // Toggle DFC LED

  // Debug: check if UART config was overwritten
  if(DEBUG && NRF_UART0->ENABLE != 4) {
    Serial.print("!!! UART ENABLE changed to: ");
    Serial.println(NRF_UART0->ENABLE);
    peloton_uart_init();  // Re-initialize
  }

  if ( Bluefruit.connected() ) {
    
//    int16_t power = (int16_t) randomWalk();  // for testing

    pAndC = readPower();
    if((pAndC.powerValid != true) && (pAndC.cadenceValid != true)){
      if(DEBUG){
        Serial.print("***** Invalid measurement. Power valid: ");
        Serial.print(pAndC.powerValid);
        Serial.print(" Cadence valid: ");
        Serial.println(pAndC.cadenceValid);
      }
      return;
    }


    if(DEBUG_CAD){
      Serial.print("lastRevTime: ");
      Serial.print(lastRevTime);
    }
    //TODO don't need to do this for fitness machine service, remove or make toggle to reduce computation
    unsigned long now = millis();
    unsigned long time1024 = floor(now*1e3/1024);
    unsigned long revTime = time1024 % 65536;  // roll over every 64 seconds
    uint16_t intRevTime = int(revTime);
    uint16_t revTimeDiff = intRevTime - lastRevTime;
    revCount = (pAndC.cadence * revTimeDiff)/60000.0 + revCount;
    uint16_t intRevCount = int(revCount);
    lastRevTime = intRevTime;

    if(DEBUG_CAD){
      Serial.print(" RevTimeDiff: ");
      Serial.print(revTimeDiff);
      Serial.print(" intRevTime: ");
      Serial.print(intRevTime);
      Serial.print(" revCount: ");
      Serial.print(revCount);
      Serial.print(" revCount int: ");
      Serial.println(intRevCount); 
    }
    

//    uint16_t powerData[4] = {0b00100000, power, revCount, intRevTime};  // flags, power, crank revs, time
    uint16_t powerData[4];
    if(POW_CAD){
      powerData[0] = 32; // flags = power and cadence  
    } else {
      powerData[0] = 0; // flags = power
    }
    powerData[1] = pAndC.power;
    powerData[2] = intRevCount;
    powerData[3] = intRevTime;
    powerData[4] = 0;
    powerData[5] = 0;
    powerData[6] = 0;
    powerData[7] = 0;

    // Compute speed from cadence: speed (km/h) = cadence * wheel_circ * gear_ratio * 60 / 1000
    float speed_kmh = pAndC.cadence * WHEEL_CIRCUMFERENCE * GEAR_RATIO * 60.0 / 1000.0;
    uint16_t speed_ftms = (uint16_t)(speed_kmh * 100);  // FTMS unit: km/h with 0.01 resolution

    uint16_t fitnessData[5];
    fitnessData[0] = 0x64; // flags: speed (bit 0=0), cadence (bit 2), resistance (bit 5), power (bit 6)
    fitnessData[1] = speed_ftms;
    fitnessData[2] = pAndC.cadence << 1;  // FTMS cadence unit: 0.5 resolution, so RPM * 2
    fitnessData[3] = pAndC.resistance;
    fitnessData[4] = pAndC.power;

   
    // Use .notify instead of .write
    // If it is connected but CCCD is not enabled
    // The characteristic's value is still updated although notification is not sent
    long timeToUpdate = millis() - lastNotify;
    if(timeToUpdate > 500){  // wait at least 0.5 seconds to update
       if(powerMeasCharacteristic.notify(powerData, sizeof(powerData))){
         lastNotify = millis();
         if(DEBUG){
           Serial.print("##### Power Measurement updated to: "); 
           Serial.print(pAndC.power); 
           Serial.print(", Cadence: "); 
           Serial.print(pAndC.cadence); 
           Serial.print(", Resistance: "); 
           Serial.println(pAndC.resistance); 
         }
        
       }else{
         if(DEBUG){
           Serial.println("Power ERROR: Notify not set in the CCCD or not connected!");        
         }
       }
     if(indoorBikeDataCharacteristic.notify(fitnessData, sizeof(fitnessData))){
       lastNotify = millis();
       if(DEBUG){
         Serial.print("FTMS Measurements updated to: "); 
         Serial.print(pAndC.power); 
         Serial.print(", Cadence: "); 
         Serial.print(pAndC.cadence); 
         Serial.print(", Resistance: "); 
         Serial.println(pAndC.resistance); 
       }
       
     }else{
       if(DEBUG){
         Serial.println("FTMS ERROR: Notify not set in the CCCD or not connected!");        
       }
     }
     // CSC notification removed — setupCSC() is not called, service not active
    } else {
      if(DEBUG){
        Serial.print("timeToUpdate ");
        Serial.println(timeToUpdate);
      }
    }
  }
}
