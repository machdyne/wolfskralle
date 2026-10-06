# wolfctl

`wolfctl` is the command-line tool for the Wolfskralle. It runs on Linux
and needs no libraries (it uses hidraw). macOS and Windows support (through
hidapi) is planned.

**Status: phase 1-5 commands. Tested against a simulated device; not
yet on hardware.**

## Building and access

```
$ make -C host                 # host/wolfctl
$ make -C host test            # the protocol tests (docs/protocol.md)
$ sudo cp host/99-wolfskralle.rules /etc/udev/rules.d/
$ sudo udevadm control --reload
```

Then unplug and plug in the Wolfskralle. Without the rule, `wolfctl` only
works as root.

## Use

```
wolfctl [-d /dev/hidrawN] [-s serial] [-v] command [args]
```

With one Wolfskralle attached, `wolfctl` finds it. With several, choose one
by its serial number (`-s`, see `list`) or its hidraw device (`-d`). Only one
`wolfctl` at a time can use a Wolfskralle. `-v` adds the transfer counters
(below) to any command.

| Command | |
|---|---|
| `list` | the Wolfskralles attached, with their serial numbers |
| `info` | board, firmware, uptime, supply voltage, target power, LED, counters |
| `power [on\|off\|status] [-m ms]` | switch target power or show it; `-m` watches the supply voltage for up to 200 ms after switching and shows the lowest and average value |
| `led on\|off\|auto` | the white LED |
| `ping [count] [-l bytes] [-w ms] [-p n]` | round trips with random data, each checked; `-l` fixes the payload size (0-118, default random), `-w` makes the device wait in each, `-p` power-cycles the target every n pings |
| `swio on\|off` | the SWIO pins; on needs target power (the other SWIO commands switch it on themselves) |
| `swio-timing [name=ticks ...]` | show the SWIO timing, or change it (names: period, one, one-pp, zero, read, thresh, mode, stop); until the Wolfskralle restarts |
| `swio-test [count] [--load]` | write and read back the debug data register count times (default 1000) and show the measured pulses; `--load` with USB pings in between |
| `swio-scan [count] [--apply]` | find the working timing for the module in the contacts (docs/swio.md); `--apply` uses it |
| `swio-raw read REG` / `swio-raw write REG VALUE` | one packet, with every pulse measured |
| `dmi (r REG \| w REG VALUE)...` | debug module registers, up to 20 operations in one go |
| `id` | connect, stop the chip, show what it is (chip, flash size, read protection, unique ID), restart it |
| `read [ADDR [LEN]] [-o FILE]` | read memory, by default the whole flash; read twice (the Wolfskralle checks a CRC of a second read); a hex dump, or into FILE |
| `crc [ADDR [LEN]]` | CRC-32 of memory, by default the whole flash, computed on the Wolfskralle |
| `resume` | restart the chip's program |

| `flash IMAGE.bin [--force]` | program the module: Machdyne BASIC for LS10A or LS11A (checked against the chip), or with `--force` any firmware; verified, then restarted (docs/programming.md) |
| `verify IMAGE.bin` | does the module hold this image (followed by 0xFF to the end of its flash)? |
| `profile [--reset]` | where the Wolfskralle's time went since the last reset: page reads, erase, program, read-back, CRCs, SWIO packets, USB GETs. `flash -v` resets it before and shows it after the flash |

`id`, `read`, `crc`, `flash` and `verify` also take `--slow`: without the
fast paths (docs/programming.md, speed). `id` shows which are in use.

`id`, `read`, `crc`, `flash` and `verify` take `--attach` (only talk to the running chip),
`--restart` (restart it first: on board v0 by switching target power off
and on) and `--halt` (leave it stopped). docs/debug.md explains them.

Examples:

```
$ wolfctl info
$ wolfctl power on -m 100      # how far does the 3.3V rail dip?
$ wolfctl ping 100000          # the phase 1 gate
$ wolfctl ping 10000 -p 100    # with target power cycled every 100 pings
$ wolfctl ping 20 -w 1000      # long commands: the host waits while busy
$ wolfctl swio-scan --apply    # find the timing for this module
$ wolfctl swio-test 10000 --load
$ wolfctl id
$ wolfctl read -o ls10a.bin    # the whole flash, read twice
$ wolfctl read 0x08000000 64   # a hex dump
$ wolfctl flash basic-ls10a.bin
$ wolfctl verify basic-ls10a.bin
```

## When a command times out

`wolfctl` says why:

- **still busy**: the Wolfskralle answered "busy" to the end. It is running
  a command, perhaps one an earlier wolfctl left behind when it gave up.
  Wait a moment and try again; `wolfctl info` answers as soon as it is
  free.
- **USB transfers failed**: see `dmesg`; unplug and plug in the
  Wolfskralle.
- **no matching reply**: it answered, but never to this request (it kept
  losing the request): a bad USB connection.

`-v` adds the transfer counters. `wolfctl info` shows why the Wolfskralle
last started: "WATCHDOG" means its firmware hung and restarted itself,
which is a bug: please report it with what you were doing.

## Reading the counters

`ping` (and any command with `-v`) prints what happened underneath:

- **host:** reports sent (sets) and fetched (gets); busy answers; resends
  (the device did not have the request, so it was sent again); set and get
  errors (the USB transfer failed); bad replies (wrong length or CRC);
  timeouts (a command given up, which is an error).
- **device:** repeats answered from the kept reply (a request arrived
  twice; it ran once); bad requests (CRC); requests rejected because a
  command was running.

On a healthy link, after a ping run, everything except sets, gets and busy
should be 0. Errors that the protocol recovered from are not failures,
but their number says how good the USB link is: that is what phase 1 has
to find out (docs/hwtest.md).
