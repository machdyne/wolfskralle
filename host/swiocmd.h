/*
 * wolfctl: the SWIO commands (swiocmd.c) and what they share with wolfctl.c
 */

#ifndef SWIOCMD_H
#define SWIOCMD_H

#include "wkclient.h"
#include "wkhid.h"

extern struct wk_client cl;
extern struct wkhid hid;
extern int verbose;
void die_status(const char *what, int st);
void print_client_stats(void);

int do_swio(int argc, char **argv);
int do_swio_timing(int argc, char **argv);
int do_swio_test(int argc, char **argv, int load);
int do_swio_raw(int argc, char **argv);
int do_dmi(int argc, char **argv);
int do_swio_scan(int argc, char **argv, int apply);

#endif
