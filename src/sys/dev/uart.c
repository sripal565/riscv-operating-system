//  uart.c - NS8250-compatible uart port
//  
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "uart.h"
#include "device.h"
#include "intr.h"
#include "heap.h"

#include "ioimpl.h"
#include "console.h"

#include "error.h"

#include "thread.h"

#include <stdint.h>

//  COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_NAME
#define UART_NAME "uart"
#endif

//  INTERNAL TYPE DEFINITIONS
//  

struct uart_regs
{
    union
    {
        char rbr;    //  DLAB=0 read
        char thr;    //  DLAB=0 write
        uint8_t dll; //  DLAB=1
    };

    union
    {
        uint8_t ier; //  DLAB=0
        uint8_t dlm; //  DLAB=1
    };

    union
    {
        uint8_t iir; //  read
        uint8_t fcr; //  write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

struct ringbuf
{
    unsigned int hpos; //  head of queue (from where elements are removed)
    unsigned int tpos; //  tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

struct uart_device
{
    volatile struct uart_regs *regs;
    int irqno;
    int instno;

    struct io io;

    unsigned long rxovrcnt; //  number of times OE was set

    struct ringbuf rxbuf;
    struct ringbuf txbuf;

    //Condition variables
    struct condition rxbnotempty;
    struct condition txbnotfull;
};

//  INTERNAL FUNCTION DEFINITIONS
//

static int uart_open(struct io **ioptr, void *aux);
static void uart_close(struct io *io);
static long uart_read(struct io *io, void *buf, long bufsz);
static long uart_write(struct io *io, const void *buf, long len);

static void uart_isr(int srcno, void *driver_private);

static void rbuf_init(struct ringbuf *rbuf);
static int rbuf_empty(const struct ringbuf *rbuf);
static int rbuf_full(const struct ringbuf *rbuf);
static void rbuf_putc(struct ringbuf *rbuf, char c);
static char rbuf_getc(struct ringbuf *rbuf);

//  EXPORTED FUNCTION DEFINITIONS
//  

void uart_attach(void *mmio_base, int irqno)
{
    static const struct iointf uart_iointf = {
        .close = &uart_close,
        .read = &uart_read,
        .write = &uart_write};

    struct uart_device *uart;

    uart = kcalloc(1, sizeof(struct uart_device));

    uart->regs = mmio_base;
    uart->irqno = irqno;

    ioinit0(&uart->io, &uart_iointf);

    //  Check if we're trying to attach UART0, which is used for the console. It
    //  had already been initialized and should not be accessed as a normal
    //  device.

    if (mmio_base != (void *)UART0_MMIO_BASE)
    {

        uart->regs->ier = 0;
        uart->regs->lcr = LCR_DLAB;
        //  fence o,o ?
        uart->regs->dll = 0x01;
        uart->regs->dlm = 0x00;
        //  fence o,o ?
        uart->regs->lcr = 0; //  DLAB=0

        uart->instno = register_device(UART_NAME, uart_open, uart);
    }
    else
        uart->instno = register_device(UART_NAME, NULL, NULL);
}

// static int uart_open(struct io **ioptr, void *aux)
// Inputs: struct io **ioptr - pointer to store uart io interface
//         void *aux - pointer to uart device structure
// Outputs: int - 0 on success, negative error code on failure
// Description: opens the uart device for use
// Side Effects: initializes buffers, enables interrupts and condition variables
int uart_open(struct io **ioptr, void *aux)
{
    struct uart_device *const uart = aux;

    trace("%s()", __func__);

    if (iorefcnt(&uart->io) != 0)
        return -EBUSY;

    //  Reset receive and transmit buffers

    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    //  Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; //  forces a read because uart->regs is volatile

    //  FIXME your code goes here

    //Enable DR interrupt
    uart->regs->ier |= IER_DRIE;

    //Enable interrupt source
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart);

    //Initialize condition variables
    condition_init(&uart->rxbnotempty, "rxcond");
    condition_init(&uart->txbnotfull, "txcond");

    //enable io
    *ioptr = &uart->io;
    ioaddref(&uart->io);
    return 0;
}

// static void uart_close(struct io *io)
// Inputs: struct io *io - pointer to uart device interface
// Outputs: None
// Description: closes the uart device and disables interrupts
// Side Effects: disables uart interrupts and cleans up
void uart_close(struct io *io)
{
    struct uart_device *const uart =
        (void *)io - offsetof(struct uart_device, io);

    trace("%s()", __func__);
    assert(iorefcnt(io) == 0);

    //  FIXME your code goes here

    //disable interrupts
    uart->regs->ier = 0;
    disable_intr_source(uart->irqno);
}

// static long uart_read(struct io *io, void *buf, long bufsz)
// Inputs: struct io *io - pointer to uart device interface
//         void *buf - buffer to store received data
//         long bufsz - size of the buffer
// Outputs: long - number of bytes read
// Description: reads data from uart receive buffer into buf
// Side Effects: wait on condition variable if receive buffer is empty
long uart_read(struct io *io, void *buf, long bufsz)
{
    //  FIXME your code goes here
    struct uart_device *const uart = (void *)io - offsetof(struct uart_device, io);
    if (bufsz <= 0)
    {
        return 0;
    }

    //disable interrupts and condition wait if rxbuf is empty
    int pie = disable_interrupts();
    while (rbuf_empty(&uart->rxbuf))
    {
        uart->regs->ier |= IER_DRIE;
        condition_wait(&uart->rxbnotempty);
    }
    restore_interrupts(pie);

    //read buffer until bufsz
    int read = 0;
    char *rdbuf = (char *)buf;
    while (read < bufsz)
    {
        rdbuf[read] = rbuf_getc(&uart->rxbuf);
        read++;
    }

    //Re-enable data ready interrupt
    uart->regs->ier |= IER_DRIE;

    return read;
}

// static long uart_write(struct io *io, const void *buf, long len)
// Inputs: struct io *io - pointer to uart device interface
//         const void *buf - buffer containing data to write
//         long len - number of bytes to write
// Outputs: long - number of bytes written
// Description: writes data from buf into uart transmit buffer
// Side Effects: wait on condition variable if transmit buffer is full
long uart_write(struct io *io, const void *buf, long len)
{
    //  FIXME your code goes here
    struct uart_device *const uart = (void *)io - offsetof(struct uart_device, io);
    if (len <= 0)
    {
        return 0;
    }

    //write to buffer
    int write = 0;
    char *wbuf = (char *)buf;
    while (write < len)
    {
        //condition wait if txbuf is full
        int pie = disable_interrupts();
        while (rbuf_full(&uart->txbuf))
        {
            uart->regs->ier |= IER_THREIE;
            condition_wait(&uart->txbnotfull);
        }
        restore_interrupts(pie);

        //put character into buffer
        rbuf_putc(&uart->txbuf, wbuf[write]);
        write++;
        uart->regs->ier |= IER_THREIE;
    }

    return write;
}

// static void uart_isr(int srcno, void *aux)
// Inputs: int srcno - interrupt source number
//         void *aux - pointer to uart device structure
// Outputs: None
// Description: services uart interrupts for data ready and transmit holding register empty events
// Side Effects: updates ring buffers and condition variables
void uart_isr(int srcno, void *aux)
{
    //  FIXME your code goes here
    struct uart_device *uart = (struct uart_device *)aux;
    if (srcno != uart->irqno)
    {
        return;
    }

    //Check for DR interrupt
    if (uart->regs->lsr & LSR_DR)
    {
        //Write to rxbuf
        char received_char = uart->regs->rbr;
        if (!rbuf_full(&uart->rxbuf))
        {
            rbuf_putc(&uart->rxbuf, received_char);
        }
        else
        {
            uart->rxovrcnt++;
        }
    }

    //Check for THR interrupt
    if ((uart->regs->lsr & LSR_THRE) && !rbuf_empty(&uart->txbuf))
    {
        //get from txbuf
        char transmit_char = rbuf_getc(&uart->txbuf);
        uart->regs->thr = transmit_char;
    }

    //Disable interrupts if txbuf empty
    if (rbuf_empty(&uart->txbuf))
    {
        uart->regs->ier &= ~IER_THREIE;
    }

    //Disable interrupts if rxbuf full
    if (rbuf_full(&uart->rxbuf))
    {
        uart->regs->ier &= ~IER_DRIE;
    }

    //Condition broadcast if rxbuf not empty
    if (!rbuf_empty(&uart->rxbuf))
    {
        condition_broadcast(&uart->rxbnotempty);
    }

    //Condition broadcast if txbuf not full
    if (!rbuf_full(&uart->txbuf))
    {
        condition_broadcast(&uart->txbnotfull);
    }
}

void rbuf_init(struct ringbuf *rbuf)
{
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}

int rbuf_empty(const struct ringbuf *rbuf)
{
    return (rbuf->hpos == rbuf->tpos);
}

int rbuf_full(const struct ringbuf *rbuf)
{
    return ((uint16_t)(rbuf->tpos - rbuf->hpos) == UART_RBUFSZ);
}

void rbuf_putc(struct ringbuf *rbuf, char c)
{
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf *rbuf)
{
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

//  The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs *)UART0_MMIO_BASE)

void console_device_init(void)
{
    UART0.ier = 0x00;

    //  Configure UART0. We set the baud rate divisor to 1, the lowest value,
    //  for the fastest baud rate. In a physical system, the actual baud rate
    //  depends on the attached oscillator frequency. In a virtualized system,
    //  it doesn't matter.

    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    //  The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c)
{
    //  Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void)
{
    //  Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;

    return UART0.rbr;
}
