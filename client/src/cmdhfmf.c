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
// High frequency MIFARE commands
//-----------------------------------------------------------------------------

#include "cmdhfmf.h"
#include <ctype.h>

#include "cmdparser.h"    // command_t
#include "commonutil.h"   // ARRAYLEN
#include "comms.h"        // clearCommandBuffer
#include "fileutils.h"
#include "cmdtrace.h"
#include "mifare/mifaredefault.h"          // mifare default key array
#include "cliparser.h"          // argtable
#include "hardnested_bf_core.h" // SetSIMDInstr
#include "mifare/mad.h"
#include "nfc/ndef.h"
#include "protocols.h"
#include "util_posix.h"         // msclock
#include "cmdhfmfhard.h"
#include "crapto1/crapto1.h"    // prng_successor
#include "cmdhf14a.h"           // exchange APDU
#include "crypto/libpcrypto.h"
#include "wiegand_formats.h"
#include "wiegand_formatutils.h"

#define MIFARE_4K_MAXBLOCK 256
#define MIFARE_2K_MAXBLOCK 128
#define MIFARE_1K_MAXBLOCK 64
#define MIFARE_MINI_MAXBLOCK 20

#define MIFARE_MINI_MAXSECTOR 5
#define MIFARE_1K_MAXSECTOR 16
#define MIFARE_2K_MAXSECTOR 32
#define MIFARE_4K_MAXSECTOR 40

static int CmdHelp(const char *Cmd);

int mfc_ev1_print_signature(uint8_t *uid, uint8_t uidlen, uint8_t *signature, int signature_len) {
    // ref:  MIFARE Classic EV1 Originality Signature Validation
#define PUBLIC_MFCEV1_ECDA_KEYLEN 33
    const ecdsa_publickey_t nxp_mfc_public_keys[] = {
        {"NXP Mifare Classic MFC1C14_x", "044F6D3F294DEA5737F0F46FFEE88A356EED95695DD7E0C27A591E6F6F65962BAF"},
    };

    uint8_t i;
    bool is_valid = false;

    for (i = 0; i < ARRAYLEN(nxp_mfc_public_keys); i++) {

        int dl = 0;
        uint8_t key[PUBLIC_MFCEV1_ECDA_KEYLEN];
        param_gethex_to_eol(nxp_mfc_public_keys[i].value, 0, key, PUBLIC_MFCEV1_ECDA_KEYLEN, &dl);

        int res = ecdsa_signature_r_s_verify(MBEDTLS_ECP_DP_SECP128R1, key, uid, uidlen, signature, signature_len, false);
        is_valid = (res == 0);
        if (is_valid)
            break;
    }

    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Tag Signature"));
    if (is_valid == false || i == ARRAYLEN(nxp_mfc_public_keys)) {
        PrintAndLogEx(INFO, "    Elliptic curve parameters: NID_secp128r1");
        PrintAndLogEx(INFO, "             TAG IC Signature: %s", sprint_hex_inrow(signature, 32));
        PrintAndLogEx(SUCCESS, "       Signature verification: " _RED_("failed"));
        return PM3_ESOFT;
    }

    PrintAndLogEx(INFO, " IC signature public key name: %s", nxp_mfc_public_keys[i].desc);
    PrintAndLogEx(INFO, "IC signature public key value: %s", nxp_mfc_public_keys[i].value);
    PrintAndLogEx(INFO, "    Elliptic curve parameters: NID_secp128r1");
    PrintAndLogEx(INFO, "             TAG IC Signature: %s", sprint_hex_inrow(signature, 32));
    PrintAndLogEx(SUCCESS, "       Signature verification: " _GREEN_("successful"));
    return PM3_SUCCESS;
}

static int GetHFMF14AUID(uint8_t *uid, int *uidlen) {
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (!WaitForResponseTimeout(CMD_ACK, &resp, 2500)) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        DropField();
        return 0;
    }

    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));
    memcpy(uid, card.uid, card.uidlen * sizeof(uint8_t));
    *uidlen = card.uidlen;
    return 1;
}

static char *GenerateFilename(const char *prefix, const char *suffix) {
    if (! IfPm3Iso14443a()) {
        return NULL;
    }
    uint8_t uid[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int uidlen = 0;
    char *fptr = calloc(sizeof(char) * (strlen(prefix) + strlen(suffix)) + sizeof(uid) * 2 + 1,  sizeof(uint8_t));

    GetHFMF14AUID(uid, &uidlen);
    if (!uidlen) {
        PrintAndLogEx(WARNING, "No tag found.");
        free(fptr);
        return NULL;
    }

    strcpy(fptr, prefix);
    FillFileNameByUID(fptr, uid, suffix, uidlen);
    return fptr;
}

static int32_t initSectorTable(sector_t **src, int32_t items) {

    (*src) = calloc(items, sizeof(sector_t));

    if (*src == NULL)
        return -1;

    // empty e_sector
    for (int i = 0; i < items; ++i) {
        for (int j = 0; j < 2; ++j) {
            (*src)[i].Key[j] = 0xffffffffffff;
            (*src)[i].foundKey[j] = false;
        }
    }
    return items;
}

static void decode_print_st(uint16_t blockno, uint8_t *data) {
    if (mfIsSectorTrailer(blockno)) {
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "----------------------- " _CYAN_("Sector trailer decoder") " -----------------------");
        PrintAndLogEx(INFO, "key A........ " _GREEN_("%s"), sprint_hex_inrow(data, 6));
        PrintAndLogEx(INFO, "acr.......... " _GREEN_("%s"), sprint_hex_inrow(data + 6, 3));
        PrintAndLogEx(INFO, "user / gpb... " _GREEN_("%02x"), data[9]);
        PrintAndLogEx(INFO, "key B........ " _GREEN_("%s"), sprint_hex_inrow(data + 10, 6));
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "  # | Access rights");
        PrintAndLogEx(INFO, "----+-----------------------------------------------------------------");

        if (mfValidateAccessConditions(&data[6]) == false) {
            PrintAndLogEx(WARNING, _RED_("Invalid Access Conditions"));
        }

        int bln = mfFirstBlockOfSector(mfSectorNum(blockno));
        int blinc = (mfNumBlocksPerSector(mfSectorNum(blockno)) > 4) ? 5 : 1;
        for (int i = 0; i < 4; i++) {
            PrintAndLogEx(INFO, "%3d%c| " _YELLOW_("%s"), bln, ((blinc > 1) && (i < 3) ? '+' : ' '), mfGetAccessConditionsDesc(i, &data[6]));
            bln += blinc;
        }
        PrintAndLogEx(INFO, "----------------------------------------------------------------------");
        PrintAndLogEx(NORMAL, "");
    }
}

static uint8_t NumOfSectors(char card) {
    switch (card) {
        case '0' :
            return MIFARE_MINI_MAXSECTOR;
        case '1' :
            return MIFARE_1K_MAXSECTOR;
        case '2' :
            return MIFARE_2K_MAXSECTOR;
        case '4' :
            return MIFARE_4K_MAXSECTOR;
        default  :
            return 0;
    }
}

static char GetFormatFromSector(uint8_t sectors) {
    switch (sectors) {
        case MIFARE_MINI_MAXSECTOR:
            return '0';
        case MIFARE_1K_MAXSECTOR:
            return '1';
        case MIFARE_2K_MAXSECTOR:
            return '2';
        case MIFARE_4K_MAXSECTOR:
            return '4';
        default  :
            return ' ';
    }
}

static bool mfc_value(const uint8_t *d, int32_t *val) {
    // values
    int32_t a = (int32_t)MemLeToUint4byte(d);
    uint32_t a_inv = MemLeToUint4byte(d + 4);
    uint32_t b = MemLeToUint4byte(d + 8);

    int val_checks = (
                         (a == b) && (a == ~a_inv) &&
                         (d[12] == (~d[13] & 0xFF)) &&
                         (d[14] == (~d[15] & 0xFF))
                     );

    if (val) {
        *val = a;
    }
    return (val_checks);
}
static void mf_print_block(uint8_t blockno, uint8_t *d, bool verbose) {
    if (blockno == 0) {
        PrintAndLogEx(INFO, "%3d | " _RED_("%s"), blockno, sprint_hex_ascii(d, MFBLOCK_SIZE));
    } else if (mfIsSectorTrailer(blockno)) {
        PrintAndLogEx(INFO, "%3d | " _YELLOW_("%s"), blockno, sprint_hex_ascii(d, MFBLOCK_SIZE));
    } else {

        int32_t value = 0;
        if (verbose && mfc_value(d, &value)) {
            PrintAndLogEx(INFO, "%3d | " _CYAN_("%s") " %"PRIi32, blockno, sprint_hex_ascii(d, MFBLOCK_SIZE), value);
        } else {
            PrintAndLogEx(INFO, "%3d | %s ", blockno, sprint_hex_ascii(d, MFBLOCK_SIZE));
        }
    }
}

static void mf_print_blocks(uint16_t n, uint8_t *d, bool verbose) {
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
    PrintAndLogEx(INFO, "blk | data                                            | ascii");
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
    for (uint16_t i = 0; i < n; i++) {
        mf_print_block(i, d + (i * MFBLOCK_SIZE), verbose);
    }
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
    PrintAndLogEx(HINT, _CYAN_("cyan") " = value block with decoded value");

    // MAD detection
    if (HasMADKey(d)) {
        PrintAndLogEx(SUCCESS, "MAD key detected");
    }
    PrintAndLogEx(NORMAL, "");
}

static int mf_print_keys(uint16_t n, uint8_t *d) {

    uint8_t sectors = 0;
    switch (n) {
        case MIFARE_MINI_MAXBLOCK:
            sectors = MIFARE_MINI_MAXSECTOR;
            break;
        case MIFARE_2K_MAXBLOCK:
            sectors = MIFARE_2K_MAXSECTOR;
            break;
        case MIFARE_4K_MAXBLOCK:
            sectors = MIFARE_4K_MAXSECTOR;
            break;
        case MIFARE_1K_MAXBLOCK:
        default:
            sectors = MIFARE_1K_MAXSECTOR;
            break;
    }

    sector_t *e_sector = calloc(sectors, sizeof(sector_t));
    if (e_sector == NULL)  {
        return PM3_EMALLOC;
    }

    for (uint16_t i = 0; i < n; i++) {
        if (mfIsSectorTrailer(i)) {
            e_sector[mfSectorNum(i)].foundKey[0] = 1;
            e_sector[mfSectorNum(i)].Key[0] = bytes_to_num(d + (i * MFBLOCK_SIZE), 6);
            e_sector[mfSectorNum(i)].foundKey[1] = 1;
            e_sector[mfSectorNum(i)].Key[1] = bytes_to_num(d + (i * MFBLOCK_SIZE) + 10, 6);
        }
    }
    printKeyTable(sectors, e_sector);
    free(e_sector);
    return PM3_SUCCESS;
}

/*
static void mf_print_values(uint16_t n, uint8_t *d) {

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "Looking for value blocks...");
    PrintAndLogEx(NORMAL, "");
    uint8_t cnt = 0;
    int32_t value = 0;
    for (uint16_t i = 0; i < n; i++) {

        if (mfc_value(d + (i * MFBLOCK_SIZE), &value))  {
            PrintAndLogEx(INFO, "%03d | " _YELLOW_("%" PRIi32) " " _YELLOW_("0x%" PRIX32), i, value, value);
            ++cnt;
        }
    }

    if (cnt) {
        PrintAndLogEx(INFO, "Found %u value blocks in file", cnt);
        PrintAndLogEx(NORMAL, "");
    }
}
*/

static void mf_print_sector_hdr(uint8_t sector) {
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "  # | sector " _GREEN_("%02d") " / " _GREEN_("0x%02X") "                                | ascii", sector, sector);
    PrintAndLogEx(INFO, "----+-------------------------------------------------+-----------------");
}

static int CmdHF14AMfAcl(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf acl",
                  "Print decoded MIFARE access rights (ACL), \n"
                  "  A = key A\n"
                  "  B = key B\n"
                  "  AB = both key A and B\n"
                  "  ACCESS = access bytes inside sector trailer block\n"
                  "  Increment, decrement, transfer, restore is for value blocks",
                  "hf mf acl\n");

    void *argtable[] = {
        arg_param_begin,
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    //int acllen = 0;
    //CLIGetHexWithReturn(ctx, 1, acl, &acllen);

    CLIParserFree(ctx);

    uint8_t acl[3] = {0xFF, 0x07, 0x80};
    PrintAndLogEx(NORMAL, "");

    // look up common default ACL bytes and print a fingerprint line about it.
    if (memcmp(acl, "\xFF\x07\x80", 3) == 0) {
        PrintAndLogEx(INFO, "ACL ---> " _GREEN_("%s") " (transport configuration)", sprint_hex(acl, sizeof(acl)));
    }
    /*if (mfValidateAccessConditions(acl) == false) {
        PrintAndLogEx(ERR, _RED_("Invalid Access Conditions, NEVER write these on a card!"));
    }*/
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "  # | Access rights");
    PrintAndLogEx(INFO, "----+-----------------------------------------------------------------");
    for (int i = 0; i < 4; i++) {
        PrintAndLogEx(INFO, "%3d | " _CYAN_("%s"), i, mfGetAccessConditionsDesc(i, acl));
    }
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int CmdHF14AMfDarkside(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf darkside",
                  "Darkside attack",
                  "hf mf darkside\n"
                  "hf mf darkside --blk 16\n"
                  "hf mf darkside --blk 16 -b\n");

    void *argtable[] = {
        arg_param_begin,
        arg_int0(NULL, "blk", "<dec> ", "Target block"),
        arg_lit0("b", NULL, "Target key B instead of default key A"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t blockno = arg_get_u32_def(ctx, 1, 0);

    uint8_t key_type = MIFARE_AUTH_KEYA;

    if (arg_get_lit(ctx, 2)) {
        PrintAndLogEx(INFO, "Targeting key B");
        key_type = MIFARE_AUTH_KEYB;
    }

    CLIParserFree(ctx);

    uint64_t key = 0;
    uint64_t t1 = msclock();
    int isOK = mfDarkside(blockno, key_type, &key);
    t1 = msclock() - t1;

    switch (isOK) {
        case -1 :
            PrintAndLogEx(WARNING, "button pressed, aborted");
            return PM3_ESOFT;
        case -2 :
            PrintAndLogEx(FAILED, "card is not vulnerable to Darkside attack (doesn't send NACK on authentication requests)");
            return PM3_ESOFT;
        case -3 :
            PrintAndLogEx(FAILED, "card is not vulnerable to Darkside attack (its random number generator is not predictable)");
            return PM3_ESOFT;
        case -4 :
            PrintAndLogEx(FAILED, "card is not vulnerable to Darkside attack (its random number generator seems to be based on the wellknown");
            PrintAndLogEx(FAILED, "generating polynomial with 16 effective bits only, but shows unexpected behaviour");
            return PM3_ESOFT;
        case PM3_EOPABORTED :
            PrintAndLogEx(WARNING, "aborted via keyboard");
            return PM3_EOPABORTED;
        default :
            PrintAndLogEx(SUCCESS, "found valid key: "_GREEN_("%012" PRIx64), key);
            break;
    }
    PrintAndLogEx(SUCCESS, "time in darkside " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);
    return PM3_SUCCESS;
}

static int CmdHF14AMfWrBl(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf wrbl",
                  "Write MIFARE Classic block with 16 hex bytes of data\n"
                  " \n"
                  "Sector 0 / Block 0 - Manufacturer block\n"
                  "When writing to block 0 you must use a VALID block 0 data (UID, BCC, SAK, ATQA)\n"
                  "Writing an invalid block 0 means rendering your Magic GEN2 card undetectable. \n"
                  "Look in the magic_cards_notes.md file for help to resolve it.\n"
                  " \n"
                  "`--force` param is used to override warnings like bad ACL and BLOCK 0 writes.\n"
                  "          if not specified, it will exit if detected",
                  "hf mf wrbl --blk 1 -k FFFFFFFFFFFF -d 000102030405060708090a0b0c0d0e0f"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("n", "blk", "<dec>", "block number"),
        arg_lit0("a", NULL, "input key type is key A (def)"),
        arg_lit0("b", NULL, "input key type is key B"),
        arg_lit0("f", "force", "override warnings"),
        arg_str0("k", "key", "<hex>", "key, 6 hex bytes"),
        arg_str0("d", "data", "<hex>", "bytes to write, 16 hex bytes"),

        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int b = arg_get_int_def(ctx, 1, 1);

    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 2) && arg_get_lit(ctx, 3)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 3)) {
        keytype = MF_KEY_B;;
    }

    bool force = arg_get_lit(ctx, 4);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 5, key, &keylen);

    uint8_t block[MFBLOCK_SIZE] = {0x00};
    int blen = 0;
    CLIGetHexWithReturn(ctx, 6, block, &blen);
    CLIParserFree(ctx);

    if (blen != MFBLOCK_SIZE) {
        PrintAndLogEx(WARNING, "block data must include 16 HEX bytes. Got %i", blen);
        return PM3_EINVARG;
    }

    if (b > 255) {
        return PM3_EINVARG;
    }

    // BLOCK 0 detection
    if (b == 0 && force == false) {
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "Targeting Sector 0 / Block 0 - Manufacturer block");
        PrintAndLogEx(INFO, "Read the helptext for details before writing to this block");
        PrintAndLogEx(INFO, "You must use param `" _YELLOW_("--force") "` to write to this block");
        PrintAndLogEx(NORMAL, "");
        return PM3_EINVARG;
    }

    uint8_t blockno = (uint8_t)b;

    // Sector trailer sanity checks. 
    // Warn if ACL is strict read-only,  or invalid ACL.
    if (mfIsSectorTrailer(blockno)) {
        PrintAndLogEx(INFO, "Sector trailer (ST) write detected");

        // ensure access right isn't messed up.
        if (mfValidateAccessConditions(&block[6]) == false) {
            PrintAndLogEx(WARNING, "Invalid Access Conditions detected, replacing with default values");
            memcpy(block + 6, "\xFF\x07\x80\x69", 4);
        }

        bool ro_detected = false;
        uint8_t bar = mfNumBlocksPerSector(mfSectorNum(blockno));
        for (uint8_t foo = 0; foo < bar; foo++) {
            if (mfReadOnlyAccessConditions(foo, &block[6])) {
                PrintAndLogEx(WARNING, "Strict ReadOnly Access Conditions on block " _YELLOW_("%u") " detected", blockno - bar + 1 + foo);
                ro_detected = true;
            }
        }
        if (ro_detected) {
            if (force) {
                PrintAndLogEx(WARNING, " --force override, continuing...");
            } else {
                PrintAndLogEx(INFO, "Exiting, please run `" _YELLOW_("hf mf acl -d %s") "` to understand", sprint_hex_inrow(&block[6], 3));
                PrintAndLogEx(INFO, "Use `" _YELLOW_("--force") "` to override and write this data");
                return PM3_EINVARG;
            }
        } else {
            PrintAndLogEx(SUCCESS, "ST passed checks, continuing...");
        }
    }

    PrintAndLogEx(INFO, "Writing block no %d, key %c - %s", blockno, (keytype == MF_KEY_B) ? 'B' : 'A', sprint_hex_inrow(key, sizeof(key)));
    PrintAndLogEx(INFO, "data: %s", sprint_hex(block, sizeof(block)));

    uint8_t data[26];
    memcpy(data, key, sizeof(key));
    memcpy(data + 10, block, sizeof(block));
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_MIFARE_WRITEBL, blockno, keytype, 0, data, sizeof(data));

    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(FAILED, "Command execute timeout");
        return PM3_ETIMEOUT;
    }

    uint8_t isok  = resp.oldarg[0] & 0xff;
    if (isok) {
        PrintAndLogEx(SUCCESS, "Write ( " _GREEN_("ok") " )");
        PrintAndLogEx(HINT, "try `" _YELLOW_("hf mf rdbl") "` to verify");
    } else {
        PrintAndLogEx(FAILED, "Write ( " _RED_("fail") " )");
        // suggest the opposite keytype than what was used.
        PrintAndLogEx(HINT, "Maybe access rights? Try specify keytype `" _YELLOW_("hf mf wrbl -%c ...") "` instead", (keytype == MF_KEY_A) ? 'b' : 'a');
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfRdBl(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf rdbl",
                  "Read MIFARE Classic block",
                  "hf mf rdbl --blk 0 -k FFFFFFFFFFFF\n"
                  "hf mf rdbl --blk 3 -v   -> get block 3, decode sector trailer\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("n", "blk", "<dec>", "block number"),
        arg_lit0("a", NULL, "input key type is key A (def)"),
        arg_lit0("b", NULL, "input key type is key B"),
        arg_str0("k", "key", "<hex>", "key, 6 hex bytes"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int b = arg_get_int_def(ctx, 1, 0);

    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 2) && arg_get_lit(ctx, 3)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 3)) {
        keytype = MF_KEY_B;
    }

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 4, key, &keylen);
    bool verbose = arg_get_lit(ctx, 5);
    CLIParserFree(ctx);

    if (b > 255) {
        return PM3_EINVARG;
    }
    uint8_t blockno = (uint8_t)b;

    uint8_t data[16] = {0};
    int res =  mfReadBlock(blockno, keytype, key, data);
    if (res == PM3_SUCCESS) {

        uint8_t sector = mfSectorNum(blockno);
        mf_print_sector_hdr(sector);
        mf_print_block(blockno, data, verbose);
        if (verbose) {
            decode_print_st(blockno, data);
        }
    }
    PrintAndLogEx(NORMAL, "");
    return res;
}

static int CmdHF14AMfRdSc(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf rdsc",
                  "Read MIFARE Classic sector",
                  "hf mf rdsc -s 0 -k FFFFFFFFFFFF\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("a", NULL, "input key specified is A key (def)"),
        arg_lit0("b", NULL, "input key specified is B key"),
        arg_str0("k", "key", "<hex>", "key specified as 6 hex bytes"),
        arg_int1("s", "sec", "<dec>", "sector number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 1) && arg_get_lit(ctx, 2)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 2)) {
        keytype = MF_KEY_B;
    }

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 3, key, &keylen);

    int s = arg_get_int_def(ctx, 4, 0);
    bool verbose = arg_get_lit(ctx, 5);
    CLIParserFree(ctx);

    if (s > MIFARE_4K_MAXSECTOR) {
        PrintAndLogEx(WARNING, "Sector number must be less then 40");
        return PM3_EINVARG;
    }
    uint8_t sector = (uint8_t)s;
    uint8_t sc_size = mfNumBlocksPerSector(sector) * MFBLOCK_SIZE;
    uint8_t *data = calloc(sc_size, sizeof(uint8_t));
    if (data == NULL) {
        PrintAndLogEx(ERR, "failed to allocate memory");
        return PM3_EMALLOC;
    }

    int res =  mfReadSector(sector, keytype, key, data);
    if (res == PM3_SUCCESS) {

        uint8_t blocks = mfNumBlocksPerSector(sector);
        uint8_t start = mfFirstBlockOfSector(sector);

        mf_print_sector_hdr(sector);
        for (int i = 0; i < blocks; i++) {
            mf_print_block(start + i, data + (i * MFBLOCK_SIZE), verbose);
        }

        if (verbose) {
            decode_print_st(start + blocks - 1, data + ((blocks - 1) * MFBLOCK_SIZE));
        }
    }
    free(data);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int FastDumpWithEcFill(uint8_t numsectors) {
    PacketResponseNG resp;

    mfc_eload_t payload;
    payload.sectorcnt = numsectors;
    payload.keytype = MF_KEY_A;

    // ecfill key A
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_LOAD, (uint8_t *)&payload, sizeof(payload));

    bool res = WaitForResponseTimeout(CMD_HF_MIFARE_EML_LOAD, &resp, 2500);
    if (res == false) {
        PrintAndLogEx(WARNING, "Command execute timeout");
        return PM3_ETIMEOUT;
    }

    if (resp.status != PM3_SUCCESS) {
        PrintAndLogEx(INFO, "fast dump reported back failure w KEY A,  swapping to KEY B");

        // ecfill key B
        payload.keytype = MF_KEY_B;

        clearCommandBuffer();
        SendCommandNG(CMD_HF_MIFARE_EML_LOAD, (uint8_t *)&payload, sizeof(payload));
        res = WaitForResponseTimeout(CMD_HF_MIFARE_EML_LOAD, &resp, 2500);
        if (res == false) {
            PrintAndLogEx(WARNING, "Command execute timeout");
            return PM3_ETIMEOUT;
        }

        if (resp.status != PM3_SUCCESS) {
            PrintAndLogEx(INFO, "fast dump reported back failure w KEY B");
            PrintAndLogEx(INFO, "Dump file is " _RED_("PARTIAL") " complete");
        }
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfDump(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf dump",
                  "Dump MIFARE Classic tag to binary file\n"
                  "If no <name> given, UID will be used as filename",
                  "hf mf dump --mini                        --> MIFARE Mini\n"
                  "hf mf dump --1k                          --> MIFARE Classic 1k\n"
                  "hf mf dump --2k                          --> MIFARE 2k\n"
                  "hf mf dump --4k                          --> MIFARE 4k\n"
                  "hf mf dump --keys hf-mf-066C8B78-key.bin --> MIFARE 1k with keys from specified file\n");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of dump"),
        arg_str0("k", "keys", "<fn>", "filename of keys"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int datafnlen = 0;
    char dataFilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)dataFilename, FILE_PATH_SIZE, &datafnlen);

    int keyfnlen = 0;
    char keyFilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 2), (uint8_t *)keyFilename, FILE_PATH_SIZE, &keyfnlen);

    bool m0 = arg_get_lit(ctx, 3);
    bool m1 = arg_get_lit(ctx, 4);
    bool m2 = arg_get_lit(ctx, 5);
    bool m4 = arg_get_lit(ctx, 6);

    CLIParserFree(ctx);

    uint64_t t1 = msclock();

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t numSectors = MIFARE_1K_MAXSECTOR;

    if (m0) {
        numSectors = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        numSectors = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        numSectors = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        numSectors = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    uint8_t sectorNo, blockNo;
    uint8_t keyA[40][6];
    uint8_t keyB[40][6];
    uint8_t rights[40][4];
    uint8_t carddata[256][16];

    FILE *f;
    PacketResponseNG resp;

    char *fptr;

    // Select card to get UID/UIDLEN/ATQA/SAK information
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "iso14443a card select timeout");
        return PM3_ETIMEOUT;
    }

    uint64_t select_status = resp.oldarg[0];
    if (select_status == 0) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        return PM3_SUCCESS;
    }

    // store card info
    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));

    if (keyFilename[0] == 0x00) {
        fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(keyFilename, fptr);
        free(fptr);
    }

    if ((f = fopen(keyFilename, "rb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not find file " _YELLOW_("%s"), keyFilename);
        return PM3_EFILE;
    }

    PrintAndLogEx(INFO, "Using `" _YELLOW_("%s") "`", keyFilename);

    // Read keys A from file
    size_t bytes_read;
    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        bytes_read = fread(keyA[sectorNo], 1, MFKEY_SIZE, f);
        if (bytes_read != MFKEY_SIZE) {
            PrintAndLogEx(ERR, "File reading error.");
            fclose(f);
            return PM3_EFILE;
        }
    }

    // Read keys B from file
    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        bytes_read = fread(keyB[sectorNo], 1, MFKEY_SIZE, f);
        if (bytes_read != MFKEY_SIZE) {
            PrintAndLogEx(ERR, "File reading error.");
            fclose(f);
            return PM3_EFILE;
        }
    }

    fclose(f);

    PrintAndLogEx(INFO, "Reading sector access bits...");
    PrintAndLogEx(INFO, "." NOLF);

    uint8_t tries;
    mf_readblock_t payload;
    uint8_t current_key;
    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        current_key = MF_KEY_A;
        for (tries = 0; tries < MIFARE_SECTOR_RETRY; tries++) {
            PrintAndLogEx(NORMAL, "." NOLF);
            fflush(stdout);

            payload.blockno = mfFirstBlockOfSector(sectorNo) + mfNumBlocksPerSector(sectorNo) - 1;
            payload.keytype = current_key;

            memcpy(payload.key, current_key == MF_KEY_A ? keyA[sectorNo] : keyB[sectorNo], sizeof(payload.key));

            clearCommandBuffer();
            SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

            if (WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) {

                uint8_t *data = resp.data.asBytes;
                if (resp.status == PM3_SUCCESS) {
                    rights[sectorNo][0] = ((data[7] & 0x10) >> 2) | ((data[8] & 0x1) << 1) | ((data[8] & 0x10) >> 4); // C1C2C3 for data area 0
                    rights[sectorNo][1] = ((data[7] & 0x20) >> 3) | ((data[8] & 0x2) << 0) | ((data[8] & 0x20) >> 5); // C1C2C3 for data area 1
                    rights[sectorNo][2] = ((data[7] & 0x40) >> 4) | ((data[8] & 0x4) >> 1) | ((data[8] & 0x40) >> 6); // C1C2C3 for data area 2
                    rights[sectorNo][3] = ((data[7] & 0x80) >> 5) | ((data[8] & 0x8) >> 2) | ((data[8] & 0x80) >> 7); // C1C2C3 for sector trailer
                    break;
                } else if (tries == (MIFARE_SECTOR_RETRY / 2)) { // after half unsuccessful tries, give key B a go
                    PrintAndLogEx(WARNING, "\ntrying with key B instead...");
                    current_key = MF_KEY_B;
                    PrintAndLogEx(INFO, "." NOLF);
                } else if (tries == (MIFARE_SECTOR_RETRY - 1)) { // on last try set defaults
                    PrintAndLogEx(FAILED, "\ncould not get access rights for sector %2d. Trying with defaults...", sectorNo);
                    rights[sectorNo][0] = rights[sectorNo][1] = rights[sectorNo][2] = 0x00;
                    rights[sectorNo][3] = 0x01;
                }
            } else {
                PrintAndLogEx(FAILED, "\ncommand execute timeout when trying to read access rights for sector %2d. Trying with defaults...", sectorNo);
                rights[sectorNo][0] = rights[sectorNo][1] = rights[sectorNo][2] = 0x00;
                rights[sectorNo][3] = 0x01;
            }
        }
    }
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, "Finished reading sector access bits");
    PrintAndLogEx(INFO, "Dumping all blocks from card...");

    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        for (blockNo = 0; blockNo < mfNumBlocksPerSector(sectorNo); blockNo++) {
            bool received = false;
            current_key = MF_KEY_A;
            
            uint8_t data_area = (sectorNo < 32) ? blockNo : blockNo / 5;
            if (rights[sectorNo][data_area] == 0x07) {                                     // no key would work
                PrintAndLogEx(WARNING, "access rights do not allow reading of sector %2d block %3d, skipping", sectorNo, blockNo);
                continue;
            }
            for (tries = 0; tries < MIFARE_SECTOR_RETRY; tries++) {
                if (blockNo == mfNumBlocksPerSector(sectorNo) - 1) { // sector trailer. At least the Access Conditions can always be read with key A.

                    payload.blockno = mfFirstBlockOfSector(sectorNo) + blockNo;
                    payload.keytype = current_key;
                    memcpy(payload.key, current_key == MF_KEY_A ? keyA[sectorNo] : keyB[sectorNo], sizeof(payload.key));

                    clearCommandBuffer();
                    SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
                    received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
                } else {                                           // data block. Check if it can be read with key A or key B
                    if ((rights[sectorNo][data_area] == 0x03) || (rights[sectorNo][data_area] == 0x05)) { // only key B would work

                        payload.blockno = mfFirstBlockOfSector(sectorNo) + blockNo;
                        payload.keytype = MF_KEY_B;
                        memcpy(payload.key, keyB[sectorNo], sizeof(payload.key));

                        clearCommandBuffer();
                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
                        received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
                    } else {                                                                              // key A would work

                        payload.blockno = mfFirstBlockOfSector(sectorNo) + blockNo;
                        payload.keytype = current_key;
                        memcpy(payload.key, current_key == MF_KEY_A ? keyA[sectorNo] : keyB[sectorNo], sizeof(payload.key));

                        clearCommandBuffer();
                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
                        received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
                    }
                }
                if (received) {
                    if (resp.status == PM3_SUCCESS) {
                        // break the re-try loop
                        break;
                    }
                    if ((current_key == MF_KEY_A) && (tries == (MIFARE_SECTOR_RETRY / 2))) {
                        // Half the tries failed with key A. Swap for key B
                        current_key = MF_KEY_B;

                        // clear out keyA since it failed.
                        memset(keyA[sectorNo], 0x00, sizeof(keyA[sectorNo]));
                    }
                }
            }

            if (received) {
                uint8_t *data  = resp.data.asBytes;
                if (blockNo == mfNumBlocksPerSector(sectorNo) - 1) { // sector trailer. Fill in the keys.
                    data[0]  = (keyA[sectorNo][0]);
                    data[1]  = (keyA[sectorNo][1]);
                    data[2]  = (keyA[sectorNo][2]);
                    data[3]  = (keyA[sectorNo][3]);
                    data[4]  = (keyA[sectorNo][4]);
                    data[5]  = (keyA[sectorNo][5]);

                    data[10] = (keyB[sectorNo][0]);
                    data[11] = (keyB[sectorNo][1]);
                    data[12] = (keyB[sectorNo][2]);
                    data[13] = (keyB[sectorNo][3]);
                    data[14] = (keyB[sectorNo][4]);
                    data[15] = (keyB[sectorNo][5]);
                }
                if (resp.status == PM3_SUCCESS) {
                    memcpy(carddata[mfFirstBlockOfSector(sectorNo) + blockNo], data, 16);
                    PrintAndLogEx(SUCCESS, "successfully read block %2d of sector %2d.", blockNo, sectorNo);
                } else {
                    PrintAndLogEx(FAILED, "could not read block %2d of sector %2d", blockNo, sectorNo);
                }
            } else {
                PrintAndLogEx(WARNING, "command execute timeout when trying to read block %2d of sector %2d.", blockNo, sectorNo);
            }
        }
    }

    PrintAndLogEx(SUCCESS, "time: %" PRIu64 " seconds\n", (msclock() - t1) / 1000);

    PrintAndLogEx(SUCCESS, "\nSucceeded in dumping all blocks");

    if (strlen(dataFilename) < 1) {
        fptr = GenerateFilename("hf-mf-", "-dump");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(dataFilename, fptr);
        free(fptr);
    }

    uint16_t bytes = 16 * (mfFirstBlockOfSector(numSectors - 1) + mfNumBlocksPerSector(numSectors - 1));

    saveFile(dataFilename, ".bin", (uint8_t *)carddata, bytes);
    saveFileEML(dataFilename, (uint8_t *)carddata, bytes, MFBLOCK_SIZE);

    iso14a_mf_extdump_t xdump;
    xdump.card_info = card;
    xdump.dump = (uint8_t *)carddata;
    xdump.dumplen = bytes;
    saveFileJSON(dataFilename, jsfCardMemory, (uint8_t *)&xdump, sizeof(xdump), NULL);
    return PM3_SUCCESS;
}

//static int CmdHF14AMfDump(const char *Cmd) {
//    CLIParserContext *ctx;
//    CLIParserInit(&ctx, "hf mf dump",
//                  "Dump MIFARE Classic tag to binary file\n"
//                  "If no <name> given, UID will be used as filename",
//                  "hf mf dump --mini                        --> MIFARE Mini\n"
//                  "hf mf dump --1k                          --> MIFARE Classic 1k\n"
//                  "hf mf dump --2k                          --> MIFARE 2k\n"
//                  "hf mf dump --4k                          --> MIFARE 4k\n"
//                  "hf mf dump --keys hf-mf-066C8B78-key.bin --> MIFARE 1k with keys from specified file\n");
//
//    void *argtable[] = {
//        arg_param_begin,
//        arg_str0("f", "file", "<fn>", "filename of dump"),
//        arg_str0("k", "keys", "<fn>", "filename of keys"),
//        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
//        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
//        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
//        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
//        arg_param_end
//    };
//    CLIExecWithReturn(ctx, Cmd, argtable, true);
//
//    int datafnlen = 0;
//    char dataFilename[FILE_PATH_SIZE] = {0};
//    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)dataFilename, FILE_PATH_SIZE, &datafnlen);
//
//    int keyfnlen = 0;
//    char keyFilename[FILE_PATH_SIZE] = {0};
//    CLIParamStrToBuf(arg_get_str(ctx, 2), (uint8_t *)keyFilename, FILE_PATH_SIZE, &keyfnlen);
//
//    bool m0 = arg_get_lit(ctx, 3);
//    bool m1 = arg_get_lit(ctx, 4);
//    bool m2 = arg_get_lit(ctx, 5);
//    bool m4 = arg_get_lit(ctx, 6);
//
//    CLIParserFree(ctx);
//
//    uint64_t t1 = msclock();
//
//    // validations
//    if ((m0 + m1 + m2 + m4) > 1) {
//        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
//        return PM3_EINVARG;
//    } else if ((m0 + m1 + m2 + m4) == 0) {
//        m1 = true;
//    }
//
//    uint8_t numSectors = MIFARE_1K_MAXSECTOR;
//
//    if (m0) {
//        numSectors = MIFARE_MINI_MAXSECTOR;
//    } else if (m1) {
//        numSectors = MIFARE_1K_MAXSECTOR;
//    } else if (m2) {
//        numSectors = MIFARE_2K_MAXSECTOR;
//    } else if (m4) {
//        numSectors = MIFARE_4K_MAXSECTOR;
//    } else {
//        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
//        return PM3_EINVARG;
//    }
//
//    uint8_t sectorNo, blockNo;
//    uint8_t keyA[40][6];
//    uint8_t keyB[40][6];
//    uint8_t rights[40][4];
//    uint8_t carddata[256][16];
//
//    FILE *f;
//    PacketResponseNG resp;
//
//    char *fptr;
//
//    // Select card to get UID/UIDLEN/ATQA/SAK information
//    clearCommandBuffer();
//    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
//    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
//        PrintAndLogEx(WARNING, "iso14443a card select timeout");
//        return PM3_ETIMEOUT;
//    }
//
//    uint64_t select_status = resp.oldarg[0];
//    if (select_status == 0) {
//        PrintAndLogEx(WARNING, "iso14443a card select failed");
//        return select_status;
//    }
//
//    // store card info
//    iso14a_card_select_t card;
//    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));
//
//    if (keyFilename[0] == 0x00) {
//        fptr = GenerateFilename("hf-mf-", "-key.bin");
//        if (fptr == NULL)
//            return PM3_ESOFT;
//
//        strcpy(keyFilename, fptr);
//        free(fptr);
//    }
//
//    if ((f = fopen(keyFilename, "rb")) == NULL) {
//        PrintAndLogEx(WARNING, "Could not find file " _YELLOW_("%s"), keyFilename);
//        return PM3_EFILE;
//    }
//
//    PrintAndLogEx(INFO, "Using `" _YELLOW_("%s") "`", keyFilename);
//
//    // Read keys A from file
//    size_t bytes_read;
//    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
//        bytes_read = fread(keyA[sectorNo], 1, 6, f);
//        if (bytes_read != 6) {
//            PrintAndLogEx(ERR, "File reading error.");
//            fclose(f);
//            return PM3_EFILE;
//        }
//    }
//
//    // Read keys B from file
//    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
//        bytes_read = fread(keyB[sectorNo], 1, 6, f);
//        if (bytes_read != 6) {
//            PrintAndLogEx(ERR, "File reading error.");
//            fclose(f);
//            return PM3_EFILE;
//        }
//    }
//
//    fclose(f);
//
//    PrintAndLogEx(INFO, "Reading sector access bits...");
//    PrintAndLogEx(INFO, "." NOLF);
//
//    uint8_t tries;
//    mf_readblock_t payload;
//    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
//        for (tries = 0; tries < MIFARE_SECTOR_RETRY; tries++) {
//            PrintAndLogEx(NORMAL, "-" NOLF);
//            fflush(stdout);
//
//            payload.blockno = mfFirstBlockOfSector(sectorNo) + mfNumBlocksPerSector(sectorNo) - 1;
//            payload.keytype = MF_KEY_A;
//            memcpy(payload.key, keyA[sectorNo], sizeof(payload.key));
//
//            clearCommandBuffer();
//            SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
//
//            if (WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) {
//
//                uint8_t *data = resp.data.asBytes;
//                if (resp.status == PM3_SUCCESS) {
//                    rights[sectorNo][0] = ((data[7] & 0x10) >> 2) | ((data[8] & 0x1) << 1) | ((data[8] & 0x10) >> 4); // C1C2C3 for data area 0
//                    rights[sectorNo][1] = ((data[7] & 0x20) >> 3) | ((data[8] & 0x2) << 0) | ((data[8] & 0x20) >> 5); // C1C2C3 for data area 1
//                    rights[sectorNo][2] = ((data[7] & 0x40) >> 4) | ((data[8] & 0x4) >> 1) | ((data[8] & 0x40) >> 6); // C1C2C3 for data area 2
//                    rights[sectorNo][3] = ((data[7] & 0x80) >> 5) | ((data[8] & 0x8) >> 2) | ((data[8] & 0x80) >> 7); // C1C2C3 for sector trailer
//                    break;
//                } else if (tries == 2) { // on last try set defaults
//                    PrintAndLogEx(FAILED, "\ncould not get access rights for sector %2d. Trying with defaults...", sectorNo);
//                    rights[sectorNo][0] = rights[sectorNo][1] = rights[sectorNo][2] = 0x00;
//                    rights[sectorNo][3] = 0x01;
//                }
//            } else {
//                PrintAndLogEx(FAILED, "\ncommand execute timeout when trying to read access rights for sector %2d. Trying with defaults...", sectorNo);
//                rights[sectorNo][0] = rights[sectorNo][1] = rights[sectorNo][2] = 0x00;
//                rights[sectorNo][3] = 0x01;
//            }
//        }
//    }
//    PrintAndLogEx(NORMAL, "");
//    PrintAndLogEx(SUCCESS, "Finished reading sector access bits");
//    PrintAndLogEx(INFO, "Dumping all blocks from card...");
//
//    for (sectorNo = 0; sectorNo < numSectors; sectorNo++) {
//        for (blockNo = 0; blockNo < mfNumBlocksPerSector(sectorNo); blockNo++) {
//            bool received = false;
//
//            for (tries = 0; tries < MIFARE_SECTOR_RETRY; tries++) {
//                if (blockNo == mfNumBlocksPerSector(sectorNo) - 1) { // sector trailer. At least the Access Conditions can always be read with key A.
//
//                    payload.blockno = mfFirstBlockOfSector(sectorNo) + blockNo;
//                    payload.keytype = MF_KEY_A;
//                    memcpy(payload.key, keyA[sectorNo], sizeof(payload.key));
//
//                    clearCommandBuffer();
//                    SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
//                    received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
//                } else {                                           // data block. Check if it can be read with key A or key B
//                    uint8_t data_area = (sectorNo < 32) ? blockNo : blockNo / 5;
//                    if ((rights[sectorNo][data_area] == 0x03) || (rights[sectorNo][data_area] == 0x05)) { // only key B would work
//
//                        payload.blockno = mfFirstBlockOfSector(sectorNo) + blockNo;
//                        payload.keytype = 1;
//                        memcpy(payload.key, keyB[sectorNo], sizeof(payload.key));
//
//                        clearCommandBuffer();
//                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
//                        received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
//                    } else if (rights[sectorNo][data_area] == 0x07) {                                     // no key would work
//                        PrintAndLogEx(WARNING, "access rights do not allow reading of sector %2d block %3d", sectorNo, blockNo);
//                        // where do you want to go??  Next sector or block?
//                        break;
//                    } else {                                                                              // key A would work
//
//                        payload.blockno = mfFirstBlockOfSector(sectorNo) + blockNo;
//                        payload.keytype = MF_KEY_A;
//                        memcpy(payload.key, keyA[sectorNo], sizeof(payload.key));
//
//                        clearCommandBuffer();
//                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));
//                        received = WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500);
//                    }
//                }
//                if (received) {
//                    if (resp.status == PM3_SUCCESS) {
//                        // break the re-try loop
//                        break;
//                    }
//                }
//            }
//
//            if (received) {
//                uint8_t *data  = resp.data.asBytes;
//                if (blockNo == mfNumBlocksPerSector(sectorNo) - 1) { // sector trailer. Fill in the keys.
//                    data[0]  = (keyA[sectorNo][0]);
//                    data[1]  = (keyA[sectorNo][1]);
//                    data[2]  = (keyA[sectorNo][2]);
//                    data[3]  = (keyA[sectorNo][3]);
//                    data[4]  = (keyA[sectorNo][4]);
//                    data[5]  = (keyA[sectorNo][5]);
//
//                    data[10] = (keyB[sectorNo][0]);
//                    data[11] = (keyB[sectorNo][1]);
//                    data[12] = (keyB[sectorNo][2]);
//                    data[13] = (keyB[sectorNo][3]);
//                    data[14] = (keyB[sectorNo][4]);
//                    data[15] = (keyB[sectorNo][5]);
//                }
//                if (resp.status == PM3_SUCCESS) {
//                    memcpy(carddata[mfFirstBlockOfSector(sectorNo) + blockNo], data, 16);
//                    PrintAndLogEx(SUCCESS, "successfully read block %2d of sector %2d.", blockNo, sectorNo);
//                } else {
//                    PrintAndLogEx(FAILED, "could not read block %2d of sector %2d", blockNo, sectorNo);
//                    break;
//                }
//            } else {
//                PrintAndLogEx(WARNING, "command execute timeout when trying to read block %2d of sector %2d.", blockNo, sectorNo);
//                break;
//            }
//        }
//    }
//
//    PrintAndLogEx(SUCCESS, "time: %" PRIu64 " seconds\n", (msclock() - t1) / 1000);
//
//    PrintAndLogEx(SUCCESS, "\nSucceeded in dumping all blocks");
//
//    if (strlen(dataFilename) < 1) {
//        fptr = GenerateFilename("hf-mf-", "-dump");
//        if (fptr == NULL)
//            return PM3_ESOFT;
//
//        strcpy(dataFilename, fptr);
//        free(fptr);
//    }
//
//    uint16_t bytes = 16 * (mfFirstBlockOfSector(numSectors - 1) + mfNumBlocksPerSector(numSectors - 1));
//
//    saveFile(dataFilename, ".bin", (uint8_t *)carddata, bytes);
//    saveFileEML(dataFilename, (uint8_t *)carddata, bytes, MFBLOCK_SIZE);
//
//    iso14a_mf_extdump_t xdump;
//    xdump.card_info = card;
//    xdump.dump = (uint8_t *)carddata;
//    xdump.dumplen = bytes;
//    saveFileJSON(dataFilename, jsfCardMemory, (uint8_t *)&xdump, sizeof(xdump), NULL);
//    return PM3_SUCCESS;
//}

/*static int CmdHF14AMfRestore(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf restore",
                  "Restore MIFARE Classic dump file to tag.\n"
                  "\n"
                  "The key file and dump file will program the card sector trailers.\n"
                  "By default we authenticate to card with key B 0xFFFFFFFFFFFF.\n"
                  "If access rights in dump file is all zeros,  it will be replaced with default values\n"
                  "\n"
                  "`--uid` param is used for filename templates `hf-mf-<uid>-dump.bin` and `hf-mf-<uid>-key.bin.\n"
                  "        If not specified, it will read the card uid instead.\n"
                  " `--ka` param you can indicate that the key file should be used for authentication instead.\n"
                  "        if so we also try both B/A keys",
                  "hf mf restore\n"
                  "hf mf restore --1k --uid 04010203\n"
                  "hf mf restore --1k --uid 04010203 -k hf-mf-AABBCCDD-key.bin\n"
                  "hf mf restore --4k"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_str0("u", "uid",  "<hex>", "uid, (4|7|10 hex bytes)"),
        arg_str0("f", "file", "<fn>", "specify dump filename (bin/eml/json)"),
        arg_str0("k", "kfn",  "<fn>", "key filename"),
        arg_lit0(NULL, "ka",  "use specified keyfile to authenticate"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);
    bool m2 = arg_get_lit(ctx, 3);
    bool m4 = arg_get_lit(ctx, 4);

    int uidlen = 0;
    char uid[20] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 5), (uint8_t *)uid, sizeof(uid), &uidlen);

    int datafnlen = 0;
    char datafilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 6), (uint8_t *)datafilename, FILE_PATH_SIZE, &datafnlen);

    int keyfnlen = 0;
    char keyfilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 7), (uint8_t *)keyfilename, FILE_PATH_SIZE, &keyfnlen);

    bool use_keyfile_for_auth = arg_get_lit(ctx, 8);
    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectors = MIFARE_1K_MAXSECTOR;

    if (m0) {
        sectors = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectors = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectors = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectors = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    // if user specified UID,  use it in file templates
    if (uidlen) {

        if (keyfnlen == 0) {
            snprintf(keyfilename, FILE_PATH_SIZE, "hf-mf-%s-key.bin", uid);
            keyfnlen = strlen(keyfilename);
        }

        if (datafnlen == 0) {
            snprintf(datafilename, FILE_PATH_SIZE, "hf-mf-%s-dump.bin", uid);
            datafnlen = strlen(datafilename);
        }
    }

    // try reading card uid and create filename
    if (keyfnlen == 0) {
        char *fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(keyfilename, fptr);
        free(fptr);
    }

    FILE *f;
    if ((f = fopen(keyfilename, "rb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not find file " _YELLOW_("%s"), keyfilename);
        return PM3_EFILE;
    }

    // key arrays
    uint8_t keyA[40][6];
    uint8_t keyB[40][6];

    // read key file
    size_t bytes_read;
    for (uint8_t s = 0; s < sectors; s++) {
        bytes_read = fread(keyA[s], 1, 6, f);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error  " _YELLOW_("%s"), keyfilename);
            fclose(f);
            return PM3_EFILE;
        }
    }

    for (uint8_t s = 0; s < sectors; s++) {
        bytes_read = fread(keyB[s], 1, 6, f);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error " _YELLOW_("%s"), keyfilename);
            fclose(f);
            return PM3_EFILE;
        }
    }
    fclose(f);

    // try reading card uid and create filename
    if (datafnlen == 0) {
        char *fptr = GenerateFilename("hf-mf-", "-dump.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(datafilename, fptr);
        free(fptr);
    }

    // read dump file
    uint8_t *dump = NULL;
    bytes_read = 0;
    int res = pm3_load_dump(datafilename, (void **)&dump, &bytes_read, (MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK));
    if (res != PM3_SUCCESS) {
        return res;
    }

    // default authentication key
    uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    PrintAndLogEx(INFO, "Restoring " _YELLOW_("%s")" to card", datafilename);

    // main loop for restoreing.
    // a bit more complicated than needed
    // this is because of two things.
    // 1. we are setting keys from a key file or using the existing ones in the dump
    // 2. we need to authenticate against a card which might not have default keys.
    uint8_t *ref_dump = dump;
    for (uint8_t s = 0; s < sectors; s++) {
        for (uint8_t b = 0; b < mfNumBlocksPerSector(s); b++) {

            uint8_t bldata[MFBLOCK_SIZE] = {0x00};

            memcpy(bldata, dump, MFBLOCK_SIZE);

            // if sector trailer
            if (mfNumBlocksPerSector(s) - 1 == b) {
                if (use_keyfile_for_auth == false) {
                    // replace KEY A
                    bldata[0]  = (keyA[s][0]);
                    bldata[1]  = (keyA[s][1]);
                    bldata[2]  = (keyA[s][2]);
                    bldata[3]  = (keyA[s][3]);
                    bldata[4]  = (keyA[s][4]);
                    bldata[5]  = (keyA[s][5]);
                    // replace KEY B
                    bldata[10] = (keyB[s][0]);
                    bldata[11] = (keyB[s][1]);
                    bldata[12] = (keyB[s][2]);
                    bldata[13] = (keyB[s][3]);
                    bldata[14] = (keyB[s][4]);
                    bldata[15] = (keyB[s][5]);
                }

                // ensure access right isn't messed up.
                if (mfValidateAccessConditions(&bldata[6]) == false) {
                    PrintAndLogEx(WARNING, "Invalid Access Conditions on sector %i, replacing by default values", s);
                    memcpy(bldata + 6, "\xFF\x07\x80\x69", 4);
                }
            }

            if (bytes_read) {
                dump += MFBLOCK_SIZE;
                bytes_read -= MFBLOCK_SIZE;
            }

            uint8_t wdata[26];
            memcpy(wdata + 10, bldata, sizeof(bldata));

            if (use_keyfile_for_auth) {
                for (int8_t kt = MF_KEY_B; kt > -1; kt--) {

                    if (kt == MF_KEY_A)
                        memcpy(wdata, keyA[s], 6);
                    else
                        memcpy(wdata, keyB[s], 6);

                    PrintAndLogEx(INFO, "block %3d: %s", mfFirstBlockOfSector(s) + b, sprint_hex(bldata, sizeof(bldata)));

                    clearCommandBuffer();
                    SendCommandMIX(CMD_HF_MIFARE_WRITEBL, mfFirstBlockOfSector(s) + b, kt, 0, wdata, sizeof(wdata));
                    PacketResponseNG resp;
                    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                        uint8_t isOK  = resp.oldarg[0] & 0xff;
                        if (isOK == 0) {
                            if (b == 0) {
                                PrintAndLogEx(INFO, "Writing to manufacture block w key %c ( " _RED_("fail") " )", (kt == MF_KEY_A) ? 'A' : 'B');
                            } else {
                                PrintAndLogEx(FAILED, "Write to block %u w key %c ( " _RED_("fail") " ) ", b, (kt == MF_KEY_A) ? 'A' : 'B');
                            }
                        } else {
                            // if success,  skip to next block
                            break;
                        }
                    } else {
                        PrintAndLogEx(WARNING, "Command execute timeout");
                    }
                }
            } else {
                // use default key to authenticate for the write command
                memcpy(wdata, default_key, 6);
                PrintAndLogEx(INFO, "block %3d: %s", mfFirstBlockOfSector(s) + b, sprint_hex(bldata, sizeof(bldata)));

                clearCommandBuffer();
                SendCommandMIX(CMD_HF_MIFARE_WRITEBL, mfFirstBlockOfSector(s) + b, MF_KEY_B, 0, wdata, sizeof(wdata));
                PacketResponseNG resp;
                if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                    uint8_t isOK  = resp.oldarg[0] & 0xff;
                    if (isOK == 0) {
                        if (b == 0) {
                            PrintAndLogEx(INFO, "Writing to manufacture block w key B ( " _RED_("fail") " )");
                        } else {
                            PrintAndLogEx(FAILED, "Write to block %u w key B ( " _RED_("fail") " )", b);
                        }
                    }
                } else {
                    PrintAndLogEx(WARNING, "Command execute timeout");
                }
            } // end use_keyfile_for_auth
        } // end loop B
    } // end loop S

    free(ref_dump);
    PrintAndLogEx(INFO, "Done!");
    return PM3_SUCCESS;
}*/

static int CmdHF14AMfRestore(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf restore",
                  "Restore MIFARE Classic dump file to tag.\n"
                  "\n"
                  "The key file and dump file will program the card sector trailers.\n"
                  "By default we authenticate to card with key B 0xFFFFFFFFFFFF.\n"
                  "If access rights in dump file is all zeros,  it will be replaced with default values\n"
                  "\n"
                  "`--uid` param is used for filename templates `hf-mf-<uid>-dump.bin` and `hf-mf-<uid>-key.bin.\n"
                  "          if not specified, it will read the card uid instead.\n"
                  " `--ka` param you can indicate that the key file should be used for authentication instead.\n"
                  "          if so we also try both B/A keys\n"
                  "`--force` param is used to override warnings and allow bad ACL block writes.\n"
                  "          if not specified, it will skip blocks with bad ACL.\n",
                  "hf mf restore\n"
                  "hf mf restore --1k --uid 04010203\n"
                  "hf mf restore --1k --uid 04010203 -k hf-mf-AABBCCDD-key.bin\n"
                  "hf mf restore --4k"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_str0("u", "uid",  "<hex>", "uid, (4|7|10 hex bytes)"),
        arg_str0("f", "file", "<fn>", "specify dump filename (bin/eml/json)"),
        arg_str0("k", "kfn",  "<fn>", "key filename"),
        arg_lit0(NULL, "ka",  "use specified keyfile to authenticate"),
        arg_lit0(NULL, "force", "override warnings"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);
    bool m2 = arg_get_lit(ctx, 3);
    bool m4 = arg_get_lit(ctx, 4);

    int uidlen = 0;
    char uid[20] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 5), (uint8_t *)uid, sizeof(uid), &uidlen);

    int datafnlen = 0;
    char datafilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 6), (uint8_t *)datafilename, FILE_PATH_SIZE, &datafnlen);

    int keyfnlen = 0;
    char keyfilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 7), (uint8_t *)keyfilename, FILE_PATH_SIZE, &keyfnlen);

    bool use_keyfile_for_auth = arg_get_lit(ctx, 8);
    bool force = arg_get_lit(ctx, 9);

    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectors = MIFARE_1K_MAXSECTOR;

    if (m0) {
        sectors = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectors = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectors = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectors = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    // if user specified UID,  use it in file templates
    if (uidlen) {

        if (keyfnlen == 0) {
            snprintf(keyfilename, FILE_PATH_SIZE, "hf-mf-%s-key.bin", uid);
            keyfnlen = strlen(keyfilename);
        }

        if (datafnlen == 0) {
            snprintf(datafilename, FILE_PATH_SIZE, "hf-mf-%s-dump.bin", uid);
            datafnlen = strlen(datafilename);
        }
    }

    // try reading card uid and create filename
    if (keyfnlen == 0) {
        char *fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(keyfilename, fptr);
        free(fptr);
    }

    FILE *f;
    if ((f = fopen(keyfilename, "rb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not find file " _YELLOW_("%s"), keyfilename);
        return PM3_EFILE;
    }

    // key arrays
    uint8_t keyA[40][6];
    uint8_t keyB[40][6];

    // read key file
    size_t bytes_read;
    for (uint8_t s = 0; s < sectors; s++) {
        bytes_read = fread(keyA[s], 1, 6, f);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error  " _YELLOW_("%s"), keyfilename);
            fclose(f);
            return PM3_EFILE;
        }
    }

    for (uint8_t s = 0; s < sectors; s++) {
        bytes_read = fread(keyB[s], 1, 6, f);
        if (bytes_read != 6) {
            PrintAndLogEx(ERR, "File reading error " _YELLOW_("%s"), keyfilename);
            fclose(f);
            return PM3_EFILE;
        }
    }
    fclose(f);

    // try reading card uid and create filename
    if (datafnlen == 0) {
        char *fptr = GenerateFilename("hf-mf-", "-dump.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(datafilename, fptr);
        free(fptr);
    }

    // read dump file
    uint8_t *dump = NULL;
    bytes_read = 0;
    int res = pm3_load_dump(datafilename, (void **)&dump, &bytes_read, (MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK));
    if (res != PM3_SUCCESS) {
        return res;
    }

    // default authentication key
    uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    PrintAndLogEx(INFO, "Restoring " _YELLOW_("%s")" to card", datafilename);

    // main loop for restoring.
    // a bit more complicated than needed
    // this is because of two things.
    // 1. we are setting keys from a key file or using the existing ones in the dump
    // 2. we need to authenticate against a card which might not have default keys.
    uint8_t *ref_dump = dump;
    for (uint8_t s = 0; s < sectors; s++) {
        for (uint8_t b = 0; b < mfNumBlocksPerSector(s); b++) {

            uint8_t bldata[MFBLOCK_SIZE] = {0x00};
            memcpy(bldata, dump, MFBLOCK_SIZE);

            // if sector trailer
            if (mfNumBlocksPerSector(s) - 1 == b) {
                if (use_keyfile_for_auth == false) {
                    // replace KEY A
                    bldata[0]  = (keyA[s][0]);
                    bldata[1]  = (keyA[s][1]);
                    bldata[2]  = (keyA[s][2]);
                    bldata[3]  = (keyA[s][3]);
                    bldata[4]  = (keyA[s][4]);
                    bldata[5]  = (keyA[s][5]);
                    // replace KEY B
                    bldata[10] = (keyB[s][0]);
                    bldata[11] = (keyB[s][1]);
                    bldata[12] = (keyB[s][2]);
                    bldata[13] = (keyB[s][3]);
                    bldata[14] = (keyB[s][4]);
                    bldata[15] = (keyB[s][5]);
                }

                // ensure access right isn't messed up.
                if (mfValidateAccessConditions(&bldata[6]) == false) {
                    PrintAndLogEx(WARNING, "Invalid Access Conditions on sector %i, replacing with default values", s);
                    memcpy(bldata + 6, "\xFF\x07\x80\x69", 4);
                }

                // Warn if ACL is strict read-only
                for (uint8_t foo = 0; foo < mfNumBlocksPerSector(s); foo++) {
                    if (mfReadOnlyAccessConditions(foo, &bldata[6])) {
                        PrintAndLogEx(WARNING, "Strict ReadOnly Access Conditions on block " _YELLOW_("%u") " detected", foo);

                        // if --force isn't used, skip writing this block
                        if (force == false) {
                            PrintAndLogEx(INFO, "Skipping,  use `" _YELLOW_("--force") "` to override and write this data");
                            continue;
                        }
                    }
                }
            }

            if (bytes_read) {
                dump += MFBLOCK_SIZE;
                bytes_read -= MFBLOCK_SIZE;
            }

            uint8_t wdata[26];
            memcpy(wdata + 10, bldata, sizeof(bldata));

            if (use_keyfile_for_auth) {
                for (int8_t kt = MF_KEY_B; kt > -1; kt--) {

                    if (kt == MF_KEY_A)
                        memcpy(wdata, keyA[s], 6);
                    else
                        memcpy(wdata, keyB[s], 6);

                    PrintAndLogEx(INFO, "block %3d: %s", mfFirstBlockOfSector(s) + b, sprint_hex(bldata, sizeof(bldata)));

                    clearCommandBuffer();
                    SendCommandMIX(CMD_HF_MIFARE_WRITEBL, mfFirstBlockOfSector(s) + b, kt, 0, wdata, sizeof(wdata));
                    PacketResponseNG resp;
                    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                        uint8_t isOK  = resp.oldarg[0] & 0xff;
                        if (isOK == 0) {
                            if (b == 0) {
                                PrintAndLogEx(INFO, "Writing to manufacture block w/ key %c ( " _RED_("fail") " )", (kt == MF_KEY_A) ? 'A' : 'B');
                            } else {
                                PrintAndLogEx(FAILED, "Write to block %u w key %c ( " _RED_("fail") " ) ", b, (kt == MF_KEY_A) ? 'A' : 'B');
                            }
                        } else {
                            // if success,  skip to next block
                            break;
                        }
                    } else {
                        PrintAndLogEx(WARNING, "Command execute timeout");
                    }
                }
            } else {
                // use default key to authenticate for the write command
                memcpy(wdata, default_key, 6);
                PrintAndLogEx(INFO, "block %3d: %s", mfFirstBlockOfSector(s) + b, sprint_hex(bldata, sizeof(bldata)));

                clearCommandBuffer();
                SendCommandMIX(CMD_HF_MIFARE_WRITEBL, mfFirstBlockOfSector(s) + b, MF_KEY_B, 0, wdata, sizeof(wdata));
                PacketResponseNG resp;
                if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                    uint8_t isOK  = resp.oldarg[0] & 0xff;
                    if (isOK == 0) {
                        if (b == 0) {
                            PrintAndLogEx(INFO, "Writing to manufacture block w key B ( " _RED_("fail") " )");
                        } else {
                            PrintAndLogEx(FAILED, "Write to block %u w key B ( " _RED_("fail") " )", b);
                        }
                    }
                } else {
                    PrintAndLogEx(WARNING, "Command execute timeout");
                }
            } // end use_keyfile_for_auth
        } // end loop B
    } // end loop S

    free(ref_dump);
    PrintAndLogEx(INFO, "Done!");
    return PM3_SUCCESS;
}

static int CmdHF14AMfNested(const char *Cmd) { //TODO: single mode broken? can't find keys...
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf nested",
                  "Execute Nested attack against MIFARE Classic card for key recovery",
                  "hf mf nested --blk 0 -a -k FFFFFFFFFFFF --tblk 4 --ta           --> Use block 0 Key A to find block 4 Key A (single sector key recovery)\n"
                  "hf mf nested --mini --blk 0 -a -k FFFFFFFFFFFF                  --> Key recovery against MIFARE Mini\n"
                  "hf mf nested --1k --blk 0 -a -k FFFFFFFFFFFF                    --> Key recovery against MIFARE Classic 1k\n"
                  "hf mf nested --2k --blk 0 -a -k FFFFFFFFFFFF                    --> Key recovery against MIFARE 2k\n"
                  "hf mf nested --4k --blk 0 -a -k FFFFFFFFFFFF                    --> Key recovery against MIFARE 4k");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k", "key", "<hex>", "Key specified as 12 hex symbols"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_int0("n", "blk", "<dec>", "Input block number"),
        arg_lit0("a", NULL, "Input key specified is A key (default)"),
        arg_lit0("b", NULL, "Input key specified is B key"),
        arg_int0(NULL, "tblk", "<dec>", "Target block number"),
        arg_lit0(NULL, "ta", "Target A key (default)"),
        arg_lit0(NULL, "tb", "Target B key"),
        arg_lit0(NULL, "emu", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dump", "Dump found keys to file"),
        arg_lit0(NULL, "mem", "Use dictionary from flashmemory"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    uint8_t blockNo = arg_get_u32_def(ctx, 6, 0);

    uint8_t keyType = MF_KEY_A;

    if (arg_get_lit(ctx, 7) && arg_get_lit(ctx, 8)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 8)) {
        keyType = MF_KEY_B;
    }

    int trgBlockNo = arg_get_int_def(ctx, 9, -1);

    uint8_t trgKeyType = MF_KEY_A;

    if (arg_get_lit(ctx, 10) && arg_get_lit(ctx, 11)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Target key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 11)) {
        trgKeyType = MF_KEY_B;
    }

    bool transferToEml = arg_get_lit(ctx, 12);
    bool createDumpFile = arg_get_lit(ctx, 13);
    bool singleSector = trgBlockNo > -1;
    bool use_flashmemory = arg_get_lit(ctx, 14);

    CLIParserFree(ctx);

    //validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }

    uint8_t SectorsCnt = 1;
    if (m0) {
        SectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        SectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        SectorsCnt = MIFARE_4K_MAXSECTOR;
    }

    if (singleSector) {
        uint8_t MinSectorsCnt = 0;
        // find a MIFARE type that can accommodate the provided block number
        uint8_t s = MAX(mfSectorNum(trgBlockNo), mfSectorNum(blockNo));
        if (s < MIFARE_MINI_MAXSECTOR) {
            MinSectorsCnt = MIFARE_MINI_MAXSECTOR;
        } else if (s < MIFARE_1K_MAXSECTOR) {
            MinSectorsCnt = MIFARE_1K_MAXSECTOR;
        } else if (s < MIFARE_2K_MAXSECTOR) {
            MinSectorsCnt = MIFARE_2K_MAXSECTOR;
        } else if (s < MIFARE_4K_MAXSECTOR) {
            MinSectorsCnt = MIFARE_4K_MAXSECTOR;
        } else {
            PrintAndLogEx(WARNING, "Provided block out of possible MIFARE Type memory map");
            return PM3_EINVARG;
        }
        if (SectorsCnt == 1) {
            SectorsCnt = MinSectorsCnt;
        } else if (SectorsCnt < MinSectorsCnt) {
            PrintAndLogEx(WARNING, "Provided block out of provided MIFARE Type memory map");
            return PM3_EINVARG;
        }
    }
    if (SectorsCnt == 1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    }

    if (keylen != 6) {
        PrintAndLogEx(WARNING, "Input key must include 12 HEX symbols");
        return PM3_EINVARG;
    }

    sector_t *e_sector = NULL;
    uint8_t keyBlock[(ARRAYLEN(g_mifare_default_keys) + 1) * 6];
    uint64_t key64 = 0;

    // check if tag doesn't have static nonce
    if (detect_classic_static_nonce() == NONCE_STATIC) {
        PrintAndLogEx(WARNING, "Static nonce detected. Quitting...");
        PrintAndLogEx(INFO, "\t Try use " _YELLOW_("`hf mf staticnested`"));
        return PM3_EOPABORTED;
    }

    // check if we can authenticate to sector
    if (mfCheckKeys(blockNo, keyType, true, 1, key, &key64) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "Wrong key. Can't authenticate to block:%3d key type:%c", blockNo, keyType ? 'B' : 'A');
        return PM3_EOPABORTED;
    }

    if (singleSector) {
        int16_t isOK = mfnested(blockNo, keyType, key, trgBlockNo, trgKeyType, keyBlock, true);
        switch (isOK) {
            case PM3_ETIMEOUT:
                PrintAndLogEx(ERR, "Command execute timeout\n");
                break;
            case PM3_EOPABORTED:
                PrintAndLogEx(WARNING, "Button pressed. Aborted.\n");
                break;
            case PM3_EFAILED:
                PrintAndLogEx(FAILED, "Tag isn't vulnerable to Nested Attack (PRNG is not predictable).\n");
                break;
            case PM3_ESOFT:
                PrintAndLogEx(FAILED, "No valid key found");
                break;
            case PM3_SUCCESS:
                key64 = bytes_to_num(keyBlock, 6);

                // transfer key to the emulator
                if (transferToEml) {
                    uint8_t sectortrailer;

                    if (trgBlockNo < 32 * 4) {  // 4 block sector
                        sectortrailer = trgBlockNo | 0x03;
                    } else {                    // 16 block sector
                        sectortrailer = trgBlockNo | 0x0f;
                    }
                    mfEmlGetMem(keyBlock, sectortrailer, 1);

                    if (trgKeyType == MF_KEY_A)
                        num_to_bytes(key64, 6, keyBlock);
                    else
                        num_to_bytes(key64, 6, &keyBlock[10]);

                    mfEmlSetMem(keyBlock, sectortrailer, 1);
                    PrintAndLogEx(SUCCESS, "Key transferred to emulator memory.");
                }
                return PM3_SUCCESS;
            default :
                PrintAndLogEx(ERR, "Unknown error.\n");
        }
        return PM3_SUCCESS;
    } else { // ------------------------------------  multiple sectors working
        uint64_t t1 = msclock();

        e_sector = calloc(SectorsCnt, sizeof(sector_t));
        if (e_sector == NULL) return PM3_EMALLOC;

        // add our known key
        e_sector[mfSectorNum(blockNo)].foundKey[keyType] = 1;
        e_sector[mfSectorNum(blockNo)].Key[keyType] = key64;

        //test current key and additional standard keys first
        // add parameter key
        memcpy(keyBlock + (ARRAYLEN(g_mifare_default_keys) * 6), key, 6);

        for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++) {
            num_to_bytes(g_mifare_default_keys[cnt], 6, (uint8_t *)(keyBlock + cnt * 6));
        }

        PrintAndLogEx(SUCCESS, "Testing known keys. Sector count "_YELLOW_("%d"), SectorsCnt);
        int res = mfCheckKeys_fast(SectorsCnt, true, true, 1, ARRAYLEN(g_mifare_default_keys) + 1, keyBlock, e_sector, use_flashmemory);
        if (res == PM3_SUCCESS) {
            PrintAndLogEx(SUCCESS, "Fast check found all keys");
            goto jumptoend;
        }

        uint64_t t2 = msclock() - t1;
        PrintAndLogEx(SUCCESS, "Time to check " _YELLOW_("%zu") " known keys: %.0f seconds\n", ARRAYLEN(g_mifare_default_keys), (float)t2 / 1000.0);
        PrintAndLogEx(SUCCESS, "enter nested key recovery");

        // nested sectors
        bool calibrate = true;

        for (trgKeyType = MF_KEY_A; trgKeyType <= MF_KEY_B; ++trgKeyType) {
            for (uint8_t sectorNo = 0; sectorNo < SectorsCnt; ++sectorNo) {
                for (int i = 0; i < MIFARE_SECTOR_RETRY; i++) {

                    if (e_sector[sectorNo].foundKey[trgKeyType]) continue;

                    int16_t isOK = mfnested(blockNo, keyType, key, mfFirstBlockOfSector(sectorNo), trgKeyType, keyBlock, calibrate);
                    switch (isOK) {
                        case PM3_ETIMEOUT:
                            PrintAndLogEx(ERR, "Command execute timeout\n");
                            break;
                        case PM3_EOPABORTED:
                            PrintAndLogEx(WARNING, "button pressed. Aborted.\n");
                            break;
                        case PM3_EFAILED :
                            PrintAndLogEx(FAILED, "Tag isn't vulnerable to Nested Attack (PRNG is not predictable).\n");
                            break;
                        case PM3_ESOFT:
                            //key not found
                            calibrate = false;
                            continue;
                        case PM3_SUCCESS:
                            calibrate = false;
                            e_sector[sectorNo].foundKey[trgKeyType] = 1;
                            e_sector[sectorNo].Key[trgKeyType] = bytes_to_num(keyBlock, 6);

                            mfCheckKeys_fast(SectorsCnt, true, true, 2, 1, keyBlock, e_sector, false);
                            continue;
                        default :
                            PrintAndLogEx(ERR, "Unknown error.\n");
                    }
                    free(e_sector);
                    return PM3_ESOFT;
                }
            }
        }

        t1 = msclock() - t1;
        PrintAndLogEx(SUCCESS, "time in nested " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);


        // 20160116 If Sector A is found, but not Sector B,  try just reading it of the tag?
        PrintAndLogEx(INFO, "trying to read key B...");
        for (int i = 0; i < SectorsCnt; i++) {
            // KEY A but not KEY B
            if (e_sector[i].foundKey[0] && !e_sector[i].foundKey[1]) {

                uint8_t sectrail = (mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1);

                PrintAndLogEx(SUCCESS, "reading block %d", sectrail);

                mf_readblock_t payload;
                payload.blockno = sectrail;
                payload.keytype = MF_KEY_A;

                num_to_bytes(e_sector[i].Key[0], 6, payload.key); // KEY A

                clearCommandBuffer();
                SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

                PacketResponseNG resp;
                if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) continue;

                if (resp.status != PM3_SUCCESS) continue;

                uint8_t *data = resp.data.asBytes;
                key64 = bytes_to_num(data + 10, 6);
                if (key64) {
                    PrintAndLogEx(SUCCESS, "data: %s", sprint_hex(data + 10, 6));
                    e_sector[i].foundKey[1] = true;
                    e_sector[i].Key[1] = key64;
                }
            }
        }

jumptoend:

        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

        //print them
        printKeyTable(SectorsCnt, e_sector);

        // transfer them to the emulator
        if (transferToEml) {
            // fast push mode
            g_conn.block_after_ACK = true;
            for (int i = 0; i < SectorsCnt; i++) {
                mfEmlGetMem(keyBlock, mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1, 1);

                if (e_sector[i].foundKey[0])
                    num_to_bytes(e_sector[i].Key[0], 6, keyBlock);

                if (e_sector[i].foundKey[1])
                    num_to_bytes(e_sector[i].Key[1], 6, &keyBlock[10]);

                if (i == SectorsCnt - 1) {
                    // Disable fast mode on last packet
                    g_conn.block_after_ACK = false;
                }
                mfEmlSetMem(keyBlock, mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1, 1);
            }
            PrintAndLogEx(SUCCESS, "keys transferred to emulator memory.");
        }

        // Create dump file
        if (createDumpFile) {
            char *fptr = GenerateFilename("hf-mf-", "-key.bin");
            if (createMfcKeyDump(fptr, SectorsCnt, e_sector) != PM3_SUCCESS) {
                PrintAndLogEx(ERR, "Failed to save keys to file");
                free(e_sector);
                free(fptr);
                return PM3_ESOFT;
            }
            free(fptr);
        }
        free(e_sector);
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfNestedStatic(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf staticnested",
                  "Execute Nested attack against MIFARE Classic card with static nonce for key recovery.\n"
                  "Supply a known key from one block to recover all keys",
                  "hf mf staticnested --mini --blk 0 -a -k FFFFFFFFFFFF\n"
                  "hf mf staticnested --1k --blk 0 -a -k FFFFFFFFFFFF\n"
                  "hf mf staticnested --2k --blk 0 -a -k FFFFFFFFFFFF\n"
                  "hf mf staticnested --4k --blk 0 -a -k FFFFFFFFFFFF\n");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k", "key", "<hex>", "Known key (12 hex symbols)"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_int0("n", "blk", "<dec>", "Input block number"),
        arg_lit0("a", NULL, "Input key specified is keyA (def)"),
        arg_lit0("b", NULL, "Input key specified is keyB"),
        arg_lit0(NULL, "emu", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dump", "Dump found keys to file"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    uint8_t blockNo = arg_get_u32_def(ctx, 6, 0);

    uint8_t keyType = MF_KEY_A;

    if (arg_get_lit(ctx, 7) && arg_get_lit(ctx, 8)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 8)) {
        keyType = MF_KEY_B;
    }

    bool transferToEml = arg_get_lit(ctx, 9);
    bool createDumpFile = arg_get_lit(ctx, 10);

    CLIParserFree(ctx);

    //validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }
    if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t SectorsCnt = 1;
    if (m0) {
        SectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        SectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        SectorsCnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    if (keylen != 6) {
        PrintAndLogEx(WARNING, "Input key must include 12 HEX symbols");
        return PM3_EINVARG;
    }

    sector_t *e_sector = NULL;

    uint8_t trgKeyType = MF_KEY_A;

    uint8_t keyBlock[(ARRAYLEN(g_mifare_default_keys) + 1) * 6];
    uint64_t key64 = 0;

    // check if tag have static nonce
    if (detect_classic_static_nonce() != NONCE_STATIC) {
        PrintAndLogEx(WARNING, "Normal nonce detected, or failed read of card. Quitting...");
        PrintAndLogEx(INFO, "\t Try use " _YELLOW_("`hf mf nested`"));
        return PM3_EOPABORTED;
    }

    // check if we can authenticate to sector
    if (mfCheckKeys(blockNo, keyType, true, 1, key, &key64) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "Wrong key. Can't authenticate to block: %3d key type: %c", blockNo, keyType ? 'B' : 'A');
        return PM3_EOPABORTED;
    }

    if (IfPm3Flash()) {
        PrintAndLogEx(INFO, "RDV4 with flashmemory supported detected.");
    }

    uint64_t t1 = msclock();

    e_sector = calloc(SectorsCnt, sizeof(sector_t));
    if (e_sector == NULL) return PM3_EMALLOC;

    // add our known key
    e_sector[mfSectorNum(blockNo)].foundKey[keyType] = 1;
    e_sector[mfSectorNum(blockNo)].Key[keyType] = key64;

    //test current key and additional standard keys first
    // add parameter key
    memcpy(keyBlock + (ARRAYLEN(g_mifare_default_keys) * 6), key, 6);

    for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++) {
        num_to_bytes(g_mifare_default_keys[cnt], 6, (uint8_t *)(keyBlock + cnt * 6));
    }

    PrintAndLogEx(SUCCESS, "Testing known keys. Sector count "_YELLOW_("%d"), SectorsCnt);
    int res = mfCheckKeys_fast(SectorsCnt, true, true, 1, ARRAYLEN(g_mifare_default_keys) + 1, keyBlock, e_sector, false);
    if (res == PM3_SUCCESS) {
        // all keys found
        PrintAndLogEx(SUCCESS, "Fast check found all keys");
        goto jumptoend;
    }

    uint64_t t2 = msclock() - t1;
    PrintAndLogEx(SUCCESS, "Time to check "_YELLOW_("%zu") " known keys: %.0f seconds\n", ARRAYLEN(g_mifare_default_keys), (float)t2 / 1000.0);
    PrintAndLogEx(SUCCESS, "enter static nested key recovery");

    // nested sectors
    for (trgKeyType = MF_KEY_A; trgKeyType <= MF_KEY_B; ++trgKeyType) {
        for (uint8_t sectorNo = 0; sectorNo < SectorsCnt; ++sectorNo) {

            for (int i = 0; i < 1; i++) {

                if (e_sector[sectorNo].foundKey[trgKeyType]) continue;

                int16_t isOK = mfStaticNested(blockNo, keyType, key, mfFirstBlockOfSector(sectorNo), trgKeyType, keyBlock);
                switch (isOK) {
                    case PM3_ETIMEOUT :
                        PrintAndLogEx(ERR, "Command execute timeout");
                        break;
                    case PM3_EOPABORTED :
                        PrintAndLogEx(WARNING, "aborted via keyboard.");
                        break;
                    case PM3_ESOFT :
                        continue;
                    case PM3_SUCCESS :
                        e_sector[sectorNo].foundKey[trgKeyType] = 1;
                        e_sector[sectorNo].Key[trgKeyType] = bytes_to_num(keyBlock, 6);

                        // mfCheckKeys_fast(SectorsCnt, true, true, 2, 1, keyBlock, e_sector, false);
                        continue;
                    default :
                        PrintAndLogEx(ERR, "unknown error.\n");
                }
                free(e_sector);
                return PM3_ESOFT;
            }
        }
    }

    t1 = msclock() - t1;
    PrintAndLogEx(SUCCESS, "time in static nested " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);


    // 20160116 If Sector A is found, but not Sector B,  try just reading it of the tag?
    PrintAndLogEx(INFO, "trying to read key B...");
    for (int i = 0; i < SectorsCnt; i++) {
        // KEY A but not KEY B
        if (e_sector[i].foundKey[0] && !e_sector[i].foundKey[1]) {

            uint8_t sectrail = (mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1);

            PrintAndLogEx(SUCCESS, "reading block %d", sectrail);

            mf_readblock_t payload;
            payload.blockno = sectrail;
            payload.keytype = MF_KEY_A;

            num_to_bytes(e_sector[i].Key[0], 6, payload.key); // KEY A

            clearCommandBuffer();
            SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

            PacketResponseNG resp;
            if (WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500) == false) {
                continue;
            }

            if (resp.status != PM3_SUCCESS) continue;

            uint8_t *data = resp.data.asBytes;
            key64 = bytes_to_num(data + 10, 6);
            if (key64) {
                PrintAndLogEx(SUCCESS, "data: %s", sprint_hex(data + 10, 6));
                e_sector[i].foundKey[1] = true;
                e_sector[i].Key[1] = key64;
            }
        }
    }

jumptoend:

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

    //print them
    printKeyTable(SectorsCnt, e_sector);

    // transfer them to the emulator
    if (transferToEml) {
        // fast push mode
        g_conn.block_after_ACK = true;
        for (int i = 0; i < SectorsCnt; i++) {
            mfEmlGetMem(keyBlock, mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1, 1);

            if (e_sector[i].foundKey[0])
                num_to_bytes(e_sector[i].Key[0], 6, keyBlock);

            if (e_sector[i].foundKey[1])
                num_to_bytes(e_sector[i].Key[1], 6, &keyBlock[10]);

            if (i == SectorsCnt - 1) {
                // Disable fast mode on last packet
                g_conn.block_after_ACK = false;
            }
            mfEmlSetMem(keyBlock, mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1, 1);
        }
        PrintAndLogEx(SUCCESS, "keys transferred to emulator memory.");
    }

    // Create dump file
    if (createDumpFile) {
        char *fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (createMfcKeyDump(fptr, SectorsCnt, e_sector) != PM3_SUCCESS) {
            PrintAndLogEx(ERR, "Failed to save keys to file");
            free(e_sector);
            free(fptr);
            return PM3_ESOFT;
        }
        free(fptr);
    }
    free(e_sector);

    return PM3_SUCCESS;
}

static int CmdHF14AMfNestedHarder(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf harder",
                  "Nested attack for hardened MIFARE Classic cards.\n",
                  "dfghjksydgdsugfcdsauydfg"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k",  "key",   "<hex>", "Key, 12 hex bytes"),   
        arg_int0("n", "blk",   "<dec>", "Input block number"),   
        arg_lit0("a",   NULL,            "Input key A (def)"),   
        arg_lit0("b",   NULL,            "Input key B"),
        arg_int0(NULL, "tblk",  "<dec>", "Target block number"),
        arg_lit0(NULL, "ta",             "Target key A"),
        arg_lit0(NULL, "tb",             "Target key B"),
        arg_str0(NULL, "tk",    "<hex>", "Target key, 12 hex bytes"),
        arg_str0("u",  "uid",   "<hex>", "R/W `hf-mf-<UID>-nonces.bin` instead of default name"),
        arg_str0("f",  "file",  "<fn>",  "R/W <name> instead of default name"),
        arg_lit0("r",  "read",           "Read `hf-mf-<UID>-nonces.bin` if tag present, otherwise `nonces.bin`, and start attack"),
        arg_lit0("s",  "slow",           "Slower acquisition (required by some non standard cards)"),
        arg_lit0("t",  "tests",          "Run tests"),
        arg_lit0("w",  "write",             "Acquire nonces and UID, and write them to file `hf-mf-<UID>-nonces.bin`"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),

        arg_lit0(NULL, "in", "None (use regular CPU instruction set)"),
        arg_lit0(NULL, "ia", "Use all instructions sets. sequentially"),
        arg_lit0(NULL, "ir", "Randomly select instruction set"),
        arg_lit0(NULL, "il", "Use last available SIMD from list"),
        arg_lit0(NULL, "if", "Use first available SIMD from list"),
#if defined(COMPILER_HAS_SIMD_X86)
        arg_lit0(NULL, "im", "MMX"),
        arg_lit0(NULL, "is", "SSE2"),
        arg_lit0(NULL, "ia", "AVX"),
        arg_lit0(NULL, "i2", "AVX2"),
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
        arg_lit0(NULL, "i5", "AVX512"),
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
        arg_lit0(NULL, "ie", "NEON"),
#endif
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    uint8_t blockno = arg_get_u32_def(ctx, 2, 0);

    bool all_simd = arg_get_lit(ctx, 21);
    bool rand_simd = arg_get_lit(ctx, 22);
    bool first_simd = arg_get_lit(ctx, 23);
    bool last_simd = arg_get_lit(ctx, 24);
    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 3) && arg_get_lit(ctx, 4)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 4)) {
        keytype = MF_KEY_B;
    }

    uint8_t trg_blockno = arg_get_u32_def(ctx, 5, 0);

    uint8_t trg_keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 6) && arg_get_lit(ctx, 7)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 7)) {
        trg_keytype = MF_KEY_B;
    }

    int trg_keylen = 0;
    int instridx = 0;
    int simd_sets = 0;
    bool has_neon = false;
    uint8_t trg_key[6] = {0};
    CLIGetHexWithReturn(ctx, 8, trg_key, &trg_keylen);

    int uidlen = 0;
    char uid[14] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 9), (uint8_t *)uid, sizeof(uid), &uidlen);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 10), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool nonce_file_read = arg_get_lit(ctx, 11);
    bool slow = arg_get_lit(ctx, 12);
    bool tests = arg_get_lit(ctx, 13);
    bool nonce_file_write = arg_get_lit(ctx, 14);
    bool m0 = arg_get_lit(ctx, 15);
    bool m1 = arg_get_lit(ctx, 16);
    bool m2 = arg_get_lit(ctx, 17);
    bool m4 = arg_get_lit(ctx, 18);
    bool in = arg_get_lit(ctx, 19);
#if defined(COMPILER_HAS_SIMD_X86)
    bool im = arg_get_lit(ctx, 25);
    bool is = arg_get_lit(ctx, 26);
    bool ia = arg_get_lit(ctx, 27);
    bool i2 = arg_get_lit(ctx, 28);
    simd_sets += 4;
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
    bool i5 = arg_get_lit(ctx, 29);
    simd_sets += 1;
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
    bool ie = arg_get_lit(ctx, 30);
    has_neon = true;
    simd_sets += 1;
#endif
    CLIParserFree(ctx);

    //SetSIMDInstr(SIMD_AUTO);

    //set_simd:
    if (last_simd){
        instridx = simd_sets;
        goto simd_start;
    }
    if (first_simd){
        #ifdef COMPILER_HAS_SIMD_AVX512
        instridx = 1;
        #else
        instridx = 2;
        #endif
        goto simd_start;
    }
    if (!all_simd){
        SetSIMDInstr(SIMD_AUTO);
        #if defined(COMPILER_HAS_SIMD_AVX512)
        if (i5)
            SetSIMDInstr(SIMD_AVX512);
        #endif
        #if defined(COMPILER_HAS_SIMD_X86)
        if (i2)
            SetSIMDInstr(SIMD_AVX2);
        if (ia)
            SetSIMDInstr(SIMD_AVX);
        if (is)
            SetSIMDInstr(SIMD_SSE2);
        if (im)
            SetSIMDInstr(SIMD_MMX);
        #endif
        #if defined(COMPILER_HAS_SIMD_NEON)
        if (ie)
            SetSIMDInstr(SIMD_NEON);
        #endif
        if (in){
            if (simd_sets == 1){
                SetSIMDInstr(SIMD_AUTO);
            }
            if (simd_sets == 0){
                SetSIMDInstr(SIMD_NONE);
            }
        }
    } else if (rand_simd){
        #ifdef COMPILER_HAS_SIMD_AVX512
        int low_simd_idx = 1;
        #else
        int low_simd_idx = 2;
        #endif
        switch(simd_sets){
            case 1:
            instridx = simd_sets;
            goto simd_start;
        case 4:
            instridx = 4; 
            break;
        case 5:
            if (has_neon){
                low_simd_idx = 2;
                instridx = simd_sets;
            }
            break;
        default:
            instridx = simd_sets;
            break;
        }
        srand(time(NULL));
        instridx = (rand() % (instridx - low_simd_idx + 1)) + low_simd_idx;
        goto simd_start;
    } else {
    simd_start:
        int simd_init = 0;
        #if defined(COMPILER_HAS_SIMD_AVX512)
        if (instridx == 1){
            SetSIMDInstr(SIMD_AVX512);
            simd_init += 1;
        }
        #endif
        #if defined(COMPILER_HAS_SIMD_X86)
        if (instridx == 2){
            SetSIMDInstr(SIMD_AVX2);
            simd_init += 1;
        }
        if (instridx == 3){
            SetSIMDInstr(SIMD_AVX);
            simd_init += 1;
        }
        if (instridx == 4){
            SetSIMDInstr(SIMD_SSE2);
            simd_init += 1;
        }
        if (instridx == 5){
            SetSIMDInstr(SIMD_MMX);
            simd_init += 1;
        }
        #endif
        #if defined(COMPILER_HAS_SIMD_NEON)
        if (instridx == 6){
            SetSIMDInstr(SIMD_NEON);
        }
        #endif
        if (simd_init == 0){
            SetSIMDInstr(SIMD_AUTO);
        }
    }
#if defined(COMPILER_HAS_SIMD_AVX512)
    if (i5)
        SetSIMDInstr(SIMD_AVX512);
#endif

#if defined(COMPILER_HAS_SIMD_X86)
    if (i2)
        SetSIMDInstr(SIMD_AVX2);
    if (ia)
        SetSIMDInstr(SIMD_AVX);
    if (is)
        SetSIMDInstr(SIMD_SSE2);
    if (im)
        SetSIMDInstr(SIMD_MMX);
#endif

#if defined(COMPILER_HAS_SIMD_NEON)
    if (ie)
        SetSIMDInstr(SIMD_NEON);
#endif

    if (in)
        SetSIMDInstr(SIMD_NONE);

    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }
    if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t SectorsCnt = 1;
    if (m0) {
        SectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        SectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        SectorsCnt = MIFARE_4K_MAXSECTOR;
    } 

    if (SectorsCnt){

    }

    bool know_target_key = (trg_keylen);

    if (nonce_file_read) {
        char *fptr = GenerateFilename("hf-mf-", "-nonces.bin");
        if (fptr == NULL)
            strncpy(filename, "nonces.bin", FILE_PATH_SIZE - 1);
        else
            strncpy(filename, fptr, FILE_PATH_SIZE - 1);
        free(fptr);
    }

    if (nonce_file_write) {
        char *fptr = GenerateFilename("hf-mf-", "-nonces.bin");
        if (fptr == NULL)
            return 1;
        strncpy(filename, fptr, FILE_PATH_SIZE - 1);
        free(fptr);
    }

    if (uidlen) {
        snprintf(filename, FILE_PATH_SIZE, "hf-mf-%s-nonces.bin", uid);
    }

    if (know_target_key == false && nonce_file_read == false) {

        // check if tag doesn't have static nonce
        if (detect_classic_static_nonce() == NONCE_STATIC) {
            PrintAndLogEx(WARNING, "Static nonce detected. Quitting...");
            PrintAndLogEx(HINT, "\tTry use `" _YELLOW_("hf mf staticnested") "`");
            return PM3_EOPABORTED;
            //int16_t isOK = mfStaticNested(blockNo, keyType, key, mfFirstBlockOfSector(sectorNo), trgKeyType, keyBlock);
            //switch (isOK) {
            //    case PM3_ETIMEOUT :
            //        PrintAndLogEx(ERR, "Command execute timeout");
            //        break;
            //    case PM3_EOPABORTED :
            //        PrintAndLogEx(WARNING, "aborted via keyboard.");
            //        break;
            //    case PM3_ESOFT :
            //        continue;
            //    case PM3_SUCCESS :
            //        e_sector[sectorNo].foundKey[trgKeyType] = 1;
            //        e_sector[sectorNo].Key[trgKeyType] = bytes_to_num(keyBlock, 6);

            //        // mfCheckKeys_fast(SectorsCnt, true, true, 2, 1, keyBlock, e_sector, false);
            //        continue;
            //    default :
            //        PrintAndLogEx(ERR, "unknown error.\n");
            //}
        }

        uint64_t key64 = 0;
        // check if we can authenticate to sector
        if (mfCheckKeys(blockno, keytype, true, 1, key, &key64) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "Key is wrong. Can't authenticate to block: %3d  key type: %c", blockno, (keytype == MF_KEY_B) ? 'B' : 'A');
            return PM3_EWRONGANSWER;
        }
    }

    PrintAndLogEx(INFO, "Target block no " _YELLOW_("%3d") ", target key type: " _YELLOW_("%c") ", known target key: " _YELLOW_("%02x%02x%02x%02x%02x%02x%s"),
                  trg_blockno,
                  (trg_keytype == MF_KEY_B) ? 'B' : 'A',
                  trg_key[0], trg_key[1], trg_key[2], trg_key[3], trg_key[4], trg_key[5],
                  know_target_key ? "" : " (not set)"
                 );
    PrintAndLogEx(INFO, "File action: " _YELLOW_("%s") ", Slow: " _YELLOW_("%s") ", Tests: " _YELLOW_("%d"),
                  nonce_file_write ? "write" : nonce_file_read ? "read" : "none",
                  slow ? "Yes" : "No",
                  tests);

    uint64_t foundkey = 0;
    int16_t isOK = mfnestedhard(blockno, keytype, key, trg_blockno, trg_keytype, know_target_key ? trg_key : NULL, nonce_file_read, nonce_file_write, slow, tests, &foundkey, filename);

    if ((tests == 0) && IfPm3Iso14443a()) {
        DropField();
    }

    if (isOK) {
        switch (isOK) {
            case 1 :
                PrintAndLogEx(ERR, "Error: No response from Proxmark3.\n");
                break;
            case 2 :
                PrintAndLogEx(NORMAL, "Button pressed. Aborted.\n");
                break;
            default :
                break;
        }
        return 2;
    }
    return PM3_SUCCESS;

    /*
    // create/initialize key storage structure
    sector_t *e_sector = NULL;
    int32_t res = initSectorTable(&e_sector, sectors_cnt);
    if (res != sectors_cnt) {
        free(e_sector);
        return PM3_EMALLOC;
    }*/

    /*if (eml) {
        g_conn.block_after_ACK = true;
        uint8_t block[16] = {0x00};
        for (int i = 0; i < SectorsCnt; ++i) {
            uint8_t blockno = mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1;
            mfEmlGetMem(block, blockno, 1);

            if (e_sector[i].foundKey[0])
                num_to_bytes(e_sector[i].Key[0], 6, block);

            if (e_sector[i].foundKey[1])
                num_to_bytes(e_sector[i].Key[1], 6, block + 10);

            if (i == SectorsCnt - 1) {
                // Disable fast mode on last packet
                g_conn.block_after_ACK = false;
            }
            mfEmlSetMem(block, blockno, 1);
        }
        PrintAndLogEx(SUCCESS, "Found keys have been transferred to the emulator memory");
    }*/
}
static int CmdHF14AMfNestedHard(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf hardnested",
                  "Nested attack for hardened MIFARE Classic cards.\n"
                  "`--i<X>`  set type of SIMD instructions. Without this flag programs autodetect it.\n"
                  " or \n"
                  "    hf mf hardnested -r --tk [known target key]\n"
                  "Add the known target key to check if it is present in the remaining key space\n"
                  "    hf mf hardnested --blk 0 -a -k A0A1A2A3A4A5 --tblk 4 --ta --tk FFFFFFFFFFFF\n"
                  ,
                  "hf mf hardnested --blk 0 -a -k FFFFFFFFFFFF --tblk 4 --ta\n"
                  "hf mf hardnested --blk 0 -a -k FFFFFFFFFFFF --tblk 4 --ta -w\n"
                  "hf mf hardnested --blk 0 -a -k FFFFFFFFFFFF --tblk 4 --ta -f nonces.bin -w -s\n"
                  "hf mf hardnested -r\n"
                  "hf mf hardnested -r --tk a0a1a2a3a4a5\n"
                  "hf mf hardnested -t --tk a0a1a2a3a4a5\n"
                  "hf mf hardnested --blk 0 -a -k a0a1a2a3a4a5 --tblk 4 --ta --tk FFFFFFFFFFFF"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k",  "key",   "<hex>", "Key, 12 hex bytes"),   
        arg_int0("n", "blk",   "<dec>", "Input block number"),   
        arg_lit0("a",   NULL,            "Input key A (def)"),   
        arg_lit0("b",   NULL,            "Input key B"),
        arg_int0(NULL, "tblk",  "<dec>", "Target block number"),
        arg_lit0(NULL, "ta",             "Target key A"),
        arg_lit0(NULL, "tb",             "Target key B"),
        arg_str0(NULL, "tk",    "<hex>", "Target key, 12 hex bytes"),
        arg_str0("u",  "uid",   "<hex>", "R/W `hf-mf-<UID>-nonces.bin` instead of default name"),
        arg_str0("f",  "file",  "<fn>",  "R/W <name> instead of default name"),
        arg_lit0("r",  "read",           "Read `hf-mf-<UID>-nonces.bin` if tag present, otherwise `nonces.bin`, and start attack"),
        arg_lit0("s",  "slow",           "Slower acquisition (required by some non standard cards)"),
        arg_lit0("t",  "tests",          "Run tests"),
        arg_lit0("w",  "write",             "Acquire nonces and UID, and write them to file `hf-mf-<UID>-nonces.bin`"),

        arg_lit0(NULL, "in", "None (use regular CPU instruction set)"),
#if defined(COMPILER_HAS_SIMD_X86)
        arg_lit0(NULL, "im", "MMX"),
        arg_lit0(NULL, "is", "SSE2"),
        arg_lit0(NULL, "ia", "AVX"),
        arg_lit0(NULL, "i2", "AVX2"),
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
        arg_lit0(NULL, "i5", "AVX512"),
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
        arg_lit0(NULL, "ie", "NEON"),
#endif
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    uint8_t blockno = arg_get_u32_def(ctx, 2, 0);

    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 3) && arg_get_lit(ctx, 4)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 4)) {
        keytype = MF_KEY_B;
    }

    uint8_t trg_blockno = arg_get_u32_def(ctx, 5, 0);

    uint8_t trg_keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 6) && arg_get_lit(ctx, 7)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 7)) {
        trg_keytype = MF_KEY_B;
    }

    int trg_keylen = 0;
    uint8_t trg_key[6] = {0};
    CLIGetHexWithReturn(ctx, 8, trg_key, &trg_keylen);

    int uidlen = 0;
    char uid[14] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 9), (uint8_t *)uid, sizeof(uid), &uidlen);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 10), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool nonce_file_read = arg_get_lit(ctx, 11);
    bool slow = arg_get_lit(ctx, 12);
    bool tests = arg_get_lit(ctx, 13);
    bool nonce_file_write = arg_get_lit(ctx, 14);

    bool in = arg_get_lit(ctx, 15);
#if defined(COMPILER_HAS_SIMD_X86)
    bool im = arg_get_lit(ctx, 16);
    bool is = arg_get_lit(ctx, 17);
    bool ia = arg_get_lit(ctx, 18);
    bool i2 = arg_get_lit(ctx, 19);
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
    bool i5 = arg_get_lit(ctx, 20);
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
    bool ie = arg_get_lit(ctx, 16);
#endif
    CLIParserFree(ctx);

    // set SIM instructions
    SetSIMDInstr(SIMD_AUTO);

#if defined(COMPILER_HAS_SIMD_AVX512)
    if (i5)
        SetSIMDInstr(SIMD_AVX512);
#endif

#if defined(COMPILER_HAS_SIMD_X86)
    if (i2)
        SetSIMDInstr(SIMD_AVX2);
    if (ia)
        SetSIMDInstr(SIMD_AVX);
    if (is)
        SetSIMDInstr(SIMD_SSE2);
    if (im)
        SetSIMDInstr(SIMD_MMX);
#endif

#if defined(COMPILER_HAS_SIMD_NEON)
    if (ie)
        SetSIMDInstr(SIMD_NEON);
#endif

    if (in)
        SetSIMDInstr(SIMD_NONE);


    bool know_target_key = (trg_keylen);

    if (nonce_file_read) {
        char *fptr = GenerateFilename("hf-mf-", "-nonces.bin");
        if (fptr == NULL)
            strncpy(filename, "nonces.bin", FILE_PATH_SIZE - 1);
        else
            strncpy(filename, fptr, FILE_PATH_SIZE - 1);
        free(fptr);
    }

    if (nonce_file_write) {
        char *fptr = GenerateFilename("hf-mf-", "-nonces.bin");
        if (fptr == NULL)
            return 1;
        strncpy(filename, fptr, FILE_PATH_SIZE - 1);
        free(fptr);
    }

    if (uidlen) {
        snprintf(filename, FILE_PATH_SIZE, "hf-mf-%s-nonces.bin", uid);
    }

    if (know_target_key == false && nonce_file_read == false) {

        // check if tag doesn't have static nonce
        if (detect_classic_static_nonce() == NONCE_STATIC) {
            PrintAndLogEx(WARNING, "Static nonce detected. Quitting...");
            PrintAndLogEx(HINT, "\tTry use `" _YELLOW_("hf mf staticnested") "`");
            return PM3_EOPABORTED;
        }

        uint64_t key64 = 0;
        // check if we can authenticate to sector
        if (mfCheckKeys(blockno, keytype, true, 1, key, &key64) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "Key is wrong. Can't authenticate to block: %3d  key type: %c", blockno, (keytype == MF_KEY_B) ? 'B' : 'A');
            return PM3_EWRONGANSWER;
        }
    }

    PrintAndLogEx(INFO, "Target block no " _YELLOW_("%3d") ", target key type: " _YELLOW_("%c") ", known target key: " _YELLOW_("%02x%02x%02x%02x%02x%02x%s"),
                  trg_blockno,
                  (trg_keytype == MF_KEY_B) ? 'B' : 'A',
                  trg_key[0], trg_key[1], trg_key[2], trg_key[3], trg_key[4], trg_key[5],
                  know_target_key ? "" : " (not set)"
                 );
    PrintAndLogEx(INFO, "File action: " _YELLOW_("%s") ", Slow: " _YELLOW_("%s") ", Tests: " _YELLOW_("%d"),
                  nonce_file_write ? "write" : nonce_file_read ? "read" : "none",
                  slow ? "Yes" : "No",
                  tests);

    uint64_t foundkey = 0;
    int16_t isOK = mfnestedhard(blockno, keytype, key, trg_blockno, trg_keytype, know_target_key ? trg_key : NULL, nonce_file_read, nonce_file_write, slow, tests, &foundkey, filename);

    if ((tests == 0) && IfPm3Iso14443a()) {
        DropField();
    }

    if (isOK) {
        switch (isOK) {
            case 1 :
                PrintAndLogEx(ERR, "Error: No response from Proxmark3.\n");
                break;
            case 2 :
                PrintAndLogEx(NORMAL, "Button pressed. Aborted.\n");
                break;
            default :
                break;
        }
        return 2;
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfMAD(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf mad",
                  "Checks and prints MIFARE Application Directory (MAD)",
                  "hf mf mad -> shows MAD if exists\n"
                  "hf mf mad --aid e103 -k ffffffffffff -b -> shows NDEF data if exists. read card with custom key and key B\n"
                  "hf mf mad --dch -k ffffffffffff -> decode CardHolder information\n");

    void *argtable[] = {
        arg_param_begin,
        arg_lit0("v",  "verbose",  "show technical data"),
        arg_str0(NULL, "aid",      "<aid>", "print all sectors with specified aid"),
        arg_str0("k",  "key",      "<key>", "key for printing sectors"),
        arg_lit0("b",  "keyb",     "use key B for access printing sectors (by default: key A)"),
        arg_lit0(NULL, "be",       "(optional, BigEndian)"),
        arg_lit0(NULL, "dch",      "decode Card Holder information"),
        arg_str0("f", "file", "<fn>", "load dump file and decode MAD"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool verbose = arg_get_lit(ctx, 1);
    uint8_t aid[2] = {0};
    int aidlen = 0;
    CLIGetHexWithReturn(ctx, 2, aid, &aidlen);
    uint8_t userkey[6] = {0};
    int keylen = 0;
    CLIGetHexWithReturn(ctx, 3, userkey, &keylen);
    bool keyB = arg_get_lit(ctx, 4);
    bool swapmad = arg_get_lit(ctx, 5);
    bool decodeholder = arg_get_lit(ctx, 6);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 7), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);
    CLIParserFree(ctx);

    if (fnlen > 0) {

        // read dump file
        uint8_t *dump = NULL;
        size_t bytes_read = 0;
        int res = pm3_load_dump(filename, (void **)&dump, &bytes_read, (MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK));
        if (res != PM3_SUCCESS) {
            return res;
        }

        uint16_t block_cnt = MIN(MIFARE_1K_MAXBLOCK, (bytes_read / MFBLOCK_SIZE));
        if (bytes_read == 320)
            block_cnt = MIFARE_MINI_MAXBLOCK;
        else if (bytes_read == 2048)
            block_cnt = MIFARE_2K_MAXBLOCK;
        else if (bytes_read == 4096)
            block_cnt = MIFARE_4K_MAXBLOCK;

        if (verbose) {
            PrintAndLogEx(INFO, "File: " _YELLOW_("%s"), filename);
            PrintAndLogEx(INFO, "File size %zu bytes, file blocks %d (0x%x)", bytes_read, block_cnt, block_cnt);
        }

        // MAD detection
        if (HasMADKey(dump) == false) {
            PrintAndLogEx(FAILED, "No MAD key was detected in the dump file");
            free(dump);
            return PM3_ESOFT;
        }

        MADPrintHeader();
        bool haveMAD2 = false;
        MAD1DecodeAndPrint(dump, swapmad, verbose, &haveMAD2);

        int sector = DetectHID(dump, 0x484d);
        if (sector > -1) {

            // decode it
            PrintAndLogEx(INFO, "");
            PrintAndLogEx(INFO, _CYAN_("HID PACS detected"));

            uint8_t pacs_sector[MFBLOCK_SIZE * 3] = {0};
            memcpy(pacs_sector, dump + (sector * 4 * 16), sizeof(pacs_sector));

            if (pacs_sector[16] == 0x02) {

                PrintAndLogEx(SUCCESS, "Raw...... " _GREEN_("%s"), sprint_hex_inrow(pacs_sector + 24, 8));

                //todo:  remove preamble/sentinel
                uint32_t top = 0, mid = 0, bot = 0;
                char hexstr[16 + 1] = {0};
                hex_to_buffer((uint8_t *)hexstr, pacs_sector + 24, 8, sizeof(hexstr) - 1, 0, 0, true);
                hexstring_to_u96(&top, &mid, &bot, hexstr);

                char binstr[64 + 1];
                hextobinstring(binstr, hexstr);
                char *pbin = binstr;
                while (strlen(pbin) && *(++pbin) == '0');

                PrintAndLogEx(SUCCESS, "Binary... " _GREEN_("%s"), pbin);

                PrintAndLogEx(INFO, "Wiegand decode");
                wiegand_message_t packed = initialize_message_object(top, mid, bot, 0);
                HIDTryUnpack(&packed);
            }
        }
        free(dump);
        return PM3_SUCCESS;
    }

    if (g_session.pm3_present == false)
        return PM3_ENOTTY;


    uint8_t sector0[16 * 4] = {0};
    uint8_t sector10[16 * 4] = {0};

    bool got_first = true;
    if (mfReadSector(MF_MAD1_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector0) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "error, read sector 0. card doesn't have MAD or doesn't have MAD on default keys");
        got_first = false;
    } else {
        PrintAndLogEx(INFO, "Authentication ( " _GREEN_("ok") " )");
    }

    // User supplied key
    if (got_first == false && keylen == 6) {
        PrintAndLogEx(INFO, "Trying user specified key...");
        if (mfReadSector(MF_MAD1_SECTOR, MF_KEY_A, userkey, sector0) != PM3_SUCCESS) {
            PrintAndLogEx(ERR, "error, read sector 0. card doesn't have MAD or the custom key is wrong");
        } else {
            PrintAndLogEx(INFO, "Authentication ( " _GREEN_("ok") " )");
            got_first = true;
        }
    }

    // Both default and user supplied key failed
    if (got_first == false) {
        return PM3_ESOFT;
    }

    MADPrintHeader();

    bool haveMAD2 = false;
    MAD1DecodeAndPrint(sector0, swapmad, verbose, &haveMAD2);

    if (haveMAD2) {
        if (mfReadSector(MF_MAD2_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector10)) {
            PrintAndLogEx(ERR, "error, read sector 0x10. card doesn't have MAD or doesn't have MAD on default keys");
            return PM3_ESOFT;
        }

        MAD2DecodeAndPrint(sector10, swapmad, verbose);
    }

    if (aidlen == 2 || decodeholder) {
        uint16_t mad[7 + 8 + 8 + 8 + 8] = {0};
        size_t madlen = 0;
        if (MADDecode(sector0, sector10, mad, &madlen, swapmad)) {
            PrintAndLogEx(ERR, "can't decode MAD");
            return PM3_ESOFT;
        }

        // copy default NDEF key
        uint8_t akey[6] = {0};
        memcpy(akey, g_mifare_ndef_key, 6);

        // user specified key
        if (keylen == 6) {
            memcpy(akey, userkey, 6);
        }

        uint16_t aaid = 0x0004;
        if (aidlen == 2) {

            aaid = (aid[0] << 8) + aid[1];

            PrintAndLogEx(NORMAL, "");
            PrintAndLogEx(INFO, "-------------- " _CYAN_("AID 0x%04x") " ---------------", aaid);

            for (int i = 0; i < madlen; i++) {
                if (aaid == mad[i]) {
                    uint8_t vsector[16 * 4] = {0};
                    if (mfReadSector(i + 1, keyB ? MF_KEY_B : MF_KEY_A, akey, vsector)) {
                        PrintAndLogEx(NORMAL, "");
                        PrintAndLogEx(ERR, "error, read sector %d", i + 1);
                        return PM3_ESOFT;
                    }

                    for (int j = 0; j < (verbose ? 4 : 3); j ++)
                        PrintAndLogEx(NORMAL, " [%03d] %s", (i + 1) * 4 + j, sprint_hex(&vsector[j * 16], 16));
                }
            }
        }

        if (decodeholder) {

            PrintAndLogEx(NORMAL, "");
            PrintAndLogEx(INFO, "-------- " _CYAN_("Card Holder Info 0x%04x") " --------", aaid);

            uint8_t data[4096] = {0};
            int datalen = 0;

            for (int i = 0; i < madlen; i++) {
                if (aaid == mad[i]) {

                    uint8_t vsector[16 * 4] = {0};
                    if (mfReadSector(i + 1, keyB ? MF_KEY_B : MF_KEY_A, akey, vsector)) {
                        PrintAndLogEx(NORMAL, "");
                        PrintAndLogEx(ERR, "error, read sector %d", i + 1);
                        return PM3_ESOFT;
                    }

                    memcpy(&data[datalen], vsector, 16 * 3);
                    datalen += 16 * 3;
                }
            }

            if (!datalen) {
                PrintAndLogEx(WARNING, "no Card Holder Info data");
                return PM3_SUCCESS;
            }
            MADCardHolderInfoDecode(data, datalen, verbose);
        }
    }

    if (verbose) {
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "------------ " _CYAN_("MAD sector raw") " -------------");
        for (int i = 0; i < 4; i ++)
            PrintAndLogEx(INFO, "[%d] %s", i, sprint_hex(&sector0[i * 16], 16));
    }

    return PM3_SUCCESS;
}

static int CmdHF14AMfAutoPWN(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf autopwn",
                  "This command automates the key recovery process on MIFARE Classic cards.\n"
                  "It uses the fchk, chk, darkside, nested, hardnested and staticnested to recover keys.\n"
                  "If all keys are found, it try dumping card content both to file and emulator memory.",
                  "hf mf autopwn\n"
                  "hf mf autopwn -s 0 -a -k FFFFFFFFFFFF     --> target MFC 1K card, Sector 0 with known key A 'FFFFFFFFFFFF'\n"
                  "hf mf autopwn --1k -f mfc_default_keys    --> target MFC 1K card, default dictionary\n"
                  "hf mf autopwn --1k -s 0 -a -k FFFFFFFFFFFF -f mfc_default_keys  --> combo of the two above samples"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k",  "key",    "<hex>", "Known key, 12 hex bytes"),
        arg_int0("s",  "sector", "<dec>", "Input sector number"),
        arg_lit0("a",   NULL,             "Input key A (def)"),
        arg_lit0("b",   NULL,             "Input key B"),
        arg_str0("f", "file",    "<fn>",  "filename of dictionary"),
        arg_lit0(NULL,  "slow",            "Slower acquisition (required by some non standard cards)"),
        arg_lit0("l",  "legacy",          "legacy mode (use the slow `hf mf chk`)"),
        arg_lit0("v",  "verbose",         "verbose output (statistics)"),

        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (default)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),

        arg_lit0(NULL, "in", "None (use CPU regular instruction set)"),
#if defined(COMPILER_HAS_SIMD_X86)
        arg_lit0(NULL, "im", "MMX"),
        arg_lit0(NULL, "is", "SSE2"),
        arg_lit0(NULL, "ia", "AVX"),
        arg_lit0(NULL, "i2", "AVX2"),
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
        arg_lit0(NULL, "i5", "AVX512"),
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
        arg_lit0(NULL, "ie", "NEON"),
#endif
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int keylen = 0;
    uint8_t key[6] = {0};
    int32_t res = CLIParamHexToBuf(arg_get_str(ctx, 1), key, sizeof(key), &keylen);
    if (res) {
        CLIParserFree(ctx);
        PrintAndLogEx(FAILED, "Error parsing key bytes");
        return PM3_EINVARG;
    }

    bool know_target_key = (keylen == 6);

    uint8_t sectorno = arg_get_u32_def(ctx, 2, 0);

    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 3) && arg_get_lit(ctx, 4)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 4)) {
        keytype = MF_KEY_B;
    }

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 5), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);
    bool has_filename = (fnlen > 0);

    bool slow = arg_get_lit(ctx, 6);
    bool legacy_mfchk = arg_get_lit(ctx, 7);
    bool verbose = arg_get_lit(ctx, 8);

    bool m0 = arg_get_lit(ctx, 9);
    bool m1 = arg_get_lit(ctx, 10);
    bool m2 = arg_get_lit(ctx, 11);
    bool m4 = arg_get_lit(ctx, 12);

    bool in = arg_get_lit(ctx, 13);
#if defined(COMPILER_HAS_SIMD_X86)
    bool im = arg_get_lit(ctx, 14);
    bool is = arg_get_lit(ctx, 15);
    bool ia = arg_get_lit(ctx, 16);
    bool i2 = arg_get_lit(ctx, 17);
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
    bool i5 = arg_get_lit(ctx, 18);
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
    bool ie = arg_get_lit(ctx, 14);
#endif

    CLIParserFree(ctx);

    //validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sector_cnt = MIFARE_1K_MAXSECTOR;
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;

    if (m0) {
        sector_cnt = MIFARE_MINI_MAXSECTOR;
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        sector_cnt = MIFARE_1K_MAXSECTOR;
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        sector_cnt = MIFARE_2K_MAXSECTOR;
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        sector_cnt = MIFARE_4K_MAXSECTOR;
        block_cnt = MIFARE_4K_MAXBLOCK;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }


    // set SIM instructions
    SetSIMDInstr(SIMD_AUTO);

#if defined(COMPILER_HAS_SIMD_AVX512)
    if (i5)
        SetSIMDInstr(SIMD_AVX512);
#endif

#if defined(COMPILER_HAS_SIMD_X86)
    if (i2)
        SetSIMDInstr(SIMD_AVX2);
    if (ia)
        SetSIMDInstr(SIMD_AVX);
    if (is)
        SetSIMDInstr(SIMD_SSE2);
    if (im)
        SetSIMDInstr(SIMD_MMX);
#endif

#if defined(COMPILER_HAS_SIMD_NEON)
    if (ie)
        SetSIMDInstr(SIMD_NEON);
#endif

    if (in)
        SetSIMDInstr(SIMD_NONE);


    // Nested and Hardnested parameter
    uint64_t key64 = 0;
    bool calibrate = true;
    // Attack key storage variables
    uint8_t *keyBlock = NULL;
    uint32_t key_cnt = 0;
    sector_t *e_sector;
    uint8_t tmp_key[6] = {0};

    // Nested and Hardnested returned status
    uint64_t foundkey = 0;
    int isOK = 0;
    int current_sector_i = 0, current_key_type_i = 0;
    // Dumping and transfere to simulater memory
    uint8_t block[16] = {0x00};
    int bytes;
    // Settings
    int prng_type = PM3_EUNDEF;
    int has_staticnonce;
    uint8_t num_found_keys = 0;

// ------------------------------

    // Select card to get UID/UIDLEN/ATQA/SAK information
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "iso14443a card select timeout");
        return PM3_ETIMEOUT;
    }

    uint64_t select_status = resp.oldarg[0];
    if (select_status == 0) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        return select_status;
    }

    // store card info
    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));

    // create/initialize key storage structure
    uint32_t e_sector_size = sector_cnt > sectorno ? sector_cnt : sectorno + 1;
    res = initSectorTable(&e_sector, e_sector_size);
    if (res != e_sector_size) {
        free(e_sector);
        return PM3_EMALLOC;
    }

    // read uid to generate a filename for the key file
    char *fptr = GenerateFilename("hf-mf-", "-key.bin");

    // check if tag doesn't have static nonce
    has_staticnonce = detect_classic_static_nonce();

    // card prng type (weak=1 / hard=0 / select/card comm error = negative value)
    if (has_staticnonce == NONCE_NORMAL)  {
        prng_type = detect_classic_prng();
        if (prng_type < 0) {
            PrintAndLogEx(FAILED, "\nNo tag detected or other tag communication error");
            free(e_sector);
            free(fptr);
            return prng_type;
        }
    }

    // print parameters
    if (verbose) {
        PrintAndLogEx(INFO, "======================= " _YELLOW_("SETTINGS") " =======================");
        PrintAndLogEx(INFO, " card sectors .. " _YELLOW_("%d"), sector_cnt);
        PrintAndLogEx(INFO, " key supplied .. " _YELLOW_("%s"), know_target_key ? "True" : "False");
        PrintAndLogEx(INFO, " known sector .. " _YELLOW_("%d"), sectorno);
        PrintAndLogEx(INFO, " keytype ....... " _YELLOW_("%c"), (keytype == MF_KEY_B) ? 'B' : 'A');
        PrintAndLogEx(INFO, " known key ..... " _YELLOW_("%s"), sprint_hex(key, sizeof(key)));

        if (has_staticnonce == NONCE_STATIC)
            PrintAndLogEx(INFO, " card PRNG ..... " _YELLOW_("STATIC"));
        else if (has_staticnonce == NONCE_NORMAL)
            PrintAndLogEx(INFO, " card PRNG ..... " _YELLOW_("%s"), prng_type ? "WEAK" : "HARD");
        else
            PrintAndLogEx(INFO, " card PRNG ..... " _YELLOW_("Could not determine PRNG,") " " _RED_("read failed."));

        PrintAndLogEx(INFO, " dictionary .... " _YELLOW_("%s"), strlen(filename) ? filename : "NONE");
        PrintAndLogEx(INFO, " legacy mode ... " _YELLOW_("%s"), legacy_mfchk ? "True" : "False");

        PrintAndLogEx(INFO, "========================================================================");
    }

    // Start the timer
    uint64_t t1 = msclock();

    // check the user supplied key
    if (know_target_key == false) {
        PrintAndLogEx(WARNING, "no known key was supplied, key recovery might fail");
    } else {
        if (verbose) {
            PrintAndLogEx(INFO, "======================= " _YELLOW_("START KNOWN KEY ATTACK") " =======================");
        }

        if (mfCheckKeys(mfFirstBlockOfSector(sectorno), keytype, true, 1, key, &key64) == PM3_SUCCESS) {
            PrintAndLogEx(INFO, "target sector %3u key type %c -- using valid key [ " _GREEN_("%s") "] (used for nested / hardnested attack)",
                          sectorno,
                          (keytype == MF_KEY_B) ? 'B' : 'A',
                          sprint_hex(key, sizeof(key))
                         );

            // Store the key for the nested / hardnested attack (if supplied by the user)
            e_sector[sectorno].Key[keytype] = key64;
            e_sector[sectorno].foundKey[keytype] = 'U';

            ++num_found_keys;
        } else {
            know_target_key = false;
            PrintAndLogEx(FAILED, "Key is wrong. Can't authenticate to sector"_RED_("%3d") " key type "_RED_("%c") " key " _RED_("%s"),
                          sectorno,
                          (keytype == MF_KEY_B) ? 'B' : 'A',
                          sprint_hex(key, sizeof(key))
                         );
            PrintAndLogEx(WARNING, "falling back to dictionary");
        }

        // Check if the user supplied key is used by other sectors
        for (int i = 0; i < sector_cnt; i++) {
            for (int j = MF_KEY_A; j <= MF_KEY_B; j++) {
                if (e_sector[i].foundKey[j] == 0) {
                    if (mfCheckKeys(mfFirstBlockOfSector(i), j, true, 1, key, &key64) == PM3_SUCCESS) {
                        e_sector[i].Key[j] = bytes_to_num(key, 6);
                        e_sector[i].foundKey[j] = 'U';

                        // If the user supplied secctor / keytype was wrong --> just be nice and correct it ;)
                        if (know_target_key == false) {
                            num_to_bytes(e_sector[i].Key[j], 6, key);
                            know_target_key = true;
                            sectorno = i;
                            keytype = j;
                            PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ] (used for nested / hardnested attack)",
                                          i,
                                          (j == MF_KEY_B) ? 'B' : 'A',
                                          sprint_hex_inrow(key, sizeof(key))
                                         );
                        } else {
                            PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ]",
                                          i,
                                          (j == MF_KEY_B)  ? 'B' : 'A',
                                          sprint_hex_inrow(key, sizeof(key))
                                         );
                        }
                        ++num_found_keys;
                    }
                }
            }
        }

        if (num_found_keys == sector_cnt * 2) {
            goto all_found;
        }
    }

    bool load_success = true;
    // Load the dictionary
    if (has_filename) {
        res = loadFileDICTIONARY_safe(filename, (void **) &keyBlock, 6, &key_cnt);
        if (res != PM3_SUCCESS || key_cnt == 0 || keyBlock == NULL) {
            PrintAndLogEx(FAILED, "An error occurred while loading the dictionary! (we will use the default keys now)");
            if (keyBlock != NULL) {
                free(keyBlock);
            }
            load_success = false;
        }
    }

    if (has_filename == false || load_success == false) {
        keyBlock = calloc(ARRAYLEN(g_mifare_default_keys), 6);
        if (keyBlock == NULL) {
            free(e_sector);
            free(fptr);
            return PM3_EMALLOC;
        }

        for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++) {
            num_to_bytes(g_mifare_default_keys[cnt], 6, keyBlock + cnt * 6);
        }
        key_cnt = ARRAYLEN(g_mifare_default_keys);
        PrintAndLogEx(SUCCESS, "loaded " _GREEN_("%2d") " keys from hardcoded default array", key_cnt);
    }

    // Use the dictionary to find sector keys on the card
    if (verbose) PrintAndLogEx(INFO, "======================= " _YELLOW_("START DICTIONARY ATTACK") " =======================");

    if (legacy_mfchk) {
        PrintAndLogEx(INFO, "." NOLF);
        // Check all the sectors
        for (int i = 0; i < sector_cnt; i++) {
            for (int j = 0; j < 2; j++) {
                // Check if the key is known
                if (e_sector[i].foundKey[j] == 0) {
                    for (uint32_t k = 0; k < key_cnt; k++) {
                        PrintAndLogEx(NORMAL, "." NOLF);
                        fflush(stdout);

                        if (mfCheckKeys(mfFirstBlockOfSector(i), j, true, 1, (keyBlock + (6 * k)), &key64) == PM3_SUCCESS) {
                            e_sector[i].Key[j] = bytes_to_num((keyBlock + (6 * k)), 6);
                            e_sector[i].foundKey[j] = 'D';
                            ++num_found_keys;
                            break;
                        }
                    }
                }
            }
        }
        PrintAndLogEx(NORMAL, "");
    } else {

        uint32_t chunksize = key_cnt > (PM3_CMD_DATA_SIZE / 6) ? (PM3_CMD_DATA_SIZE / 6) : key_cnt;
        bool firstChunk = true, lastChunk = false;

        for (uint8_t strategy = 1; strategy < 3; strategy++) {
            PrintAndLogEx(INFO, "running strategy %u", strategy);
            // main keychunk loop
            for (uint32_t i = 0; i < key_cnt; i += chunksize) {

                if (kbd_enter_pressed()) {
                    PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
                    i = key_cnt;
                    strategy = 3;
                    break; // Exit the loop
                }
                uint32_t size = ((key_cnt - i)  > chunksize) ? chunksize : key_cnt - i;
                // last chunk?
                if (size == key_cnt - i)
                    lastChunk = true;

                res = mfCheckKeys_fast(sector_cnt, firstChunk, lastChunk, strategy, size, keyBlock + (i * 6), e_sector, false);
                if (firstChunk)
                    firstChunk = false;
                // all keys,  aborted
                if (res == PM3_SUCCESS) {
                    i = key_cnt;
                    strategy = 3;
                    break; // Exit the loop
                }
            } // end chunks of keys
            firstChunk = true;
            lastChunk = false;
        } // end strategy
    }

    // Analyse the dictionary attack
    for (int i = 0; i < sector_cnt; i++) {
        for (int j = MF_KEY_A; j <= MF_KEY_B; j++) {
            if (e_sector[i].foundKey[j] == 1) {
                e_sector[i].foundKey[j] = 'D';
                num_to_bytes(e_sector[i].Key[j], 6, tmp_key);

                // Store valid credentials for the nested / hardnested attack if none exist
                if (know_target_key == false) {
                    num_to_bytes(e_sector[i].Key[j], 6, key);
                    know_target_key = true;
                    sectorno = i;
                    keytype = j;
                    PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ] (used for nested / hardnested attack)",
                                  i,
                                  (j == MF_KEY_B) ? 'B' : 'A',
                                  sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                 );
                } else {
                    PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ]",
                                  i,
                                  (j == MF_KEY_B) ? 'B' : 'A',
                                  sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                 );
                }
            }
        }
    }

    // Check if at least one sector key was found
    if (know_target_key == false) {
        // Check if the darkside attack can be used
        if (prng_type && has_staticnonce != NONCE_STATIC) {
            if (verbose) {
                PrintAndLogEx(INFO, "======================= " _YELLOW_("START DARKSIDE ATTACK") " =======================");
            }
            isOK = mfDarkside(mfFirstBlockOfSector(sectorno), keytype + 0x60, &key64);

            switch (isOK) {
                case -1 :
                    PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                    goto noValidKeyFound;
                case -2 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (doesn't send NACK on authentication requests).");
                    goto noValidKeyFound;
                case -3 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (its random number generator is not predictable).");
                    goto noValidKeyFound;
                case -4 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (its random number generator seems to be based on the wellknown");
                    PrintAndLogEx(FAILED, "generating polynomial with 16 effective bits only, but shows unexpected behaviour.");
                    goto noValidKeyFound;
                case -5 :
                    PrintAndLogEx(WARNING, "\naborted via keyboard.");
                    goto noValidKeyFound;
                default :
                    PrintAndLogEx(SUCCESS, "\nFound valid key [ " _GREEN_("%012" PRIx64) " ]\n", key64);
                    break;
            }

            // Store the keys
            num_to_bytes(key64, 6, key);
            e_sector[sectorno].Key[keytype] = key64;
            e_sector[sectorno].foundKey[keytype] = 'S';
            PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%012" PRIx64) " ] (used for nested / hardnested attack)",
                          sectorno,
                          (keytype == MF_KEY_B) ? 'B' : 'A',
                          key64
                         );
        } else {
noValidKeyFound:
            PrintAndLogEx(FAILED, "No usable key was found!");
            free(keyBlock);
            free(e_sector);
            free(fptr);
            return PM3_ESOFT;
        }
    }

    free(keyBlock);
    // Clear the needed variables
    num_to_bytes(0, 6, tmp_key);
    bool nested_failed = false;

    // Iterate over each sector and key(A/B)
    for (current_sector_i = 0; current_sector_i < sector_cnt; current_sector_i++) {
        for (current_key_type_i = 0; current_key_type_i < 2; current_key_type_i++) {

            // If the key is already known, just skip it
            if (e_sector[current_sector_i].foundKey[current_key_type_i] == 0) {

                if (has_staticnonce == NONCE_STATIC)
                    goto tryStaticnested;

                // Try the found keys are reused
                if (bytes_to_num(tmp_key, 6) != 0) {
                    // <!> The fast check --> mfCheckKeys_fast(sector_cnt, true, true, 2, 1, tmp_key, e_sector, false);
                    // <!> Returns false keys, so we just stick to the slower mfchk.
                    for (int i = 0; i < sector_cnt; i++) {
                        for (int j = MF_KEY_A; j <= MF_KEY_B; j++) {
                            // Check if the sector key is already broken
                            if (e_sector[i].foundKey[j])
                                continue;

                            // Check if the key works
                            if (mfCheckKeys(mfFirstBlockOfSector(i), j, true, 1, tmp_key, &key64) == PM3_SUCCESS) {
                                e_sector[i].Key[j] = bytes_to_num(tmp_key, 6);
                                e_sector[i].foundKey[j] = 'R';
                                PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ]",
                                              i,
                                              (j == MF_KEY_B) ? 'B' : 'A',
                                              sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                             );
                            }
                        }
                    }
                }
                // Clear the last found key
                num_to_bytes(0, 6, tmp_key);

                if (current_key_type_i == MF_KEY_B) {
                    if (e_sector[current_sector_i].foundKey[0] && !e_sector[current_sector_i].foundKey[1]) {
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START READ B KEY ATTACK") " =======================");
                            PrintAndLogEx(INFO, "reading B key of sector %3d with key type %c",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? 'B' : 'A');
                        }
                        uint8_t sectrail = (mfFirstBlockOfSector(current_sector_i) + mfNumBlocksPerSector(current_sector_i) - 1);

                        mf_readblock_t payload;
                        payload.blockno = sectrail;
                        payload.keytype = MF_KEY_A;

                        num_to_bytes(e_sector[current_sector_i].Key[0], 6, payload.key); // KEY A

                        clearCommandBuffer();
                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

                        if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) goto skipReadBKey;

                        if (resp.status != PM3_SUCCESS) goto skipReadBKey;

                        uint8_t *data = resp.data.asBytes;
                        key64 = bytes_to_num(data + 10, 6);
                        if (key64) {
                            e_sector[current_sector_i].foundKey[current_key_type_i] = 'A';
                            e_sector[current_sector_i].Key[current_key_type_i] = key64;
                            num_to_bytes(key64, 6, tmp_key);
                            PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ]",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? 'B' : 'A',
                                          sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                         );
                        } else {
                            if (verbose) {
                                PrintAndLogEx(WARNING, "unknown  B  key: sector: %3d key type: %c",
                                              current_sector_i,
                                              (current_key_type_i == MF_KEY_B) ? 'B' : 'A'
                                             );
                                PrintAndLogEx(INFO, " -- reading the B key was not possible, maybe due to access rights?");

                            }

                        }
                    }
                }

                // Use the nested / hardnested attack
skipReadBKey:
                if (e_sector[current_sector_i].foundKey[current_key_type_i] == 0) {

                    if (has_staticnonce == NONCE_STATIC)
                        goto tryStaticnested;

tryNested:
                    if (prng_type && (nested_failed == false)) {
                        uint8_t retries = 0;
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START NESTED ATTACK") " =======================");
                            PrintAndLogEx(INFO, "sector no %3d, target key type %c",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? 'B' : 'A');
                        }
//tryNested:
                        isOK = mfnested(mfFirstBlockOfSector(sectorno), keytype, key, mfFirstBlockOfSector(current_sector_i), current_key_type_i, tmp_key, calibrate);

                        switch (isOK) {
                            case PM3_ETIMEOUT: {
                                PrintAndLogEx(ERR, "\nError: No response from Proxmark3.");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                            case PM3_EOPABORTED: {
                                PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                                free(e_sector);
                                free(fptr);
                                return PM3_EOPABORTED;
                            }
                            case PM3_EFAILED: {
                                PrintAndLogEx(FAILED, "Tag isn't vulnerable to Nested Attack (PRNG is probably not predictable).");
                                PrintAndLogEx(FAILED, "Nested attack failed --> try hardnested");
                                goto tryHardnested;
                            }
                            case PM3_ESOFT: {
                                // key not found
                                calibrate = false;
                                // this can happen on some old cards, it's worth trying some more before switching to slower hardnested
                                if (retries++ < MIFARE_SECTOR_RETRY) {
                                    PrintAndLogEx(FAILED, "Nested attack failed, trying again (%i/%i)", retries, MIFARE_SECTOR_RETRY);
                                    goto tryNested;
                                } else {
                                    PrintAndLogEx(FAILED, "Nested attack failed, moving to hardnested");
                                    nested_failed = true;
                                    goto tryHardnested;
                                }
                                break;
                            }
                            case PM3_SUCCESS: {
                                calibrate = false;
                                e_sector[current_sector_i].Key[current_key_type_i] = bytes_to_num(tmp_key, 6);
                                e_sector[current_sector_i].foundKey[current_key_type_i] = 'N';
                                break;
                            }
                            default: {
                                PrintAndLogEx(ERR, "unknown Error.\n");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                        }

                    } else {
tryHardnested: // If the nested attack fails then we try the hardnested attack
                        if (verbose){
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START HARDNESTED ATTACK") " =======================");
                            PrintAndLogEx(INFO, "[sec -> %3d | tkey -> %s | slow -> %s]",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? _YELLOW_("B") : _GREEN_("[A]"),
                                          slow ? _YELLOW_("yes") : _GREEN_("no"));
                        }

                        isOK = mfnestedhard(mfFirstBlockOfSector(sectorno), keytype, key, mfFirstBlockOfSector(current_sector_i), current_key_type_i, NULL, false, false, slow, 0, &foundkey, NULL);
                        DropField();
                        if (isOK) {
                            switch (isOK) {
                                case 1: {
                                    PrintAndLogEx(ERR, "\nError: No response from Proxmark3");
                                    break;
                                }
                                case 2: {
                                    PrintAndLogEx(NORMAL, "\nButton pressed, user aborted");
                                    break;
                                }
                                default: {
                                    break;
                                }
                            }
                            free(e_sector);
                            free(fptr);
                            return PM3_ESOFT;
                        }

                        // Copy the found key to the tmp_key variale (for the following print statement, and the mfCheckKeys above)
                        num_to_bytes(foundkey, 6, tmp_key);
                        e_sector[current_sector_i].Key[current_key_type_i] = foundkey;
                        e_sector[current_sector_i].foundKey[current_key_type_i] = 'H';
                    }
tryStaticnested:
                    if (has_staticnonce == NONCE_STATIC) {
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _YELLOW_("START STATIC NESTED ATTACK") " =======================");
                            PrintAndLogEx(INFO, "sector no %3d, target key type %c",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? 'B' : 'A');
                        }

                        isOK = mfStaticNested(sectorno, keytype, key, mfFirstBlockOfSector(current_sector_i), current_key_type_i, tmp_key);
                        DropField();
                        switch (isOK) {
                            case PM3_ETIMEOUT: {
                                PrintAndLogEx(ERR, "\nError: No response from Proxmark3");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                            case PM3_EOPABORTED: {
                                PrintAndLogEx(WARNING, "\nButton pressed, user aborted");
                                free(e_sector);
                                free(fptr);
                                return PM3_EOPABORTED;
                            }
                            case PM3_SUCCESS: {
                                e_sector[current_sector_i].Key[current_key_type_i] = bytes_to_num(tmp_key, 6);
                                e_sector[current_sector_i].foundKey[current_key_type_i] = 'C';
                                break;
                            }
                            default: {
                                break;
                            }
                        }
                    }

                    // Check if the key was found
                    if (e_sector[current_sector_i].foundKey[current_key_type_i]) {
                        PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ]",
                                      current_sector_i,
                                      (current_key_type_i == MF_KEY_B) ? 'B' : 'A',
                                      sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                     );
                    }
                }
            }
        }
    }

all_found:

    // Show the results to the user
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

    printKeyTable(sector_cnt, e_sector);

    // Dump the keys
    PrintAndLogEx(NORMAL, "");

    if (createMfcKeyDump(fptr, sector_cnt, e_sector) != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "Failed to save keys to file");
    }

    // clear emulator mem
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_MEMCLR, NULL, 0);

    PrintAndLogEx(SUCCESS, "transferring keys to simulator memory (Cmd Error: 04 can occur)");

    for (current_sector_i = 0; current_sector_i < sector_cnt; current_sector_i++) {
        mfEmlGetMem(block, current_sector_i, 1);
        if (e_sector[current_sector_i].foundKey[0])
            num_to_bytes(e_sector[current_sector_i].Key[0], 6, block);
        if (e_sector[current_sector_i].foundKey[1])
            num_to_bytes(e_sector[current_sector_i].Key[1], 6, block + 10);

        mfEmlSetMem(block, mfFirstBlockOfSector(current_sector_i) + mfNumBlocksPerSector(current_sector_i) - 1, 1);
    }

    // use ecfill trick
    FastDumpWithEcFill(sector_cnt);

    bytes = block_cnt * MFBLOCK_SIZE;
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(ERR, "Fail, cannot allocate memory");
        free(e_sector);
        free(fptr);
        return PM3_EMALLOC;
    }

    PrintAndLogEx(INFO, "downloading the card content from emulator memory");
    if (!GetFromDevice(BIG_BUF_EML, dump, bytes, 0, NULL, 0, NULL, 2500, false)) {
        PrintAndLogEx(ERR, "Fail, transfer from device time-out");
        free(e_sector);
        free(dump);
        free(fptr);
        return PM3_ETIMEOUT;
    }

    free(fptr);
    fptr = GenerateFilename("hf-mf-", "-dump");
    if (fptr == NULL) {
        free(dump);
        free(e_sector);
        free(fptr);
        return PM3_ESOFT;
    }
    strcpy(filename, fptr);
    free(fptr);

    saveFile(filename, ".bin", dump, bytes);
    saveFileEML(filename, dump, bytes, MFBLOCK_SIZE);
    iso14a_mf_extdump_t xdump;
    xdump.card_info = card;
    xdump.dump = dump;
    xdump.dumplen = bytes;
    saveFileJSON(filename, jsfCardMemory, (uint8_t *)&xdump, sizeof(xdump), NULL);

    // Generate and show statistics
    t1 = msclock() - t1;
    PrintAndLogEx(INFO, "autopwn execution time: " _YELLOW_("%.0f") " seconds", (float)t1 / 1000.0);

    free(dump);
    free(e_sector);
    return PM3_SUCCESS;
}



static int CmdHF14AMfECFill(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ecfill",
                  "Dump card and transfer the data to emulator memory.\n"
                  "Keys must be laid in the emulator memory",
                  "hf mf ecfill          --> use key type A\n"
                  "hf mf ecfill --4k -b  --> target 4K card with key type B"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("a", NULL, "input key type is key A(def)"),
        arg_lit0("b", NULL, "input key type is key B"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 1) && arg_get_lit(ctx, 2)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 2)) {
        keytype = MF_KEY_B;
    }

    bool m0 = arg_get_lit(ctx, 3);
    bool m1 = arg_get_lit(ctx, 4);
    bool m2 = arg_get_lit(ctx, 5);
    bool m4 = arg_get_lit(ctx, 6);
    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectors_cnt = MIFARE_1K_MAXSECTOR;

    if (m0) {
        sectors_cnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectors_cnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectors_cnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectors_cnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    mfc_eload_t payload = {
        .sectorcnt = sectors_cnt,
        .keytype = keytype
    };

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_LOAD, (uint8_t *)&payload, sizeof(payload));

    // 2021, iceman:  should get a response from device when its done.
    return PM3_SUCCESS;
}


static int CmdHF14AMfSim(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf sim",
                  "Simulate MIFARE Classic family type based upon\n"
                  "ISO/IEC 14443 type A tag with 4,7 or 10 byte UID\n"
                  "from emulator memory.  See `hf mf eload` first.\n"
                  "The UID from emulator memory will be used if not specified.",
                  "hf mf sim --mini                    --> MIFARE Mini\n"
                  "hf mf sim --1k                      --> MIFARE Classic 1k (default)\n"
                  "hf mf sim --1k -u 0a0a0a0a          --> MIFARE Classic 1k with 4b UID\n"
                  "hf mf sim --1k -u 11223344556677    --> MIFARE Classic 1k with 7b UID\n"
                  "hf mf sim --1k -u 11223344 -i -x    --> Perform reader attack in interactive mode\n"
                  "hf mf sim --2k                      --> MIFARE 2k\n"
                  "hf mf sim --4k                      --> MIFARE 4k"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid", "<hex>", "<4|7|10> hex bytes UID"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_str0("a", "atqa", "<hex>", "Provide explicit ATQA"),
        arg_str0("s", "sak", "<hex>", "Provide explicit SAK"),
        arg_int0("n", "num", "<dec> ", "Automatically exit simulation after <numreads> blocks have been read by reader. 0 = infinite"),
        arg_lit0("i", "interactive", "Console will not be returned until simulation finishes or is aborted"),
        arg_lit0("x", NULL, "Performs the 'reader attack', nr/ar attack against a reader"),
        arg_lit0("e", "emu", "Fill simulator keys from found keys"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_lit0(NULL, "cve", "trigger CVE 2021_0430"),
        arg_lit0(NULL, "fa", "Force ATQA from sim"),
        arg_lit0(NULL, "fs", "Force SAK from sim"),
        arg_lit0(NULL, "fb", "Force both SAK and ATQA from sim"),
        arg_lit0(NULL, "fd", "Force default SAK and ATQA"),
        arg_lit0(NULL, "b", "Disable button for simulation time"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint16_t flags = 0;

    int uidlen = 0;
    uint8_t uid[10] = {0};
    CLIGetHexWithReturn(ctx, 1, uid, &uidlen);

    char uidsize[8] = {0};
    if (uidlen > 0) {
        switch (uidlen) {
            case 10:
                flags |= FLAG_10B_UID_IN_DATA;
                snprintf(uidsize, sizeof(uidsize), "10 byte");
                break;
            case 7:
                flags |= FLAG_7B_UID_IN_DATA;
                snprintf(uidsize, sizeof(uidsize), "7 byte");
                break;
            case 4:
                flags |= FLAG_4B_UID_IN_DATA;
                snprintf(uidsize, sizeof(uidsize), "4 byte");
                break;
            default:
                PrintAndLogEx(WARNING, "Invalid parameter for UID");
                CLIParserFree(ctx);
                return PM3_EINVARG;
        }
    }

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    if (! (m0 || m1 || m2 || m4)){
        m0 = true;
    }

    int atqalen = 0;
    uint8_t atqa[2] = {0};
    CLIGetHexWithReturn(ctx, 6, atqa, &atqalen);
    int saklen = 0;
    uint8_t sak[1] = {0};
    CLIGetHexWithReturn(ctx, 7, sak, &saklen);
    //uint8_t default_atqa[2] = {0x00, 0x04};
    //uint8_t default_sak[1] = {0x08};
    if (arg_get_lit(ctx, 17)){
        sak[0] = 0x08;
        atqa[0] = 0x00;
        atqa[1] = 0x04;
    }

    uint8_t exitAfterNReads = arg_get_u32_def(ctx, 8, 0);

    if (arg_get_lit(ctx, 9)) {
        flags |= FLAG_INTERACTIVE;
    }

    if (arg_get_lit(ctx, 10)) {
        flags |= FLAG_NR_AR_ATTACK;
    }

    bool setEmulatorMem = arg_get_lit(ctx, 11);
    bool verbose = arg_get_lit(ctx, 12);

    if (arg_get_lit(ctx, 13)) {
        flags |= FLAG_CVE21_0430;
    }
    if (arg_get_lit(ctx, 14)||arg_get_lit(ctx, 16)) {
        flags |= FLAG_FORCED_ATQA;
    }
    if (arg_get_lit(ctx, 15)||arg_get_lit(ctx, 16)) {
        flags |= FLAG_FORCED_SAK;
    }
    CLIParserFree(ctx);

    nonces_t data[1];

    sector_t *k_sector = NULL;

    //Validations
    if (atqalen > 0) {
        if (atqalen != 2) {
            PrintAndLogEx(WARNING, "Wrong ATQA length");
            return PM3_EINVARG;

        }
        flags |= FLAG_FORCED_ATQA;
    }
    if (saklen > 0) {
        if (saklen != 1) {
            PrintAndLogEx(WARNING, "Wrong SAK length");
            return PM3_EINVARG;

        }
        flags |= FLAG_FORCED_SAK;
    }

    // Use UID, SAK, ATQA from EMUL, if uid not defined
    if ((flags & (FLAG_4B_UID_IN_DATA | FLAG_7B_UID_IN_DATA | FLAG_10B_UID_IN_DATA)) == 0) {
        flags |= FLAG_UID_IN_EMUL;
    }

    uint8_t k_sectorsCount = 40;
    char csize[13] = { 0 };

    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Multiple MIFARE types specified, defaulting to 1k");
        m0 = false;
        m1 = true;
        m2 = false;
        m4 = false;
    }

    if (m0) {
        flags |= FLAG_MF_MINI;
        snprintf(csize, sizeof(csize), "MINI");
        k_sectorsCount = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        flags |= FLAG_MF_1K;
        snprintf(csize, sizeof(csize), "1K");
        k_sectorsCount = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        flags |= FLAG_MF_2K;
        snprintf(csize, sizeof(csize), "2K with RATS");
        k_sectorsCount = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        flags |= FLAG_MF_4K;
        snprintf(csize, sizeof(csize), "4K");
        k_sectorsCount = MIFARE_4K_MAXSECTOR;
    } else {
        flags |= FLAG_MF_1K;
        PrintAndLogEx(WARNING, "No type specified, defaulting to MIFARE 1K");
    }

    PrintAndLogEx(INFO, _YELLOW_("MIFARE %s") " | %s UID == " _GREEN_("|%s|") ""
                  , csize
                  , uidsize
                  , (uidlen == 0) ? "N/A" : sprint_hex(uid, uidlen)
                 );

    PrintAndLogEx(INFO, "Options [ numreads: %d, flags: %d (0x%02x) ]"
                  , exitAfterNReads
                  , flags
                  , flags);

    struct {
        uint16_t flags;
        uint8_t exitAfter;
        uint8_t uid[10];
        uint16_t atqa;
        uint8_t sak;
    } PACKED payload;

    payload.flags = flags;
    payload.exitAfter = exitAfterNReads;
    memcpy(payload.uid, uid, uidlen);
    payload.atqa = (atqa[1] << 8) | atqa[0];
    payload.sak = sak[0];

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_SIMULATE, (uint8_t *)&payload, sizeof(payload));
    PacketResponseNG resp;

    if (flags & FLAG_INTERACTIVE) {
        PrintAndLogEx(INFO, "Press pm3-button or send another cmd to abort simulation");

        while (!kbd_enter_pressed()) {
            if (!WaitForResponseTimeout(CMD_ACK, &resp, 1500)) continue;
            if (!(flags & FLAG_NR_AR_ATTACK)) break;
            if ((resp.oldarg[0] & 0xffff) != CMD_HF_MIFARE_SIMULATE) break;

            memcpy(data, resp.data.asBytes, sizeof(data));
            readerAttack(k_sector, k_sectorsCount, data[0], setEmulatorMem, verbose);
        }
        showSectorTable(k_sector, k_sectorsCount);
    } else {
        PrintAndLogEx(INFO, "Press pm3-button to abort simulation");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfESave(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf esave",
                  "Save emulator memory into three files (BIN/EML/JSON) ",
                  "hf mf esave\n"
                  "hf mf esave --4k\n"
                  "hf mf esave --4k -f hf-mf-01020304.eml"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0("b", "bin", "Save to .bin only"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    bool binsave = arg_get_lit(ctx, 6);
    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;

    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
    }

    int bytes = block_cnt * MFBLOCK_SIZE;

    // reserv memory
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }
    memset(dump, 0, bytes);

    PrintAndLogEx(INFO, "downloading %u bytes from emulator memory", bytes);
    if (!GetFromDevice(BIG_BUF_EML, dump, bytes, 0, NULL, 0, NULL, 2500, false)) {
        PrintAndLogEx(WARNING, "Fail, transfer from device time-out");
        free(dump);
        return PM3_ETIMEOUT;
    }

    // user supplied filename?
    if (fnlen < 1) {
        char *fptr = filename;
        fptr += snprintf(fptr, sizeof(filename), "hf-mf-");
        FillFileNameByUID(fptr, dump, "-dump", 4);
    }

    saveFile(filename, ".bin", dump, bytes);
    if (!binsave){
        saveFileEML(filename, dump, bytes, MFBLOCK_SIZE);
        iso14a_mf_extdump_t xdump = {0};
        xdump.card_info.ats_len = 0;
        // Check for 4 bytes uid: bcc corrected and single size uid bits in ATQA
        if ((dump[0] ^ dump[1] ^ dump[2] ^ dump[3]) == dump[4] && (dump[6] & 0xc0) == 0) {
            xdump.card_info.uidlen = 4;
            memcpy(xdump.card_info.uid, dump, xdump.card_info.uidlen);
            xdump.card_info.sak = dump[5];
            memcpy(xdump.card_info.atqa, &dump[6], sizeof(xdump.card_info.atqa));
        }
        // Check for 7 bytes UID: double size uid bits in ATQA
        else if ((dump[8] & 0xc0) == 0x40) {
            xdump.card_info.uidlen = 7;
            memcpy(xdump.card_info.uid, dump, xdump.card_info.uidlen);
            xdump.card_info.sak = dump[7];
            memcpy(xdump.card_info.atqa, &dump[8], sizeof(xdump.card_info.atqa));
        } else {
            PrintAndLogEx(WARNING, "Invalid dump. UID/SAK/ATQA not found");
        }
        xdump.dump = dump;
        xdump.dumplen = bytes;
        saveFileJSON(filename, jsfCardMemory, (uint8_t *)&xdump, sizeof(xdump), NULL);
    }
    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AMfEView(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eview",
                  "It displays emulator memory",
                  "hf mf eview\n"
                  "hf mf eview --4k"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0("v", "verbose", "Show verbose dump with annotations"),
        arg_lit0("k", "keys", "Show both contents and keys"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);
    bool m2 = arg_get_lit(ctx, 3);
    bool m4 = arg_get_lit(ctx, 4);
    bool vrb = arg_get_lit(ctx, 5);
    bool verbose = arg_get_lit(ctx, 6);
    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;

    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    int bytes = block_cnt * MFBLOCK_SIZE;

    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    PrintAndLogEx(INFO, "downloading emulator memory");
    if (!GetFromDevice(BIG_BUF_EML, dump, bytes, 0, NULL, 0, NULL, 2500, false)) {
        PrintAndLogEx(WARNING, "Fail, transfer from device time-out");
        free(dump);
        return PM3_ETIMEOUT;
    }

    mf_print_blocks(block_cnt, dump, verbose);

    if (verbose) {
        mf_print_keys(block_cnt, dump);
    }
    if (vrb){
        if (m0){
            CmdHF14AMfESave("--mini -b -f dmp.mfd");
        }
        if (m1){
            CmdHF14AMfESave("--1k -b -f dmp.mfd");
        }
        if (m2){
            CmdHF14AMfESave("--2k -b -f dmp.mfd");
        }
        if (m4){
            CmdHF14AMfESave("--4k -b -f dmp.mfd");
        }
        if (m0){
            return system("mdfread.py -1 dmp.mfd");
        } else {
            return system("mdfread.py dmp.mfd");
        }
        free(dump);
        return system("rm dmp.mfd");
    }
    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AMfEKeyPrn(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ekey",
                  "Download and print the keys from emulator memory",
                  "hf mf ekey --1k --> print MFC 1K keyset\n"
                  "hf mf ekey -w   --> write keys to binary file"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("w", "write", "write keys to binary file `hf-mf-<UID>-key.bin`"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
    //    arg_int0("s", "sec", "<dec>", "Number of sectors to read"),
    //    arg_lit0("o", "only", "Read only sector specified above"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    bool create_dumpfile = arg_get_lit(ctx, 1);
    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    //bool sct = arg_get_int(ctx, 6);
    //bool sct_only = arg_get_int(ctx, 7);
    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectors_cnt = MIFARE_1K_MAXSECTOR;

    if (m0) {
        sectors_cnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectors_cnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectors_cnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectors_cnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    sector_t *e_sector = NULL;

    // create/initialize key storage structure
    int32_t res = initSectorTable(&e_sector, sectors_cnt);
    if (res != sectors_cnt) {
        free(e_sector);
        return PM3_EMALLOC;
    }

    // read UID from EMUL
    uint8_t data[16];
    if (mfEmlGetMem(data, 0, 1) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "error get block 0");
        free(e_sector);
        return PM3_ESOFT;
    }

    // assuming 4byte UID.
    uint8_t uid[4];
    memcpy(uid, data, sizeof(uid));

    // download keys from EMUL
    int loopstart = 0;
    //if (sct > 0){
    //    loopstart = sct;
    //}
    //if (sct > 0){
    //    sectors_cnt = sct;
    //}
    for (int i = loopstart; i < sectors_cnt; i++) {

        if (mfEmlGetMem(data, mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1, 1) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "error get block %d", mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1);
            e_sector[i].foundKey[0] = false;
            e_sector[i].foundKey[1] = false;
        } else {
            e_sector[i].foundKey[0] = true;
            e_sector[i].Key[0] = bytes_to_num(data, 6);
            e_sector[i].foundKey[1] = true;
            e_sector[i].Key[1] = bytes_to_num(data + 10, 6);
        }
        //if (sct_only){
        //    break;
        //}
    }

    // print keys
    printKeyTable(sectors_cnt, e_sector);

    // dump the keys
    if (create_dumpfile) {
        char filename[FILE_PATH_SIZE] = {0};
        char *fptr = filename;
        fptr += snprintf(fptr, sizeof(filename), "mf-");
        FillFileNameByUID(fptr + strlen(fptr), uid, "-key", sizeof(uid));
        createMfcKeyDump(filename, sectors_cnt, e_sector);
    }

    free(e_sector);
    return PM3_SUCCESS;
}

static int CmdHF14AMfHarvest(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf harvest",
                  "This command performs advanced keys extraction (Mifare Classic cards)\n"
                  "It uses all available methods along with granular customization to recover keys.\n"
                  "When all keys are found, they can be saved to file/emulator, and direct simulation/cloning can optionally proceed",
                  "hf mf harvest\n"
                  "hf mf harvest -v -d -s 2 -x -f mfc_default_keys --il -> Use default dict and strategy 2. Verbose output and last available SIMD set\n"
                  "hf mf harvest --hard -k ffffffffffff --1k -L -n -s 0 -b -> Target Mifare 1k sector 0 with known key B. Wait longer for finalization of ACK phase\n"
                  "hf mf harvest --emu --mem --dump -n -> Extract keys using in-memory dictionary and save them to file and emulator. Button click is disabled\n"
                  "hf mf harvest --detect -> Detect PRNG type and nonces, then exit\n"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_str0("k",  "key",    "<hex>", "Known key, 12 hex bytes"),
        arg_int0("n",   NULL, "<dec>", "Input sector number"),
        arg_lit0("a",   NULL,             "Input key A (def)"),
        arg_lit0("b",   NULL,             "Input key B"),
        arg_str0("f", "file",    "<fn>",  "filename of dictionary"),
        arg_lit0(NULL,  "slow",            "Slower acquisition (required by some non standard cards)"),
        arg_lit0("l",  "legacy",          "Legacy mode (use the slow `hf mf chk`)"),
        arg_lit0("v",  "verbose",         "Verbose output"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (default)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "in", "None (use CPU regular instruction set)"),
        arg_lit0("*", "both", "Target both A and B key for Known Keys attack"),
        arg_int0("i", "iter", "<dec>", "Number of iterations"),
        arg_int0("t", "tests", "<dec>", "Run this number of tests before nested attacks"),
        arg_int0("s", "strat", "<dec>", "Strategy to start from during standard keychecks (1-3, def: all)"),
        arg_lit0("m", "mem", "Force use of in-mem dictionary for fchk"),
        arg_lit0("S", "skip", "Fully skip dictionary key checks"),
        arg_lit0("e", "emu", "Transfer found contents to emulator memory"),
        arg_lit0("d", "dict", "Perform only dictionary (standard) key checks"),
        arg_lit0("A", "ia", "Use all instructions sets. sequentially"),
        arg_lit0("x", "explicit", "Use explicitly and only the selected strategy"),
        arg_lit0("r", "rand", "Randomly set aquisition speed"),
        arg_lit0("D", "noclick", "Button interactions are disabled"),
        arg_lit0("c", "clickrst", "Button interactions will restart key extraction"),
        arg_lit0("L", "longer", "Wait longer for ISO CMD_ACK phase"),
        arg_lit0("R", "selret", "When SELECT fails, retry it"),
        arg_lit0("N", "nocal", "Do not perform calibration for nested attacks"),
        arg_lit0(NULL, "ir", "Randomly select instruction set"),
        arg_lit0(NULL, "dump", "Dump full contents of emulator memory into separate file"),
        arg_str0("F", "file2", "<fn>", "Name of the secondary dict file"),
        arg_lit0("I", "ignoreprng", "Ignore PRNG warnings and force nested attempts"),
        arg_int0("1", "start", "<dec>", "Start sector for darkside attack"),
        arg_int0("2", "end", "<dec>", "End sector for darkside attack"),
        arg_lit0(NULL, "limit", "Use above sector range for dictionary attack only"),
        arg_int0("K", "keys", "<dec>", "Exit procedure after this amount of keys was found"),
        arg_lit0(NULL, "sleep", "Wait after printing statistics"),
        arg_lit0(NULL, "il", "Use last available SIMD from list"),
        arg_lit0(NULL, "known", "Perform Known Key check only, then exit"),
        arg_lit0(NULL, "nomad", "Disable in-depth MAD reporting"),
        arg_lit0(NULL, "old", "Run original autopwn before harvesting"),
        arg_lit0(NULL, "norm", "Overwrite most settings and enforce standard execution flow of this command"),
        arg_lit0(NULL, "static", "Perform only static nested attempts"),
        arg_lit0(NULL, "niter", "Use '--iter' option exclusively for nested phase"),
        arg_lit0(NULL, "if", "Use first available SIMD from list"),
        arg_lit0(NULL, "sim", "Perform simulation after successful extraction"),
        arg_lit0(NULL, "present", "In conjunction with '--sim', fill emulator directly from card (must be present on a reader)"),
        arg_lit0(NULL, "follow",  "In conjunction with '--present', follow standard ACK timeouts/attempts"),
        arg_lit0(NULL, "nofill",  "Do not run 'ecfill' or 'eload' before simulation"),
        arg_lit0(NULL, "loop",  "Run simulation in an endless loop"),
        arg_lit0(NULL, "eml",  "When '--dump' is specified, save also in EML format"),
        arg_lit0(NULL, "json",  "When '--dump' is specified, save also in JSON format"),
        arg_lit0(NULL, "eview",  "View emulator memory right before starting simulation"),
        arg_lit0(NULL, "nosave",  "Do not save extracted keys to file"),
        arg_lit0(NULL, "clone",  "Clone extracted data to Gen1/Gen2/Gen3"),
        arg_lit0(NULL, "hard",  "Run only hardnested phase for key recovery"),
        arg_int0(NULL, "select", "<dec>", "Modify delay between each SELECT attempt when '-R' is used"),
        arg_int0(NULL, "timeout", "<dec>", "Set max timeout for this command"),
        arg_int0(NULL, "ntry", "<dec>", "Maximum number of failed nested attempts (default: 10)"),
        arg_lit0(NULL, "debug",  "Enable in-depth debugging (for this command only)"),
        arg_str0(NULL, "nonce",    "<fn>",  "Read nonces from a file for hardnested phase"),
        arg_lit0(NULL, "detect",  "Only detect prng type and nonce, then exit"),
        arg_lit0(NULL, "prng",  "When '--detect' is specified, prng_detect() is used twice for better accuracy"),
        arg_lit0(NULL, "append",  "Add found keys to default dictionary"),
        arg_lit0(NULL, "atype",  "When '--append' is specified, add only found keys of type A to dictionary"),
        arg_str0(NULL, "prefix", "<fn>", "Prefix of the full dump file"),
#if defined(COMPILER_HAS_SIMD_X86)
        arg_lit0(NULL, "im", "MMX"),
        arg_lit0(NULL, "is", "SSE2"),
        arg_lit0(NULL, "iv", "AVX"),
        arg_lit0(NULL, "i2", "AVX2"),
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
        arg_lit0(NULL, "i5", "AVX512"),
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
        arg_lit0(NULL, "ie", "NEON"),
#endif
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool type_a = arg_get_lit(ctx, 3);
    bool type_b = arg_get_lit(ctx, 4);
    bool used_first_dict = false;
    bool used_second_dict = false;
    bool both_keys = arg_get_lit(ctx, 14);
    int iters = arg_get_int_def(ctx, 15, 0);
    int tests = arg_get_int_def(ctx, 16, 0);
    bool use_tests = false;
    if (tests > 0){
        use_tests = true;
    }
    int selected_strategy = arg_get_int_def(ctx, 17, 0);
    bool fchk_use_mem = arg_get_lit(ctx, 18);
    bool skip_chk = arg_get_lit(ctx, 19);
    bool emul = arg_get_lit(ctx, 20);
    bool dict_only = arg_get_lit(ctx, 21);
    bool all_simd = arg_get_lit(ctx, 22);
    bool explicit_strategy = arg_get_lit(ctx, 23);
    bool rand_slow = arg_get_lit(ctx, 24);
    bool noclick = arg_get_lit(ctx, 25);
    bool click_restart = arg_get_lit(ctx, 26);
    bool longer_ack = arg_get_lit(ctx, 27);
    bool retry_select = arg_get_lit(ctx, 28);
    bool nocal = arg_get_lit(ctx, 29);
    bool rand_simd = arg_get_lit(ctx, 30);
    bool fulldump = arg_get_lit(ctx, 31);
    bool ignore_prng = arg_get_lit(ctx, 33);
    int _start = arg_get_int_def(ctx, 34, 0);
    int _end = arg_get_int_def(ctx, 35, 0);
    bool limit_dict = arg_get_lit(ctx, 36);
    int max_keynum = arg_get_int_def(ctx, 37, 0);
    bool goto_slep = arg_get_lit(ctx, 38);
    bool last_simd = arg_get_lit(ctx, 39);
    bool known_sec = arg_get_lit(ctx, 40);
    bool nomad = arg_get_lit(ctx, 41);
    bool old = arg_get_lit(ctx, 42);
    bool norm = arg_get_lit(ctx, 43);
    bool staticn = arg_get_lit(ctx, 44);
    bool nest_iter = arg_get_lit(ctx, 45);
    bool first_simd = arg_get_lit(ctx, 46);
    bool sim = arg_get_lit(ctx, 47);
    bool sim_card_present = arg_get_lit(ctx, 48);
    bool follow = arg_get_lit(ctx, 49);
    bool nofill = arg_get_lit(ctx, 50);
    bool sim_loop = arg_get_lit(ctx, 51);
    bool save_eml = arg_get_lit(ctx, 52);
    bool save_json = arg_get_lit(ctx, 53);
    bool eview = arg_get_lit(ctx, 54);
    bool nosave = arg_get_lit(ctx, 55);
    bool clone = arg_get_lit(ctx, 56);
    bool hard = arg_get_lit(ctx, 57);
    int select_delay = arg_get_int_def(ctx, 58, 0);
    int max_timeout = arg_get_int_def(ctx, 59, 50000);
    int nested_attempts = arg_get_int_def(ctx, 60, 10);
    bool debug = arg_get_lit(ctx, 61);
    int noncelen = 0;
    int found_keynum = 0;
    char nonce_file[FILE_PATH_SIZE] = {0x00};
    CLIParamStrToBuf(arg_get_str(ctx, 62), (uint8_t *)nonce_file, FILE_PATH_SIZE, &noncelen);
    bool detect_only = arg_get_lit(ctx, 63);
    bool prng_twice = arg_get_lit(ctx, 64);
    bool append_dict = arg_get_lit(ctx, 65);
    bool append_dict_only_a = arg_get_lit(ctx, 66); 
    int flenpref = 0;
    char dump_prefix[25] = {0x00};
    CLIParamStrToBuf(arg_get_str(ctx, 67), (uint8_t *)dump_prefix, 25, &flenpref);
    if (!nested_attempts){
        nested_attempts = 10;
    }
    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0x00};
    char outname[FILE_PATH_SIZE] = {0x00};
    CLIParamStrToBuf(arg_get_str(ctx, 5), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);
    bool has_filename = (fnlen > 0);

    int fnlen2 = 0;
    char filename2[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 32), (uint8_t *)filename2, FILE_PATH_SIZE, &fnlen2);
    if(fnlen2 > 0){
        if (!has_filename){
            has_filename = true;
        }
    }
    int iterations = 0;
    int ack_timeout = 1500;
    if (longer_ack){
        if (!select_delay){
            select_delay = 1200;
        }
        ack_timeout = 3000;
    }
    if (!select_delay){
        select_delay = 800;
    }
    int instridx = 0;
    int keylen = 0;
    int simd_sets = 0;
    uint8_t key[6] = {0};
    int32_t res = CLIParamHexToBuf(arg_get_str(ctx, 1), key, sizeof(key), &keylen);
    if (res) {
        CLIParserFree(ctx);
        PrintAndLogEx(FAILED, "Error parsing key bytes");
        return PM3_EINVARG;
    }

    bool know_target_key = (keylen == 6);

    uint8_t sectorno = arg_get_u32_def(ctx, 2, 0);

    uint8_t keytype = MF_KEY_A;

    bool slow = arg_get_lit(ctx, 6);
    bool legacy_mfchk = arg_get_lit(ctx, 7);
    //if (legacy_mfchk){
    //    fchk_use_mem = true;
    //}
    bool verbose = arg_get_lit(ctx, 8);
    bool nested_failed = false;
    if (goto_slep){
        verbose = true;
    }
    int has_neon = 0;

    bool m0 = arg_get_lit(ctx, 9);
    bool m1 = arg_get_lit(ctx, 10);
    bool m2 = arg_get_lit(ctx, 11);
    bool m4 = arg_get_lit(ctx, 12);

    bool in = arg_get_lit(ctx, 13);
#if defined(COMPILER_HAS_SIMD_X86)
    bool im = arg_get_lit(ctx, 68);
    bool is = arg_get_lit(ctx, 69);
    bool ia = arg_get_lit(ctx, 70);
    bool i2 = arg_get_lit(ctx, 71);
#endif
#if defined(COMPILER_HAS_SIMD_AVX512)
    bool i5 = arg_get_lit(ctx, 72);
#endif
#if defined(COMPILER_HAS_SIMD_NEON)
    bool ie = arg_get_lit(ctx, 73);
#endif

    if(norm){
        PrintAndLogEx(SUCCESS, "Setting back options that modify execution flow to default");
        iterations = 0;
        nested_attempts = 1;
        both_keys = false;
        explicit_strategy = false;
        dict_only = false;
        skip_chk = false;
        legacy_mfchk = false;
        old = false;
        longer_ack = false;
        nocal = true;
        nomad = false;
        selected_strategy = 1;
        srand(time(NULL));
        if ((rand()%2)){
            selected_strategy = 2;
        }
        staticn = false;
        nest_iter = false;
        if (!sim){
            retry_select = false;
        }
        last_simd = false;
        all_simd = false;
        clone = false;
        sim = false;
        verbose = true;
        select_delay = 1000;
        ack_timeout = 1300;
        debug = false;
        srand(time(NULL));
        if ((rand()%2)){
            type_a = false;
            type_b = true;
        } else {
            type_a = true;
            type_b = false;
        }
    }
    if (detect_only){
        PrintAndLogEx(INFO, "Enabling select re-tries");
        retry_select = true;
    }
    if (type_a && type_b){
        if (!both_keys){
            PrintAndLogEx(WARNING, "Both known key types specified");
            srand(time(NULL));
            if (!(rand()%2)){
                PrintAndLogEx(INFO, "Using key B as default key");
                keytype = MF_KEY_B;
            }
        } else {
            PrintAndLogEx(INFO, "Using key A as default key");
            keytype = MF_KEY_A;
        }
    } else if (type_b) {
        PrintAndLogEx(INFO, "Using key B as default key");
        keytype = MF_KEY_B;
    }
    bool calibrate = true;
    if (nocal){
        calibrate = false;
    }
    if(sim || clone){
        PrintAndLogEx(INFO, "Enabled '--emu' and '--dump' (clone/sim requirement)");
        emul = true;
        nosave = false;
    }
    if (!fulldump){
        if (save_eml || save_json){
            save_eml = false;
            save_json = false;
            PrintAndLogEx(WARNING, "Fulldump disabled, JSON and EML won't be created . . .");
        }
    }
    if ((flenpref && (nosave || ! fulldump))){
        PrintAndLogEx(WARNING, "Multiple SIMD sets specified, exiting . . .");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (im + is + ia + i2 > 1){
        PrintAndLogEx(WARNING, "Multiple SIMD sets specified, exiting . . .");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (staticn + dict_only > 1){
        PrintAndLogEx(WARNING, "Conflicting options: '--static' and '--dict'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (rand_simd + all_simd > 1){
        PrintAndLogEx(WARNING, "Conflicting options: '--ir' and '--ia'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (staticn + skip_chk > 1){
        PrintAndLogEx(WARNING, "Conflicting options: '--static' and '--skip'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (dict_only + skip_chk > 1){
        PrintAndLogEx(WARNING, "Conflicting options: '--dict' and '--skip'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (noclick + click_restart > 1){
        PrintAndLogEx(WARNING, "Conflicting options: '-n/--noclick' and '-c/--clickrst'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if ((!sim && sim_card_present)){
        PrintAndLogEx(WARNING, "Error: please specify '--sim' when using '--present'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if ((!sim_card_present && follow)){
        PrintAndLogEx(WARNING, "Error: please specify '--present' when using '--follow'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if ((last_simd && first_simd )){
        PrintAndLogEx(WARNING, "Conflicting options: '--il' and '--if'");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }
    if (nosave + sim > 1){
        PrintAndLogEx(ERR, "Simulation might fail"); 
    }
    if (nosave + clone > 1){
        PrintAndLogEx(ERR, "Cloning might fail");
    }
    CLIParserFree(ctx);

    int prv_dbg = g_session.client_debug_level;
    if(debug){
        g_session.client_debug_level = 2;
    }

    int mftype = m0 + m1 + m2 + m4;

    if (mftype > 1) {
        PrintAndLogEx(WARNING, "Multiple card types specified");
    }

    switch(mftype){
        case 0:
            m1 = true;
        case 2:
            if (m0 + m1 > 1){
                m0 = true;
                m1 = false;
                m2 = false;
                m4 = false;
            }
            if (m0 + m1 + m4 == 3){
                m0 = false;
                m1 = false;
                m2 = false;
                m4 = true;
            }

    }

    uint8_t sector_cnt = MIFARE_1K_MAXSECTOR;
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;

    if(mftype > 2){
        srand(time(NULL));
        if(rand()%2){
            m0 = true;
            PrintAndLogEx(WARNING, "Multiple card types specified ---> defaulting to Mini");
        } else {
            m1 = true;
            PrintAndLogEx(WARNING, "Multiple card types specified ---> defaulting to 1k");
        }
    }
    if (m0) {
        sector_cnt = MIFARE_MINI_MAXSECTOR;
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        sector_cnt = MIFARE_1K_MAXSECTOR;
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        sector_cnt = MIFARE_2K_MAXSECTOR;
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        sector_cnt = MIFARE_4K_MAXSECTOR;
        block_cnt = MIFARE_4K_MAXBLOCK;
    }
    #if defined(COMPILER_HAS_SIMD_AVX512)
    simd_sets += 1;
    #endif
    #if defined(COMPILER_HAS_SIMD_X86)
    simd_sets += 4;
    #endif
    #if defined(COMPILER_HAS_SIMD_NEON)
    has_neon = 1;
    simd_sets += 1;
    #endif
    set_simd:
    if (last_simd){
        instridx = simd_sets;
        goto simd_start;
    }
    if (first_simd){
        #ifdef COMPILER_HAS_SIMD_AVX512
        instridx = 1;
        #else
        instridx = 2;
        #endif
        goto simd_start;
    }
    if (!all_simd){
        SetSIMDInstr(SIMD_AUTO);
        #if defined(COMPILER_HAS_SIMD_AVX512)
        if (i5)
            SetSIMDInstr(SIMD_AVX512);
        #endif
        #if defined(COMPILER_HAS_SIMD_X86)
        if (i2)
            SetSIMDInstr(SIMD_AVX2);
        if (ia)
            SetSIMDInstr(SIMD_AVX);
        if (is)
            SetSIMDInstr(SIMD_SSE2);
        if (im)
            SetSIMDInstr(SIMD_MMX);
        #endif
        #if defined(COMPILER_HAS_SIMD_NEON)
        if (ie)
            SetSIMDInstr(SIMD_NEON);
        #endif
        if (in){
            if (simd_sets == 1){
                SetSIMDInstr(SIMD_AUTO);
            }
            if (simd_sets == 0){
                SetSIMDInstr(SIMD_NONE);
            }
        }
    } else if (rand_simd){
        #ifdef COMPILER_HAS_SIMD_AVX512
        int low_simd_idx = 1;
        #else
        int low_simd_idx = 2;
        #endif
        switch(simd_sets){
            case 1:
            instridx = simd_sets;
            goto simd_start;
        case 4:
            instridx = 4; 
            break;
        case 5:
            if (has_neon){
                low_simd_idx = 2;
                instridx = simd_sets;
            }
            break;
        default:
            instridx = simd_sets;
            break;
        }
        srand(time(NULL));
        instridx = (rand() % (instridx - low_simd_idx + 1)) + low_simd_idx;
        goto simd_start;
    } else {
    simd_start:
        int simd_init = 0;
        #if defined(COMPILER_HAS_SIMD_AVX512)
        if (instridx == 1){
            SetSIMDInstr(SIMD_AVX512);
            simd_init += 1;
        }
        #endif
        #if defined(COMPILER_HAS_SIMD_X86)
        if (instridx == 2){
            SetSIMDInstr(SIMD_AVX2);
            simd_init += 1;
        }
        if (instridx == 3){
            SetSIMDInstr(SIMD_AVX);
            simd_init += 1;
        }
        if (instridx == 4){
            SetSIMDInstr(SIMD_SSE2);
            simd_init += 1;
        }
        if (instridx == 5){
            SetSIMDInstr(SIMD_MMX);
            simd_init += 1;
        }
        #endif
        #if defined(COMPILER_HAS_SIMD_NEON)
        if (instridx == 6){
            SetSIMDInstr(SIMD_NEON);
        }
        #endif
        if (simd_init == 0){
            SetSIMDInstr(SIMD_AUTO);
        }
    }



    // Nested and Hardnested parameter
    uint64_t key64 = 0;
    // Attack key storage variables
    uint8_t *keyBlock = NULL;
    uint32_t key_cnt = 0;
    sector_t *e_sector;
    uint8_t tmp_key[6] = {0};
    uint8_t mad_key[6] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
    uint8_t mad_key_b[6] = {0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5};
    uint8_t def_key[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    // Nested and Hardnested returned status
    uint64_t foundkey = 0;
    int isOK = 0;
    int current_sector_i = 0, current_key_type_i = 0;
    // Dumping and transfere to simulater memory
    uint8_t block[16] = {0x00};
    int bytes;
    // Settings
    int prng_type = PM3_EUNDEF;
    int has_staticnonce;
    uint8_t num_found_keys = 0;

    if (old){
        char * auto_cmd = NULL;
        char * mifare_type = NULL;
        srand(time(0));
        if (rand() % 2){
            goto run_old_auto;
        }
        /*if (m0){
            mifare_type[0] = "-";
            mifare_type[1] = "-";
            mifare_type[2] = "m";
            mifare_type[3] = "i";
            mifare_type[4] = "n";
            mifare_type[5] = "i";
        }
        if (m1){
            mifare_type[0] = "-";
            mifare_type[1] = "-";
            mifare_type[2] = "1";
            mifare_type[3] = "k";
        }
        if (m2){
            mifare_type[0] = "-";
            mifare_type[1] = "-";
            mifare_type[2] = "2";
            mifare_type[3] = "k";
        }
        if (m4){
            mifare_type[0] = "-";
            mifare_type[1] = "-";
            mifare_type[2] = "4";
            mifare_type[3] = "k";
        }*/
        if(known_sec){
            char auto_cmd_fmt[] = "%s -k %s -s %d";
            if (m0){
                sprintf(auto_cmd, auto_cmd_fmt, "--mini", sprint_hex(key, 6), known_sec);
            } else if (m1){
                sprintf(auto_cmd, auto_cmd_fmt, "--1k", sprint_hex(key, 6), known_sec);
            } else if (m2){
                sprintf(auto_cmd, auto_cmd_fmt, "--2k", sprint_hex(key, 6), known_sec);
            } else if (m4){
                sprintf(auto_cmd, auto_cmd_fmt, "--4k", sprint_hex(key, 6), known_sec);
            }
        } else {
            char auto_cmd_fmt[] = "%s";
            sprintf(auto_cmd, auto_cmd_fmt, mifare_type);
        }
        CmdHF14AMfAutoPWN(auto_cmd);
        goto try_select;
        run_old_auto:
        int auto_res = CmdHF14AMfAutoPWN(auto_cmd);
        if (auto_res == PM3_SUCCESS){
            PrintAndLogEx(SUCCESS, "Legacy autopwn finished");
        } else {
            PrintAndLogEx(FAILED, "Legacy autopwn finished");
        }
    }

    try_select:
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, ack_timeout) == false) {
        if (retry_select){
            PrintAndLogEx(WARNING, "card select timeout - re-trying");
            msleep(select_delay);
            goto try_select;
        } else {
            PrintAndLogEx(WARNING, "iso14443a card select timeout");
            return PM3_ETIMEOUT;
        }
    }

    uint64_t select_status = resp.oldarg[0];
    if (select_status == 0) {
        if (retry_select){
            PrintAndLogEx(WARNING, "card select failed - re-trying");
            msleep(select_delay);
            goto try_select;
        } else {
            PrintAndLogEx(WARNING, "iso14443a card select failed");
            return select_status;
        }
    }

    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));
    iso14a_card_select_t *cardinfo = (iso14a_card_select_t *) resp.data.asBytes;
    char uid[20];
    int uid_len = cardinfo->uidlen;
    for (uint16_t m = 0; m < cardinfo->uidlen; m++) {
        int offset = 2 * m;
        snprintf(uid + offset, sizeof(uid) - offset, "%02X", cardinfo->uid[m]);
    }


    uint32_t e_sector_size = sector_cnt > sectorno ? sector_cnt : sectorno + 1;
    res = initSectorTable(&e_sector, e_sector_size);
    if (res != e_sector_size) {
        free(e_sector);
        return PM3_EMALLOC;
    }

    // read uid to generate a filename for the key file
    char *fptr = GenerateFilename("key-", ".bin");

    // check if tag doesn't have static nonce
    prng_detect:
    has_staticnonce = detect_classic_static_nonce();

    // card prng type (weak=1 / hard=0 / select/card comm error = negative value)
    if (detect_only && prng_twice){
        prng_type = detect_classic_prng();
    }
    if (has_staticnonce == NONCE_NORMAL)  {
        prng_type = detect_classic_prng();
        if (prng_type < 0) {
            PrintAndLogEx(FAILED, "\nNo tag detected or other tag communication error");
            if (retry_select){
                PrintAndLogEx(INFO, "Re-trying PRNG detection . . . ");
                msleep(select_delay);
                goto prng_detect;
            } else {
                free(e_sector);
                free(fptr);
                return prng_type;
            }
        }
    }
    if (detect_only){
        PrintAndLogEx(NORMAL, "");
        switch(prng_type){
            case 1: {
                PrintAndLogEx(INFO, "PRNG ------> " _LIGHT_GREEN_("WEAK"));
                break;
            }
            case 2: {
                PrintAndLogEx(INFO, "PRNG ------> " _RED_("HARD"));
                break;
            }
            default: {
                PrintAndLogEx(INFO, "PRNG ------> " _LIGHT_RED_("ERROR"));
                break;
            }
        }
        switch(has_staticnonce){
            case NONCE_STATIC:{
                PrintAndLogEx(INFO, "NONCE -----> " _GREEN_("STATIC"));
                break;
            }
            case NONCE_NORMAL:{
                PrintAndLogEx(INFO, "NONCE -----> " "%s", prng_type ? _GREEN_("[+]") : _RED_("[-]"));
                break;
            }
            default : {
                PrintAndLogEx(INFO, "NONCE -----> " _RED_("ERROR"));
                break;
            }
        }
        PrintAndLogEx(NORMAL, "");
        return PM3_SUCCESS;
    }

    PrintAndLogEx(NORMAL, "[::] ------------------------------ " _CYAN_("[ - OPTIONS - ]") "-----------------------[::]");
    if(m0){
        PrintAndLogEx(NORMAL, " card type ..... [" _CYAN_("mini") "|1k|2k|4k]");
    }
    if(m1){
        PrintAndLogEx(NORMAL, " card type ..... [mini|" _GREEN_("1k") "|2k|4k]");
    }
    if(m2){
        PrintAndLogEx(NORMAL, " card type ..... [mini|1k|" _YELLOW_("2k") "|4k]");
    }
    if(m4){
        PrintAndLogEx(NORMAL, " card type ..... [mini|1k|2k|" _RED_("4k") "]");
    }
    PrintAndLogEx(NORMAL, " card UID ...... " _MAGENTA_("%s"), (uid_len == 0) ? "N/A" : uid);
    PrintAndLogEx(NORMAL, " card sectors .. " _YELLOW_("%d"), sector_cnt);
    PrintAndLogEx(NORMAL, " key supplied .. " "%s", know_target_key ? _GREEN_(":>") : _RED_(":<"));
    PrintAndLogEx(NORMAL, " known sector .. " "%d", sectorno);
    PrintAndLogEx(NORMAL, " known key ..... " _GREEN_("%s") "%s", sprint_hex(key, sizeof(key)), (keytype == MF_KEY_B) ? _YELLOW_("B") : _GREEN_("[A]"));
    PrintAndLogEx(NORMAL, " slow mode ..... " "%s", slow ? _YELLOW_("yes") : "no");
    PrintAndLogEx(NORMAL, " save to sim ... " "%s", emul ? _LIGHT_GREEN_("[+]") : _LIGHT_RED_("[-]"));
    if (debug){
        PrintAndLogEx(NORMAL, " debug level ... " _RED_("%d!"), g_session.client_debug_level);
    } else {
        PrintAndLogEx(NORMAL, " debug level ... " _GREEN_("%d"), g_session.client_debug_level);
    }
    if (_start){
        PrintAndLogEx(NORMAL, " start sector ... " _LIGHT_CYAN_("%d"), _start);
    }
    if (_end){
        PrintAndLogEx(NORMAL, " end sector ..... " _LIGHT_CYAN_("%d"), _end);
    }
    if(append_dict){
        PrintAndLogEx(NORMAL, " dict add ....... " _LIGHT_CYAN_("mfc_default_keys.dic"));
        if (append_dict_only_a){
            PrintAndLogEx(NORMAL, " add keytype .... " _YELLOW_("(B)"));
        } else {
            PrintAndLogEx(NORMAL, " add keytype .... " _YELLOW_("(B)") " + " _GREEN_("[A]"));
        }
    }
    if (verbose) {
        PrintAndLogEx(NORMAL, " timeout ....... " "%d seconds", max_timeout);
        if (has_staticnonce == NONCE_STATIC){
            PrintAndLogEx(NORMAL, " card PRNG ..... " _YELLOW_("[~]"));
        } else if (has_staticnonce == NONCE_NORMAL){
            PrintAndLogEx(NORMAL, " card PRNG ..... " "%s", prng_type ? _GREEN_("[+]") : _RED_("[-]"));
        }
        if (!skip_chk){
            PrintAndLogEx(NORMAL, " dictionary .... " _CYAN_("%s"), strlen(filename) ? filename : "N/A");
            if (fnlen2 > 0){
                PrintAndLogEx(NORMAL, " dictionaryII .. " _GREEN_("+%s"), strlen(filename2) ? filename2 : "N/A");
            } else {
                PrintAndLogEx(NORMAL, " dictionaryII .. " _YELLOW_("N/A"));
            }
        }
        PrintAndLogEx(NORMAL, " simulator ..... " "%s", sim ? _GREEN_(":>") : _RED_(":<"));
        PrintAndLogEx(NORMAL, " cloner ........ " "%s", clone ? _GREEN_(":>") : _RED_(":<"));
        PrintAndLogEx(NORMAL, " load card ..... " "%s", sim_card_present ? _GREEN_("no") : _RED_("yes"));
        PrintAndLogEx(NORMAL, " legacy auto ... " "%s", old ? "no" : _YELLOW_("yes"));
        PrintAndLogEx(NORMAL, " SIMD sets ..... " _YELLOW_("%d"), simd_sets);
        PrintAndLogEx(NORMAL, " SIMD neon ..... " "%s", has_neon ? _GREEN_(":>") : _RED_(":<"));
        PrintAndLogEx(NORMAL, " legacy chk .... " _MAGENTA_("%s"), legacy_mfchk ? "slow" : "fast");
        PrintAndLogEx(NORMAL, " ACK timeout ... " "%d", select_delay);
        if (max_keynum){
            PrintAndLogEx(NORMAL, " max keys ....... after" _RED_("%d restored"), max_keynum);
        }
        PrintAndLogEx(NORMAL, "------------------------------------------------------------------------");
        PrintAndLogEx(NORMAL, "");
    }
    switch(sectorno){
        case 0:
            CmdHF14AMfAcl("");
        /*case 3:
            CmdHF14AMfAcl("");
        default:
            break;*/
    }
    if(goto_slep){
        msleep(6500);
    }

    // Start the timer
    uint64_t t1 = msclock();


    // check the user supplied key
    keycheck_start:
    if (rand_slow){
        srand(time(NULL));
        slow = (rand() % 2);
    }
    if (dict_only){
        goto dict_start;
    }
    if (staticn){
        goto tryStaticnested;
       // goto start_darkside;
       // goto tryNested;
    }
    if (both_keys) {
        if (keytype == MF_KEY_A){
            keytype = MF_KEY_B;
        }
    }
    if (know_target_key == false) {
        PrintAndLogEx(WARNING, "no known key was supplied");
        srand(time(NULL));
        //bool helo = false;
        if ((rand() % 2)){
            srand(time(NULL));
            if (rand()%2){
                if (keytype == MF_KEY_A){
                    for (int i = 0; i < sizeof(mad_key); i++){
                        key[i] = mad_key[i];
                        PrintAndLogEx(INFO, "Randomly selected MAD key for known key A");
                    }
                } else {
                    for (int i = 0; i < sizeof(mad_key); i++){
                        key[i] = mad_key_b[i];
                        PrintAndLogEx(INFO, "Randomly selected MAD key for known key B");
                    }
                }
            } else {
                for (int i = 0; i < sizeof(def_key); i++){
                    key[i] = def_key[i];
                    PrintAndLogEx(INFO, "Randomly selected MAD key for known key");
                }
            }
            goto known_key;
        } else {
            PrintAndLogEx(WARNING, "above is required for running k-k attack only. Exiting . . . ");
            return PM3_SUCCESS;
        }
    } else {
        known_key:
        PrintAndLogEx(INFO, "----------------------- " _BLUE_("[ KNOWN KEY METHOD ]") " ----------------------");

        if (known_sec){
            int known_start = 0;
            int known_end = sector_cnt;
            if (_start > 0){
                known_start = _start;
            }
            if (_end > 0){
                known_end = _end;
            }
            for (int scno = known_start; scno < known_end; scno++){
                if (mfCheckKeys(mfFirstBlockOfSector(scno), keytype, true, 1, key, &key64) == PM3_SUCCESS) {
                    PrintAndLogEx(INFO, "target sector %3u key type %s -- using valid key [ " _GREEN_("%s") "] (used for nested / hardnested attack)",
                                  scno,
                                  (keytype == MF_KEY_B) ? _YELLOW_("[B]") : _GREEN_("[A]"),
                                  sprint_hex(key, sizeof(key))
                                 );

                    // Store the key for the nested / hardnested attack (if supplied by the user)
                    e_sector[scno].Key[keytype] = key64;
                    e_sector[scno].foundKey[keytype] = 'K';

                    ++num_found_keys;
                    if (max_keynum == num_found_keys){
                        goto all_found;
                    }
                } else {
                    know_target_key = false;
                    PrintAndLogEx(FAILED, "Key is wrong. Can't authenticate to sector"_RED_("(%3d)") " key type "_RED_("%c") " key " _RED_("%s"),
                                  scno,
                                  (keytype == MF_KEY_B) ? 'B' : 'A',
                                  sprint_hex(key, sizeof(key))
                                 );
                    PrintAndLogEx(WARNING, "falling back to dictionary");
                }
            }
            return PM3_SUCCESS;
        } else {
            if (mfCheckKeys(mfFirstBlockOfSector(sectorno), keytype, true, 1, key, &key64) == PM3_SUCCESS) {
                PrintAndLogEx(INFO, "target sector %3u key type %s -- using valid key [ " _GREEN_("%s") "] (used for nested / hardnested attack)",
                              sectorno,
                              (keytype == MF_KEY_B) ? _YELLOW_("[B]") : _GREEN_("[A]"),
                              sprint_hex(key, sizeof(key))
                             );

                // Store the key for the nested / hardnested attack (if supplied by the user)
                e_sector[sectorno].Key[keytype] = key64;
                e_sector[sectorno].foundKey[keytype] = 'K';

                ++num_found_keys;
                if (max_keynum == num_found_keys){
                    goto all_found;
                }
            } else {
                know_target_key = false;
                PrintAndLogEx(FAILED, "Key is wrong. Can't authenticate to sector"_RED_("(%3d)") " key type "_RED_("%c") " key " _RED_("%s"),
                              sectorno,
                              (keytype == MF_KEY_B) ? 'B' : 'A',
                              sprint_hex(key, sizeof(key))
                             );
                PrintAndLogEx(WARNING, "falling back to dictionary");
            }
        }

        for (int i = 0; i < sector_cnt; i++) {
            for (int j = MF_KEY_A; j <= MF_KEY_B; j++) {
                if (e_sector[i].foundKey[j] == 0) {
                    if (mfCheckKeys(mfFirstBlockOfSector(i), j, true, 1, key, &key64) == PM3_SUCCESS) {
                        e_sector[i].Key[j] = bytes_to_num(key, 6);
                        e_sector[i].foundKey[j] = '*';

                        // If the user supplied secctor / keytype was wrong --> just be nice and correct it ;)
                        if (know_target_key == false) {
                            num_to_bytes(e_sector[i].Key[j], 6, key);
                            know_target_key = true;
                            sectorno = i;
                            keytype = j;
                            PrintAndLogEx(SUCCESS, "target sector %3u key type %s -- found valid key [ " _GREEN_("%s") " ] (used for nested / hardnested attack)",
                                          i,
                                          (j == MF_KEY_B) ? _YELLOW_("[B]") : _GREEN_("[A]"),
                                          sprint_hex_inrow(key, sizeof(key))
                                         );
                        } else {
                            PrintAndLogEx(SUCCESS, "target sector %3u key type %s -- found valid key [ " _GREEN_("%s") " ]",
                                          i,
                                          (j == MF_KEY_B) ? _YELLOW_("[B]") : _GREEN_("[A]"),
                                          sprint_hex_inrow(key, sizeof(key))
                                         );
                        }
                        ++num_found_keys;
                        if (max_keynum == num_found_keys){
                            goto all_found;
                        }
                    }
                }
            }
        }

        if (both_keys){
            if (keytype == MF_KEY_A){
                goto keycheck_start;
            }
        }
        if (num_found_keys == sector_cnt * 2) {
            goto all_found;
        }
    }

    dict_start:
    int sec2 = (float)(msclock()-t1) / 1000.0;
    if (sec2 > max_timeout) {
        goto when_timeout_hits;
    }
    if (hard){
        goto tryHardnested;
    }
    if (skip_chk){
        goto dict_end;
    }
    bool load_success = true;
    if (has_filename) {
        if ((fnlen2 > 0 && used_first_dict)){
            used_second_dict = true;
            keyBlock = NULL;
            key_cnt = 0;
            res = loadFileDICTIONARY_safe(filename2, (void **) &keyBlock, 6, &key_cnt);
            if (res != PM3_SUCCESS || key_cnt == 0 || keyBlock == NULL) {
                PrintAndLogEx(FAILED, "An error occurred while loading the dictionary! (we will use the default keys now)");
                if (keyBlock != NULL) {
                    free(keyBlock);
                }
                load_success = false;
            }

        } else {
            res = loadFileDICTIONARY_safe(filename, (void **) &keyBlock, 6, &key_cnt);
            used_first_dict = true;
            if (res != PM3_SUCCESS || key_cnt == 0 || keyBlock == NULL) {
                PrintAndLogEx(FAILED, "An error occurred while loading the dictionary! (we will use the default keys now)");
                if (keyBlock != NULL) {
                    free(keyBlock);
                }
                load_success = false;
            }
        }
    }

    if ((has_filename == false || load_success == false || fchk_use_mem)) {
        keyBlock = calloc(ARRAYLEN(g_mifare_default_keys), 6);
        if (keyBlock == NULL) {
            free(e_sector);
            free(fptr);
            return PM3_EMALLOC;
        }

        for (int cnt = 0; cnt < ARRAYLEN(g_mifare_default_keys); cnt++) {
            num_to_bytes(g_mifare_default_keys[cnt], 6, keyBlock + cnt * 6);
        }
        key_cnt = ARRAYLEN(g_mifare_default_keys);
        PrintAndLogEx(SUCCESS, "loaded " _GREEN_("%2d") " keys from hardcoded default array", key_cnt);
    }

    PrintAndLogEx(INFO, "-----------------------"  _BLUE_("START DICTIONARY ATTACK") " -----------------------");

    //uint64_t t2 = msclock();
    if (legacy_mfchk){ //|| slow) {
        PrintAndLogEx(INFO, _CYAN_(">") NOLF);
        // Check all the sectors
        for (int i = 0; i < sector_cnt; i++) {
            for (int j = 0; j < 2; j++) {
                // Check if the key is known
                if (e_sector[i].foundKey[j] == 0) {
                    for (uint32_t k = 0; k < key_cnt; k++) {
                        PrintAndLogEx(NORMAL, _MAGENTA_("-") NOLF);
                        fflush(stdout);

                        if (mfCheckKeys(mfFirstBlockOfSector(i), j, true, 1, (keyBlock + (6 * k)), &key64) == PM3_SUCCESS) {
                            e_sector[i].Key[j] = bytes_to_num((keyBlock + (6 * k)), 6);
                            e_sector[i].foundKey[j] = 'F';
                            ++num_found_keys;
                            if (max_keynum == num_found_keys){
                                goto all_found;
                            }
                            break;
                        }
                    }
                }
            }
        }
        PrintAndLogEx(NORMAL, "");
    } 
    uint32_t chunksize = key_cnt > (PM3_CMD_DATA_SIZE / 6) ? (PM3_CMD_DATA_SIZE / 6) : key_cnt;
    bool firstChunk = true, lastChunk = false;
    int sstrat = 1;
    if (selected_strategy > 0){
        sstrat = selected_strategy;
    }
    for (uint8_t strategy = sstrat; strategy < 3; strategy++) {
        //if (max_timeout != 0){
        int sec = (float)(msclock()-t1)/ 1000.0;
        if (sec >= max_timeout){
            goto when_timeout_hits;
        }
        //}
        PrintAndLogEx(INFO, "strategy = %u", strategy);
        for (uint32_t i = 0; i < key_cnt; i += chunksize) {
            int sc = (float)(msclock()-t1)/ 1000.0;
            if (sc >= max_timeout) {
                goto when_timeout_hits;
            }
            if (kbd_enter_pressed()) {
                PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
                i = key_cnt;
                break; // Exit the loop
            }
            uint32_t size = ((key_cnt - i)  > chunksize) ? chunksize : key_cnt - i;
            // last chunk?
            if (size == key_cnt - i)
                lastChunk = true;

            res = mfCheckKeys_fast(sector_cnt, firstChunk, lastChunk, strategy, size, keyBlock + (i * 6), e_sector, fchk_use_mem);
            if (firstChunk){
                firstChunk = false;
            }
            if (res == PM3_SUCCESS) {
                i = key_cnt;
                break; // Exit the loop
            }
            if (explicit_strategy){
                break;
            }
        } // end chunks of keys
        firstChunk = true;
        lastChunk = false;
    } // end strategy

    // Analyse the dictionary attack
    for (int i = 0; i < sector_cnt; i++) {
        for (int j = MF_KEY_A; j <= MF_KEY_B; j++) {
            if (e_sector[i].foundKey[j] == 1) {
                e_sector[i].foundKey[j] = 'D';
                num_to_bytes(e_sector[i].Key[j], 6, tmp_key);

                // Store valid credentials for the nested / hardnested attack if none exist
                if (know_target_key == false) {
                    num_to_bytes(e_sector[i].Key[j], 6, key);
                    know_target_key = true;
                    sectorno = i;
                    keytype = j;
                    PrintAndLogEx(SUCCESS, "target sector %3u key type %s -- found valid key [ " _GREEN_("%s") " ] (for nested attacks)",
                                  i,
                                  (j == MF_KEY_B) ? _YELLOW_("[B]") : _GREEN_("[A]"),
                                  sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                 );
                } else {
                    PrintAndLogEx(SUCCESS, "target sector %3u key type %s -- found valid key [ " _GREEN_("%s") " ]",
                                  i,
                                  (j == MF_KEY_B) ? _YELLOW_("[B]") : _GREEN_("[A]"),
                                  sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                 );
                }
            }
        }
    }
    dict_end:
    if ((fnlen2 > 0 && !used_second_dict)){
        goto dict_start;
    }
    // Check if at least one sector key was found
        // Check if the darkside attack can be used
        if (ignore_prng){
            prng_type = true;
        }
        if (prng_type && has_staticnonce != NONCE_STATIC) {
//when_timeout_hits:
//tryDarkside:
            if (verbose) {
                PrintAndLogEx(INFO, "----------------------- " _BLUE_("[ START DARKSIDE ATTACK ]") " -----------------------");
            }
            isOK = mfDarkside(mfFirstBlockOfSector(sectorno), keytype + 0x60, &key64);

            switch (isOK) {
                case -1 :
                    if (!noclick){
                        PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                        goto noValidKeyFound;
                    } else {
                        goto dict_end;
                    }
                case -2 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (doesn't send NACK on authentication requests).");
                    goto noValidKeyFound;
                case -3 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (its random number generator is not predictable).");
                    goto noValidKeyFound;
                case -4 :
                    PrintAndLogEx(FAILED, "\nCard is not vulnerable to Darkside attack (its random number generator seems to be based on the wellknown");
                    PrintAndLogEx(FAILED, "generating polynomial with 16 effective bits only, but shows unexpected behaviour.");
                    goto noValidKeyFound;
                case -5 :
                    PrintAndLogEx(WARNING, "\naborted via keyboard.");
                    goto noValidKeyFound;
                default :
                    PrintAndLogEx(SUCCESS, "\nFound valid key [ " _GREEN_("%012" PRIx64) " ]\n", key64);
                    break;
            }

            // Store the keys
            num_to_bytes(key64, 6, key);
            e_sector[sectorno].Key[keytype] = key64;
            e_sector[sectorno].foundKey[keytype] = 'S';
            PrintAndLogEx(SUCCESS, "target sector %3u key type %s -- found valid key [ " _GREEN_("%012" PRIx64) " ] (used for nested / hardnested attack)",
                          sectorno,
                          (keytype == MF_KEY_B) ? _YELLOW_("[B]") : _GREEN_("[A]"),
                          key64
                         );
noValidKeyFound:
            PrintAndLogEx(FAILED, "No usable key was found!");
            if (iters > 0){
                if (iterations < iters){
                    iterations += 1;
                    PrintAndLogEx(SUCCESS, "Re-trying attack");
                    goto keycheck_start;
                }
            }
            PrintAndLogEx(ERR, "Exiting . . .");
            free(keyBlock);
            free(e_sector);
            free(fptr);
            return PM3_ESOFT;
when_timeout_hits:
            if (g_session.client_debug_level >= 1){
                PrintAndLogEx(DEBUG, "Timeout reached . . .");
            }
            if (iters > 0){
                if (iterations < iters){
                    iterations += 1;
                    PrintAndLogEx(SUCCESS, "Re-trying attack");
                    goto keycheck_start;
                }
            }
            PrintAndLogEx(ERR, "Exiting . . .");
            free(keyBlock);
            free(e_sector);
            free(fptr);
            return PM3_ESOFT;
    }

    free(keyBlock);
    // Clear the needed variables
    num_to_bytes(0, 6, tmp_key);

    // Iterate over each sector and key(A/B)
    if (_start > 0){
        current_sector_i = _start;
    }
    if (_end > 0){
        sector_cnt = _end;
    }
    if (limit_dict){
        if (_start > 0){
            current_sector_i = _start;
        }
        if (_end > 0){
            sector_cnt = _end;
        }
    }
    for (current_sector_i = 0; current_sector_i < sector_cnt; current_sector_i++) {
        for (current_key_type_i = 0; current_key_type_i < 2; current_key_type_i++) {

            // If the key is already known, just skip it
            if (e_sector[current_sector_i].foundKey[current_key_type_i] == 0) {

                if (has_staticnonce == NONCE_STATIC)
                    goto tryStaticnested;

                // Try the found keys are reused
                if (bytes_to_num(tmp_key, 6) != 0) {
                    // <!> The fast check --> mfCheckKeys_fast(sector_cnt, true, true, 2, 1, tmp_key, e_sector, false);
                    // <!> Returns false keys, so we just stick to the slower mfchk.
                    for (int i = 0; i < sector_cnt; i++) {
                        for (int j = MF_KEY_A; j <= MF_KEY_B; j++) {
                            // Check if the sector key is already broken
                            if (e_sector[i].foundKey[j])
                                continue;

                            // Check if the key works
                            if (mfCheckKeys(mfFirstBlockOfSector(i), j, true, 1, tmp_key, &key64) == PM3_SUCCESS) {
                                e_sector[i].Key[j] = bytes_to_num(tmp_key, 6);
                                e_sector[i].foundKey[j] = 'R';
                                PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ]",
                                              i,
                                              (j == MF_KEY_B) ? 'B' : 'A',
                                              sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                             );
                            }
                        }
                    }
                }
                // Clear the last found key
                num_to_bytes(0, 6, tmp_key);

                if (current_key_type_i == MF_KEY_B) {
                    if (e_sector[current_sector_i].foundKey[0] && !e_sector[current_sector_i].foundKey[1]) {
                        PrintAndLogEx(INFO, "reading B key of sector %3d with key type %c",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? 'B' : 'A');
                        uint8_t sectrail = (mfFirstBlockOfSector(current_sector_i) + mfNumBlocksPerSector(current_sector_i) - 1);

                        mf_readblock_t payload;
                        payload.blockno = sectrail;
                        payload.keytype = MF_KEY_A;

                        num_to_bytes(e_sector[current_sector_i].Key[0], 6, payload.key); // KEY A

                        clearCommandBuffer();
                        SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

                        if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) goto skipReadBKey;

                        if (resp.status != PM3_SUCCESS) goto skipReadBKey;

                        uint8_t *data = resp.data.asBytes;
                        key64 = bytes_to_num(data + 10, 6);
                        if (key64) {
                            e_sector[current_sector_i].foundKey[current_key_type_i] = 'A';
                            e_sector[current_sector_i].Key[current_key_type_i] = key64;
                            num_to_bytes(key64, 6, tmp_key);
                            PrintAndLogEx(SUCCESS, "target sector %3u key type %c -- found valid key [ " _GREEN_("%s") " ]",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? 'B' : 'A',
                                          sprint_hex_inrow(tmp_key, sizeof(tmp_key))
                                         );
                        } else {
                            if (verbose) {
                                PrintAndLogEx(WARNING, "unknown B key: sector: %3d key type: %c",
                                              current_sector_i,
                                              (current_key_type_i == MF_KEY_B) ? 'B' : 'A'
                                             );
                                PrintAndLogEx(ERR, " -- reading the B key was not possible, maybe due to access rights?");

                            }

                        }
                    }
                }

                if (rand_slow){
                    srand(time(0));
                    slow = rand() % 2;
                }
                // Use the nested / hardnested attack
skipReadBKey:
                if (e_sector[current_sector_i].foundKey[current_key_type_i] == 0) {

                    if (has_staticnonce == NONCE_STATIC){
                        if (ignore_prng){
                            PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                            //prng_type = false;
                            goto tryNested;
                        } else {
                            goto tryStaticnested;
                        }
                    }
                    if (rand_slow){
                        srand(time(0));
                        slow = rand() % 2;
                    }
                    
tryNested:
                    int sec3 = (float)(msclock()-t1)/ 1000.0;
                    if (sec3 > max_timeout) {
                        goto when_timeout_hits;
                    }
                    if (prng_type && (nested_failed == false)) {
                        uint8_t retries = 0;
                        if (verbose) {
                            PrintAndLogEx(INFO, "~~~~~~~~~~~~~~~~~~~~~~~ " _BLUE_("[ START NESTED ATTACK ]") " ~~~~~~~~~~~~~~~~~~~~~~~");
                            PrintAndLogEx(INFO, "sector no %3d, target key type %s",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? _YELLOW_("(B)") : _GREEN_("[A]"));
                        }
                        isOK = mfnested(mfFirstBlockOfSector(sectorno), keytype, key, mfFirstBlockOfSector(current_sector_i), current_key_type_i, tmp_key, calibrate);

                        switch (isOK) {
                            case PM3_ETIMEOUT: {
                                PrintAndLogEx(ERR, "\nError: No response from Proxmark3.");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                            case PM3_EOPABORTED: {
                                if (click_restart){
                                    goto keycheck_start;
                                }
                                if (!noclick){
                                    PrintAndLogEx(WARNING, "\nButton pressed. Aborted.");
                                    free(e_sector);
                                    free(fptr);
                                    return PM3_EOPABORTED;
                                } else {
                                    goto tryNested;
                                }
                            }
                            case PM3_EFAILED: {
                                PrintAndLogEx(FAILED, "Tag isn't vulnerable to Nested Attack (PRNG is probably not predictable).");
                                PrintAndLogEx(INFO, "Trying hardnested");
                                goto tryHardnested;
                            }
                            case PM3_ESOFT: {
                                // key not found
                                calibrate = false;
                                // this can happen on some old cards, it's worth trying some more before switching to slower hardnested
                                if (retries++ < nested_attempts) {
                                    PrintAndLogEx(FAILED, "Nested attack failed, trying again (%i/%i)", retries, nested_attempts);
                                    goto tryNested;
                                } else {
                                    PrintAndLogEx(FAILED, "Nested attack failed, moving to hardnested");
                                    nested_failed = true;
                                    goto tryHardnested;
                                }
                                break;
                            }
                            case PM3_SUCCESS: {
                                calibrate = false;
                                e_sector[current_sector_i].Key[current_key_type_i] = bytes_to_num(tmp_key, 6);
                                e_sector[current_sector_i].foundKey[current_key_type_i] = 'N';
                                break;
                            }
                            default: {
                                PrintAndLogEx(ERR, "UNKNOWN ERROR");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                        }

                    } else {
tryHardnested:
                        int sec4 = (float)(msclock()-t1)/ 1000.0;
                        if (sec4 > max_timeout) {
                            goto when_timeout_hits;
                        }
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _GREEN_("[ START HARDNESTED ATTACK ]") " =======================");
                            if (rand_slow){
                            PrintAndLogEx(INFO, "[ sec -> %3d | tkey -> %s | slow -> %s]",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? _YELLOW_("B") : _GREEN_("[A]"),
                                          _RED_("RANDOM"));

                            } else {
                            PrintAndLogEx(INFO, "[ sec -> %3d | tkey -> %s | slow -> %s]",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? _YELLOW_("B") : _GREEN_("[A]"),
                                          slow ? _GREEN_("[+]") : _YELLOW_("[-]"));

                            }
                        }

                        if (noncelen > 0){
                            isOK = mfnestedhard(mfFirstBlockOfSector(sectorno), keytype, key, mfFirstBlockOfSector(current_sector_i), current_key_type_i, NULL, true, false, slow, use_tests, &foundkey, nonce_file);
                        } else {
                            isOK = mfnestedhard(mfFirstBlockOfSector(sectorno), keytype, key, mfFirstBlockOfSector(current_sector_i), current_key_type_i, NULL, false, false, slow, use_tests, &foundkey, NULL);
                        }
                        if (tests == 0){
                            DropField();
                        }
                        if (isOK) {
                            switch (isOK) {
                                case 1: {
                                    PrintAndLogEx(ERR, "\nError: No response from Proxmark3");
                                    goto tryHardnested;
                                    //break;
                                }
                                case 2: {
                                    if (click_restart){
                                        goto keycheck_start;
                                    }
                                    if (!noclick){
                                        PrintAndLogEx(NORMAL, "\nButton pressed, user aborted");
                                        break;
                                    } else {
                                        goto tryHardnested;
                                    }
                                }
                                default: {
                                    break;
                                }
                            }
                            free(e_sector);
                            free(fptr);
                            return PM3_ESOFT;
                        }

                        // Copy the found key to the tmp_key variale (for the following print statement, and the mfCheckKeys above)
                        num_to_bytes(foundkey, 6, tmp_key);
                        e_sector[current_sector_i].Key[current_key_type_i] = foundkey;
                        e_sector[current_sector_i].foundKey[current_key_type_i] = 'H';
                    }

                    if (has_staticnonce == NONCE_STATIC) {
tryStaticnested:
                        if (verbose) {
                            PrintAndLogEx(INFO, "======================= " _RED_("[ START STATIC NESTED ATTACK ]") " =======================");
                            PrintAndLogEx(INFO, "sector no %3d, target key type %c",
                                          current_sector_i,
                                          (current_key_type_i == MF_KEY_B) ? 'B' : 'A');
                        }

                        isOK = mfStaticNested(sectorno, keytype, key, mfFirstBlockOfSector(current_sector_i), current_key_type_i, tmp_key);
                        DropField();
                        switch (isOK) {
                            case PM3_ETIMEOUT: {
                                PrintAndLogEx(ERR, "\nError: No response from Proxmark3");
                                free(e_sector);
                                free(fptr);
                                return PM3_ESOFT;
                            }
                            case PM3_EOPABORTED: {
                                if (click_restart){
                                    goto keycheck_start;
                                }
                                if (!noclick){
                                    PrintAndLogEx(WARNING, "\nButton pressed, user aborted");
                                    free(e_sector);
                                    free(fptr);
                                    return PM3_EOPABORTED;
                                } else {
                                    goto tryStaticnested;
                                }
                            }
                            case PM3_SUCCESS: {
                                e_sector[current_sector_i].Key[current_key_type_i] = bytes_to_num(tmp_key, 6);
                                e_sector[current_sector_i].foundKey[current_key_type_i] = 'S';
                                break;
                            }
                            default: {
                                break;
                            }
                        }
                    }

                    // Check if the key was found
                    if (e_sector[current_sector_i].foundKey[current_key_type_i]) {
                        PrintAndLogEx(SUCCESS, "[ sec -> %3u | key -> %s ]",
                                      current_sector_i,
                                      (current_key_type_i == MF_KEY_B) ? _YELLOW_("B") : _GREEN_("[A]"));
                                     
                    }
                }
                if (nest_iter){
                    if (iters > 0){
                        if (iterations < iters){
                            if ((nested_failed || iterations < 3)){
                                nested_failed = false;
                            }
                            iterations += 1;
                            srand(time(NULL));
                            if (rand()%2){
                                goto tryNested;
                            } else {
                                goto tryHardnested;
                            }
                        }
                    }
                }
            }
        }
    }
if(all_simd){
    if (instridx < simd_sets){
        instridx += 1;
        goto set_simd;
    }
}
all_found:
    for (uint8_t i = 0; i < sector_cnt; i++) {
        if (e_sector[i].foundKey[MF_KEY_A]){
            found_keynum += 1;
        }
        if (e_sector[i].foundKey[MF_KEY_B]){
            found_keynum += 1;
        }
    }
    PrintAndLogEx(SUCCESS, "Found %d keys", found_keynum);
    // MAD detection
    //if (e_sector[MF_MAD1_SECTOR].foundKey[0] && e_sector[MF_MAD1_SECTOR].Key[MF_KEY_A] == 0xA0A1A2A3A4A5) {
    bool mad_a = (e_sector[MF_MAD1_SECTOR].Key[MF_KEY_A] == 0xA0A1A2A3A4A5);
    bool mad_b = (e_sector[MF_MAD1_SECTOR].Key[MF_KEY_B] == 0xB0B1B2B3B4B5);
    if ((mad_a || mad_b)){
        if (nomad){
            goto nomad_oneliner_report;
        }
        if (mad_a){
            PrintAndLogEx(SUCCESS, "MAD key detected (A) ---- ", _GREEN_("A0A1A2A3A4A5"));
        }
        if (mad_b){
            PrintAndLogEx(SUCCESS, "MAD key detected (B) ---- ", _GREEN_("B0B1B2B3B4B5"));
        }
        if (keytype == MF_KEY_B){
            CmdHF14AMfMAD("-v --be --dch -k b0b1b2b3b4b5 -b");
        } else {
            CmdHF14AMfMAD("-v --be --dch -k a0a1a2a3a4a5");
        }
        srand(time(NULL));
        if(rand()%2){
            CmdHF14AMfMAD("-v --be --dch -k ffffffffffff -b");
        }
        nomad_oneliner_report:
        PrintAndLogEx(SUCCESS, "MAD key detected");
    }
    // Show the results to the user
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, _GREEN_("[KEYS]"));
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "");
    printKeyTable(sector_cnt, e_sector);
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "");

    if(append_dict){
        PrintAndLogEx(INFO, "Appending found keys to default dictionary");
        FILE *kf = fopen("./client/dictionaries/mfc_default_keys.dic", "a");
        if (kf == NULL) {
            PrintAndLogEx(ERR, "Could not open keyfile -> mfc_default_keys");
            goto key_append_error;
        }
        char strA[13] = {0x0a};
        char strB[13] = {0x0a};
        for (uint8_t i = 0; i < sector_cnt; i++) {
            if (e_sector[i].foundKey[MF_KEY_A]){
                snprintf(strA, sizeof(strA), "%012" PRIX64, e_sector[i].Key[0]);
            }
            if (e_sector[i].foundKey[MF_KEY_B]){
                snprintf(strB, sizeof(strB), "%012" PRIX64, e_sector[i].Key[1]);
            }
            fputs(strA, kf);
            if (!append_dict_only_a){
                fputs(strB, kf);
            }
        }
        fclose(kf);
    }
    key_append_error:
    if ((nosave && !emul)){
        goto __sim;
    }

    if (createMfcKeyDump(fptr, sector_cnt, e_sector) != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "Failed to save keys to file");
    }
    if (!emul){
        goto __sim;
    }
    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_EML_MEMCLR, NULL, 0);

    PrintAndLogEx(SUCCESS, "Transferring keys to simulator memory");

    for (current_sector_i = 0; current_sector_i < sector_cnt; current_sector_i++) {
        mfEmlGetMem(block, current_sector_i, 1);
        if (e_sector[current_sector_i].foundKey[MF_KEY_A])
            num_to_bytes(e_sector[current_sector_i].Key[MF_KEY_A], 6, block);
        if (e_sector[current_sector_i].foundKey[MF_KEY_B])
            num_to_bytes(e_sector[current_sector_i].Key[MF_KEY_B], 6, block + 10);

        mfEmlSetMem(block, mfFirstBlockOfSector(current_sector_i) + mfNumBlocksPerSector(current_sector_i) - 1, 1);
    }


    // use ecfill trick
    FastDumpWithEcFill(sector_cnt);

    bytes = block_cnt * MFBLOCK_SIZE;
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(ERR, "Fail, cannot allocate memory");
        free(e_sector);
        free(fptr);
        return PM3_EMALLOC;
    }
    if (fulldump) {
        PrintAndLogEx(INFO, "downloading card contents from emulator memory");
        if (!GetFromDevice(BIG_BUF_EML, dump, bytes, 0, NULL, 0, NULL, 5000, false)) {
            PrintAndLogEx(ERR, "Fail, transfer from device time-out");
            free(e_sector);
            free(dump);
            free(fptr);
            return PM3_ETIMEOUT;
        }
        free(fptr);
        fptr = GenerateFilename(dump_prefix, "-dump");
        if (fptr == NULL) {
            free(dump);
            free(e_sector);
            free(fptr);
            return PM3_ESOFT;
        }
        strcpy(outname, fptr);
        free(fptr);
        saveFile(outname, ".bin", dump, bytes);
        if (save_eml){
            PrintAndLogEx(INFO, "Saving as .eml");
            saveFileEML(outname, dump, bytes, MFBLOCK_SIZE);
        }
        if(save_json){
            PrintAndLogEx(INFO, "Saving as .json");
            iso14a_mf_extdump_t xdump;
            xdump.card_info = card;
            xdump.dump = dump;
            xdump.dumplen = bytes;
            saveFileJSON(outname, jsfCardMemory, (uint8_t *)&xdump, sizeof(xdump), NULL);
        }
    }
    t1 = msclock() - t1;
    PrintAndLogEx(INFO, "exec time: " _MAGENTA_("%.0f") " seconds", (float)t1 / 1000.0);
    if (clone){

    }
    __sim:
    if(sim){
        if(nofill){
            goto simulator_init;
        }
        if (sim_card_present){
            int ack_tmp = ack_timeout;
            if (!follow){
                retry_select = true;
                ack_timeout = 1500;
            }
            try_select_sim:
            clearCommandBuffer();
            SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
            PacketResponseNG nresp;
            if (WaitForResponseTimeout(CMD_ACK, &nresp, ack_timeout) == false) {
                if (retry_select){
                    msleep(select_delay);
                    goto try_select_sim;
                } else {
                    PrintAndLogEx(WARNING, "iso14443a card select timeout");
                    return PM3_ETIMEOUT;
                }
            }
            uint64_t nstat = nresp.oldarg[0];
            if (nstat == 0) {
                if (retry_select){
                    msleep(select_delay);
                    goto try_select;
                } else {
                    PrintAndLogEx(WARNING, "iso14443a card select failed");
                    return select_status;
                }
            }
            int ecfill_res = 0;
            if (m0){
                ecfill_res = CmdHF14AMfECFill("--mini");
            } else if (m1){
               ecfill_res = CmdHF14AMfECFill("--1k");
            } else if (m2){
               ecfill_res = CmdHF14AMfECFill("--2k");
            } else if (m4){
               ecfill_res = CmdHF14AMfECFill("--4k");
            }
            if (!follow){
                retry_select = false;
                ack_timeout = ack_tmp;
            }
            if (ecfill_res == PM3_SUCCESS){
                PrintAndLogEx(SUCCESS, "Filled emulator with contents from card");
            } else {
                PrintAndLogEx(FAILED, "Cannot fill emulator with contents from card");
                if(sim_loop){
                    retry_select = true;
                    msleep(1000);
                    goto simulator_init;
                } else {
                    return PM3_SUCCESS;
                }
            }
        } else {
            char eload_cmd[30] = "";
            char eload_fmt[10] = "-f %s.bin";
            sprintf(eload_cmd, eload_fmt, outname);
            CmdHF14AMfELoad(eload_cmd);
        }
        simulator_init:
        if(eview){
            PrintAndLogEx(SUCCESS, "Emulator contents:");
            if (m0){
               CmdHF14AMfEView("--mini");
            } else if (m1){
               CmdHF14AMfEView("--1k");
            } else if (m2){
               CmdHF14AMfEView("--2k");
            } else if (m4){
               CmdHF14AMfEView("--4k");
            }
            if (verbose){
                PrintAndLogEx(SUCCESS, "Keys:");
                CmdHF14AMfEKeyPrn("");
            }
            if(goto_slep){
                msleep(10000);
            }
        }
        PrintAndLogEx(SUCCESS, "Starting simulation . . . ");
        int sim_res = 0;
        if (m0){
            sim_res = CmdHF14AMfSim("--mini -v --fb -n 0 -i -b");
        } else if (m1){
            sim_res = CmdHF14AMfSim("--1k -v --fb -n 0 -i -b");
        } else if (m2){
            sim_res = CmdHF14AMfSim("--2k -v --fb -n 0 -i -b");
        } else if (m4){
            sim_res = CmdHF14AMfSim("--4k -v --fb -n 0 -i -b");
        }
        if (sim_res == PM3_SUCCESS){
            PrintAndLogEx(SUCCESS, "Finished simulation");
        } else {
            PrintAndLogEx(FAILED, "Finished simulation with error code %d", sim_res);
        }
        if(sim_loop){
            goto simulator_init;
        }
        t1 = msclock() - t1;
        PrintAndLogEx(INFO, "simulation time: " _GREEN_("%.0f") " seconds", (float)t1 / 1000.0);
    }

    if(debug){
        g_session.client_debug_level = prv_dbg;
    }
    //free(dump);
    free(e_sector);
    return PM3_SUCCESS;
}

static int mfLoadKeys(uint8_t **pkeyBlock, uint32_t *pkeycnt, uint8_t *userkey, int userkeylen, const char *filename, int fnlen) {
    // Handle Keys
    *pkeycnt = 0;
    *pkeyBlock = NULL;
    uint8_t *p;
    // Handle user supplied key
    // (it considers *pkeycnt and *pkeyBlock as possibly non-null so logic can be easily reordered)
    if (userkeylen >= 6) {
        int numKeys = userkeylen / 6;
        p = realloc(*pkeyBlock, (*pkeycnt + numKeys) * 6);
        if (!p) {
            PrintAndLogEx(FAILED, "cannot allocate memory for Keys");
            free(*pkeyBlock);
            return PM3_EMALLOC;
        }
        *pkeyBlock = p;

        memcpy(*pkeyBlock + *pkeycnt * 6, userkey, numKeys * 6);

        for (int i = 0; i < numKeys; i++) {
            PrintAndLogEx(INFO, "[%2d] key %s", *pkeycnt + i, sprint_hex(*pkeyBlock + (*pkeycnt + i) * 6, 6));
        }
        *pkeycnt += numKeys;
    }

    // Handle default keys
    p = realloc(*pkeyBlock, (*pkeycnt + ARRAYLEN(g_mifare_default_keys)) * 6);
    if (!p) {
        PrintAndLogEx(FAILED, "cannot allocate memory for Keys");
        free(*pkeyBlock);
        return PM3_EMALLOC;
    }
    *pkeyBlock = p;
    // Copy default keys to list
    for (int i = 0; i < ARRAYLEN(g_mifare_default_keys); i++) {
        num_to_bytes(g_mifare_default_keys[i], 6, (uint8_t *)(*pkeyBlock + (*pkeycnt + i) * 6));
        PrintAndLogEx(DEBUG, "[%2d] key %s", *pkeycnt + i, sprint_hex(*pkeyBlock + (*pkeycnt + i) * 6, 6));
    }
    *pkeycnt += ARRAYLEN(g_mifare_default_keys);

    // Handle user supplied dictionary file
    if (fnlen > 0) {
        uint32_t loaded_numKeys = 0;
        uint8_t *keyBlock_tmp = NULL;
        int res = loadFileDICTIONARY_safe(filename, (void **) &keyBlock_tmp, 6, &loaded_numKeys);
        if (res != PM3_SUCCESS || loaded_numKeys == 0 || *pkeyBlock == NULL) {
            PrintAndLogEx(FAILED, "An error occurred while loading the dictionary!");
            free(keyBlock_tmp);
            free(*pkeyBlock);
            return PM3_EFILE;
        } else {
            p = realloc(*pkeyBlock, (*pkeycnt + loaded_numKeys) * 6);
            if (!p) {
                PrintAndLogEx(FAILED, "cannot allocate memory for Keys");
                free(keyBlock_tmp);
                free(*pkeyBlock);
                return PM3_EMALLOC;
            }
            *pkeyBlock = p;
            memcpy(*pkeyBlock + *pkeycnt * 6, keyBlock_tmp, loaded_numKeys * 6);
            *pkeycnt += loaded_numKeys;
            free(keyBlock_tmp);
        }
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfChk_fast(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf fchk",
                  "This is a improved checkkeys method speedwise. It checks MIFARE Classic tags sector keys against a dictionary file with keys",
                  "hf mf fchk --mini -k FFFFFFFFFFFF              --> Key recovery against MIFARE Mini\n"
                  "hf mf fchk --1k -k FFFFFFFFFFFF                --> Key recovery against MIFARE Classic 1k\n"
                  "hf mf fchk --2k -k FFFFFFFFFFFF                --> Key recovery against MIFARE 2k\n"
                  "hf mf fchk --4k -k FFFFFFFFFFFF                --> Key recovery against MIFARE 4k\n"
                  "hf mf fchk --1k -f mfc_default_keys.dic        --> Target 1K using default dictionary file\n"
                  "hf mf fchk --1k --emu                          --> Target 1K, write keys to emulator memory\n"
                  "hf mf fchk --1k --dump                         --> Target 1K, write keys to file\n"
                  "hf mf fchk --1k --mem                          --> Target 1K, use dictionary from flash memory");

    void *argtable[] = {
        arg_param_begin,
        arg_strx0("k", "key", "<hex>", "Key specified as 12 hex symbols"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (default)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "emu", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dump", "Dump found keys to binary file"),
        arg_lit0(NULL, "mem", "Use dictionary from flashmemory"),
        arg_str0("f", "file", "<fn>", "filename of dictionary"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int keylen = 0;
    uint8_t key[255 * 6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);

    bool transferToEml = arg_get_lit(ctx, 6);
    bool createDumpFile = arg_get_lit(ctx, 7);
    bool use_flashmemory = arg_get_lit(ctx, 8);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 9), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    CLIParserFree(ctx);

    //validations

    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    uint8_t sectorsCnt = 1;
    if (m0) {
        sectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        sectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        sectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        sectorsCnt = MIFARE_4K_MAXSECTOR;
    } else {
        PrintAndLogEx(WARNING, "Defaulting to 1k");
        sectorsCnt = MIFARE_1K_MAXSECTOR;
    }

    uint8_t *keyBlock = NULL;
    uint32_t keycnt = 0;
    int ret = mfLoadKeys(&keyBlock, &keycnt, key, keylen, filename, fnlen);
    if (ret != PM3_SUCCESS) {
        return ret;
    }

    // create/initialize key storage structure
    sector_t *e_sector = NULL;
    int32_t res = initSectorTable(&e_sector, sectorsCnt);
    if (res != sectorsCnt) {
        free(keyBlock);
        return PM3_EMALLOC;
    }

    uint32_t chunksize = keycnt > (PM3_CMD_DATA_SIZE / 6) ? (PM3_CMD_DATA_SIZE / 6) : keycnt;
    bool firstChunk = true, lastChunk = false;

    int i = 0;
    // time
    uint64_t t1 = msclock();

    if (use_flashmemory) {
        PrintAndLogEx(SUCCESS, "Using dictionary in flash memory");
        mfCheckKeys_fast(sectorsCnt, true, true, 1, 0, keyBlock, e_sector, use_flashmemory);
        mfCheckKeys_fast(sectorsCnt, true, true, 2, 0, keyBlock, e_sector, use_flashmemory);
        mfCheckKeys_fast(sectorsCnt, true, lastChunk, 1, 0, keyBlock, e_sector, use_flashmemory);
        mfCheckKeys_fast(sectorsCnt, true, lastChunk, 2, 0, keyBlock, e_sector, use_flashmemory);
    } else {

        // strategys. 1= deep first on sector 0 AB,  2= width first on all sectors
        for (uint8_t strategy = 1; strategy < 3; strategy++) {
            PrintAndLogEx(INFO, "Running strategy %u", strategy);

            // main keychunk loop
            for (i = 0; i < keycnt; i += chunksize) {

                if (kbd_enter_pressed()) {
                    PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
                    goto out;
                }

                uint32_t size = ((keycnt - i)  > chunksize) ? chunksize : keycnt - i;

                // last chunk?
                if (size == keycnt - i)
                    lastChunk = true;

                res = mfCheckKeys_fast(sectorsCnt, firstChunk, lastChunk, strategy, size, keyBlock + (i * 6), e_sector, false);

                if (kbd_enter_pressed()) {
                    PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
                    goto out;
                }

                if (firstChunk){
                    firstChunk = false;
                }
                // all keys,  aborted
                if ((res == PM3_SUCCESS || res == 2)){
                    goto out;
                }
            } // end chunks of keys
            firstChunk = true;
            lastChunk = false;
        } // end strategy
    }
out:
    t1 = msclock() - t1;
    PrintAndLogEx(INFO, "time in fchk: " _GREEN_("%.1fs") "\n", (float)(t1 / 1000.0));

    // check..
    uint8_t found_keys = 0;
    for (i = 0; i < sectorsCnt; ++i) {

        if (e_sector[i].foundKey[0])
            found_keys++;

        if (e_sector[i].foundKey[1])
            found_keys++;
    }

    if (found_keys == 0) {
        PrintAndLogEx(WARNING, "No keys found");
    } else {

        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

        printKeyTable(sectorsCnt, e_sector);

        if (use_flashmemory && found_keys == (sectorsCnt << 1)) {
            PrintAndLogEx(SUCCESS, "Card dumped as well. run " _YELLOW_("`%s %c`"),
                          "hf mf esave",
                          GetFormatFromSector(sectorsCnt)
                         );
        }

        if (transferToEml) {
            // fast push mode
            g_conn.block_after_ACK = true;
            uint8_t block[16] = {0x00};
            for (i = 0; i < sectorsCnt; ++i) {
                uint8_t b = mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1;
                mfEmlGetMem(block, b, 1);

                if (e_sector[i].foundKey[0])
                    num_to_bytes(e_sector[i].Key[0], 6, block);

                if (e_sector[i].foundKey[1])
                    num_to_bytes(e_sector[i].Key[1], 6, block + 10);

                if (i == sectorsCnt - 1) {
                    // Disable fast mode on last packet
                    g_conn.block_after_ACK = false;
                }
                mfEmlSetMem(block, b, 1);
            }
            PrintAndLogEx(SUCCESS, "Found keys have been transferred to the emulator memory");

            if (found_keys == (sectorsCnt << 1)) {
                FastDumpWithEcFill(sectorsCnt);
            }
        }

        if (createDumpFile) {

            char *fptr = GenerateFilename("hf-mf-", "-key.bin");
            if (createMfcKeyDump(fptr, sectorsCnt, e_sector) != PM3_SUCCESS) {
                PrintAndLogEx(ERR, "Failed to save keys to file");
            }
            free(fptr);
        }
    }

    free(keyBlock);
    free(e_sector);
    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

static int CmdHF14AMfChk(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf chk",
                  "Check keys on MIFARE Classic card",
                  "hf mf chk --mini -k FFFFFFFFFFFF              --> Check all sectors, all keys against MIFARE Mini\n"
                  "hf mf chk --1k -k FFFFFFFFFFFF                --> Check all sectors, all keys against MIFARE Classic 1k\n"
                  "hf mf chk --2k -k FFFFFFFFFFFF                --> Check all sectors, all keys against MIFARE 2k\n"
                  "hf mf chk --4k -k FFFFFFFFFFFF                --> Check all sectors, all keys against MIFARE 4k\n"
                  "hf mf chk --1k --emu                          --> Check all sectors, all keys, 1K, and write to emulator memory\n"
                  "hf mf chk --1k --dump                         --> Check all sectors, all keys, 1K, and write to file\n"
                  "hf mf chk -a --tblk 0 -f mfc_default_keys.dic --> Check dictionary against block 0, key A");

    void *argtable[] = {
        arg_param_begin,
        arg_strx0("k", "key", "<hex>", "Key specified as 12 hex symbols"),
        arg_int0(NULL, "tblk", "<dec>", "Target block number"),
        arg_lit0("a", NULL, "Target Key A"),
        arg_lit0("b", NULL, "Target Key B"),
        arg_lit0("*", "all", "Target both key A & B (default)"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (default)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "emu", "Fill simulator keys from found keys"),
        arg_lit0(NULL, "dump", "Dump found keys to binary file"),
        arg_str0("f", "file", "<fn>", "Filename of dictionary"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int keylen = 0;
    uint8_t key[255 * 6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    int blockNo = arg_get_int_def(ctx, 2, -1);

    uint8_t keyType = 2;

    if ((arg_get_lit(ctx, 3) && arg_get_lit(ctx, 4)) || arg_get_lit(ctx, 5)) {
        keyType = 2;
    } else if (arg_get_lit(ctx, 3)) {
        keyType = MF_KEY_A;
    } else if (arg_get_lit(ctx, 4)) {
        keyType = MF_KEY_B;
    }

    bool m0 = arg_get_lit(ctx, 6);
    bool m1 = arg_get_lit(ctx, 7);
    bool m2 = arg_get_lit(ctx, 8);
    bool m4 = arg_get_lit(ctx, 9);

    bool transferToEml = arg_get_lit(ctx, 10);
    bool createDumpFile = arg_get_lit(ctx, 11);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 12), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    CLIParserFree(ctx);
    bool singleSector = blockNo > -1;
    if (! singleSector) {
        // start from first trailer block
        blockNo = 3;
    }

    //validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    }

    uint8_t SectorsCnt = 1;
    if (m0) {
        SectorsCnt = MIFARE_MINI_MAXSECTOR;
    } else if (m1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    } else if (m2) {
        SectorsCnt = MIFARE_2K_MAXSECTOR;
    } else if (m4) {
        SectorsCnt = MIFARE_4K_MAXSECTOR;
    }

    if (singleSector) {
        uint8_t MinSectorsCnt = 0;
        // find a MIFARE type that can accommodate the provided block number
        uint8_t s =  mfSectorNum(blockNo);
        if (s < MIFARE_MINI_MAXSECTOR) {
            MinSectorsCnt = MIFARE_MINI_MAXSECTOR;
        } else if (s < MIFARE_1K_MAXSECTOR) {
            MinSectorsCnt = MIFARE_1K_MAXSECTOR;
        } else if (s < MIFARE_2K_MAXSECTOR) {
            MinSectorsCnt = MIFARE_2K_MAXSECTOR;
        } else if (s < MIFARE_4K_MAXSECTOR) {
            MinSectorsCnt = MIFARE_4K_MAXSECTOR;
        } else {
            PrintAndLogEx(WARNING, "Provided block out of possible MIFARE Type memory map");
            return PM3_EINVARG;
        }
        if (SectorsCnt == 1) {
            SectorsCnt = MinSectorsCnt;
        } else if (SectorsCnt < MinSectorsCnt) {
            PrintAndLogEx(WARNING, "Provided block out of provided MIFARE Type memory map");
            return PM3_EINVARG;
        }
    }
    if (SectorsCnt == 1) {
        SectorsCnt = MIFARE_1K_MAXSECTOR;
    }

    uint8_t *keyBlock = NULL;
    uint32_t keycnt = 0;
    int ret = mfLoadKeys(&keyBlock, &keycnt, key, keylen, filename, fnlen);
    if (ret != PM3_SUCCESS) {
        return ret;
    }

    uint64_t key64 = 0;

    // create/initialize key storage structure
    sector_t *e_sector = NULL;
    int32_t res = initSectorTable(&e_sector, SectorsCnt);
    if (res != SectorsCnt) {
        free(keyBlock);
        return PM3_EMALLOC;
    }

    uint8_t trgKeyType = MF_KEY_A;
    uint16_t max_keys = keycnt > KEYS_IN_BLOCK ? KEYS_IN_BLOCK : keycnt;

    PrintAndLogEx(INFO, "Start check for keys...");
    PrintAndLogEx(INFO, "*" NOLF);

    // fast push mode
    g_conn.block_after_ACK = true;

    // clear trace log by first check keys call only
    bool clearLog = true;

    // time
    uint64_t t1 = msclock();

    // check keys.
    for (trgKeyType = (keyType == 2) ? 0 : keyType; trgKeyType < 2; (keyType == 2) ? (++trgKeyType) : (trgKeyType = 2)) {

        // loop sectors but block is used as to keep track of from which blocks to test
        int b = blockNo;
        for (int i = mfSectorNum(b); i < SectorsCnt; ++i) {

            // skip already found keys.
            if (e_sector[i].foundKey[trgKeyType]) continue;

            for (uint32_t c = 0; c < keycnt; c += max_keys) {

                PrintAndLogEx(NORMAL, "." NOLF);
                fflush(stdout);

                if (kbd_enter_pressed()) {
                    PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
                    goto out;
                }

                uint32_t size = keycnt - c > max_keys ? max_keys : keycnt - c;

                if (mfCheckKeys(b, trgKeyType, clearLog, size, &keyBlock[6 * c], &key64) == PM3_SUCCESS) {
                    e_sector[i].Key[trgKeyType] = key64;
                    e_sector[i].foundKey[trgKeyType] = true;
                    clearLog = false;
                    break;
                }
                clearLog = false;
            }
            if (singleSector)
                break;
            b < 127 ? (b += 4) : (b += 16);
        }
    }
    t1 = msclock() - t1;
    PrintAndLogEx(INFO, "\ntime in checkkeys " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);

    // 20160116 If Sector A is found, but not Sector B,  try just reading it of the tag?
    if (keyType != MF_KEY_B) {
        PrintAndLogEx(INFO, "testing to read key B...");

        // loop sectors but block is used as to keep track of from which blocks to test
        int b = blockNo;
        for (int i = mfSectorNum(b); i < SectorsCnt; i++) {

            // KEY A but not KEY B
            if (e_sector[i].foundKey[0] && !e_sector[i].foundKey[1]) {

                uint8_t sectrail = mfSectorTrailerOfSector(i);
                PrintAndLogEx(INFO, "Sector: %u, First block: %u, Last block: %u, Num of blocks: %u", i, mfFirstBlockOfSector(i), sectrail, mfNumBlocksPerSector(i));
                PrintAndLogEx(INFO, "Reading sector trailer");

                mf_readblock_t payload;
                payload.blockno = sectrail;
                payload.keytype = MF_KEY_A;

                // Use key A
                num_to_bytes(e_sector[i].Key[0], 6, payload.key);

                clearCommandBuffer();
                SendCommandNG(CMD_HF_MIFARE_READBL, (uint8_t *)&payload, sizeof(mf_readblock_t));

                PacketResponseNG resp;
                if (!WaitForResponseTimeout(CMD_HF_MIFARE_READBL, &resp, 1500)) continue;

                if (resp.status != PM3_SUCCESS) continue;

                uint8_t *data = resp.data.asBytes;
                key64 = bytes_to_num(data + 10, 6);
                if (key64) {
                    PrintAndLogEx(NORMAL, "Data:%s", sprint_hex(data + 10, 6));
                    e_sector[i].foundKey[1] = 1;
                    e_sector[i].Key[1] = key64;
                }
            }
            if (singleSector)
                break;
            b < 127 ? (b += 4) : (b += 16);
        }
    }

out:
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(SUCCESS, _GREEN_("found keys:"));

    //print keys
    if (singleSector)
        printKeyTableEx(1, e_sector, mfSectorNum(blockNo));
    else
        printKeyTable(SectorsCnt, e_sector);

    if (transferToEml) {
        // fast push mode
        g_conn.block_after_ACK = true;
        uint8_t block[16] = {0x00};
        for (int i = 0; i < SectorsCnt; ++i) {
            uint8_t blockno = mfFirstBlockOfSector(i) + mfNumBlocksPerSector(i) - 1;
            mfEmlGetMem(block, blockno, 1);

            if (e_sector[i].foundKey[0])
                num_to_bytes(e_sector[i].Key[0], 6, block);

            if (e_sector[i].foundKey[1])
                num_to_bytes(e_sector[i].Key[1], 6, block + 10);

            if (i == SectorsCnt - 1) {
                // Disable fast mode on last packet
                g_conn.block_after_ACK = false;
            }
            mfEmlSetMem(block, blockno, 1);
        }
        PrintAndLogEx(SUCCESS, "Found keys have been transferred to the emulator memory");
    }

    if (createDumpFile) {
        char *fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (createMfcKeyDump(fptr, SectorsCnt, e_sector) != PM3_SUCCESS) {
            PrintAndLogEx(ERR, "Failed to save keys to file");
        }
        free(fptr);
    }

    free(keyBlock);
    free(e_sector);

    // Disable fast mode and send a dummy command to make it effective
    g_conn.block_after_ACK = false;
    SendCommandNG(CMD_PING, NULL, 0);
    if (!WaitForResponseTimeout(CMD_PING, NULL, 1000)) {
        PrintAndLogEx(WARNING, "command execution time out");
        return PM3_ETIMEOUT;
    }

    PrintAndLogEx(NORMAL, "");
    return PM3_SUCCESS;
}

void showSectorTable(sector_t *k_sector, uint8_t k_sectorsCount) {
    if (k_sector != NULL) {
        printKeyTable(k_sectorsCount, k_sector);
        free(k_sector);
    }
}

void readerAttack(sector_t *k_sector, uint8_t k_sectorsCount, nonces_t data, bool setEmulatorMem, bool verbose) {

    uint64_t key = 0;
    bool success = false;

    if (k_sector == NULL) {
        int32_t res = initSectorTable(&k_sector, k_sectorsCount);
        if (res != k_sectorsCount) {
            free(k_sector);
            return;
        }
    }

    success = mfkey32_moebius(&data, &key);
    if (success) {
        uint8_t sector = data.sector;
        uint8_t keytype = data.keytype;

        PrintAndLogEx(INFO, "Reader is trying authenticate with: Key %s, sector %02d: [%012" PRIx64 "]"
                      , (keytype == MF_KEY_B) ? "B" : "A"
                      , sector
                      , key
                     );

        k_sector[sector].Key[keytype] = key;
        k_sector[sector].foundKey[keytype] = true;

        //set emulator memory for keys
        if (setEmulatorMem) {
            uint8_t memBlock[16] = {0, 0, 0, 0, 0, 0, 0xff, 0x0F, 0x80, 0x69, 0, 0, 0, 0, 0, 0};
            num_to_bytes(k_sector[sector].Key[0], 6, memBlock);
            num_to_bytes(k_sector[sector].Key[1], 6, memBlock + 10);
            //iceman,  guessing this will not work so well for 4K tags.
            PrintAndLogEx(INFO, "Setting Emulator Memory Block %02d: [%s]"
                          , (sector * 4) + 3
                          , sprint_hex(memBlock, sizeof(memBlock))
                         );
            mfEmlSetMem(memBlock, (sector * 4) + 3, 1);
        }
    }

    free(k_sector);
}


void printKeyTable(uint8_t sectorscnt, sector_t *e_sector) {
    return printKeyTableEx(sectorscnt, e_sector, 0);
}

void printKeyTableEx(uint8_t sectorscnt, sector_t *e_sector, uint8_t start_sector) {
    char strA[12 + 1] = {0};
    char strB[12 + 1] = {0};
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "-----+-----+--------------+---+--------------+----");
    PrintAndLogEx(INFO, " Sec | Blk | key A        |res| key B        |res");
    PrintAndLogEx(INFO, "-----+-----+--------------+---+--------------+----");
    for (uint8_t i = 0; i < sectorscnt; i++) {
        int res_A = e_sector[i].foundKey[0];
        int res_B = e_sector[i].foundKey[1];
        #define RES_A _RED_
        #define RES_B _RED_
        if (res_A){
            #undef RES_A
            #define RES_A _GREEN_
        }
        if (res_B){
            #undef RES_B
            #define RES_B _GREEN_
        }
        snprintf(strA, sizeof(strA), "????????????");
        snprintf(strB, sizeof(strB), "????????????");

        if (e_sector[i].foundKey[0]){
            snprintf(strA, sizeof(strA), "%012" PRIX64, e_sector[i].Key[0]);
        }

        if (e_sector[i].foundKey[1]){
            snprintf(strB, sizeof(strB), "%012" PRIX64, e_sector[i].Key[1]);
        }

        if (e_sector[i].foundKey[0] > 1) {
            PrintAndLogEx(INFO, " "_MAGENTA_("%03d")" | %03d | " _GREEN_("%s")" | " RES_A("%c")" | " _GREEN_("%s")" | " RES_B("%c")
                          , i
                          , mfSectorTrailerOfSector(i)
                          , strA, res_A
                          , strB, res_B
                         );
        } else {

            // keep track if we use start_sector or i...
            uint8_t s = start_sector;
            if (start_sector == 0)
                s = i;

            PrintAndLogEx(INFO, " "_RED_("%03d")" | %03d | " _GREEN_("%s")" | " RES_A("%d")" | " _GREEN_("%s")" | " RES_B("%d")
                          , s
                          , mfSectorTrailerOfSector(s)
                          , strA, res_A
                          , strB, res_B
                         );
        }
    }
    PrintAndLogEx(NORMAL, "-----+-----+--------------+---+--------------+----");
    if (e_sector[0].foundKey[0] > 1) {
        PrintAndLogEx(SUCCESS, "( "
                      _MAGENTA_("F") ":file / "
                      _MAGENTA_("D") ":darkside / "
                      _YELLOW_("K") ":known / "
                      _GREEN_("R") ":reused / "
                      _MAGENTA_("N") ":nested / "
                      _MAGENTA_("H") ":hardnested / "
                      _MAGENTA_("S") ":staticnested / "
                      _GREEN_("A") ":keyA "
                      " )"
                     );
    } else {
        PrintAndLogEx(SUCCESS, "( " _RED_("0") ":Failed / " _GREEN_("1") ":Success )");
    }

    PrintAndLogEx(NORMAL, "");
}


// EMULATOR COMMANDS
static int CmdHF14AMfEGetBlk(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf egetblk",
                  "Get emulator memory block",
                  "hf mf egetblk --blk 0      -> get block 0 (manufacturer)\n"
                  "hf mf egetblk --blk 3 -v   -> get block 3, decode sector trailer\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("n",  "blk", "<dec>", "block number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int b = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx);

    if (b > 255) {
        return PM3_EINVARG;
    }
    uint8_t blockno = (uint8_t)b;

    uint8_t data[16] = {0x00};
    if (mfEmlGetMem(data, blockno, 1) == PM3_SUCCESS) {

        uint8_t sector = mfSectorNum(blockno);
        mf_print_sector_hdr(sector);
        mf_print_block(blockno, data, verbose);
    }
    if (verbose) {
        decode_print_st(blockno, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfEGetSc(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf egetsc",
                  "Get emulator memory sector",
                  "hf mf egetsc -s 0"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("s",  "sec", "<dec>", "sector number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int s = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx);

    if (s > 39) {
        PrintAndLogEx(WARNING, "Sector number must be less then 40");
        return PM3_EINVARG;
    }

    uint8_t sector = (uint8_t)s;
    mf_print_sector_hdr(sector);

    uint8_t blocks = mfNumBlocksPerSector(sector);
    uint8_t start = mfFirstBlockOfSector(sector);

    uint8_t data[16] = {0};
    for (int i = 0; i < blocks; i++) {
        int res = mfEmlGetMem(data, start + i, 1);
        if (res == PM3_SUCCESS) {
            mf_print_block(start + i, data, verbose);
        }
    }
    if (verbose) {
        decode_print_st(start + blocks - 1, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfEClear(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eclr",
                  "Clears emulator to 0xFFFFFFFFFFFF",
                  "hf mf eclr"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("f", "full", "Wipe block 0 with null bytes"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool full_wipe = arg_get_lit(ctx, 1);
    CLIParserFree(ctx);
    clearCommandBuffer();
    if (full_wipe){
        SendCommandNG(CMD_HF_MIFARE_EML_MEMCLR_FULL, NULL, 0);
    } else {
        SendCommandNG(CMD_HF_MIFARE_EML_MEMCLR, NULL, 0);
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfEBCC(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ebcc",
                  "Check/fix BCC checksum",
                  "hf mf ebcc -f"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("f", "fix", "Fix BCC based on current UID in addition to check"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    bool fix = arg_get_lit(ctx, 1);
    uint8_t data[16] = {0x00};

    if (mfEmlGetMem(data, 0, 1) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "cannot get blk 0");
        //free(sec_0_trailer);
        return PM3_ESOFT;
    }
    uint8_t bcc_val[1] = {data[0] ^ data[1] ^ data[2] ^ data[3]}; 
    uint8_t bcc_idx = 4;
    if (data[bcc_idx] == bcc_val[0]){
        PrintAndLogEx(SUCCESS, "BCC is correct [%s]", _GREEN_("%s"), sprint_hex(bcc_val, 1));
        return PM3_SUCCESS;
    } else {
        PrintAndLogEx(SUCCESS, "BCC is incorrect [%s]", _RED_("%s"), sprint_hex(bcc_val, 1));
        if (fix){
            data[4] = data[0] ^ data[1] ^ data[2] ^ data[3];   
            if (sizeof(data) != 16){
                PrintAndLogEx(WARNING, "Invalid write-back length of blk 0");
                return PM3_ESOFT;
            }
            return mfEmlSetMem(data, 0, 1);
        }
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfEUID(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf euid",
                  "Overwrite/print current UID in emulator",
                  "hf mf euid -u a0a1a2a3a4a5"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid", "<hex>", "UID to write (4/7/10 bytes)"),
        arg_lit0("c", "crc", "Overwrite crc (BCC checksum)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    bool crc = arg_get_lit(ctx, 2);
    uint8_t uid[10];
    uint8_t data[16] = {0x00};
    int datalen = 16;
    int uidlen = 0;
    int res = CLIParamHexToBuf(arg_get_str(ctx, 1), uid, sizeof(uid), &uidlen);
    if (res){
        PrintAndLogEx(FAILED, "Error parsing UID bytes");
        return PM3_EINVARG;
    }
    //sector_t *sec_0_trailer = NULL;
    switch(uidlen){
        case 4:
            break;
        case 10:
            break;
        case 7:
            break;
        default:
            PrintAndLogEx(WARNING, "Wrong UID length");
            return PM3_EINVARG;
    }

    if (mfEmlGetMem(data, 0, 1) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "cannot get blk 0");
        //free(sec_0_trailer);
        return PM3_ESOFT;
    }
    if ((data[8] & 0xc0) == 0x40) {
        PrintAndLogEx(WARNING, "Invalid write-back length of blk 0");
        return PM3_ESOFT;
    }
    if (uidlen == 0){
        uint8_t uid_r[10];
        for (int i = 0; i < uidlen; i++){
            uid_r[i] = data[i];
        }
        PrintAndLogEx(SUCCESS, "Current UID ..... ", _LIGHT_MAGENTA_("%s"), sprint_hex(uid_r, sizeof(uid_r)));
        return PM3_SUCCESS;
    }
    for (int i = 0; i < uidlen; i++){
        data[i] = uid[i];
    }
    if (crc){
        data[4] = data[0] ^ data[1] ^ data[2] ^ data[3];
        PrintAndLogEx(SUCCESS, "Setting BCC within emulator memory . . .");
    }
    if (sizeof(data) != datalen){
        PrintAndLogEx(WARNING, "Invalid write-back length of blk 0");
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "Set UID within emulator memory (%d bytes)", uidlen);
    return mfEmlSetMem(data, 0, 1);
}

static int CmdHF14AMfEDAT(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf edat",
                  "Overwrite manufacturer data",
                  "hf mf edat -d ffffffffffffffff"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("d", "data", "<hex>", "Data to write (8 bytes)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    uint8_t dat[8];
    uint8_t data[16] = {0x00};
    int datlen = 0;
    int res = CLIParamHexToBuf(arg_get_str(ctx, 1), dat, sizeof(dat), &datlen);
    if (res){
        PrintAndLogEx(FAILED, "Error parsing bytes");
        return PM3_EINVARG;
    }
    if (mfEmlGetMem(data, 0, 1) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "error get block 0");
        //free(sec_0_trailer);
        return PM3_ESOFT;
    }
    int endrange = datlen + 6;
    int dtin = 0;
    for (int i = 9; i < endrange; i++){
        data[i] = dat[dtin];
        dtin += 1;
    }
    if (sizeof(data) != 16){
        PrintAndLogEx(WARNING, "Invalid write-back length of blk 0");
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "Set DATA within blk 0 of emulator memory (%d bytes)", datlen);
    return mfEmlSetMem(data, 0, 1);
}

static int CmdHF14AMfEACL(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eacl",
                  "Overwrite ACL of a single/all sectors",
                  "hf mf eacl -d ffffff"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("d", "data", "<hex>", "Data to write (3 bytes)"),
        arg_str0("u", "user", "<hex>", "Custom User byte (default: 0x00)"),
        arg_int0("n", "sec", "<dec>", "Number of sector to overwrite (default: 0, set to -1 to write to all sectors)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    uint8_t userbyte[1] = {0x00};    
    uint8_t dat[3];
    uint8_t data[16] = {0x00};
    int datlen = 0;
    int usrlen = 0;
    int blnum = 0;
    int secnr = arg_get_int_def(ctx, 3, 0);

    int res = CLIParamHexToBuf(arg_get_str(ctx, 1), dat, sizeof(dat), &datlen);
    if (res){
        PrintAndLogEx(FAILED, "Error parsing ACL bytes");
        return PM3_EINVARG;
    }
    int usres = CLIParamHexToBuf(arg_get_str(ctx, 2), userbyte, sizeof(userbyte), &usrlen);
    if (usres){
        PrintAndLogEx(FAILED, "Error parsing user byte");
        return PM3_EINVARG;
    }
    if (secnr == -1){
        for (int x = 0; x < 255; x++){
            if (mfIsSectorTrailer(x)){
                blnum = x; 
                if (mfEmlGetMem(data, blnum, 1) != PM3_SUCCESS) {
                    PrintAndLogEx(WARNING, "error get block ", _RED_("%d"), blnum);
                    //free(sec_0_trailer);
                    return PM3_ESOFT;
                }
                int endrange = 8;//datlen + 6;
                int startrange = 6;
                for (int i = 0; i < datlen; i++){
                    data[startrange+i] = dat[i];
                }
                data[endrange+1] = userbyte[0];
                if (sizeof(data) != 16){
                    PrintAndLogEx(WARNING, "Invalid write-back length of block %d", blnum);
                    return PM3_ESOFT;
                }
                if (g_session.client_debug_level >= 2){
                    PrintAndLogEx(SUCCESS, "Setting ACL within blk %d of emulator memory (%d bytes)", blnum, datlen);
                }
            }
        }
    } else {
        blnum = mfSectorTrailerOfSector(secnr);
        if (mfEmlGetMem(data, blnum, 1) != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "error get block ", _RED_("%d"), blnum);
            //free(sec_0_trailer);
            return PM3_ESOFT;
        }
        int endrange = 8;//datlen + 6;
        int startrange = 6;
        for (int i = 0; i < datlen; i++){
            data[startrange+i] = dat[i];
        }
        data[endrange+1] = userbyte[0];
        if (sizeof(data) != 16){
            PrintAndLogEx(WARNING, "Invalid write-back length of block 0");
            return PM3_ESOFT;
        }
        PrintAndLogEx(SUCCESS, "Setting ACL within blk %d of emulator memory (%d bytes)", blnum, datlen);
        return mfEmlSetMem(data, blnum, 1);
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfEINS(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eins",
                  "Insert data into any given emulator space",
                  "hf mf eins -d ffff -o 2"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("d", "data", "<hex>", "Data to write"),
        arg_int0("o", "offset", "<dec>", "Block's offset"),
        arg_int0("n", "blk", "<dec>", "Block number to calculate offset from"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    uint8_t data[16] = {0x00};
    uint8_t input[16] = {0x00};
    int inputlen = 0;
    int pos = arg_get_int(ctx, 2);
    int blk = arg_get_int(ctx, 3);
    int res = CLIParamHexToBuf(arg_get_str(ctx, 1), input, sizeof(input), &inputlen);
    if (res){
        PrintAndLogEx(FAILED, "Error parsing bytes");
        return PM3_EINVARG;
    }
    if (pos + inputlen > 16){
        PrintAndLogEx(FAILED, "Error - offset exceeds max length of blk");
        return PM3_EINVARG;
    }
    if (mfEmlGetMem(data, blk, 1) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "error get blk %d", blk);
        return PM3_ESOFT;
    }
    int range = pos + inputlen;
    for (int i = pos; i < range; i++){
        data[i] = input[i];
    }
    if (sizeof(data) != 16){
        PrintAndLogEx(WARNING, "Invalid write-back length of block");
        return PM3_ESOFT;
    }
    PrintAndLogEx(SUCCESS, "Inserted into blk %d of at offset %d (%d bytes)", blk, pos, inputlen);
    return mfEmlSetMem(data, blk, 1);
}

static int CmdHF14AMfESet(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf esetblk",
                  "Set emulator memory block",
                  "hf mf esetblk --blk 1 -d 000102030405060708090a0b0c0d0e0f"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("n", "blk", "<dec>", "block number"),
        arg_str0("d", "data", "<hex>", "bytes to write, 16 hex bytes"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int b = arg_get_int_def(ctx, 1, 0);

    uint8_t data[16] = {0x00};
    int datalen = 0;
    int res = CLIParamHexToBuf(arg_get_str(ctx, 2), data, sizeof(data), &datalen);
    CLIParserFree(ctx);
    if (res) {
        PrintAndLogEx(FAILED, "Error parsing bytes");
        return PM3_EINVARG;
    }

    if (b > 255) {
        return PM3_EINVARG;
    }

    if (datalen != sizeof(data)) {
        PrintAndLogEx(WARNING, "block data must include 16 HEX bytes. Got %i", datalen);
        return PM3_EINVARG;
    }

    //  1 - blocks count
    return mfEmlSetMem(data, b, 1);
}

static int CmdHF14AMfESAK(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf esak",
                  "Get/set ATQA and SAK values",
                  "hf mf esak --sak 08"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("s", "sak", "<hex>", "bytes to write, 1 hex byte"),
        arg_str0("a", "atqa", "<hex>", "bytes to write, 2 hex bytes"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);


    uint8_t data[16] = {0x00};
    uint8_t sak[1] = {0x00};
    uint8_t atqa[2] = {0x00};
    int saklen = 0;
    int atqalen = 0;
    int sakidx = 0;
    int atqaidx = 0;
    bool writemem = false;
    int res1 = CLIParamHexToBuf(arg_get_str(ctx, 1), sak, sizeof(sak), &saklen);
    int res2 = CLIParamHexToBuf(arg_get_str(ctx, 2), atqa, sizeof(atqa), &atqalen);
    CLIParserFree(ctx);
    if (res1) {
        PrintAndLogEx(FAILED, "Error parsing SAK bytes");
        return PM3_EINVARG;
    }
    if (res2) {
        PrintAndLogEx(FAILED, "Error parsing ATQA bytes");
        return PM3_EINVARG;
    }
    if (saklen != sizeof(sak)) {
        PrintAndLogEx(WARNING, "SAK must include 1 hex bytes. Got %i", saklen);
        return PM3_EINVARG;
    }
    if (atqalen != sizeof(atqa)) {
        PrintAndLogEx(WARNING, "ATQA must include 2 hex bytes. Got %i", atqalen);
        return PM3_EINVARG;
    }
    if (mfEmlGetMem(data, 0, 1) != PM3_SUCCESS) {
        PrintAndLogEx(WARNING, "error get blk 0");
        return PM3_ESOFT;
    }
    if ((data[0] ^ data[1] ^ data[2] ^ data[3]) == data[4] && (data[6] & 0xc0) == 0) {
        sakidx = 5; 
        atqaidx = 6;
    } else if ((data[8] & 0xc0) == 0x40) {
        sakidx = 7;
        atqaidx = 8;
    } else {
        PrintAndLogEx(WARNING, "Emulator layout doesn't indicate 4 nor 7 byte UID. Please inspect.");
        return PM3_ESOFT;
    }
        //else if ((dump[8] & 0xc0) == 0x40) {
    if (saklen != 0){
        data[sakidx] = sak[0];
        writemem = true;
    }
    if (atqalen != 0){
        data[atqaidx] = atqa[0];
        data[atqaidx+1] = atqa[1];
        writemem = true;
    }
    if (writemem){
        if (16 != sizeof(data)) {
            PrintAndLogEx(WARNING, "block data must include 16 HEX bytes. Got %i", sizeof(data));
            return PM3_EINVARG;
        }
        return mfEmlSetMem(data, 0, 1);
    } else {
        sak[0] = data[sakidx];
        atqa[0] = data[atqaidx];
        atqa[1] = data[atqaidx+1];
        PrintAndLogEx(SUCCESS, "SAK  ---> %s", sprint_hex(sak, sizeof(sak)));
        PrintAndLogEx(SUCCESS, "ATQA ---> %s", sprint_hex(atqa, sizeof(atqa)));
        return PM3_SUCCESS;
    }
}

static int CmdHF14AMfECP(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ecp",
                  "Copy sector/block from source to destination",
                  "hf mf ecp --src 2 --dst 3 --sec"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int0("s", "src", "<dec>", "source blk/sec"),
        arg_int0("d", "dst", "<dec>", "destination blk/sec"),
        arg_lit0(NULL, "sec", "copy sector, not block"),
        arg_lit0(NULL, "swap", "swap source with destination"),
        arg_param_end
    };
    int src = arg_get_int(ctx, 1);
    int dst = arg_get_int(ctx, 2);
    bool sectors = arg_get_lit(ctx, 3);
    bool swap = arg_get_lit(ctx, 4);
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    uint8_t src_blk[16] = {0x00};
    uint8_t dst_blk[16] = {0x00};
    //uint8_t src_sec[64] = {0x00};
    //uint8_t dst_sec[64] = {0x00};
    if (!sectors){
        mfEmlGetMem(src_blk, src, 1);
        mfEmlGetMem(dst_blk, dst, 1);
        if (swap){
            mfEmlSetMem(src_blk, dst, 1);
            mfEmlSetMem(dst_blk, src, 1);
        }
    } else {
        int dst_block = mfFirstBlockOfSector(dst);
        if (swap){
            int _t = dst;
            dst = src;
            src = _t;
        }
        for (int i = mfFirstBlockOfSector(src); i < mfSectorTrailerOfSector(src); i++){
            if (mfEmlSetMem(src_blk, i, 1) != PM3_SUCCESS){
                PrintAndLogEx(WARNING, "error get block ", _RED_("%d"), i);
            }
            if (mfEmlSetMem(src_blk, dst_block, 1) != PM3_SUCCESS){
                PrintAndLogEx(WARNING, "error set block ", _RED_("%d"), i);
            }
            dst_block += 1;
        }
    }
    return PM3_SUCCESS;
}

int CmdHF14AMfELoad(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf eload",
                  "Load emulator memory with data from (bin/eml/json) dump file",
                  "hf mf eload -f hf-mf-01020304.bin\n"
                  "hf mf eload --4k -f hf-mf-01020304.eml\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str1("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "ul", "MIFARE Ultralight family"),
        arg_int0("q", "qty", "<dec>", "manually set number of blocks (overrides)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    bool mu = arg_get_lit(ctx, 6);

    int numblks = arg_get_int_def(ctx, 7, -1);

    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4 + mu) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4 + mu) == 0) {
        m1 = true;
    }

    uint8_t block_width = 16;
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;
    uint8_t hdr_len = 0;

    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
    } else if (mu) {
        block_cnt = MFU_MAX_BLOCKS;
        block_width = MFU_BLOCK_SIZE;
        hdr_len = MFU_DUMP_PREFIX_LENGTH;
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    PrintAndLogEx(INFO, "%d blocks ( %u bytes ) to upload", block_cnt, block_cnt * block_width);

    if (numblks > 0) {
        block_cnt = MIN(numblks, block_cnt);
        PrintAndLogEx(INFO, "overriding number of blocks, will use %d blocks ( %u bytes )", block_cnt, block_cnt * block_width);
    }

    uint8_t *data = NULL;
    size_t bytes_read = 0;
    int res = pm3_load_dump(filename, (void **)&data, &bytes_read, (block_width * block_cnt + hdr_len));
    if (res != PM3_SUCCESS) {
        return res;
    }

    // 64 or 256 blocks.
    if ((bytes_read % block_width) != 0) {
        PrintAndLogEx(FAILED, "File content error. Size doesn't match blockwidth ");
        free(data);
        return PM3_ESOFT;
    }

    // convert plain or old mfu format to new format
    if (block_width == MFU_BLOCK_SIZE) {
        res = convert_mfu_dump_format(&data, &bytes_read, true);
        if (res != PM3_SUCCESS) {
            PrintAndLogEx(FAILED, "Failed convert on load to new Ultralight/NTAG format");
            free(data);
            return res;
        }

        mfu_dump_t *mfu_dump = (mfu_dump_t *)data;
        printMFUdumpEx(mfu_dump, mfu_dump->pages + 1, 0);

        // update expected blocks to match converted data.
        block_cnt = bytes_read / MFU_BLOCK_SIZE;
        PrintAndLogEx(INFO, "MIFARE Ultralight override, will use %d blocks ( %u bytes )", block_cnt, block_cnt * block_width);
    }

    PrintAndLogEx(INFO, "Uploading to emulator memory");
    PrintAndLogEx(INFO, ">" NOLF);

    // fast push mode
    g_conn.block_after_ACK = true;

    size_t offset = 0;
    int cnt = 0;

    while (bytes_read && cnt < block_cnt) {
        if (bytes_read == block_width) {
            // Disable fast mode on last packet
            g_conn.block_after_ACK = false;
        }

        if (mfEmlSetMem_xt(data + offset, cnt, 1, block_width) != PM3_SUCCESS) {
            PrintAndLogEx(FAILED, "Can't set emulator mem at block: %3d", cnt);
            free(data);
            return PM3_ESOFT;
        }
        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);

        cnt++;
        offset += block_width;
        bytes_read -= block_width;
    }
    free(data);
    PrintAndLogEx(NORMAL, "");

    if (block_width == MFU_BLOCK_SIZE) {
        PrintAndLogEx(HINT, "You are ready to simulate. See " _YELLOW_("`hf mfu sim -h`"));
        // MFU / NTAG
        if ((cnt != block_cnt)) {
            PrintAndLogEx(WARNING, "Warning, Ultralight/Ntag file content, Loaded %d blocks of expected %d blocks into emulator memory", cnt, block_cnt);
            return PM3_SUCCESS;
        }
    } else {
        PrintAndLogEx(HINT, "You are ready to simulate. See " _YELLOW_("`hf mf sim -h`"));
        // MFC
        if ((cnt != block_cnt)) {
            PrintAndLogEx(WARNING, "Error, file content, Only loaded %d blocks, must be %d blocks into emulator memory", cnt, block_cnt);
            return PM3_SUCCESS;
        }
    }
    PrintAndLogEx(INFO, "Done!");
    return PM3_SUCCESS;
}


// CHINESE MAGIC COMMANDS
static int CmdHF14AMfCSetUID(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf csetuid",
                  "Set UID, ATQA, and SAK for magic gen1a card",
                  "hf mf csetuid -u 01020304\n"
                  "hf mf csetuid -w -u 01020304 --atqa 0004 --sak 08"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("w", "wipe", "wipes card with backdoor cmd`"),
        arg_str0("u", "uid",  "<hex>", "UID, 4/7 hex bytes"),
        arg_str0("a", "atqa", "<hex>", "ATQA, 2 hex bytes"),
        arg_str0("s", "sak",  "<hex>", "SAK, 1 hex byte"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t wipe_card = arg_get_lit(ctx, 1);

    int uidlen = 0;
    uint8_t uid[7] = {0x00};
    CLIGetHexWithReturn(ctx, 2, uid, &uidlen);

    int alen = 0;
    uint8_t atqa[2] = {0x00};
    CLIGetHexWithReturn(ctx, 3, atqa, &alen);

    int slen = 0;
    uint8_t sak[1] = {0x00};
    CLIGetHexWithReturn(ctx, 4, sak, &slen);
    CLIParserFree(ctx);

    // sanity checks
    if (uidlen != 4 && uidlen != 7) {
        PrintAndLogEx(FAILED, "UID must be 4 or 7 hex bytes. Got %d", uidlen);
        return PM3_EINVARG;
    }
    if (alen && alen != 2) {
        PrintAndLogEx(FAILED, "ATQA must be 2 hex bytes. Got %d", alen);
        return PM3_EINVARG;
    }
    if (slen && slen != 1) {
        PrintAndLogEx(FAILED, "SAK must be 1 hex byte. Got %d", slen);
        return PM3_EINVARG;
    }

    uint8_t old_uid[7] = {0};
    uint8_t verify_uid[7] = {0};

    int res = mfCSetUID(
                  uid,
                  uidlen,
                  (alen) ? atqa : NULL,
                  (slen) ? sak : NULL,
                  old_uid,
                  verify_uid,
                  wipe_card
              );

    if (res) {
        PrintAndLogEx(ERR, "Can't set UID. error %d", res);
        return PM3_ESOFT;
    }

    res = memcmp(uid, verify_uid, uidlen);

    PrintAndLogEx(SUCCESS, "Old UID... %s", sprint_hex(old_uid, uidlen));
    PrintAndLogEx(SUCCESS, "New UID... %s ( %s )",
                  sprint_hex(verify_uid, uidlen),
                  (res == 0) ? _GREEN_("verified") : _RED_("fail")
                 );
    return PM3_SUCCESS;
}

static int CmdHF14AMfCWipe(const char *cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cwipe",
                  "Wipe gen1 magic chinese card.\n"
                  "Set UID / ATQA / SAK / Data / Keys / Access to default values",
                  "hf mf cwipe\n"
                  "hf mf cwipe -u 09080706 -a 0004 -s 18 --> set UID, ATQA and SAK and wipe card");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid",  "<hex>", "UID, 4 hex bytes"),
        arg_str0("a", "atqa", "<hex>", "ATQA, 2 hex bytes"),
        arg_str0("s", "sak",  "<hex>", "SAK, 1 hex byte"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, cmd, argtable, true);

    int uidlen = 0;
    uint8_t uid[8] = {0x00};
    CLIGetHexWithReturn(ctx, 1, uid, &uidlen);

    int alen = 0;
    uint8_t atqa[2] = {0x00};
    CLIGetHexWithReturn(ctx, 2, atqa, &alen);

    int slen = 0;
    uint8_t sak[1] = {0x00};
    CLIGetHexWithReturn(ctx, 3, sak, &slen);
    CLIParserFree(ctx);

    if (uidlen && uidlen != 4) {
        PrintAndLogEx(ERR, "UID length must be 4 bytes, got %d", uidlen);
        return PM3_EINVARG;
    }
    if (alen && alen != 2) {
        PrintAndLogEx(ERR, "ATQA length must be 2 bytes, got %d", alen);
        return PM3_EINVARG;
    }
    if (slen && slen != 1) {
        PrintAndLogEx(ERR, "SAK length must be 1 byte, got %d", slen);
        return PM3_EINVARG;
    }

    int res = mfCWipe((uidlen) ? uid : NULL, (alen) ? atqa : NULL, (slen) ? sak : NULL);
    if (res) {
        PrintAndLogEx(ERR, "Can't wipe card. error %d", res);
        return PM3_ESOFT;
    }

    PrintAndLogEx(SUCCESS, "Card wiped successfully");
    return PM3_SUCCESS;
}

static int CmdHF14AMfCSetBlk(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf csetblk",
                  "Set block data on a magic gen1a card",
                  "hf mf csetblk --blk 1 -d 000102030405060708090a0b0c0d0e0f"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("b", "blk", "<dec>", "block number"),
        arg_str0("d", "data", "<hex>", "bytes to write, 16 hex bytes"),
        arg_lit0("w", "wipe", "wipes card with backdoor cmd before writing"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int b = arg_get_int_def(ctx, 1, -1);

    uint8_t data[MFBLOCK_SIZE] = {0x00};
    int datalen = 0;
    CLIGetHexWithReturn(ctx, 2, data, &datalen);

    uint8_t wipe_card = arg_get_lit(ctx, 3);
    CLIParserFree(ctx);

    if (b < 0 ||  b >= MIFARE_1K_MAXBLOCK) {
        PrintAndLogEx(FAILED, "target block number out-of-range, got %i", b);
        return PM3_EINVARG;
    }

    if (datalen != MFBLOCK_SIZE) {
        PrintAndLogEx(FAILED, "expected 16 bytes data, got %i", datalen);
        return PM3_EINVARG;
    }

    uint8_t params = MAGIC_SINGLE;
    if (wipe_card) {
        params |= MAGIC_WIPE;
    }

    PrintAndLogEx(INFO, "Writing block number:%2d data:%s", b, sprint_hex_inrow(data, sizeof(data)));

    int res = mfCSetBlock(b, data, NULL, params);
    if (res) {
        PrintAndLogEx(ERR, "Can't write block. error=%d", res);
        return PM3_ESOFT;
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfCLoad(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cload",
                  "Load magic gen1a card with data from (bin/eml/json) dump file\n"
                  "or from emulator memory.",
                  "hf mf cload --emu\n"
                  "hf mf cload -f hf-mf-01020304.eml\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "emu", "from emulator memory"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool fill_from_emulator = arg_get_lit(ctx, 2);

    CLIParserFree(ctx);

    if (fill_from_emulator) {

        PrintAndLogEx(INFO, "Start download from emulator memory");
        PrintAndLogEx(INFO, "." NOLF);

        for (int b = 0; b < MIFARE_1K_MAXBLOCK; b++) {
            int flags = 0;
            uint8_t buf8[MFBLOCK_SIZE] = {0x00};

            // read from emul memory
            if (mfEmlGetMem(buf8, b, 1)) {
                PrintAndLogEx(WARNING, "Can't read from emul block: %d", b);
                return PM3_ESOFT;
            }

            // switch on field and send magic sequence
            if (b == 0) {
                flags = MAGIC_INIT + MAGIC_WUPC;
            }

            // just write
            if (b == 1) {
                flags = 0;
            }

            // Done. Magic Halt and switch off field.
            if (b == ((MFBLOCK_SIZE * 4) - 1)) {
                flags = MAGIC_HALT + MAGIC_OFF;
            }

            // write to card
            if (mfCSetBlock(b, buf8, NULL, flags)) {
                PrintAndLogEx(WARNING, "Can't set magic card block: %d", b);
                return PM3_ESOFT;
            }
            PrintAndLogEx(NORMAL, "." NOLF);
            fflush(stdout);
        }
        PrintAndLogEx(NORMAL, "");
        return PM3_SUCCESS;
    }

    // reserve memory
    uint8_t *data = NULL;
    size_t bytes_read = 0;
    int res = pm3_load_dump(filename, (void **)&data, &bytes_read, (MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK));
    if (res != PM3_SUCCESS) {
        return res;
    }

    // 64 or 256blocks.
    if (bytes_read != (MIFARE_1K_MAXBLOCK * MFBLOCK_SIZE) &&
            bytes_read != (MIFARE_4K_MAXBLOCK * MFBLOCK_SIZE)) {
        PrintAndLogEx(ERR, "File content error. Read %zu bytes", bytes_read);
        free(data);
        return PM3_EFILE;
    }

    PrintAndLogEx(INFO, "Copying to magic gen1a card");
    PrintAndLogEx(INFO, "." NOLF);

    int blockno = 0;
    int flags = 0;
    while (bytes_read) {

        // switch on field and send magic sequence
        if (blockno == 0) {
            flags = MAGIC_INIT + MAGIC_WUPC;
        }

        // write
        if (blockno == 1) {
            flags = 0;
        }

        // switch off field
        if (blockno == MFBLOCK_SIZE * 4 - 1) {
            flags = MAGIC_HALT + MAGIC_OFF;
        }

        if (mfCSetBlock(blockno, data + (MFBLOCK_SIZE * blockno), NULL, flags)) {
            PrintAndLogEx(WARNING, "Can't set magic card block: %d", blockno);
            free(data);
            return PM3_ESOFT;
        }

        bytes_read -= MFBLOCK_SIZE;

        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);

        blockno++;

        // magic card type - mifare 1K
        if (blockno >= MIFARE_1K_MAXBLOCK) break;
    }
    PrintAndLogEx(NORMAL, "\n");

    free(data);

    // confirm number written blocks. Must be 64 or 256 blocks
    if (blockno != MIFARE_1K_MAXBLOCK) {
        if (blockno != MIFARE_4K_MAXBLOCK) {
            PrintAndLogEx(ERR, "File content error. There must be %u blocks", MIFARE_4K_MAXBLOCK);
            return PM3_EFILE;
        }
        PrintAndLogEx(ERR, "File content error. There must be %d blocks", MIFARE_1K_MAXBLOCK);
        return PM3_EFILE;
    }

    PrintAndLogEx(SUCCESS, "Card loaded " _YELLOW_("%d") " blocks from file", blockno);
    PrintAndLogEx(INFO, "Done!");
    return PM3_SUCCESS;
}

static int CmdHF14AMfCGetBlk(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cgetblk",
                  "Get block data from magic Chinese card.\n"
                  "Only works with magic gen1a cards",
                  "hf mf cgetblk --blk 0      --> get block 0 (manufacturer)\n"
                  "hf mf cgetblk --blk 3 -v   --> get block 3, decode sector trailer\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("b",  "blk", "<dec>", "block number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int b = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx);

    if (b > 255) {
        return PM3_EINVARG;
    }

    uint8_t blockno = (uint8_t)b;
    uint8_t data[16] = {0};
    int res = mfCGetBlock(blockno, data, MAGIC_SINGLE);
    if (res) {
        PrintAndLogEx(ERR, "Can't read block. error=%d", res);
        return PM3_ESOFT;
    }

    uint8_t sector = mfSectorNum(blockno);
    mf_print_sector_hdr(sector);
    mf_print_block(blockno, data, verbose);

    if (verbose) {
        decode_print_st(blockno, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfCGetSc(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cgetsc",
                  "Get sector data from magic Chinese card.\n"
                  "Only works with magic gen1a cards",
                  "hf mf cgetsc -s 0"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_int1("s",  "sec", "<dec>", "sector number"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int s = arg_get_int_def(ctx, 1, 0);
    bool verbose = arg_get_lit(ctx, 2);
    CLIParserFree(ctx);
    if (s > 39) {
        PrintAndLogEx(WARNING, "Sector number must be less then 40");
        return PM3_EINVARG;
    }

    uint8_t sector = (uint8_t)s;
    mf_print_sector_hdr(sector);

    uint8_t blocks = 4;
    uint8_t start = sector * 4;
    if (sector >= 32) {
        blocks = 16;
        start = 128 + (sector - 32) * 16;
    }

    int flags = MAGIC_INIT + MAGIC_WUPC;
    uint8_t data[16] = {0};
    for (int i = 0; i < blocks; i++) {
        if (i == 1) flags = 0;
        if (i == blocks - 1) flags = MAGIC_HALT + MAGIC_OFF;

        int res = mfCGetBlock(start + i, data, flags);
        if (res) {
            PrintAndLogEx(ERR, "Can't read block. %d error=%d", start + i, res);
            return PM3_ESOFT;
        }
        mf_print_block(start + i, data, verbose);
    }
    if (verbose) {
        decode_print_st(start + blocks - 1, data);
    } else {
        PrintAndLogEx(NORMAL, "");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfCSave(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf csave",
                  "Save magic gen1a card memory into three files (BIN/EML/JSON)"
                  "or into emulator memory",
                  "hf mf csave\n"
                  "hf mf csave --4k"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of dump"),
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0(NULL, "emu", "save to emulator"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    bool m0 = arg_get_lit(ctx, 2);
    bool m1 = arg_get_lit(ctx, 3);
    bool m2 = arg_get_lit(ctx, 4);
    bool m4 = arg_get_lit(ctx, 5);
    bool fill_emulator = arg_get_lit(ctx, 6);
    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    char s[6];
    memset(s, 0, sizeof(s));
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;
    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
        strncpy(s, "Mini", 5);
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
        strncpy(s, "1K", 3);
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
        strncpy(s, "2K", 3);
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
        strncpy(s, "4K", 3);
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }

    PrintAndLogEx(SUCCESS, "Dumping magic Gen1a MIFARE Classic " _GREEN_("%s") " card memory", s);
    PrintAndLogEx(INFO, "@" NOLF);

    // Select card to get UID/UIDLEN information
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "iso14443a card select timeout");
        return PM3_ETIMEOUT;
    }

    /*
        0: couldn't read
        1: OK, with ATS
        2: OK, no ATS
        3: proprietary Anticollision
    */
    uint64_t select_status = resp.oldarg[0];
    if (select_status == 0) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        return select_status;
    }

    // store card info
    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));

    // reserve memory
    uint16_t bytes = block_cnt * MFBLOCK_SIZE;
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    // switch on field and send magic sequence
    uint8_t flags = MAGIC_INIT + MAGIC_WUPC;
    for (uint16_t i = 0; i < block_cnt; i++) {

        // read
        if (i == 1) {
            flags = 0;
        }
        // switch off field
        if (i == block_cnt - 1) {
            flags = MAGIC_HALT + MAGIC_OFF;
        }

        if (mfCGetBlock(i, dump + (i * MFBLOCK_SIZE), flags)) {
            PrintAndLogEx(WARNING, "Can't get magic card block: %d", i);
            PrintAndLogEx(HINT, "Verify your card size, and try again or try another tag position");
            free(dump);
            return PM3_ESOFT;
        }
        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);
    }
    PrintAndLogEx(NORMAL, "");

    if (fill_emulator) {
        PrintAndLogEx(INFO, "uploading to emulator memory");
        PrintAndLogEx(INFO, "." NOLF);
        // fast push mode
        g_conn.block_after_ACK = true;
        for (int i = 0; i < block_cnt; i += 5) {
            if (i == block_cnt - 1) {
                // Disable fast mode on last packet
                g_conn.block_after_ACK = false;
            }
            if (mfEmlSetMem(dump + (i * MFBLOCK_SIZE), i, 5) != PM3_SUCCESS) {
                PrintAndLogEx(WARNING, "Can't set emul block: " _YELLOW_("%d"), i);
            }
            PrintAndLogEx(NORMAL, "." NOLF);
            fflush(stdout);
        }
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(SUCCESS, "uploaded " _GREEN_("%d") " bytes to emulator memory", bytes);
    }

    // user supplied filename?
    if (fnlen < 1) {
        char *fptr = filename;
        fptr += snprintf(fptr, sizeof(filename), "hf-mf-");
        FillFileNameByUID(fptr, card.uid, "-dump", card.uidlen);
    }

    saveFile(filename, ".bin", dump, bytes);
    saveFileEML(filename, dump, bytes, MFBLOCK_SIZE);
    iso14a_mf_extdump_t xdump;
    xdump.card_info = card;
    xdump.dump = dump;
    xdump.dumplen = bytes;
    saveFileJSON(filename, jsfCardMemory, (uint8_t *)&xdump, sizeof(xdump), NULL);
    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AMfCView(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf cview",
                  "View `magic gen1a` card memory",
                  "hf mf cview\n"
                  "hf mf cview --4k"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);
    bool m2 = arg_get_lit(ctx, 3);
    bool m4 = arg_get_lit(ctx, 4);
    bool verbose = arg_get_lit(ctx, 5);
    CLIParserFree(ctx);

    // validations
    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    char s[6];
    memset(s, 0, sizeof(s));
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;
    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
        strncpy(s, "Mini", 5);
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
        strncpy(s, "1K", 3);
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
        strncpy(s, "2K", 3);
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
        strncpy(s, "4K", 3);
    } else {
        PrintAndLogEx(WARNING, "Please specify a MIFARE Type");
        return PM3_EINVARG;
    }
    PrintAndLogEx(SUCCESS, "View magic Gen1a MIFARE Classic " _GREEN_("%s"), s);
    PrintAndLogEx(INFO, "." NOLF);

    // Select card to get UID/UIDLEN information
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "iso14443a card select timeout");
        return PM3_ETIMEOUT;
    }

    /*
        0: couldn't read
        1: OK, with ATS
        2: OK, no ATS
        3: proprietary Anticollision
    */
    uint64_t select_status = resp.oldarg[0];

    if (select_status == 0) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        return select_status;
    }

    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));

    // reserve memory
    uint16_t bytes = block_cnt * MFBLOCK_SIZE;
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    // switch on field and send magic sequence
    uint8_t flags = MAGIC_INIT + MAGIC_WUPC;
    for (uint16_t i = 0; i < block_cnt; i++) {
        // read
        if (i == 1) {
            flags = 0;
        }
        // switch off field
        if (i == block_cnt - 1) {
            flags = MAGIC_HALT + MAGIC_OFF;
        }

        if (mfCGetBlock(i, dump + (i * MFBLOCK_SIZE), flags)) {
            PrintAndLogEx(WARNING, "Can't get magic card block: " _YELLOW_("%u"), i);
            PrintAndLogEx(HINT, "Verify your card size, and try again or try another tag position");
            free(dump);
            return PM3_ESOFT;
        }
        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);
    }

    PrintAndLogEx(NORMAL, "");
    mf_print_blocks(block_cnt, dump, verbose);

    if (verbose) {
        mf_print_keys(block_cnt, dump);
    }

    free(dump);
    return PM3_SUCCESS;
}

//needs nt, ar, at, Data to decrypt
static int CmdHf14AMfDecryptBytes(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf decrypt",
                  "Decrypt Crypto-1 encrypted bytes given some known state of crypto. See tracelog to gather needed values",
                  "hf mf decrypt --nt b830049b --ar 9248314a --at 9280e203 -d 41e586f9\n"
                  " -> 41e586f9 becomes 3003999a\n"
                  " -> which annotates 30 03 [99 9a] read block 3 [crc]"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str1(NULL, "nt",  "<hex>", "tag nonce"),
        arg_str1(NULL, "ar",  "<hex>", "ar_enc, encrypted reader response"),
        arg_str1(NULL, "at",  "<hex>", "at_enc, encrypted tag response"),
        arg_str1("d", "data", "<hex>", "encrypted data, taken directly after at_enc and forward"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    uint32_t nt = 0;
    int res = arg_get_u32_hexstr_def(ctx, 1, 0, &nt);
    if (res != 1) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "check `nt` parameter");
        return PM3_EINVARG;
    }

    uint32_t ar_enc = 0;
    res = arg_get_u32_hexstr_def(ctx, 2, 0, &ar_enc);
    if (res != 1) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "check `ar` parameter");
        return PM3_EINVARG;
    }

    uint32_t at_enc = 0;
    res = arg_get_u32_hexstr_def(ctx, 3, 0, &at_enc);
    if (res != 1) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "check `at` parameter");
        return PM3_EINVARG;
    }

    int datalen = 0;
    uint8_t data[512] = {0x00};
    CLIGetHexWithReturn(ctx, 4, data, &datalen);
    CLIParserFree(ctx);

    PrintAndLogEx(INFO, "nt....... %08X", nt);
    PrintAndLogEx(INFO, "ar enc... %08X", ar_enc);
    PrintAndLogEx(INFO, "at enc... %08X", at_enc);

    return tryDecryptWord(nt, ar_enc, at_enc, data, datalen);
}

static int CmdHf14AMfSetMod(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf setmod",
                  "Sets the load modulation strength of a MIFARE Classic EV1 card",
                  "hf mf setmod -k ffffffffffff -0"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0("0", NULL, "normal modulation"),
        arg_lit0("1", NULL, "strong modulation (def)"),
        arg_str0("k", "key", "<hex>", "key A, Sector 0,  6 hex bytes"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 3, key, &keylen);
    CLIParserFree(ctx);

    if (m0 + m1 > 1) {
        PrintAndLogEx(WARNING, "please select one modulation");
        return PM3_EINVARG;
    }

    uint8_t data[7] = {0};
    memcpy(data + 1, key, 6);

    if (m1) {
        data[0] = 1;
    } else {
        data[0] = 0;
    }

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_SETMOD, data, sizeof(data));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_HF_MIFARE_SETMOD, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "Command execute timeout");
        return PM3_ETIMEOUT;
    }

    if (resp.status == PM3_SUCCESS)
        PrintAndLogEx(SUCCESS, "Change ( " _GREEN_("ok") " )");
    else
        PrintAndLogEx(FAILED, "Change ( " _RED_("fail") " )");

    return resp.status;
}

// MIFARE NACK bug detection
static int CmdHf14AMfNack(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf nack",
                  "Test a MIFARE Classic based card for the NACK bug",
                  "hf mf nack"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);
    bool verbose = true;
    if (verbose)
        PrintAndLogEx(INFO, "Started testing card for NACK bug. Press Enter to abort");

    detect_classic_nackbug(verbose);
    return PM3_SUCCESS;
}

static int CmdHF14AMfice(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ice",
                  "Collect MIFARE Classic nonces to file",
                  "hf mf ice\n"
                  "hf mf ice -f nonces.bin");

    void *argtable[] = {
        arg_param_begin,
        arg_str0("f", "file", "<fn>", "filename of nonce dump"),
        arg_u64_0(NULL, "limit", "<dec>", "nonces to be collected"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    uint32_t limit = arg_get_u32_def(ctx, 2, 50000);

    CLIParserFree(ctx);

    // Validations
    char *fptr;

    if (filename[0] == '\0') {
        fptr = GenerateFilename("hf-mf-", "-nonces.bin");
        if (fptr == NULL)
            return PM3_EFILE;
        strcpy(filename, fptr);
        free(fptr);
    }

    uint8_t blockNo = 0;
    uint8_t keyType = MF_KEY_A;
    uint8_t trgBlockNo = 0;
    uint8_t trgKeyType = MF_KEY_B;
    bool slow = false;
    bool initialize = true;
    bool acquisition_completed = false;
    uint32_t total_num_nonces = 0;
    PacketResponseNG resp;

    uint32_t part_limit = 3000;

    PrintAndLogEx(NORMAL, "Collecting "_YELLOW_("%u")" nonces \n", limit);

    FILE *fnonces = NULL;
    if ((fnonces = fopen(filename, "wb")) == NULL) {
        PrintAndLogEx(WARNING, "Could not create file " _YELLOW_("%s"), filename);
        return PM3_EFILE;
    }

    clearCommandBuffer();

    uint64_t t1 = msclock();

    do {
        if (kbd_enter_pressed()) {
            PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
            break;
        }

        uint32_t flags = 0;
        flags |= initialize ? 0x0001 : 0;
        flags |= slow ? 0x0002 : 0;
        clearCommandBuffer();
        SendCommandMIX(CMD_HF_MIFARE_ACQ_NONCES, blockNo + keyType * 0x100, trgBlockNo + trgKeyType * 0x100, flags, NULL, 0);

        if (!WaitForResponseTimeout(CMD_ACK, &resp, 3000)) goto out;
        if (resp.oldarg[0])  goto out;

        uint32_t items = resp.oldarg[2];
        fwrite(resp.data.asBytes, 1, items * 4, fnonces);
        fflush(fnonces);

        total_num_nonces += items;
        if (total_num_nonces > part_limit) {
            PrintAndLogEx(INFO, "Total nonces %u\n", total_num_nonces);
            part_limit += 3000;
        }

        acquisition_completed = (total_num_nonces > limit);

        initialize = false;

    } while (!acquisition_completed);

out:
    PrintAndLogEx(SUCCESS, "time: %" PRIu64 " seconds\n", (msclock() - t1) / 1000);

    if (fnonces) {
        fflush(fnonces);
        fclose(fnonces);
    }

    clearCommandBuffer();
    SendCommandMIX(CMD_HF_MIFARE_ACQ_NONCES, blockNo + keyType * 0x100, trgBlockNo + trgKeyType * 0x100, 4, NULL, 0);
    return PM3_SUCCESS;
}

static int CmdHF14AMfAuth4(const char *Cmd) {
    uint8_t keyn[20] = {0};
    int keynlen = 0;
    uint8_t key[16] = {0};
    int keylen = 0;

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf auth4",
                  "Executes AES authentication command in ISO14443-4",
                  "hf mf auth4 4000 000102030405060708090a0b0c0d0e0f -> executes authentication\n"
                  "hf mf auth4 9003 FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -> executes authentication\n");

    void *argtable[] = {
        arg_param_begin,
        arg_str1(NULL,  NULL,     "<Key Num (HEX 2 bytes)>", NULL),
        arg_str1(NULL,  NULL,     "<Key Value (HEX 16 bytes)>", NULL),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    CLIGetHexWithReturn(ctx, 1, keyn, &keynlen);
    CLIGetHexWithReturn(ctx, 2, key, &keylen);
    CLIParserFree(ctx);

    if (keynlen != 2) {
        PrintAndLogEx(ERR, "<Key Num> must be 2 bytes long instead of: %d", keynlen);
        return PM3_ESOFT;
    }

    if (keylen != 16) {
        PrintAndLogEx(ERR, "<Key Value> must be 16 bytes long instead of: %d", keylen);
        return PM3_ESOFT;
    }

    return MifareAuth4(NULL, keyn, key, true, false, true, true, false);
}

// https://www.nxp.com/docs/en/application-note/AN10787.pdf

int CmdHFMFNDEFRead(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf ndefread",
                  "Prints NFC Data Exchange Format (NDEF)",
                  "hf mf ndefread -> shows NDEF parsed data\n"
                  "hf mf ndefread -vv -> shows NDEF parsed and raw data\n"
                  "hf mf ndefread --aid e103 -k ffffffffffff -b -> shows NDEF data with custom AID, key and with key B\n"
                  "hf mf ndefread -f myfilename -> save raw NDEF to file"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_litn("v",  "verbose",  0, 2, "show technical data"),
        arg_str0(NULL, "aid",      "<aid>", "replace default aid for NDEF"),
        arg_str0("k",  "key",      "<key>", "replace default key for NDEF"),
        arg_lit0("b",  "keyb",     "use key B for access sectors (by default: key A)"),
        arg_str0("f", "file", "<fn>", "save raw NDEF to file"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    bool verbose = arg_get_lit(ctx, 1);
    bool verbose2 = arg_get_lit(ctx, 1) > 1;
    uint8_t aid[2] = {0};
    int aidlen;
    CLIGetHexWithReturn(ctx, 2, aid, &aidlen);
    uint8_t key[6] = {0};
    int keylen;
    CLIGetHexWithReturn(ctx, 3, key, &keylen);
    bool keyB = arg_get_lit(ctx, 4);
    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 5), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);

    CLIParserFree(ctx);

    uint16_t ndefAID = 0xe103;
    if (aidlen == 2)
        ndefAID = (aid[0] << 8) + aid[1];

    uint8_t ndefkey[6] = {0};
    memcpy(ndefkey, g_mifare_ndef_key, 6);
    if (keylen == 6) {
        memcpy(ndefkey, key, 6);
    }

    uint8_t sector0[16 * 4] = {0};
    uint8_t sector10[16 * 4] = {0};
    uint8_t data[4096] = {0};
    int datalen = 0;

    if (verbose)
        PrintAndLogEx(INFO, "reading MAD v1 sector");

    if (mfReadSector(MF_MAD1_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector0)) {
        PrintAndLogEx(ERR, "error, read sector 0. card doesn't have MAD or doesn't have MAD on default keys");
        PrintAndLogEx(HINT, "Try " _YELLOW_("`hf mf ndefread -k `") " with your custom key");
        return PM3_ESOFT;
    }

    bool haveMAD2 = false;
    int res = MADCheck(sector0, NULL, verbose, &haveMAD2);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "MAD error %d", res);
        return res;
    }

    if (haveMAD2) {
        if (verbose)
            PrintAndLogEx(INFO, "reading MAD v2 sector");

        if (mfReadSector(MF_MAD2_SECTOR, MF_KEY_A, (uint8_t *)g_mifare_mad_key, sector10)) {
            PrintAndLogEx(ERR, "error, read sector 0x10. card doesn't have MAD or doesn't have MAD on default keys");
            PrintAndLogEx(HINT, "Try " _YELLOW_("`hf mf ndefread -k `") " with your custom key");
            return PM3_ESOFT;
        }
    }

    uint16_t mad[7 + 8 + 8 + 8 + 8] = {0};
    size_t madlen = 0;
    res = MADDecode(sector0, (haveMAD2 ? sector10 : NULL), mad, &madlen, false);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "can't decode MAD");
        return res;
    }

    PrintAndLogEx(INFO, "reading data from tag");
    for (int i = 0; i < madlen; i++) {
        if (ndefAID == mad[i]) {
            uint8_t vsector[16 * 4] = {0};
            if (mfReadSector(i + 1, keyB ? MF_KEY_B : MF_KEY_A, ndefkey, vsector)) {
                PrintAndLogEx(ERR, "error, reading sector %d ", i + 1);
                return PM3_ESOFT;
            }

            memcpy(&data[datalen], vsector, 16 * 3);
            datalen += 16 * 3;

            PrintAndLogEx(INPLACE, "%d", i);
        }
    }
    PrintAndLogEx(NORMAL, "");

    if (!datalen) {
        PrintAndLogEx(WARNING, "no NDEF data");
        return PM3_SUCCESS;
    }

    if (verbose2) {
        PrintAndLogEx(NORMAL, "");
        PrintAndLogEx(INFO, "--- " _CYAN_("MFC NDEF raw") " ----------------");
        print_buffer(data, datalen, 1);
    }

    if (fnlen != 0) {
        saveFile(filename, ".bin", data, datalen);
    }
    NDEFDecodeAndPrint(data, datalen, verbose);

    PrintAndLogEx(HINT, "Try " _YELLOW_("`hf mf ndefread -vv`") " for more details");
    return PM3_SUCCESS;
}

static int CmdHFMFPersonalize(const char *cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf personalize",
                  "Personalize the UID of a MIFARE Classic EV1 card. This is only possible \n"
                  "if it is a 7Byte UID card and if it is not already personalized.",
                  "hf mf personalize --f0                    -> double size UID\n"
                  "hf mf personalize --f1                    -> double size UID, optional usage of selection process shortcut\n"
                  "hf mf personalize --f2                    -> single size random ID\n"
                  "hf mf personalize --f3                    -> single size NUID\n"
                  "hf mf personalize -b -k B0B1B2B3B4B5 --f3 -> use key B = 0xB0B1B2B3B4B5"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_lit0("a", NULL, "use key A to authenticate sector 0 (def)"),
        arg_lit0("b", NULL, "use key B to authenticate sector 0"),
        arg_str0("k",  "key", "<hex>", "key (def FFFFFFFFFFFF)"),
        arg_lit0(NULL, "f0", "UIDFO, double size UID"),
        arg_lit0(NULL, "f1", "UIDF1, double size UID, optional usage of selection process shortcut"),
        arg_lit0(NULL, "f2", "UIDF2, single size random ID"),
        arg_lit0(NULL, "f3", "UIDF3, single size NUID"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, cmd, argtable, true);

    bool use_a = arg_get_lit(ctx, 1);
    bool use_b = arg_get_lit(ctx, 2);

    if (use_a + use_b > 1) {
        PrintAndLogEx(ERR, "error, use only one key type");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }

    uint8_t keytype = 0;
    if (use_b) {
        keytype = 1;
    }

    uint8_t key[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int key_len;
    int res = CLIParamHexToBuf(arg_get_str(ctx, 3), key, 6, &key_len);
    if (res || (!res && key_len && key_len != 6)) {
        PrintAndLogEx(ERR, "ERROR: not a valid key. Key must be 12 hex digits");
        CLIParserFree(ctx);
        return PM3_EINVARG;
    }

    bool f0 = arg_get_lit(ctx, 4);
    bool f1 = arg_get_lit(ctx, 5);
    bool f2 = arg_get_lit(ctx, 6);
    bool f3 = arg_get_lit(ctx, 7);
    CLIParserFree(ctx);

    uint8_t tmp = f0 + f1 + f2 + f3;
    if (tmp > 1) {
        PrintAndLogEx(WARNING, "select only one key type");
        return PM3_EINVARG;
    }
    if (tmp == 0) {
        PrintAndLogEx(WARNING, "select one key type");
        return PM3_EINVARG;
    }

    uint8_t pers_option = MIFARE_EV1_UIDF3;
    if (f0) {
        pers_option = MIFARE_EV1_UIDF0;
    } else if (f1) {
        pers_option = MIFARE_EV1_UIDF1;
    } else if (f2) {
        pers_option = MIFARE_EV1_UIDF2;
    }

    CLIParserFree(ctx);

    struct {
        uint8_t keytype;
        uint8_t pers_option;
        uint8_t key[6];
    } PACKED payload;
    payload.keytype = keytype;
    payload.pers_option = pers_option;
    memcpy(payload.key, key, sizeof(payload.key));

    clearCommandBuffer();
    SendCommandNG(CMD_HF_MIFARE_PERSONALIZE_UID, (uint8_t *)&payload, sizeof(payload));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_HF_MIFARE_PERSONALIZE_UID, &resp, 2500) == false) {
        return PM3_ETIMEOUT;
    }

    if (resp.status == PM3_SUCCESS) {
        PrintAndLogEx(SUCCESS, "Personalization ( %s )", _GREEN_("ok"));
    } else {
        PrintAndLogEx(FAILED, "Personalization ( %s )",  _RED_("fail"));
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfList(const char *Cmd) {
    return CmdTraceListAlias(Cmd, "hf mf", "mf");
}

static int CmdHf14AGen3UID(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf gen3uid",
                  "Set UID for magic Gen3 card _without_ changes to manufacturer block 0",
                  "hf mf gen3uid --uid 01020304       --> set 4 byte uid\n"
                  "hf mf gen3uid --uid 01020304050607 --> set 7 byte uid"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("u", "uid", "<hex>", "UID 4/7 hex bytes"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t uid[7] = {0};
    int uidlen = 0;
    CLIGetHexWithReturn(ctx, 1, uid, &uidlen);
    CLIParserFree(ctx);

    // sanity checks
    if (uidlen != 4 && uidlen != 7) {
        PrintAndLogEx(FAILED, "UID must be 4 or 7 hex bytes. Got %d", uidlen);
        return PM3_EINVARG;
    }

    uint8_t old_uid[10] = {0};

    int res = mfGen3UID(uid, uidlen, old_uid);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "Can't set UID");
        PrintAndLogEx(HINT, "Are you sure your card is a Gen3 ?");
        return PM3_ESOFT;
    }

    PrintAndLogEx(SUCCESS, "Old UID... %s", sprint_hex(old_uid, uidlen));
    PrintAndLogEx(SUCCESS, "New UID... %s", sprint_hex(uid, uidlen));
    return PM3_SUCCESS;
}

static int CmdHf14AGen3Block(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf gen3blk",
                  "Overwrite full manufacturer block for magic Gen3 card\n"
                  " - You can specify part of manufacturer block as\n"
                  "   4/7-bytes for UID change only\n"
                  "\n"
                  "NOTE: BCC, SAK, ATQA will be calculated automatically"
                  ,
                  "hf mf gen3blk                      --> print current data\n"
                  "hf mf gen3blk -d 01020304          --> set 4 byte uid\n"
                  "hf mf gen3blk -d 01020304050607    --> set 7 byte uid \n"
                  "hf mf gen3blk -d 01020304FFFFFFFF0102030405060708"

                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("d", "data", "<hex>", "manufacturer block data up to 16 hex bytes"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint8_t data[MFBLOCK_SIZE] = {0x00};
    int datalen = 0;
    CLIGetHexWithReturn(ctx, 1, data, &datalen);
    CLIParserFree(ctx);

    uint8_t new_block[MFBLOCK_SIZE] = {0x00};
    int res = mfGen3Block(data, datalen, new_block);
    if (res) {
        PrintAndLogEx(ERR, "Can't change manufacturer block data. error %d", res);
        return PM3_ESOFT;
    }

    PrintAndLogEx(SUCCESS, "Current block... %s", sprint_hex_inrow(new_block, sizeof(new_block)));
    return PM3_SUCCESS;
}

static int CmdHf14AGen3Freeze(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf gen3freeze",
                  "Perma lock further UID changes. No more UID changes available after operation completed\n"
                  "\nNote: operation is " _RED_("! irreversible !"),

                  "hf mf gen3freeze -y"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit1("y", "yes", "confirm UID lock operation"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    bool confirm = arg_get_lit(ctx, 1);
    CLIParserFree(ctx);
    if (confirm == false) {
        PrintAndLogEx(INFO, "please confirm that you want to perma lock the card");
        return PM3_SUCCESS;
    }

    int res = mfGen3Freeze();
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(ERR, "Can't lock UID changes. error %d", res);
    } else {
        PrintAndLogEx(SUCCESS, "MFC Gen3 UID card is now perma-locked");
    }
    return res;
}

static int CmdHf14AMfSuperCard(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf supercard",
                  "Extract info from a `super card`",
                  "hf mf supercard");

    void *argtable[] = {
        arg_param_begin,
        arg_lit0("r",  "reset",  "reset card"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool reset_card = arg_get_lit(ctx, 1);
    CLIParserFree(ctx);

    bool activate_field = true;
    bool keep_field_on = true;
    int res = 0;

    if (reset_card)  {

        keep_field_on = false;
        uint8_t response[6];
        int resplen = 0;

        // --------------- RESET CARD ----------------
        uint8_t aRESET[] = { 0x00, 0xa6, 0xc0,  0x00 };
        res = ExchangeAPDU14a(aRESET, sizeof(aRESET), activate_field, keep_field_on, response, sizeof(response), &resplen);
        if (res != PM3_SUCCESS) {
            PrintAndLogEx(FAILED, "Super card reset [ " _RED_("fail") " ]");
            DropField();
            return res;
        }
        PrintAndLogEx(SUCCESS, "Super card reset ( " _GREEN_("ok") " )");
        return PM3_SUCCESS;
    }


    uint8_t responseA[22];
    uint8_t responseB[22];
    int respAlen = 0;
    int respBlen = 0;

    // --------------- First ----------------
    uint8_t aFIRST[] = { 0x00, 0xa6, 0xb0,  0x00,  0x10 };
    res = ExchangeAPDU14a(aFIRST, sizeof(aFIRST), activate_field, keep_field_on, responseA, sizeof(responseA), &respAlen);
    if (res != PM3_SUCCESS) {
        DropField();
        return res;
    }

    // --------------- Second ----------------
    activate_field = false;
    keep_field_on = false;

    uint8_t aSECOND[] = { 0x00, 0xa6, 0xb0,  0x01,  0x10 };
    res = ExchangeAPDU14a(aSECOND, sizeof(aSECOND), activate_field, keep_field_on, responseB, sizeof(responseB), &respBlen);
    if (res != PM3_SUCCESS) {
        DropField();
        return res;
    }

// uint8_t inA[] = { 0x72, 0xD7, 0xF4, 0x3E, 0xFD, 0xAB, 0xF2, 0x35, 0xFD, 0x49, 0xEE, 0xDC, 0x44, 0x95, 0x43, 0xC4};
// uint8_t inB[] = { 0xF0, 0xA2, 0x67, 0x6A, 0x04, 0x6A, 0x72, 0x12, 0x76, 0xA4, 0x1D, 0x02, 0x1F, 0xEA, 0x20, 0x85};

    uint8_t outA[16] = {0};
    uint8_t outB[16] = {0};

    uint8_t key[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    for (uint8_t i = 0; i < 16; i += 8) {
        des_decrypt(outA + i, responseA + i, key);
        des_decrypt(outB + i, responseB + i, key);
    }

    PrintAndLogEx(DEBUG, " in : %s", sprint_hex_inrow(responseA, respAlen));
    PrintAndLogEx(DEBUG, "out : %s", sprint_hex_inrow(outA, sizeof(outA)));
    PrintAndLogEx(DEBUG, " in : %s", sprint_hex_inrow(responseB, respAlen));
    PrintAndLogEx(DEBUG, "out : %s", sprint_hex_inrow(outB, sizeof(outB)));

    if (memcmp(outA, "\x01\x01\x01\x01\x01\x01\x01\x01", 8) == 0) {
        PrintAndLogEx(INFO, "No trace recorded");
        return PM3_SUCCESS;
    }

    // second trace?
    if (memcmp(outB, "\x01\x01\x01\x01\x01\x01\x01\x01", 8) == 0) {
        PrintAndLogEx(INFO, "Only one trace recorded");
        return PM3_SUCCESS;
    }

    nonces_t data;

    // first
    uint16_t NT0 = (outA[6] << 8) | outA[7];
    data.cuid = bytes_to_num(outA, 4);
    data.nonce = prng_successor(NT0, 31);
    data.nr = bytes_to_num(outA + 8, 4);
    data.ar = bytes_to_num(outA + 12, 4);
    data.at = 0;

    // second
    NT0 = (outB[6] << 8) | outB[7];
    data.nonce2 =  prng_successor(NT0, 31);;
    data.nr2 = bytes_to_num(outB + 8, 4);
    data.ar2 = bytes_to_num(outB + 12, 4);
    data.sector = mfSectorNum(outA[5]);
    data.keytype = outA[4];
    data.state = FIRST;

    PrintAndLogEx(INFO, "A Sector %02x", data.sector);
    PrintAndLogEx(INFO, "A NT  %08x", data.nonce);
    PrintAndLogEx(INFO, "A NR  %08x", data.nr);
    PrintAndLogEx(INFO, "A AR  %08x", data.ar);
    PrintAndLogEx(INFO, "");
    PrintAndLogEx(INFO, "B NT  %08x", data.nonce2);
    PrintAndLogEx(INFO, "B NR  %08x", data.nr2);
    PrintAndLogEx(INFO, "B AR  %08x", data.ar2);

    uint64_t key64 = -1;
    res = mfkey32_moebius(&data, &key64);

    if (res) {
        PrintAndLogEx(SUCCESS, "UID: %s Sector %02x key %c [ " _GREEN_("%12" PRIX64) " ]"
                      , sprint_hex_inrow(outA, 4)
                      , data.sector
                      , (data.keytype == 0x60) ? 'A' : 'B'
                      , key64);
    } else {
        PrintAndLogEx(FAILED, "failed to recover any key");
    }
    return PM3_SUCCESS;
}

static int CmdHF14AMfWipe(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf wipe",
                  "Wipe card to zeros and default keys/acc. This command takes a key file to wipe card\n"
                  "Will use UID from card to generate keyfile name if not specified.\n"
                  "New A/B keys.....  FF FF FF FF FF FF\n"
                  "New acc rights...  FF 07 80\n"
                  "New GPB..........  69",
                  "hf mf wipe                --> reads card uid to generate file name\n"
                  "hf mf wipe --gen2         --> force write to S0, B0 manufacture block\n"
                  "hf mf wipe -f mykey.bin   --> use mykey.bin\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("f",  "file", "<fn>", "key filename"),
        arg_lit0(NULL, "gen2", "force write to Sector 0, block 0  (GEN2)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    int keyfnlen = 0;
    char keyFilename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)keyFilename, FILE_PATH_SIZE, &keyfnlen);

    bool gen2 = arg_get_lit(ctx, 2);
    CLIParserFree(ctx);

    char *fptr;
    if (keyfnlen == 0) {
        fptr = GenerateFilename("hf-mf-", "-key.bin");
        if (fptr == NULL)
            return PM3_ESOFT;

        strcpy(keyFilename, fptr);
        free(fptr);
    }

    uint8_t *keys;
    size_t keyslen = 0;
    if (loadFile_safeEx(keyFilename, ".bin", (void **)&keys, (size_t *)&keyslen, false) != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "failed to load key file");
        return PM3_ESOFT;
    }

    uint8_t keyA[MIFARE_4K_MAXSECTOR * 6];
    uint8_t keyB[MIFARE_4K_MAXSECTOR * 6];
    uint8_t num_sectors = 0;

    uint8_t mf[MFBLOCK_SIZE];
    switch (keyslen) {
        case (MIFARE_MINI_MAXSECTOR * 2 * 6): {
            PrintAndLogEx(INFO, "Loaded keys matching MIFARE Classic Mini 320b");
            memcpy(keyA, keys, (MIFARE_MINI_MAXSECTOR * 6));
            memcpy(keyB, keys + (MIFARE_MINI_MAXSECTOR * 6), (MIFARE_MINI_MAXSECTOR * 6));
            num_sectors = NumOfSectors('0');
            memcpy(mf, "\x11\x22\x33\x44\x44\x09\x04\x00\x62\x63\x64\x65\x66\x67\x68\x69", MFBLOCK_SIZE);
            break;
        }
        case (MIFARE_1K_MAXSECTOR * 2 * 6): {
            PrintAndLogEx(INFO, "Loaded keys matching MIFARE Classic 1K");
            memcpy(keyA, keys, (MIFARE_1K_MAXSECTOR * 6));
            memcpy(keyB, keys + (MIFARE_1K_MAXSECTOR * 6), (MIFARE_1K_MAXSECTOR * 6));
            num_sectors = NumOfSectors('1');

            memcpy(mf, "\x11\x22\x33\x44\x44\x08\x04\x00\x62\x63\x64\x65\x66\x67\x68\x69", MFBLOCK_SIZE);
            break;
        }
        case (MIFARE_4K_MAXSECTOR * 2 * 6): {
            PrintAndLogEx(INFO, "Loaded keys matching MIFARE Classic 4K");
            memcpy(keyA, keys, (MIFARE_4K_MAXSECTOR * 6));
            memcpy(keyB, keys + (MIFARE_4K_MAXSECTOR * 6), (MIFARE_4K_MAXSECTOR * 6));
            num_sectors = NumOfSectors('4');
            memcpy(mf, "\x11\x22\x33\x44\x44\x18\x02\x00\x62\x63\x64\x65\x66\x67\x68\x69", MFBLOCK_SIZE);
            break;
        }
        default: {
            PrintAndLogEx(INFO, "wrong key file size");
            goto out;
        }
    }

    if (gen2)
        PrintAndLogEx(INFO, "Forcing overwrite of sector 0 / block 0 ");
    else
        PrintAndLogEx(INFO, "Skipping sector 0 / block 0");
    PrintAndLogEx(NORMAL, "");

    uint8_t zeros[MFBLOCK_SIZE] = {0};
    memset(zeros, 0x00, sizeof(zeros));
    uint8_t st[MFBLOCK_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // time to wipe card
    for (uint8_t s = 0; s < num_sectors; s++) {

        for (uint8_t b = 0; b < mfNumBlocksPerSector(s); b++) {

            // Skipp write to manufacture block if not enforced
            if (s == 0 && b == 0 && gen2 == false) {
                continue;
            }

            uint8_t data[26];
            memset(data, 0, sizeof(data));
            if (mfIsSectorTrailer(b)) {
                memcpy(data + 10, st, sizeof(st));
            } else {
                memcpy(data + 10, zeros, sizeof(zeros));
            }

            // add correct manufacture block if UID Gen2
            if (s == 0 && b == 0 && gen2) {
                memcpy(data + 10, mf, sizeof(mf));
            }

            // try both A/B keys, start with B key first
            for (int8_t kt = MF_KEY_B; kt > -1; kt--) {

                if (kt == MF_KEY_A)
                    memcpy(data, keyA + (s * 6), 6);
                else
                    memcpy(data, keyB + (s * 6), 6);

                PrintAndLogEx(INFO, "block %3d: %s" NOLF, mfFirstBlockOfSector(s) + b, sprint_hex(data + 10, MFBLOCK_SIZE));
                clearCommandBuffer();
                SendCommandMIX(CMD_HF_MIFARE_WRITEBL, mfFirstBlockOfSector(s) + b, kt, 0, data, sizeof(data));
                PacketResponseNG resp;
                if (WaitForResponseTimeout(CMD_ACK, &resp, 1500)) {
                    uint8_t isOK  = resp.oldarg[0] & 0xff;
                    if (isOK == 0) {
                        PrintAndLogEx(NORMAL, "( " _RED_("fail") " )");
                    } else {
                        PrintAndLogEx(NORMAL, "( " _GREEN_("ok") " )");
                        break;
                    }
                } else {
                    PrintAndLogEx(WARNING, "Command execute timeout");
                }
            }
        }
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "Done!");
out:
    free(keys);
    return PM3_SUCCESS;
}

int CmdSAK(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf sak",
                  "Show help for default SAK and ATQA values\n",
                  "hf mf sak\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    PrintAndLogEx(SUCCESS, "MIFARE mini:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t0400 [09]:\n"));
    PrintAndLogEx(SUCCESS, "MIFARE 1k:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t0400 [08]:\n"));
    PrintAndLogEx(SUCCESS, "MIFARE 4k:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t0200 [18]:\n"));
    PrintAndLogEx(SUCCESS, "MIFARE Ultralight:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t4400 [00]:\n"));
    PrintAndLogEx(SUCCESS, "MIFARE DESFire & EV1:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t4403 [20]:\n"));
    PrintAndLogEx(SUCCESS, "JCOP31:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t0403 [28]:"));
    PrintAndLogEx(SUCCESS, "JCOP31 v. 2.4.1 & 41 2.2:\n");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t4800 [20]:"));
    PrintAndLogEx(SUCCESS, "JCOP41 v. 2.31:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t0400 [28]:\n"));
    PrintAndLogEx(SUCCESS, "INFINEON 1k:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t0400 [88]:\n"));
    PrintAndLogEx(SUCCESS, "MPCOS:");
    PrintAndLogEx(NORMAL, _GREEN_("\t\t0200 [98]:\n"));
    return PM3_SUCCESS;
}

static int CmdHF14AMfView(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf view",
                  "Print a MIFARE Classic dump file (bin/eml/json)",
                  "hf mf view -f hf-mf-01020304-dump.bin"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str1("f", "file", "<fn>", "filename of dump"),
        //arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);
    int fnlen = 0;
    char filename[FILE_PATH_SIZE];
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);
    bool verbose = true;//arg_get_lit(ctx, 2);
    CLIParserFree(ctx);

    // read dump file
    uint8_t *dump = NULL;
    size_t bytes_read = 0;
    int res = pm3_load_dump(filename, (void **)&dump, &bytes_read, (MFBLOCK_SIZE * MIFARE_4K_MAXBLOCK));
    if (res != PM3_SUCCESS) {
        return res;
    }

    uint16_t block_cnt = MIN(MIFARE_1K_MAXBLOCK, (bytes_read / MFBLOCK_SIZE));
    if (bytes_read == 320)
        block_cnt = MIFARE_MINI_MAXBLOCK;
    else if (bytes_read == 2048)
        block_cnt = MIFARE_2K_MAXBLOCK;
    else if (bytes_read == 4096)
        block_cnt = MIFARE_4K_MAXBLOCK;

    if (verbose) {
        PrintAndLogEx(INFO, "File: " _YELLOW_("%s"), filename);
        PrintAndLogEx(INFO, "File size %zu bytes, file blocks %d (0x%x)", bytes_read, block_cnt, block_cnt);
    }

    mf_print_blocks(block_cnt, dump, verbose);

    if (verbose) {
        mf_print_keys(block_cnt, dump);
    }

    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AGen4View(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf gview",
                  "View `magic gen4 gtu` card memory",
                  "hf mf gview\n"
                  "hf mf gview --4k"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_lit0(NULL, "mini", "MIFARE Classic Mini / S20"),
        arg_lit0(NULL, "1k", "MIFARE Classic 1k / S50 (def)"),
        arg_lit0(NULL, "2k", "MIFARE Classic/Plus 2k"),
        arg_lit0(NULL, "4k", "MIFARE Classic 4k / S70"),
        arg_str0("p", "pwd", "<hex>", "password 4bytes"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    bool m0 = arg_get_lit(ctx, 1);
    bool m1 = arg_get_lit(ctx, 2);
    bool m2 = arg_get_lit(ctx, 3);
    bool m4 = arg_get_lit(ctx, 4);

    int pwd_len = 0;
    uint8_t pwd[4] = {0};
    CLIGetHexWithReturn(ctx, 5, pwd, &pwd_len);

    bool verbose = arg_get_lit(ctx, 6);
    CLIParserFree(ctx);

    // validations
    if (pwd_len != 4 && pwd_len != 0) {
        PrintAndLogEx(FAILED, "Must specify 4 bytes, got " _YELLOW_("%u"), pwd_len);
        return PM3_EINVARG;
    }

    if ((m0 + m1 + m2 + m4) > 1) {
        PrintAndLogEx(WARNING, "Only specify one MIFARE Type");
        return PM3_EINVARG;
    } else if ((m0 + m1 + m2 + m4) == 0) {
        m1 = true;
    }

    char s[6];
    memset(s, 0, sizeof(s));
    uint16_t block_cnt = MIFARE_1K_MAXBLOCK;
    if (m0) {
        block_cnt = MIFARE_MINI_MAXBLOCK;
        strncpy(s, "Mini", 5);
    } else if (m1) {
        block_cnt = MIFARE_1K_MAXBLOCK;
        strncpy(s, "1K", 3);
    } else if (m2) {
        block_cnt = MIFARE_2K_MAXBLOCK;
        strncpy(s, "2K", 3);
    } else if (m4) {
        block_cnt = MIFARE_4K_MAXBLOCK;
        strncpy(s, "4K", 3);
    } else {
        PrintAndLogEx(WARNING, "No card type specified, defaulting to 1k");
        block_cnt = MIFARE_1K_MAXBLOCK;
        strncpy(s, "1K", 3);
    }
    PrintAndLogEx(SUCCESS, "View magic gen4 GTU MIFARE Classic " _GREEN_("[%s]"), s);
    PrintAndLogEx(INFO, "." NOLF);

    // Select card to get UID/UIDLEN information
    clearCommandBuffer();
    SendCommandMIX(CMD_HF_ISO14443A_READER, ISO14A_CONNECT, 0, 0, NULL, 0);
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
        PrintAndLogEx(WARNING, "iso14443a card select timeout");
        return PM3_ETIMEOUT;
    }

    /*
        0: couldn't read
        1: OK, with ATS
        2: OK, no ATS
        3: proprietary Anticollision
    */
    uint64_t select_status = resp.oldarg[0];

    if (select_status == 0) {
        PrintAndLogEx(WARNING, "iso14443a card select failed");
        return select_status;
    }

    iso14a_card_select_t card;
    memcpy(&card, (iso14a_card_select_t *)resp.data.asBytes, sizeof(iso14a_card_select_t));

    // reserve memory
    uint16_t bytes = block_cnt * MFBLOCK_SIZE;
    uint8_t *dump = calloc(bytes, sizeof(uint8_t));
    if (dump == NULL) {
        PrintAndLogEx(WARNING, "Fail, cannot allocate memory");
        return PM3_EMALLOC;
    }

    for (uint16_t i = 0; i < block_cnt; i++) {

        if (mfG4GetBlock(pwd, i, dump + (i * MFBLOCK_SIZE)) !=  PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "Can't get magic card block: %u", i);
            PrintAndLogEx(HINT, "Verify your card size, and try again or try another tag position");
            free(dump);
            return PM3_ESOFT;
        }
        PrintAndLogEx(NORMAL, "." NOLF);
        fflush(stdout);
    }

    PrintAndLogEx(NORMAL, "");
    mf_print_blocks(block_cnt, dump, verbose);

    if (verbose) {
        mf_print_keys(block_cnt, dump);
    }

    free(dump);
    return PM3_SUCCESS;
}

static int CmdHF14AMfValue(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf mf value",
                  "MIFARE Classic value data commands\n",
                  "hf mf value --blk 16 -k FFFFFFFFFFFF --set 1000\n"
                  "hf mf value --blk 16 -k FFFFFFFFFFFF --inc 10\n"
                  "hf mf value --blk 16 -k FFFFFFFFFFFF -b --dec 10\n"
                  "hf mf value --blk 16 -k FFFFFFFFFFFF -b --get\n"
                  "hf mf value --get -d 87D612007829EDFF87D6120011EE11EE\n"
                 );
    void *argtable[] = {
        arg_param_begin,
        arg_str0("k", "key", "<hex>", "key, 6 hex bytes"),
        arg_lit0("a", NULL, "input key type is key A (def)"),
        arg_lit0("b", NULL, "input key type is key B"),
        arg_u64_0(NULL, "inc", "<dec>", "Incremenet value by X (0 - 2147483647)"),
        arg_u64_0(NULL, "dec", "<dec>", "Dcrement value by X (0 - 2147483647)"),
        arg_u64_0(NULL, "set", "<dec>", "Set value to X (-2147483647 - 2147483647)"),
        arg_lit0(NULL, "get", "Get value from block"),
        arg_int0(NULL, "blk", "<dec>", "block number"),
        arg_str0("d", "data", "<hex>", "block data to extract values from (16 hex bytes)"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    uint8_t blockno = (uint8_t)arg_get_int_def(ctx, 8, 1);

    uint8_t keytype = MF_KEY_A;
    if (arg_get_lit(ctx, 2) && arg_get_lit(ctx, 3)) {
        CLIParserFree(ctx);
        PrintAndLogEx(WARNING, "Input key type must be A or B");
        return PM3_EINVARG;
    } else if (arg_get_lit(ctx, 3)) {
        keytype = MF_KEY_B;;
    }

    int keylen = 0;
    uint8_t key[6] = {0};
    CLIGetHexWithReturn(ctx, 1, key, &keylen);

    /*
        Value    /Value   Value    BLK /BLK BLK /BLK
        00000000 FFFFFFFF 00000000 10  EF   10  EF
        BLK is used to referece where the backup come from, I suspect its just the current block for the actual value ?
        increment and decrement are an unsigned value
        set value is a signed value

        We are getting signed and/or bigger values to allow a defult to be set meaning users did not supply that option.
    */
    int64_t incval = (int64_t)arg_get_u64_def(ctx, 4, -1); // Inc by -1 is invalid, so not set.
    int64_t decval = (int64_t)arg_get_u64_def(ctx, 5, -1); // Inc by -1 is invalid, so not set.
    int64_t setval = (int64_t)arg_get_u64_def(ctx, 6, 0x7FFFFFFFFFFFFFFF);  // out of bounds (for int32) so not set
    bool getval = arg_get_lit(ctx, 7);
    uint8_t block[MFBLOCK_SIZE] = {0x00};
    int dlen = 0;
    uint8_t data[16] = {0};
    CLIGetHexWithReturn(ctx, 9, data, &dlen);
    CLIParserFree(ctx);

    uint8_t action = 3; // 0 Increment, 1 - Decrement, 2 - Set, 3 - Get, 4 - Decode from data
    uint32_t value = 0;
    uint8_t isok = true;

    // Need to check we only have 1 of inc/dec/set and get the value from the selected option
    int optionsprovided = 0;

    if (incval != -1) {
        optionsprovided++;
        action = 0;
        if ((incval <= 0) || (incval > 2147483647)) {
            PrintAndLogEx(WARNING, "increment value must be between 1 and 2147483647. Got %lli", incval);
            return PM3_EINVARG;
        } else
            value = (uint32_t)incval;
    }

    if (decval != -1) {
        optionsprovided++;
        action = 1;
        if ((decval <= 0) || (decval > 2147483647)) {
            PrintAndLogEx(WARNING, "decrement value must be between 1 and 2147483647. Got %lli", decval);
            return PM3_EINVARG;
        } else
            value = (uint32_t)decval;
    }

    if (setval != 0x7FFFFFFFFFFFFFFF) {
        optionsprovided++;
        action = 2;
        if ((setval < -2147483647) || (setval > 2147483647)) {
            PrintAndLogEx(WARNING, "set value must be between -2147483647 and 2147483647. Got %lli", setval);
            return PM3_EINVARG;
        } else
            value = (uint32_t)setval;
    }

    if (dlen != 0)  {
        optionsprovided++;
        action = 4;
        if (dlen != 16) {
            PrintAndLogEx(WARNING, "date length must be 16 hex bytes long, got %d", dlen);
            return PM3_EINVARG;
        }
    }

    if (optionsprovided > 1) { // more then one option provided
        PrintAndLogEx(WARNING, "must have one and only one of --inc, --dec, --set or --data");
        return PM3_EINVARG;
    }

    // dont want to write value data and break something
    if ((blockno == 0) || (mfIsSectorTrailer(blockno))) {
        PrintAndLogEx(WARNING, "invlaid block number, should be a data block ");
        return PM3_EINVARG;
    }

    if (action < 3) {

        if (g_session.pm3_present == false)
            return PM3_ENOTTY;

        if (action <= 1) { // increment/decrement value
            memcpy(block, (uint8_t *)&value, 4);
            uint8_t cmddata[26];
            memcpy(cmddata, key, sizeof(key));  // Key == 6 data went to 10, so lets offset 9 for inc/dec
            if (action == 0)
                PrintAndLogEx(INFO, "value increment by : %d", value);
            else
                PrintAndLogEx(INFO, "value decrement by : %d", value);

            PrintAndLogEx(INFO, "Writing block no %d, key %c - %s", blockno, (keytype == MF_KEY_B) ? 'B' : 'A', sprint_hex_inrow(key, sizeof(key)));

            cmddata[9] = action; // 00 if increment, 01 if decrement.
            memcpy(cmddata + 10, block, sizeof(block));

            clearCommandBuffer();
            SendCommandMIX(CMD_HF_MIFARE_VALUE, blockno, keytype, 0, cmddata, sizeof(cmddata));

            PacketResponseNG resp;
            if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
                PrintAndLogEx(FAILED, "Command execute timeout");
                return PM3_ETIMEOUT;
            }
            isok  = resp.oldarg[0] & 0xff;
        } else { // set value
            // To set a value block (or setup) we can use the normal mifare classic write block
            // So build the command options can call CMD_HF_MIFARE_WRITEBL
            PrintAndLogEx(INFO, "set value to : %d", (int32_t)value);

            uint8_t writedata[26] = {0x00};
            int32_t invertvalue = value ^ 0xFFFFFFFF;
            memcpy(writedata, key, sizeof(key));
            memcpy(writedata + 10, (uint8_t *)&value, 4);
            memcpy(writedata + 14, (uint8_t *)&invertvalue, 4);
            memcpy(writedata + 18, (uint8_t *)&value, 4);
            writedata[22] = blockno;
            writedata[23] = (blockno ^ 0xFF);
            writedata[24] = blockno;
            writedata[25] = (blockno ^ 0xFF);

            clearCommandBuffer();
            SendCommandMIX(CMD_HF_MIFARE_WRITEBL, blockno, keytype, 0, writedata, sizeof(writedata));

            PacketResponseNG resp;
            if (WaitForResponseTimeout(CMD_ACK, &resp, 1500) == false) {
                PrintAndLogEx(FAILED, "Command execute timeout");
                return PM3_ETIMEOUT;
            }
            isok  = resp.oldarg[0] & 0xff;
        }

        if (isok) {
            PrintAndLogEx(SUCCESS, "Update ... : " _GREEN_("success"));
            getval = true; // all ok so set flag to read current value
        } else {
            PrintAndLogEx(FAILED, "Update ... : " _RED_("failed"));
        }
    }

    // If all went well getval will be true, so read the current value and display
    if (getval) {
        int32_t readvalue;
        int res = -1;

        if (action == 4) {
            res = PM3_SUCCESS; // alread have data from command line
        } else {
            res = mfReadBlock(blockno, keytype, key, data);
        }

        if (res == PM3_SUCCESS) {
            if (mfc_value(data, &readvalue))  {
                PrintAndLogEx(SUCCESS, "Dec ...... : " _YELLOW_("%" PRIi32), readvalue);
                PrintAndLogEx(SUCCESS, "Hex ...... : " _YELLOW_("0x%" PRIX32), readvalue);
            } else {
                PrintAndLogEx(FAILED, "No value block detected");
            }
        } else {
            PrintAndLogEx(FAILED, "failed to read value block");
        }
    }

    return PM3_SUCCESS;
}

static command_t CommandTable[] = {
    {"help",        CmdHelp,                AlwaysAvailable, "This help"},
    {"-----------", CmdHelp,                AlwaysAvailable,  "----------------------- " _CYAN_("recovery") " -----------------------"},
    {"darkside",    CmdHF14AMfDarkside,     AlwaysAvailable,  "Darkside attack"},
    {"nested",      CmdHF14AMfNested,       AlwaysAvailable,  "Nested attack"},
    {"hard",  CmdHF14AMfNestedHard,   AlwaysAvailable, "Nested attack for hardened MIFARE Classic cards"},
    {"harder",  CmdHF14AMfNestedHarder,   AlwaysAvailable, "Same as above, just tries harder"},
    {"static", CmdHF14AMfNestedStatic, AlwaysAvailable, "Nested attack against static nonce MIFARE Classic cards"},
    {"auto",     CmdHF14AMfAutoPWN,      AlwaysAvailable,  "Automatic key recovery tool for MIFARE Classic"},
    {"harvest",     CmdHF14AMfHarvest,      AlwaysAvailable,  "Perform advanced key harvesting"},
    {"nack",        CmdHf14AMfNack,         AlwaysAvailable,  "Test for MIFARE NACK bug"},
    {"chk",         CmdHF14AMfChk,          AlwaysAvailable,  "Check keys"},
    {"fchk",        CmdHF14AMfChk_fast,     AlwaysAvailable,  "Check keys fast, targets all keys on card"},
    {"decrypt",     CmdHf14AMfDecryptBytes, AlwaysAvailable, "[nt] [ar_enc] [at_enc] [data] - to decrypt sniff or trace"},
    {"supercard",   CmdHf14AMfSuperCard,    AlwaysAvailable,  "Extract info from a `super card`"},
    {"ice",         CmdHF14AMfice,          AlwaysAvailable,  "collect MIFARE Classic nonces to file"},
    {"-----------", CmdHelp,                AlwaysAvailable,  "----------------------- " _CYAN_("operations") " -----------------------"},
    {"auth4",       CmdHF14AMfAuth4,        AlwaysAvailable,  "ISO14443-4 AES authentication"},
    {"acl",         CmdHF14AMfAcl,          AlwaysAvailable, "Decode and print MIFARE Classic access rights bytes"},
    {"dump",        CmdHF14AMfDump,         AlwaysAvailable,  "Dump MIFARE Classic tag to binary file"},
    {"mad",         CmdHF14AMfMAD,          AlwaysAvailable, "Checks and prints MAD"},
    {"ndefread",    CmdHFMFNDEFRead,        AlwaysAvailable,  "Prints NDEF records from card"},
    {"personalize", CmdHFMFPersonalize,     AlwaysAvailable,  "Personalize UID (MIFARE Classic EV1 only)"},
    {"rdbl",        CmdHF14AMfRdBl,         AlwaysAvailable,  "Read MIFARE Classic block"},
    {"rdsc",        CmdHF14AMfRdSc,         AlwaysAvailable,  "Read MIFARE Classic sector"},
    {"restore",     CmdHF14AMfRestore,      AlwaysAvailable,  "Restore MIFARE Classic binary file to tag"},
    {"setmod",      CmdHf14AMfSetMod,       AlwaysAvailable,  "Set MIFARE Classic EV1 load modulation strength"},
    {"value",       CmdHF14AMfValue,        AlwaysAvailable, "Value blocks"},
    {"view",        CmdHF14AMfView,         AlwaysAvailable, "Display content from tag dump file"},
    {"wipe",        CmdHF14AMfWipe,         AlwaysAvailable,  "Wipe card to zeros and default keys/acc"},
    {"wrbl",        CmdHF14AMfWrBl,         AlwaysAvailable,  "Write MIFARE Classic block"},
    {"-----------", CmdHelp,                AlwaysAvailable,  "----------------------- " _CYAN_("simulation") " -----------------------"},
    {"sim",         CmdHF14AMfSim,          AlwaysAvailable,  "Simulate MIFARE card"},
    {"list",        CmdHF14AMfList,         AlwaysAvailable, "List MIFARE history"},
    {"sak",         CmdSAK,                 AlwaysAvailable, "Show SAK and ATQA help"},
    {"-----------", CmdHelp,                AlwaysAvailable,  "----------------------- " _CYAN_("emulator") " -----------------------"},
    {"ecfill",      CmdHF14AMfECFill,       AlwaysAvailable,  "Fill emulator memory with help of keys from emulator"},
    {"eclr",        CmdHF14AMfEClear,       AlwaysAvailable,  "Clear emulator memory"},
    {"erdbl",     CmdHF14AMfEGetBlk,      AlwaysAvailable,  "Get emulator memory block"},
    {"erdsc",      CmdHF14AMfEGetSc,       AlwaysAvailable,  "Get emulator memory sector"},
    {"ekey",     CmdHF14AMfEKeyPrn,      AlwaysAvailable,  "Print keys from emulator memory"},
    {"eload",       CmdHF14AMfELoad,        AlwaysAvailable,  "Load from file emul dump"},
    {"esave",       CmdHF14AMfESave,        AlwaysAvailable,  "Save to file emul dump"},
    {"ewrbl",     CmdHF14AMfESet,         AlwaysAvailable,  "Set emulator memory block"},
    {"eview",       CmdHF14AMfEView,        AlwaysAvailable,  "View emulator memory"},
    {"euid",        CmdHF14AMfEUID,         AlwaysAvailable,  "Overwrite UID (4-7 bytes)"},
    {"ebcc",        CmdHF14AMfEBCC,         AlwaysAvailable,  "BCC checksum operations"},
    {"edat",        CmdHF14AMfEDAT,         AlwaysAvailable,  "Overwrite manufacturer data"},
    {"ecp",         CmdHF14AMfECP,         AlwaysAvailable,  "Copy block/sector"},
    {"eacl",        CmdHF14AMfEACL,         AlwaysAvailable,  "Overwrite access condition bytes"},
    {"eins",        CmdHF14AMfEINS,         AlwaysAvailable,  "Insert arbitrary data into emulator memory"},
    {"esak",        CmdHF14AMfESAK,         AlwaysAvailable,  "View/change SAK and ATQA values"},
    {"-----------", CmdHelp,                AlwaysAvailable,  "----------------------- " _CYAN_("magic gen1") " -----------------------"},
    {"cgetblk",     CmdHF14AMfCGetBlk,      AlwaysAvailable,  "Read block from card"},
    {"cgetsc",      CmdHF14AMfCGetSc,       AlwaysAvailable,  "Read sector from card"},
    {"cload",       CmdHF14AMfCLoad,        AlwaysAvailable,  "Load dump to card"},
    {"csave",       CmdHF14AMfCSave,        AlwaysAvailable,  "Save dump from card into file or emulator"},
    {"csetblk",     CmdHF14AMfCSetBlk,      AlwaysAvailable,  "Write block to card"},
    {"csetuid",     CmdHF14AMfCSetUID,      AlwaysAvailable,  "Set UID on card"},
    {"cview",       CmdHF14AMfCView,        AlwaysAvailable,  "View card"},
    {"cwipe",       CmdHF14AMfCWipe,        AlwaysAvailable,  "Wipe card to default UID/Sectors/Keys"},
    {"-----------", CmdHelp,                AlwaysAvailable,  "----------------------- " _CYAN_("magic gen3") " -----------------------"},
    {"gen3uid",     CmdHf14AGen3UID,        AlwaysAvailable,  "Set UID without changing manufacturer block"},
    {"gen3blk",     CmdHf14AGen3Block,      AlwaysAvailable,  "Overwrite manufacturer block"},
    {"gen3freeze",  CmdHf14AGen3Freeze,     AlwaysAvailable,  "Perma lock UID changes. irreversible"},
    {"-----------", CmdHelp,                AlwaysAvailable,  "-------------------- " _CYAN_("magic gen4 GTU") " --------------------------"},
    {"gview",       CmdHF14AGen4View,       AlwaysAvailable,  "View card"},
    {NULL, NULL, NULL, NULL}
};

static int CmdHelp(const char *Cmd) {
    (void)Cmd; // Cmd is not used so far
    CmdsHelp(CommandTable);
    return PM3_SUCCESS;
}

int CmdHFMF(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}
