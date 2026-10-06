#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

/* rv003usb needs SysTick on HCLK (48 MHz, from the HSI and the PLL) */
#define FUNCONF_SYSTICK_USE_HCLK    1

/* printf over the Wolfskralle's own SWIO (J2 pin 5) to a WCH-LinkE:
 * make DEBUG=1 */
#ifdef WK_DEBUG
#define FUNCONF_USE_DEBUGPRINTF     1
#define FUNCONF_DEBUGPRINTF_TIMEOUT 10
#else
#define FUNCONF_USE_DEBUGPRINTF     0
#endif

#endif
