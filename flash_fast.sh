#!/bin/bash
sudo ./pm3-flash-fullimage
sudo ./pm3 --flush -w --debug 0 -c "coffee; hw version; hint --off; s -t 2000; hw status; hw ping; hw led; x r finder" -i

