#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/*
 * HardFault dump & post-mortem support for Cortex-M4 / STM32G4
 *
 * Features:
 *  - Saves SCB fault registers, core registers, and a slice of the
 *    faulted stack into a persistent .noinit buffer that survives reset.
 *  - Optional FreeRTOS integration: captures task name, priority and stack
 *    high-water mark of the faulted task (if the scheduler is running).
 *  - On next boot, you call HardFaultDumps_Init() and it will:
 *      * Enable detailed Mem/Bus/Usage faults.
 *      * If a previous HardFault dump exists, decode it and print it via HF_LOGF.
 *
 *  - PC side: a tiny Python script (hf_addr2line.py) parses the UART log and
 *    resolves PC/LR addresses to function and file:line using addr2line.
 *
 * Usage:
 *  - Add .noinit section in your linker script (see README.md).
 *  - Add hardfault_dump.c to your build.
 *  - Ensure your UART/printf is ready, then call HardFaultDumps_Init() in main().
 *  - Trigger a HardFault (or wait for a real one), then inspect the next boot log.
 */

/* You can override HF_LOGF to use your own logger, e.g. RTT or custom UART. */
#ifndef HF_LOGF
#define HF_LOGF printf
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Call once after clocks + UART are ready, early in main(). */
void HardFaultDumps_Init(void);

/* Explicit check if you want (Init already decodes & clears if present). */
bool HardFault_DumpAvailable(void);

/* Manually decode & print (Init calls this automatically if dump exists). */
void HardFault_DecodeAndPrint(void);

/* Manually clear dump region. */
void HardFault_ClearDump(void);

/* You don't call this yourself; installed in the vector table. */
void HardFault_Handler(void);

#ifdef __cplusplus
}
#endif
