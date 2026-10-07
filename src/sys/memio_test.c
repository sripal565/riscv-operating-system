#include "conf.h"
#include "heap.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_blob_start[];
extern char _kimg_blob_end[];
extern char _kimg_start[];
extern char _kimg_end[]; 



void main(void) {
    struct io *blkio;
    struct io *termio;
    struct io *trekio;
    struct io *mio;
    int result;
    int i;
    int tid;
    int trektid;
    void (*trek_entry)(void);
    void (*exe_entry)(struct io*);
    //char *buf = kcalloc(1, _kimg_blob_end - _kimg_blob_start);

    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, UMEM_START); 
    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    kprintf("Size: %ul \n", _kimg_blob_end - _kimg_blob_start);
    mio = create_memory_io((char*)_kimg_blob_start, (_kimg_blob_end -_kimg_blob_start));
    
    kprintf("Size: %ul \n", _kimg_blob_end - _kimg_blob_start);

    elf_load(mio, &trek_entry);
    trektid = thread_spawn("trek", trek_entry);
    assert (0 < trektid);
    thread_join(trektid);
    // kprintf("Blob Data:\n");
    // for (size_t i = 0; i < 60; i++) {
    //     kprintf("%d\n",i);
    //     kprintf("%02x ", (unsigned char)buf[i]);
    // }
    // kprintf("\n");
    ioclose(mio);




    // TODO:
    // 1. Load the trek file into memory

    // 2. Verify the loading of the file into memory

    // 3. Run trek on a new thread

    // 4. Verify that the thread was able to run properly, if it was have the main thread wait for trek to finish
}

