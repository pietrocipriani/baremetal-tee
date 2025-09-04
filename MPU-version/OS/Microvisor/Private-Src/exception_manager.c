#include "exception_manager.h"
#include "stm32l4xx_hal.h"
#include "core_cm4.h"
#include "it.h"
#include "scheduler.h"

#define PRIGROUP_MASK	0b11100000000
#define PRIGROUP_SHIFT	8

/**
 * Given an exception number, returns the execution priority (group priority)
 * of the specified exception.
 * 
 * Parameters:
 * exception_number: number of the exception (1 = Reset)
 * 
 * Returns:
 * execution priority of the specified exception
 */
static int get_exception_priority(int exception_number) {
	uint8_t pri_bits;
	/* Fixed priorities handlers */
	switch (exception_number)
	{
		case 1:	// Reset
			return -3;
		case 2:	// NMI
			return -2;
		case 3:	// HardFault
			return -1;
	}
	if (4 <= exception_number && exception_number <= 15) {
		/* System handlers */
		pri_bits = SCB->SHP[exception_number];
	} else {
		/* External interrupts */
		/* The mapping between the extern interrup number and the exception number is shifted by 16 */
		pri_bits = NVIC->IP[exception_number - 16];	// external interrupt 0 = exception 16
	}
	int pri_group = SCB->AIRCR & PRIGROUP_MASK >> PRIGROUP_SHIFT;
	pri_bits = pri_bits >> (pri_group + 1);	// isolate group priority
	
	return pri_bits;
}


static void set_exc_priority(int exc_priority) {
    if (exc_priority < 0) {
        soft_reset();
    } else if (exc_priority == 0) {
        // TODO: use PRIMASK.
    } else {
        // TODO: use BASEPRI = exc_priority.
    }
}

static void (*)() get_original_handler(int exc_number) {

    if (exc_number < 1 || exc_number > MAX_EXC_NUMBER) {
        soft_reset();
    }

    void (*isr_table)[]() = __flash_start__;
    return isr_table[exc_number];

}

/**
 * Default handler used inside Vector Table
 * Catches exception and saves key context information on stack
 * After the execution of this handler the stacks are in the following state
 * (displayed in descending order):
 * 	- pre-entry stack (PSP or MSP)
 * 		...,xPSR,RetAddr,LR,r12,r3,r2,r1,r0
 * 	- MSP additionally contains
 * 		...,EXC_RETURN,BASEPRI,padding,virtual_IPSR,r11,r10,r9,r8,r7,r6,r5,r4
 * The 8-byte alignment of the stack is maintained (if present).
 * 
 * Before ending execution, the functions creates a fake return frame that
 * results in the execution of Exception_Simulator immediately after
 * exception return while passing the parameters required.
 * 
 * Why save BASEPRI and EXC_RETURN?
 * BASEPRI: if the exception pre-empted another deprioritized interrupt execution, than it's necessary
 * to save its priority in order to restore it later (once this new deprioritized interrupt
 * has been handled) otherwise the restored code would execute with the wrong priority!
 * Note that the priority of the pre-empted deprioritized interrupt will be certainly stored in BASEPRI
 * because it must be lower than 0 (since the currently running code cannot have priority
 * higher than 0 and was able to successfully pre-empt the previously executing one)
 * If BASEPRI has value 0, it means that "regular" code running at the lowest possible
 * priority (since BASEPRI set to 0 does not mask priority) was pre-empted.
 * 
 * EXCRETURN: this value must be stored in order to set the correct stack and mode when returning
 * form the Exception_Simulator routine.
 * 
 */
static void _exception_catcher(Context *ctx) {
    // TODO: save BASEPRI.
    // TODO: save virtual_IPSR.
    // TODO: the original implementation cared about padding. Must be managed by the scheduler.

    int exc_number = __IPSR();
    int exc_priority = get_exception_priority(exc_number);

    _virtual_IPSR = exc_number;

    scheduler_schedule(ctx, COMPONENT_CA, get_original_handler(exc_number), Exception_Return_Handler);


    ctx->auto_frame->XPSR = 0x01000000;
    ctx->auto_frame->r0 = svc_number;
    ctx->manual_frame.EXC_RETURN = EXC_RETURN_THREAD_PSP;

    set_exc_priority(exc_priority);

}


__attribute__((naked))
void Exception_Catcher() {
    // Delegate the call to scheduler_handle_context that must call _exception_catcher.
    // Executes in-register to avoid modification to sp. Also only caller-preserved registers are used.
    __asm__ volatile (
        "ldr a0, =_exception_handler\n"
        "bx scheduler_handle_context\n"
    );
}


/**
 * Performs exception return to previosly pre-empted execution
 * setting correctly the priority and the context.
 * Only done when the HardFault originates form a pre-determined instruction range
 * (within the Exception_Simulator function).
 *
 * Parameters:
 * - auto_frame: pointer to frame created automatically during exception entry
 */
void Exception_Return_Handler(Context *ctx) {

    // TODO: clear CFSR.
    // TODO: restore virtual_IPSR from CTX.
    // TODO: restore BASEPRI.
    // TODO: disable PRIMASK.

}
