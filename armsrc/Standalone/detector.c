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
#include "standalone.h"
#include "proxmark3_arm.h"
#include "appmain.h"
#include "fpgaloader.h"
#include "util.h"
#include "dbprint.h"
#include "cmd.h"


void ModInfo(void) {
    DbpString("  Standalone field detector (Wintrmvte)");
}

void RunMod(void) {
    StandAloneMode();
    Dbprintf("[=] Standalone field detector started");
    //FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    //int16_t stat = 0;
    //uint8_t arg = 1;
    int16_t stat = 1;
    uint8_t arg = 0;

    for (;;) {
        WDT_HIT();
        if (data_available()) break;
        int button_pressed = BUTTON_CLICKED(950);
        switch (button_pressed) {
            case BUTTON_SINGLE_CLICK:
                break;
        }
        //int reply_ng(uint16_t cmd, int16_t status, uint8_t *data, size_t len) {
        reply_ng(CMD_LISTEN_READER_FIELD, stat, (uint8_t*)&arg, sizeof(arg));
        //SendCommandNG(CMD_LISTEN_READER_FIELD, (uint8_t *)&arg, sizeof(arg));

    }

    DbpString("[=] exiting");
    LEDsoff();
}
