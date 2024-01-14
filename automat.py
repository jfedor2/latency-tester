#!/usr/bin/env python3

import serial
import sys
import os
import time

if len(sys.argv) < 2:
    raise Exception("required argument missing")

ser = serial.Serial("/dev/ttyACM1")

ser.write(b"ar")  # disable auto-start, enable resetting DUT via RUN pin

ret = os.system(
    "cd /home/jfedor/openocd-0.12.0; src/openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -s tcl -c 'adapter speed 5000; init; reset halt; load_image /home/jfedor/pico-examples/build/flash/nuke/flash_nuke.elf; resume 0x20000000; exit'"
)

if ret != 0:
    raise Exception("openocd failed")

time.sleep(10)  # might need more time if your board has more flash

ret = os.system(
    "cd /home/jfedor/openocd-0.12.0; src/openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -s tcl -c 'adapter speed 5000; init; reset; program {} verify reset exit'".format(
        sys.argv[1]
    )
)

if ret != 0:
    raise Exception("openocd failed")

results = {}

for config_file in sys.argv[2:] or [""]:
    time.sleep(3)
    if config_file:
        ret = os.system(
            "cd /home/jfedor/openocd-0.12.0; src/openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -s tcl -c 'adapter speed 5000; init; reset; program {} verify reset exit 0x101FC000'".format(
                config_file
            )
        )
        if ret != 0:
            raise Exception("openocd failed")
        time.sleep(3)
    ser.write(b"t")
    while True:
        line = ser.readline().decode("ascii").rstrip()
        print(line)
        if "average latency" in line:
            results[config_file] = line
            break

print(results)
