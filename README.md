# Latency tester

## Native version

Wire GPIO2 on the tester Pico to a button pin on the device under test.

Connect RX pin on a USB-UART adapter to GPIO0 on the tester Pico. Connect GND and 5V between the USB-UART adapter and the tester Pico. Connect the USB-UART adapter to a computer and set the baud rate to 921600. 

Connect the device under test to the tester Pico using an OTG cable.

## PIO version

Wire GPIO2 on the tester Pico to a button pin on the device under test.

Wire a female USB Type A port to the tester Pico as follows:

| Pico | USB port |
| ---- | -------- |
| VBUS | VBUS |
| GND | GND |
| GPIO0 | D+ |
| GPIO1 | D- |

Connect the device under test to this port.

The tester Pico will be visible as a virtual serial port on the computer.

## How to use

The test starts automatically after the device is plugged in and takes around 45 seconds. The LED on the tester Pico will be on during the test. The numbers on each printed out line are:

* time within USB frame at which button state was toggled
* time at which the input report resulting from that button state change arrived, relative to start of next USB frame

They can be used to make a scatter plot.

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
