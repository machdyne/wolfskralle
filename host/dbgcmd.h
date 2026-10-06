/*
 * wolfctl: the debug commands (dbgcmd.c)
 */

#ifndef DBGCMD_H
#define DBGCMD_H

int do_id(int mode, int keep_halted);
int do_resume(void);
int do_read(int argc, char **argv, int mode, const char *outfile, int keep_halted);
int do_crc(int argc, char **argv, int mode, int keep_halted);
int do_flash(int argc, char **argv, int mode, int force);
int do_verify(int argc, char **argv, int mode);
int do_profile(int reset);

#endif
