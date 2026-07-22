/* CMSDK APB UART0 on mps2-an385, plus a tiny printf and semihosting exit. */
#include <stdarg.h>
#include <stdint.h>

#define UART0_BASE 0x40004000u
#define UART_DATA (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART_STATE (*(volatile uint32_t *)(UART0_BASE + 0x04))
#define UART_CTRL (*(volatile uint32_t *)(UART0_BASE + 0x08))
#define UART_BAUDDIV (*(volatile uint32_t *)(UART0_BASE + 0x10))

void uart_init(void)
{
    UART_BAUDDIV = 16;
    UART_CTRL = 1; /* TX enable */
}

static void uart_putc(char c)
{
    while (UART_STATE & 1u) {
    }
    UART_DATA = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void put_uint(unsigned long v, unsigned base)
{
    char buf[12];
    int n = 0;
    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n)
        uart_putc(buf[--n]);
}

/* Supports %s %d %u %x %lu %c %%. */
void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt);
            continue;
        }
        int is_long = 0;
        if (*++fmt == 'l') {
            is_long = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's': uart_puts(va_arg(ap, const char *)); break;
        case 'c': uart_putc((char)va_arg(ap, int)); break;
        case 'd': {
            long v = is_long ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) {
                uart_putc('-');
                v = -v;
            }
            put_uint((unsigned long)v, 10);
            break;
        }
        case 'u': put_uint(is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10); break;
        case 'x': put_uint(is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16); break;
        case '%': uart_putc('%'); break;
        default: break;
        }
    }
    va_end(ap);
}

/* ARM semihosting SYS_EXIT_EXTENDED: QEMU exits with the given status. */
void semihost_exit(int code)
{
    volatile uint32_t block[2] = { 0x20026u /* ADP_Stopped_ApplicationExit */, (uint32_t)code };
    register uint32_t r0 __asm__("r0") = 0x20; /* SYS_EXIT_EXTENDED */
    register volatile uint32_t *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xab" : : "r"(r0), "r"(r1) : "memory");
    for (;;) {
    }
}
