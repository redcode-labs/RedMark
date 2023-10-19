//-----------------------------------------------------------------------------
// Copyright (C) Federico Dotta and Maurizio Agazzini, 2015
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
// PROXMARK3 - HID CORPORATE 1000 BRUTEFORCER (STAND-ALONE MODE)
//
// The new stand-alone mode allows to execute a bruteforce on HID Corporate 1000 readers, by
// reading a specific badge and bruteforcing the Card Number (incrementing and decrementing it),
// mainteining the same Facility Code of the original badge.
//
// Based on an idea of Brad Antoniewicz of McAfee® Foundstone® Professional Services (ProxBrute),
// the stand-alone mode has been rewritten in order to overcome some limitations of ProxBrute firmware,
// that does not consider parity bits.
//
// https://github.com/federicodotta/proxmark3
//
//-----------------------------------------------------------------------------------
// main code for LF aka HID corporate brutefore by Federico Dotta & Maurizio Agazzini
//-----------------------------------------------------------------------------------

#ifndef __LF_HIDBRUTE_H
#define __LF_HIDBRUTE_H

#include <stdint.h>

void hid_corporate_1000_calculate_checksum_and_set(uint32_t *high, uint32_t *low, uint32_t cardnum, uint32_t fc);

#endif /* __LF_HIDBRUTE_H */
