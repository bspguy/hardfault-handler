#include "hardfault_dump.h"

#include <string.h>
#include <inttypes.h>
#include <stddef.h>

#include "stm32g4xx.h"   /* Pulls in CMSIS core headers on STM32G4 */

/*
 * Optional FreeRTOS support.
 *
 * Define HF_ENABLE_FREERTOS_SUPPORT in your build if this image uses FreeRTOS.
 * At runtime we still detect whether the scheduler is running before trying
 * to query task info, so this code is safe to use in early-boot faults too.
 */
#ifdef HF_ENABLE_FREERTOS_SUPPORT
  #include "FreeRTOS.h"
  #include "task.h"

  /* Weak refs: if FreeRTOS isn't linked in this image, these will be NULL. */
  extern BaseType_t xTaskGetSchedulerState(void) __attribute__((weak));
  extern void vTaskGetInfo(TaskHandle_t, TaskStatus_t *, BaseType_t, eTaskState)
      __attribute__((weak));
#endif

#ifndef MIN
#define MIN(a,b) (( (a) < (b) ) ? (a) : (b))
#endif

/* ========= Persistent buffer in .noinit (NOT cleared on reset) ========= */
/* Add .noinit section in linker script (see README).                      */
__attribute__((section(".noinit")))
static uint8_t s_hf_dump_area[8 * 1024];   /* tune size as needed */

/* ------------ FreeRTOS task name length abstraction ------------ */
#ifdef HF_ENABLE_FREERTOS_SUPPORT
  #ifndef configMAX_TASK_NAME_LEN
    #error "configMAX_TASK_NAME_LEN must be defined when HF_ENABLE_FREERTOS_SUPPORT is enabled"
  #endif
  #define HF_MAX_TASK_NAME_LEN (configMAX_TASK_NAME_LEN)
#else
  #define HF_MAX_TASK_NAME_LEN (16U)
#endif

#define HF_MAGIC   0x48464450u   /* 'HFDP' */
#define HF_VERSION 0x0003u

/* Dump header (everything about the fault context + optional FreeRTOS task) */
typedef struct __attribute__((__packed__)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;

    uint32_t exc_return;
    uint32_t msp;
    uint32_t psp;
    uint32_t active_sp;   /* pointer to the stacked frame */
    uint32_t used_sp;     /* 0 = MSP, 1 = PSP */
    uint32_t has_fp;      /* 0/1 whether FP context was stacked */

    /* SCB fault info */
    uint32_t scb_cfsr;
    uint32_t scb_hfsr;
    uint32_t scb_dfsr;
    uint32_t scb_mmfar;
    uint32_t scb_bfar;
    uint32_t scb_afsr;
    uint32_t shcsr;

    /* Core regs from stacked frame */
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t psr;

    /* FreeRTOS info about the faulted task (if scheduler started) */
    uint32_t rtos_present;                /* 0/1 */
    uint32_t rtos_task_priority;          /* uxCurrentPriority */
    uint32_t rtos_stack_high_water_bytes; /* minimum free stack during life */
    uint32_t rtos_stack_base;             /* pxStackBase */
    char     rtos_task_name[HF_MAX_TASK_NAME_LEN + 1];

    /* Payload (stack dump) */
    uint32_t stack_bytes;  /* number of bytes after this header */
    uint32_t checksum;     /* XOR of header(with checksum=0) + payload */
} hf_dump_hdr_t;

/* ================== Local helpers for dump memory ================== */

static void hf_memclear(void *p, uint32_t len)
{
    /* Mark as 0xFF to be distinguishable from zeroed BSS. */
    memset(p, 0xFF, len);
}

static void hf_memwrite(uint32_t off, const void *data, uint32_t len)
{
    if (off >= sizeof(s_hf_dump_area)) return;
    if (off + len > sizeof(s_hf_dump_area)) {
        len = (uint32_t)sizeof(s_hf_dump_area) - off;
    }
    memcpy(&s_hf_dump_area[off], data, len);
}

static void hf_memread(uint32_t off, void *data, uint32_t len)
{
    if (off >= sizeof(s_hf_dump_area)) {
        memset(data, 0xFF, len);
        return;
    }
    if (off + len > sizeof(s_hf_dump_area)) {
        len = (uint32_t)sizeof(s_hf_dump_area) - off;
    }
    memcpy(data, &s_hf_dump_area[off], len);
}

static uint32_t hf_xor(const void *p, uint32_t len)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t x = 0;
    for (uint32_t i = 0; i < len; i++) {
        x ^= b[i];
    }
    return x;
}

/* ====================== Public dump helpers ====================== */

bool HardFault_DumpAvailable(void)
{
    hf_dump_hdr_t h;
    hf_memread(0, &h, sizeof(h));

    if (h.magic != HF_MAGIC)                             return false;
    if (h.version != HF_VERSION)                         return false;
    if (h.header_len != sizeof(hf_dump_hdr_t))           return false;
    if (h.stack_bytes > sizeof(s_hf_dump_area) - sizeof(hf_dump_hdr_t))
        return false;

    uint32_t saved = h.checksum;
    h.checksum = 0;
    uint32_t computed = hf_xor(&h, sizeof(h))
                      ^ hf_xor(&s_hf_dump_area[sizeof(h)],
                               h.stack_bytes);
    return (saved == computed);
}

void HardFault_ClearDump(void)
{
    hf_memclear(s_hf_dump_area, (uint32_t)sizeof(s_hf_dump_area));
}

/* ===================== Fault enable helper ===================== */

static inline void Fault_EnableAll(void)
{
    /* Enable MemManage, BusFault, UsageFault so HardFault is more specific. */
    SCB->SHCSR |= (SCB_SHCSR_MEMFAULTENA_Msk |
                   SCB_SHCSR_BUSFAULTENA_Msk |
                   SCB_SHCSR_USGFAULTENA_Msk);
}

/* ========================= Decode & print ========================= */

void HardFault_DecodeAndPrint(void)
{
    if (!HardFault_DumpAvailable()) {
        return;
    }

    hf_dump_hdr_t h;
    hf_memread(0, &h, sizeof(h));

    HF_LOGF("\r\n===== HARD FAULT DUMP =====\r\n");
    HF_LOGF("Magic: 0x%08" PRIX32 ", Ver: %" PRIu16 "\r\n",
            h.magic, h.version);
    HF_LOGF("EXC_RETURN: 0x%08" PRIX32 "  MSP: 0x%08" PRIX32
            "  PSP: 0x%08" PRIX32 "\r\n",
            h.exc_return, h.msp, h.psp);
    HF_LOGF("Active SP: 0x%08" PRIX32 "  Used: %s  FP ctx: %s\r\n",
            h.active_sp,
            (h.used_sp ? "PSP" : "MSP"),
            (h.has_fp ? "YES" : "NO"));

    HF_LOGF("Core regs:\r\n");
    HF_LOGF(" R0 : 0x%08" PRIX32 "  R1 : 0x%08" PRIX32 "\r\n", h.r0, h.r1);
    HF_LOGF(" R2 : 0x%08" PRIX32 "  R3 : 0x%08" PRIX32 "\r\n", h.r2, h.r3);
    HF_LOGF(" R12: 0x%08" PRIX32 "  LR : 0x%08" PRIX32 "\r\n",
            h.r12, h.lr);
    HF_LOGF(" PC : 0x%08" PRIX32 "  PSR: 0x%08" PRIX32 "\r\n",
            h.pc, h.psr);

    /* Fault registers */
    uint8_t  mmfsr = (uint8_t)(h.scb_cfsr & 0xFFu);
    uint8_t  bfsr  = (uint8_t)((h.scb_cfsr >> 8) & 0xFFu);
    uint16_t ufsr  = (uint16_t)((h.scb_cfsr >> 16) & 0xFFFFu);

    HF_LOGF("CFSR: 0x%08" PRIX32 " (MMFSR=0x%02" PRIX8
            " BFSR=0x%02" PRIX8 " UFSR=0x%04" PRIX16 ")\r\n",
            h.scb_cfsr, mmfsr, bfsr, ufsr);
    HF_LOGF("HFSR: 0x%08" PRIX32 "  DFSR: 0x%08" PRIX32 "\r\n",
            h.scb_hfsr, h.scb_dfsr);
    HF_LOGF("MMFAR: 0x%08" PRIX32 "  BFAR: 0x%08" PRIX32 "\r\n",
            h.scb_mmfar, h.scb_bfar);
    HF_LOGF("AFSR: 0x%08" PRIX32 "  SHCSR: 0x%08" PRIX32 "\r\n",
            h.scb_afsr, h.shcsr);

    if (h.rtos_present) {
        HF_LOGF("FreeRTOS:\r\n");
        HF_LOGF(" Task : '%s'\r\n", h.rtos_task_name);
        HF_LOGF(" Prio : %" PRIu32 "\r\n", h.rtos_task_priority);
        HF_LOGF(" Stack base : 0x%08" PRIX32 "\r\n", h.rtos_stack_base);
        HF_LOGF(" Min free   : %" PRIu32 " bytes\r\n",
                h.rtos_stack_high_water_bytes);
    } else {
        HF_LOGF("FreeRTOS info: not available (no RTOS or scheduler not started)\r\n");
    }

    HF_LOGF("Stack dump bytes: %" PRIu32 "\r\n", h.stack_bytes);

    /* Machine-friendly line for PC-side addr2line script */
    HF_LOGF("HF_ADDR PC=0x%08" PRIX32 " LR=0x%08" PRIX32 "\r\n", h.pc, h.lr);

    HF_LOGF("===== END HARD FAULT DUMP =====\r\n");
}

/* ===================== HardFault handler core ===================== */

static inline uint32_t get_main_stack_top(void)
{
    extern uint32_t _estack[];   /* defined in linker script */
    return (uint32_t)_estack;
}

/* forward declaration of C helper called by the naked handler */
static void prvGetRegistersFromStack(uint32_t *fault_sp, uint32_t exc_return);

/* This is the vector-table entry. Do NOT call directly. */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile
    (
        "tst lr, #4                   \n" /* Check EXC_RETURN bit[2] SP used */
        "ite eq                       \n"
        "mrseq r0, msp                \n" /* r0 = active SP (MSP)  */
        "mrsne r0, psp                \n" /* r0 = active SP (PSP)  */
        "mov   r1, lr                 \n" /* r1 = EXC_RETURN       */
        "b     prvGetRegistersFromStack \n"
    );
}

static void prvGetRegistersFromStack(uint32_t *fault_sp, uint32_t exc_return)
{
    const uint32_t used_psp = (exc_return & (1U << 2)) ? 1U : 0U; /* bit2 */
    const uint32_t msp = __get_MSP();
    const uint32_t psp = __get_PSP();
    const uint32_t has_fp = ((exc_return & (1U << 4)) == 0U) ? 1U : 0U;

    /* core regs from stacked frame (basic frame, ignoring FP extension) */
    uint32_t r0  = fault_sp[0];
    uint32_t r1  = fault_sp[1];
    uint32_t r2  = fault_sp[2];
    uint32_t r3  = fault_sp[3];
    uint32_t r12 = fault_sp[4];
    uint32_t lr  = fault_sp[5];
    uint32_t pc  = fault_sp[6];
    uint32_t psr = fault_sp[7];

    hf_dump_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.magic      = HF_MAGIC;
    hdr.version    = HF_VERSION;
    hdr.header_len = sizeof(hf_dump_hdr_t);

    hdr.exc_return = exc_return;
    hdr.msp        = msp;
    hdr.psp        = psp;
    hdr.active_sp  = (uint32_t)fault_sp;
    hdr.used_sp    = used_psp;
    hdr.has_fp     = has_fp;

    hdr.scb_cfsr   = SCB->CFSR;
    hdr.scb_hfsr   = SCB->HFSR;
    hdr.scb_dfsr   = SCB->DFSR;
    hdr.scb_mmfar  = SCB->MMFAR;
    hdr.scb_bfar   = SCB->BFAR;
    hdr.scb_afsr   = SCB->AFSR;
    hdr.shcsr      = SCB->SHCSR;

    hdr.r0 = r0; hdr.r1 = r1; hdr.r2 = r2; hdr.r3 = r3;
    hdr.r12 = r12; hdr.lr = lr; hdr.pc = pc; hdr.psr = psr;

    /* Try to capture FreeRTOS info about the current task (if compiled in) */
    hdr.rtos_present = 0;

#ifdef HF_ENABLE_FREERTOS_SUPPORT
    if (xTaskGetSchedulerState != NULL &&
        vTaskGetInfo           != NULL &&
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        TaskStatus_t ts;
        vTaskGetInfo(NULL, &ts, pdTRUE, eInvalid);

        hdr.rtos_present = 1;
        hdr.rtos_task_priority = ts.uxCurrentPriority;
        hdr.rtos_stack_high_water_bytes =
            (uint32_t)ts.usStackHighWaterMark * sizeof(StackType_t);
        hdr.rtos_stack_base = (uint32_t)ts.pxStackBase;

        strncpy(hdr.rtos_task_name, ts.pcTaskName, HF_MAX_TASK_NAME_LEN);
        hdr.rtos_task_name[HF_MAX_TASK_NAME_LEN] = '\0';
    }
#endif

    /* Now, copy some of the faulted stack */
    hf_memclear(s_hf_dump_area, (uint32_t)sizeof(s_hf_dump_area));

    const uint32_t max_payload =
        (uint32_t)sizeof(s_hf_dump_area) - (uint32_t)sizeof(hf_dump_hdr_t);

    /* Without precise RTOS stack bounds, we just limit to some sane cap (e.g. 2KB) */
    uint32_t max_stack_copy = MIN(max_payload, 2048U);

    hdr.stack_bytes = 0;
    hdr.checksum    = 0;

    /* Write header first (checksum to be updated later) */
    hf_memwrite(0, &hdr, sizeof(hdr));

    /* Basic sanity: fault_sp must be below main stack top */
    if ((uint32_t)fault_sp < get_main_stack_top()) {
        hf_memwrite(sizeof(hdr), fault_sp, max_stack_copy);
        hdr.stack_bytes = max_stack_copy;

        /* Compute checksum over header(with checksum=0) + payload */
        hdr.checksum = 0;
        hdr.checksum = hf_xor(&hdr, sizeof(hdr))
                     ^ hf_xor(&s_hf_dump_area[sizeof(hdr)],
                              hdr.stack_bytes);

        hf_memwrite(0, &hdr, sizeof(hdr));
    }

#ifdef DEBUG
    __ASM volatile ("BKPT #01");
#endif

    __DSB();
    __ISB();
    NVIC_SystemReset();
}

/* ========================= Public init API ========================= */

void HardFaultDumps_Init(void)
{
    /* Enable detailed faults */
    Fault_EnableAll();

    /* If a dump exists from a previous reset, decode & print it. */
    if (HardFault_DumpAvailable()) {
        HardFault_DecodeAndPrint();
        /* Optionally clear afterwards so it doesn't spam every boot */
        HardFault_ClearDump();
    }
}
