//-----------------------------------------------------------------------------
// Copyright (C) Daniel Karling, 2021
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
// main code for Multi Loader
//-----------------------------------------------------------------------------
#include "standalone.h" // standalone definitions
#include "proxmark3_arm.h"
#include "appmain.h"
#include "fpgaloader.h"
#include "ticks.h"
#include "util.h"
#include "dbprint.h"

#define NUMRUN 1 
#define SPECIAL_START 10
//#define NO_DEF_BREAK 
/*
 * This mode will allow you to access multiple different standalone
 * modes at the same time. The below section defines which modes are
 * available. Modes are added to the mode list according to the
 * following:
 *
 *  1) Include "dankarmulti.h"
 *  2) Define MODE_NAME and MODE_FILE for a mode (MODE_NAME can
 *     of your choosing, but must be a valid C token. I.e. pretend
 *     that you are naming a variable)
 *  3) Include "dankarmulti.h" again
 *  4) Repeat steps 2 and 3 for additional modes.
 *  5) Use the macros START_MODE_LIST, ADD_MODE and END_MODE_LIST
 *     to create the list of modes. You need to use the same names
 *     here as defined earlier.
 *
 *  How this works:
 *  The basic idea is to simply include several c-files with additional
 *  modes into this file. Hopefully the only collision of symbols are
 *  the RunMod and ModInfo, and these are solved by dankarmulti.h.
 *
 *  First, dankarmulti.h is included once by itself to define some macros
 *  used later.
 *
 *  For each mode to be included we define MODE_NAME which is a unique
 *  name to give the mode and MODE_FILE which is the name of the C-file
 *  for the mode. dankarmulti.h will make sure that RunMod and
 *  ModInfo is renamed to RunMod_NAME where name is what we defined. It
 *  will also include the actual mode source code and create a struct
 *  with function pointer to the run and info functions of the mode.
 *  At the end of dankarmulti.h it does #undef on MODE_NAME and MODE_FILE
 *  so that they can be redefined for the next mode to include.
 *
 *  To create a list of the modes we now have available, it is necessary
 *  to use the START_MODE_LIST, ADD_MODE and END_MODE_LIST macros. This
 *  could also have been done with some linker magic and a new section,
 *  or by some other dirty hack. But this works for now.
 *
 *  Usage:
 *  Single press to cycle between the modes.
 *  The LEDs will show the currently selected mode.
 *  Hold button to start the selected mode.
 *
 *  How many modes can you have at the same time? Depends on memory,
 *  but the LEDs will overflow after 15.
 *
 *  I don't know if this works with all the different standalone modes.
 *  I would imagine that it doesn't. If two modes are included with
 *  functions that collide (name-wise) there will be issues.
 *
 *  NOTE: You will have to keep track of if the included modes require
 *  external memory or bluetooth yourself. The mode selection in
 *  the Makefiles is not able to do it.
 */


/*******************
 * Begin mode list *
 *******************/


#include "dankarmulti.h"
#define MODE_NAME mattyrun
#define MODE_FILE "hf_mattyrun.c"
#include "dankarmulti.h"
#define MODE_NAME mfcsim
#define MODE_FILE "hf_mfcsim.c"
#include "dankarmulti.h"
//#define MODE_NAME ikea_rothult
//#define MODE_FILE "hf_tcprst.c"
//#include "dankarmulti.h"
#define MODE_NAME EM_toolkit
#define MODE_FILE "lf_em4100rsww.c"
#include "dankarmulti.h"
#define MODE_NAME legic
#define MODE_FILE "hf_legic.c"
#include "dankarmulti.h"
#define MODE_NAME visa
#define MODE_FILE "hf_msdsal.c"
#include "dankarmulti.h"
#define MODE_NAME proxIIbrute
#define MODE_FILE "lf_proxbrute.c"
#include "dankarmulti.h"
#define MODE_NAME icehid
#define MODE_FILE "lf_icehid.c"
#include "dankarmulti.h"
#define MODE_NAME usb_disabler
#define MODE_FILE "usb_disabler.c"
#include "dankarmulti.h"
#define MODE_NAME field_detector
#define MODE_FILE "detector.c"
#include "dankarmulti.h"
#define MODE_NAME mudford
#define MODE_FILE "hf_tmudford.c"
#include "dankarmulti.h"
#define MODE_NAME uid_stealer
#define MODE_FILE "hf_craftbyte.c"
#include "dankarmulti.h"
#define MODE_NAME young
#define MODE_FILE "hf_young.c"
#include "dankarmulti.h"
#define MODE_NAME bog 
#define MODE_FILE "hf_bog.c"
#include "dankarmulti.h"
#define MODE_NAME iceclass 
#define MODE_FILE "hf_iceclass.c"
#include "dankarmulti.h"
#define MODE_NAME legicsim 
#define MODE_FILE "hf_legicsim.c"
#include "dankarmulti.h"
#define MODE_NAME hf14asniffer
#define MODE_FILE "hf_14asniff.c"
#include "dankarmulti.h"
#define MODE_NAME lfemul
#define MODE_FILE "lf_em4100emul.c"
#include "dankarmulti.h"
#define MODE_NAME tharexde
#define MODE_FILE "lf_tharexde.c"
#include "dankarmulti.h"
//#define MODE_NAME aveful
//#define MODE_FILE "hf_aveful.c"
//#include "dankarmulti.h"

#ifdef STATE_READ
#undef STATE_READ
#endif
START_MODE_LIST
ADD_MODE(mattyrun)
ADD_MODE(icehid)
ADD_MODE(mfcsim)
//ADD_MODE(ikea_rothult)
ADD_MODE(visa)
ADD_MODE(EM_toolkit)
ADD_MODE(legic)
ADD_MODE(proxIIbrute)
ADD_MODE(usb_disabler)
ADD_MODE(field_detector)
//ADD_MODE(hf14asniffer)
ADD_MODE(uid_stealer)
ADD_MODE(young)
ADD_MODE(mudford)
ADD_MODE(bog)
ADD_MODE(iceclass)
ADD_MODE(legicsim)
ADD_MODE(hf14asniffer)
ADD_MODE(lfemul)
ADD_MODE(tharexde)
END_MODE_LIST

/*******************
 *  End mode list  *
 *******************/

void update_mode(int selected);

void ModInfo(void) {
    DbpString("Multi-standalone loader v. 2 (WintrMvlti)");
}

void mrun(int modnum){
    //update_mode(modnum);
    mode_list[modnum]->run();
}

void update_mode(int selected) {
    if (selected <= 0){
        selected = NUM_MODES;
    }
    //if (selected > NUM_MODES) {
    //    //SpinDown(100);
    //    //Dbprintf("Invalid mode selected");
    //    //LEDsoff();
    //    SpinDown(500);
    //    selected = 1;
    //}
    Dbprintf("Selected mode: '%s'", mode_list[selected]->name);
    LEDsoff();
    LED(selected + 1, 0);
}

void RunMod(void) {
    int selected_mode = 15;
    //int mode_rerun = 1;
    //int full_exit = 0;
    //int return_multi = 0;
    StandAloneMode();
    Dbprintf("[<*>] Multi-standalone loader v. II (a.k.a. Wintrmvlti)");
    Dbprintf("-------------------------------------------------------");
    Dbprintf("[=] Available modes:");
    for (int i = 0; i < NUM_MODES; i++) {
        Dbprintf("%s   '%s'", _GREEN_("-"), mode_list[i]->name);
    }
    if (NUM_MODES > 15){
        SpinErr(LED_B, 30, 50);
        Dbprintf("[=] More than 15 modules loaded");
    } 
    /*multimode_start:
    LED_D_ON();
    bool broken = true;
    for (;;) {
        WDT_HIT();
        if (data_available()) break;
        int press = BUTTON_CLICKED(950);
        switch (press) {
        case BUTTON_SINGLE_CLICK:
            SpinErr(LED_D, 200, 4);
            SpinErr(LED_A, 300, 2);
            //LED_B_ON();
            Dbprintf("Entering mode selector");
            broken = true;
        case BUTTON_HOLD:
            SpinErr(LED_D, 200, 4);
            SpinErr(LED_C, 300, 2);
            //LED_C_ON();
            Dbprintf("Entering exit selector");
            break;
        case BUTTON_DOUBLE_CLICK:
            SpinErr(LED_D, 200, 4);
            SpinErr(LED_B, 300, 2);
            //LED_D_ON();
            Dbprintf("Starting execution of pool 2 (mod #%d) %d", SPECIAL_START, NUMRUN);
            //mrun(NUMRUN);
            selected_mode = SPECIAL_START;
            broken = true;
            break;
        default:
            break;
        }
    }
    if (broken){
        goto mode_selector;
    }
    //SpinErr(LED_D, 400, 4);
    LEDsoff();
    StandaloneBlink();
    StandaloneBlink();
    LED_D_ON();
    for (;;) {
        WDT_HIT();
        if (data_available()) break;
        int press = BUTTON_CLICKED(950);
        switch (press) {
        case BUTTON_SINGLE_CLICK:
            mode_rerun = 0;
            LED_D_OFF();
            SpinErr(LED_D, 200, 4);
            SpinErr(LED_A, 300, 2);
            Dbprintf("Selected mode will not re-enter after exit");
        case BUTTON_DOUBLE_CLICK:
            return_multi = 1;
            mode_rerun = 0;
            LED_B_OFF();
            SpinErr(LED_D, 200, 4);
            SpinErr(LED_C, 300, 2);
            Dbprintf("Multimode selector will be re-entered after exit");
            break;
        case BUTTON_HOLD:
            full_exit = 1;    //StandaloneBlink();
            LED_B_OFF();
            SpinErr(LED_D, 200, 4);
            SpinErr(LED_B, 300, 2);
            Dbprintf("Standalone mode will fully quit after exit");
            break;
        default:
            break;
        }
    }
    mode_selector:*/
    //start:
    LEDsoff();
    StandaloneBlink();
    for (;;) {
        WDT_HIT();
        if (data_available()) break;
        if (selected_mode == SPECIAL_START){
            update_mode(selected_mode);
            mrun(selected_mode);
        }
        if(selected_mode < 0){
            selected_mode += 1;//NUM_MODES;
        }
        int button_pressed = BUTTON_CLICKED(1000);
        switch (button_pressed) {
            case BUTTON_DOUBLE_CLICK:
                selected_mode = selected_mode - 1;//(selected_mode - 1) % NUM_MODES;
                update_mode(selected_mode);
                SpinDelay(200);
                break;
            case BUTTON_SINGLE_CLICK:
                selected_mode = selected_mode + 1;//(selected_mode + 1) % NUM_MODES;
                update_mode(selected_mode);
                SpinDelay(200);
                break;
            case BUTTON_HOLD:
                Dbprintf("Starting selected mode ('%s')", mode_list[selected_mode]->name);
                mode_list[selected_mode]->run();
                //mrun(selected_mode);
                Dbprintf("Exited from selected mode");
                break;
                /*if(mode_rerun){
                    Dbprintf("Re-running mode (%s)", mode_list[selected_mode]->name);
                    mrun(selected_mode);
                    break;
                } else if(full_exit){
                    DbpString("[=] exiting");
                    return;
                } else if(return_multi){
                    Dbprintf("Reloading WitrMvlti . . .");
                    break;
                    //goto start;
                }*/
            default:
                break;
        }
    }
    /*if (return_multi){
        LEDsoff();
        goto mode_selector;
    }
    if(!full_exit){
        goto multimode_start;
    }*/
    DbpString("[=] exiting");
    LEDsoff();
}
