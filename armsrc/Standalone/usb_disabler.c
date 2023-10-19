//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// main code for skeleton  by Iceman
//-----------------------------------------------------------------------------
#include "standalone.h" // standalone definitions
#include "proxmark3_arm.h"
#include "appmain.h"
#include "fpgaloader.h"
#include "util.h"
#include "dbprint.h"
#include "usb_cdc.h"

void ModInfo(void) {
    DbpString(" USB-CDC internal hardware disabler (Wintrmvte)");
}

void RunMod(void) {
    int disabled = 0;
    StandAloneMode();
    Dbprintf("[=] USB Disabler started");
    for (;;) {
        WDT_HIT();
        if (data_available()) break;
        int button_pressed = BUTTON_CLICKED(950);
        switch(button_pressed){
            case BUTTON_SINGLE_CLICK:
            if (disabled){
                usb_enable();
                DbpString("Enabled USB");
                LEDsoff();
                SpinErr(LED_A, 400, 2);
                disabled = 0;
            } else {
                usb_disable();
                DbpString("Disabled USB");
                LEDsoff();
                SpinErr(LED_B, 400, 2);
                disabled = 1;
            }
            case BUTTON_HOLD:
                break;
        }
    }
    DbpString("[=] exiting");
    LEDsoff();
}
