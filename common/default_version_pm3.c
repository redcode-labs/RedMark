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
#include "common.h"
/* Generated file, do not edit */
#ifndef ON_DEVICE
#define SECTVERSINFO
#else
#define SECTVERSINFO __attribute__((section(".version_information")))
#endif

const struct version_information_t SECTVERSINFO g_version_information = {
    VERSION_INFORMATION_MAGIC,
    1,
    1,
    0,
    "Iceman/main/63cd92e-dirty",
    "2022-12-27 23:50:31",
    "f456432f4"
};