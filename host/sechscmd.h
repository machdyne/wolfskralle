/*
 * wolfctl: the Sechs commands (sechscmd.c)
 */

#ifndef SECHSCMD_H
#define SECHSCMD_H

#include <stdint.h>

int do_sechs(int argc, char **argv, uint8_t addr, int restart, int console);
int sechs_check(uint8_t addr, int restart, int console, const char *module,
                const uint8_t *img, uint32_t img_len);

#endif
