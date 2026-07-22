/* Minimal Cortex-M3 startup for QEMU mps2-an385. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);
void uart_puts(const char *s);
void semihost_exit(int code);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; dst++)
        *dst = 0;
    main();
    for (;;) {
    }
}

void Fault_Handler(void)
{
    uart_puts("\r\nFAULT\r\nRESULT: FAIL\r\n");
    semihost_exit(1);
}

__attribute__((section(".isr_vector"), used)) static void (*const vectors[16 + 32])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Fault_Handler, /* NMI */
    Fault_Handler, /* HardFault */
    Fault_Handler, /* MemManage */
    Fault_Handler, /* BusFault */
    Fault_Handler, /* UsageFault */
    0, 0, 0, 0,
    vPortSVCHandler,
    0, 0,
    xPortPendSVHandler,
    xPortSysTickHandler,
};
