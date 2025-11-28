# Cortex-M4 HardFault Dump Kit

```text
╔══════════════════════════════════════╗
║   CORTEX‑M4 HARDFAULT DUMP ENGINE    ║
╚══════════════════════════════════════╝
```

This is a small, **drop‑in HardFault crash‑dump system** for Cortex‑M4
(e.g. STM32G4) that:

- Captures **SCB fault registers**, **core registers**, and a slice of the
  **faulted stack** into a persistent `.noinit` buffer.
- Optionally grabs **FreeRTOS task info** for the crashing task:
  - task name
  - priority
  - stack high‑water mark (minimum free stack)
- On the *next boot*, decodes the dump and prints it over UART using `HF_LOGF`.
- Emits an `HF_ADDR PC=... LR=...` line that a **PC‑side Python script**
  parses and resolves into `function() @ file.c:line` via `addr2line`.

Designed to be:

- **Bare‑metal friendly** (no RTOS required).
- **FreeRTOS‑aware at runtime**, only if present and running.
- Minimal and self‑contained.

---

## Files

- `hardfault_dump.h` – public API + logger macro.
- `hardfault_dump.c` – implementation (Cortex‑M4 + STM32G4).
- `hf_addr2line.py` – PC‑side helper to resolve PC/LR addresses using your `.elf`.
- `README.md` – this document.

---

## 1. Integration on MCU side

### 1.1. Linker script: `.noinit` section

In your GCC linker script (`.ld`), inside the RAM region, add:

```ld
.noinit (NOLOAD) :
{
    . = ALIGN(4);
    KEEP(*(.noinit))
    . = ALIGN(4);
} >RAM
```

The library places its persistent buffer in `.noinit`, so it **survives reset**
and is not cleared by the C runtime.

No other changes to the linker script are required, except ensuring `_estack`
is defined as the **initial MSP** (top of stack), which you likely already have in
a standard STM32G4 linker script.

---

### 1.2. Add the sources

Add these two files to your project:

```text
Src/hardfault_dump.c
Inc/hardfault_dump.h
```

Make sure the include path to `Inc/` is in your compiler flags.

---

### 1.3. FreeRTOS vs bare‑metal

By default, the code works in **bare‑metal** mode: it does not include any
FreeRTOS headers and the dump will contain only core + SCB + stack info.

If you *do* use FreeRTOS and want **task info** in the dump, define:

```c
#define HF_ENABLE_FREERTOS_SUPPORT
```

either:

- in your global config header, **before** including `hardfault_dump.h`, or
- via compiler flags: `-DHF_ENABLE_FREERTOS_SUPPORT`.

Then make sure your `FreeRTOSConfig.h` has:

```c
#define configUSE_TRACE_FACILITY              1
#define INCLUDE_xTaskGetCurrentTaskHandle     1
#define INCLUDE_xTaskGetHandle                1
#define INCLUDE_uxTaskGetStackHighWaterMark   1
```

At **runtime**, the handler will only use FreeRTOS APIs if:

- the symbols `xTaskGetSchedulerState` and `vTaskGetInfo` are actually linked
  (we declare them `__attribute__((weak))`), and
- the scheduler state is **not** `taskSCHEDULER_NOT_STARTED`.

Otherwise, the dump falls back to pure bare‑metal mode and sets
`rtos_present = 0`.

---

### 1.4. Vector table

`hardfault_dump.c` defines:

```c
void HardFault_Handler(void);
```

as a **naked** handler that pulls the stacked frame pointer (MSP/PSP) and
EXC_RETURN, then jumps into the C helper.

Make sure this symbol is the one wired in your startup file. Usually the
ST startup file has a weak:

```c
void HardFault_Handler(void) __attribute__((weak));
```

So simply **do not** define another `HardFault_Handler` anywhere else, and
the linker will bind it to this implementation.

---

### 1.5. Call the init function

In your `main.c`, after clocks and UART are ready:

```c
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_USART2_UART_Init();   // or your logging UART

    HardFaultDumps_Init();   // <--- enable detailed faults + decode old dump

    // now continue with the rest of init
    MX_GPIO_Init();
    // ...
#ifdef USE_FREERTOS
    MX_FREERTOS_Init();
    vTaskStartScheduler();
#else
    while (1) {
        // bare-metal main loop
    }
#endif
}
```

`HardFaultDumps_Init()` does two things:

1. Enables MemManage, BusFault and UsageFault (`Fault_EnableAll()`).
2. Checks if a valid dump exists in `.noinit`.
   - If yes:
     - decodes and prints it over `HF_LOGF`
     - clears the dump so it won't be printed every boot.

If you want to keep the dump until you explicitly clear it, you can remove or
comment out the `HardFault_ClearDump()` call inside `HardFaultDumps_Init()`.

---

### 1.6. Custom logger (instead of printf)

By default, the header defines:

```c
#ifndef HF_LOGF
#define HF_LOGF printf
#endif
```

If you prefer a custom UART logger:

```c
// my_logger.h
void my_uart_logf(const char *fmt, ...);

// some global header *before* including hardfault_dump.h:
#define HF_LOGF  my_uart_logf
#include "hardfault_dump.h"
```

Now all dump output goes through `my_uart_logf`.

---

## 2. What happens on HardFault

When your firmware hits a HardFault:

1. `HardFault_Handler` chooses MSP or PSP based on EXC_RETURN bit[2],
   and passes:
   - `fault_sp` (the stacked frame pointer)
   - `exc_return` (the LR/EXC_RETURN value)
   to `prvGetRegistersFromStack()`.

2. `prvGetRegistersFromStack()`:
   - Extracts **R0–R3, R12, LR, PC, PSR** from the stacked frame.
   - Reads SCB registers:
     - `SCB->CFSR`, `SCB->HFSR`, `SCB->DFSR`,
       `SCB->MMFAR`, `SCB->BFAR`, `SCB->AFSR`, `SCB->SHCSR`.
   - Captures:
     - `MSP`, `PSP`, active SP, whether FP context was stacked (`has_fp`).
   - If FreeRTOS support is compiled in and running:
     - Gets the current task (`vTaskGetInfo(NULL, ...)`).
     - Stores task name, priority, stack base, and stack high‑water mark.

3. It then:
   - Clears the `.noinit` dump buffer to `0xFF`.
   - Writes the header.
   - Copies up to **2 KB** of the faulted stack into the dump buffer.
   - Computes a simple XOR checksum over header+payload.
   - Writes back the header with the checksum.
   - Issues a breakpoint in `#ifdef DEBUG` builds.
   - Calls `NVIC_SystemReset()`.

On the very next boot, `HardFaultDumps_Init()` sees the dump and prints a
human‑readable summary over UART, including a line:

```text
HF_ADDR PC=0x08001234 LR=0x08000F00
```

---

## 3. PC‑side ELF decoding with `hf_addr2line.py`

Once you captured the UART output into a log file (or copy/pasted it into one),
you can resolve the program counter and link‑register addresses using the
provided helper.

### 3.1. Usage

```bash
python hf_addr2line.py firmware.elf hardfault.log
```

- `firmware.elf` – your debug build with symbols
- `hardfault.log` – UART capture that contains the HardFault dump

The script:

1. Scans the log for lines like:
   ```text
   HF_ADDR PC=0x08001234 LR=0x08000F00
   ```
2. Extracts all PC/LR pairs and deduplicates them.
3. For each unique address, runs:

   ```bash
   arm-none-eabi-addr2line -f -C -e firmware.elf 0x08001234
   ```

4. Prints something like:

   ```text
   Found 2 unique addresses. Resolving with addr2line...

   0x08001234:
   HardFaultingFunction
   Src/app/foo.c:123

   0x08000F00:
   SomeCaller
   Src/app/bar.c:87
   ```

This gives you an immediate mapping from your crash PC/LR to source locations.

You can easily extend the script to also scan for arbitrary hex addresses
in the dump (e.g. `MMFAR`, `BFAR`, suspicious values from the stack) and
resolve them too.

---

## 4. Quick checklist

1. Add `.noinit` section to your linker script.
2. Add `hardfault_dump.c/.h` to your project.
3. If using FreeRTOS:
   - define `HF_ENABLE_FREERTOS_SUPPORT`
   - enable the trace‑related config macros.
4. Ensure UART + `HF_LOGF` are usable very early in `main()`.
5. Call `HardFaultDumps_Init()` right after clock + UART init.
6. Build, flash, run.
7. When a HardFault happens:
   - on the next boot, observe the UART dump.
   - save the log to a file.
   - run `python hf_addr2line.py firmware.elf hardfault.log`.

Enjoy real‑time, low‑jitter HardFault post‑mortems. 😈🔧
