#ifndef CMDSMARTCARD_H__
#define CMDSMARTCARD_H__

#include "common.h"
#include "pm3_cmd.h" // structs

// On ARM side, ISO7816_MAX_FRAME is set to 260
// This means we can receive a full short APDU (256 bytes) of data and have enough room for
// SW status code and surrounding metadata without creating a buffer overflow.
#define MAX_APDU_SIZE 256

int CmdSmartcard(const char *Cmd);

bool smart_select(bool verbose, smart_card_atr_t *atr);
int ExchangeAPDUSC(bool verbose, uint8_t *datain, int datainlen, bool activateCard, bool leaveSignalON, uint8_t *dataout, int maxdataoutlen, int *dataoutlen);

#endif