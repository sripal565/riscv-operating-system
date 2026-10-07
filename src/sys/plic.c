//  plic.c - RISC-V PLIC
//
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "assert.h"
#include "error.h"

#include <stdint.h>

//  INTERNAL MACRO DEFINITIONS
//

//  CTX(i,0) is hartid /i/ M-mode context
//  CTX(i,1) is hartid /i/ S-mode context

#define CTX(i, s) (2 * (i) + (s))
#define WORDSZ 32

//  INTERNAL TYPE DEFINITIONS
//  

struct plic_regs
{
	union
	{
		uint32_t priority[PLIC_SRC_CNT];
		char _reserved_priority[0x1000];
	};

	union
	{
		uint32_t pending[PLIC_SRC_CNT / 32];
		char _reserved_pending[0x1000];
	};

	union
	{
		uint32_t enable[PLIC_CTX_CNT][32];
		char _reserved_enable[0x200000 - 0x2000];
	};

	struct
	{
		union
		{
			struct
			{
				uint32_t threshold;
				uint32_t claim;
			};

			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs *)PLIC_MMIO_BASE)

//  INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority(
	uint_fast32_t srcno, uint_fast32_t level);
static int plic_source_pending(uint_fast32_t srcno);
static void plic_enable_source_for_context(
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_disable_source_for_context(
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_set_context_threshold(
	uint_fast32_t ctxno, uint_fast32_t level);
static uint_fast32_t plic_claim_context_interrupt(
	uint_fast32_t ctxno);
static void plic_complete_context_interrupt(
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

//  We currently only support single-hart operation, sending interrupts to S mode
//  on hart 0 (context 0). The low-level PLIC functions already understand
//  contexts, so we only need to modify the high-level functions (plit_init,
//  plic_claim_request, plic_finish_request)to add support for multiple harts.

//  EXPORTED FUNCTION DEFINITIONS
//  

void plic_init(void)
{
	int i;

	//  Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);

	//  Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);

	plic_enable_all_sources_for_context(CTX(0, 1));
}

extern void plic_enable_source(int srcno, int prio)
{
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert(0 < srcno && srcno <= PLIC_SRC_CNT);
	assert(prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno)
{
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void)
{
	//  FIXME: Hardwired S-mode hart 0
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0, 1));
}

extern void plic_finish_interrupt(int irqno)
{
	//  FIXME: Hardwired S-mode hart 0
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0, 1), irqno);
}

//  INTERNAL FUNCTION DEFINITIONS
//

// static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
// Inputs: uint_fast32_t srcno - interrupt source number
//         uint_fast32_t level - priority level
// Outputs: None
// Description: sets priority level in plic
// Side Effects: updates plic priority array
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
{
	//  FIXME your code goes here
	if (srcno >= PLIC_SRC_CNT || srcno <= 0)
	{
		return;
	}
	// Set priority level
	PLIC.priority[srcno] = level;
}

// static inline int plic_source_pending(uint_fast32_t srcno)
// Inputs: uint_fast32_t srcno - interrupt source number
// Outputs: source pending status
// Description: checks if interrupt is pending in plic
// Side Effects: None
static inline int plic_source_pending(uint_fast32_t srcno)
{
	//  FIXME your code goes here
	if (srcno >= PLIC_SRC_CNT || srcno <= 0)
	{
		return 0;
	}

	// find index and bit of array
	int index = srcno / WORDSZ;
	int bit = srcno % WORDSZ;

	// Return source val
	int val = (PLIC.pending[index] >> bit) & 1;
	return val;
}

// static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t srcno - interrupt source number
// Outputs: None
// Description: enables interrupt source for a given context in plic
// Side Effects: updates plic enable array for context
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
{
	//  FIXME your code goes here
	if (srcno >= PLIC_SRC_CNT || srcno <= 0 || ctxno >= PLIC_CTX_CNT)
	{
		return;
	}

	//find index and bit
	int index = srcno / WORDSZ;
	int bit = srcno % WORDSZ;

	//enable bit
	PLIC.enable[ctxno][index] |= (1 << bit);
}

// static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t srcid - interrupt source number
// Outputs: None
// Description: disables interrupt source for a given context in plic
// Side Effects: updates plic enable array for context
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
{
	//  FIXME your code goes here
	if (srcid >= PLIC_SRC_CNT || srcid <= 0 || ctxno >= PLIC_CTX_CNT)
	{
		return;
	}

	//index and bit
	int index = srcid / WORDSZ;
	int bit = srcid % WORDSZ;

	//clear bit
	PLIC.enable[ctxno][index] &= ~(1 << bit);
}

// static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t level - threshold level
// Outputs: None
// Description: sets context threshold in plic
// Side Effects: updates plic context threshold
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
{
	//  FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT)
	{
		return;
	}

	//update threshold
	PLIC.ctx[ctxno].threshold = level;
}

// static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: uint_fast32_t - claimed interrupt number
// Description: claims an interrupt for a given context in plic
// Side Effects: may clear pending interrupt in claim register
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
{
	//  FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT)
	{
		return -1;
	}
	//get claim number
	uint32_t claimno = PLIC.ctx[ctxno].claim;

	return claimno;
}

// static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - context number
//         uint_fast32_t srcno - interrupt source number
// Outputs: None
// Description: completes an interrupt for a given context in plic
// Side Effects: writes back to claim register in plic
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
{
	//  FIXME your code goes here
	if (srcno >= PLIC_SRC_CNT || srcno <= 0 || ctxno >= PLIC_CTX_CNT)
	{
		return;
	}
	//write srcno to claim
	PLIC.ctx[ctxno].claim = srcno;
}

// static void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: None
// Description: enables all interrupt sources for a given context in plic
// Side Effects: sets all bits in the plic enable array for context
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
{
	//  FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT)
	{
		return;
	}

	//all 1 bit mask
	uint32_t enable_all = ~0;

	//for all contexts, enable all
	for (int i = 0; i < WORDSZ; i++)
		PLIC.enable[ctxno][i] = enable_all;
}

// static void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: None
// Description: disables all interrupt sources for a given context in plic
// Side Effects: clears all bits in the plic enable array for context
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
{
	//  FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT)
	{
		return;
	}

	//for all contexts, disable all
	for (int i = 0; i < WORDSZ; i++)
		PLIC.enable[ctxno][i] = 0;
}
