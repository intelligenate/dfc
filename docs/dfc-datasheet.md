# DFC Datasheet

DFC by Intelligenate

### Technical Specifications

- Built around an ARM Cortex-M4 32-bit processor with a 2.4 GHz transceiver
- 11 exposed GPIO pins in a breadboard- and IDC-compatible layout
    - 4 analog or digital pins
    - 7 digital-only pins
- 16 M-Bit QSPI external flash memory
- Two channels of bi-directional RS-232-to-TTL/CMOS conversion via MAX3222 IC
- I²C JST connector compatible with SparkFun's Qwiic or Adafruit STEMMA QT Connect System 
- SWD connections on the front and back of the board via Tag Connect footprints for firmware programming and debugging
- 20 V max to 3.3 V voltage regulator
- Two 3.5 mm stereo jacks connected to one another and to the RS-232 driver/receiver IC.
    - An off-by-default jumper allows the transmission of RS-232 data through the jacks
- Two USB Type-C connectors that allow DFC to be placed in-line with a USB cable operating at up to 20 V
    - This experimental, use-at-your-own-risk configuration allows you to branch off signals present in the USB cable—such and connect them to the processor
    - Configurable routes via jumper headers and solder pads for USB D+/- and SBU1/2