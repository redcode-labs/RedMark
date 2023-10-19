#!/usr/bin/env python3

import pm3
from output_grabber import OutputGrabber

out = OutputGrabber()
p=pm3.pm3("/dev/ttyACM0")
print("Device:", p.name)
with out:
    p.console("hw status")
for line in out.capturedtext.split('\n'):
    if "Unique ID" in line:
        print(line)
