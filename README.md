# Latency tester

Wire pin 2 on the tester Pico to a button pin on the device under test.

Connect RX pin on a USB-UART adapter to pin 0 on the tester Pico. Connect GND and 5V between the USB-UART adapter and the tester Pico. Connect the USB-UART adapter to a computer and set the baud rate to 921600. 

Connect the device under test to the tester Pico using an OTG cable.

The test starts automatically after the device is plugged in and lasts 30 seconds. The numbers on each line are:

* time within USB frame at which button state was toggled
* time at which the input report resulting from that button state change arrived, relative to start of next USB frame

## How to compile
```
git clone https://github.com/jfedor2/latency-tester.git
cd latency-tester
git submodule update --init
mkdir build
cd build
cmake ..
make
```
