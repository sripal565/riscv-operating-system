#include "conf.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "process.h"
#include "memory.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "heap.h"
#include "string.h"
#include "timer.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 

#define INIT_NAME "trekfib"
#define NUM_UARTS 3


void main(void) {
    struct io *blkio;
    int result;
    int i;

    
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();
    


    
    for (i = 0; i < NUM_UARTS; i++) // change for number of UARTs
        uart_attach((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO+i);
    
    rtc_attach((void*)RTC_MMIO_BASE); 
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }
    enable_interrupts();

    result = open_device("vioblk", 0, &blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open vioblk\n");
    }

    result = fsmount(blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to mount filesystem\n");
    }


    // insert testcase below
    // struct io* fibio;
    // result = fsopen("forktest", &fibio);
    // if(result < 0){
    //     kprintf("Error wrong");
    //     panic("Failed to open trekfib");
    // }
    // // char *argv[] = {"fork_test", NULL};
    // result = process_exec(fibio, 0, NULL);

    struct io *shellio;
    result = fsopen("shell.elf", &shellio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open shell\n");
    }
    result = process_exec(shellio, 0, NULL);
}






