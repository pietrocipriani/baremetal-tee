#include "scheduler.h"
#include <stdint.h>
#include <stddef.h>

#define NUM_INTERRUPTS  84                      // The number of non-system interrupts supported by the platform.
#define CALL_STACK_SIZE (NUM_INTERRUPTS + 1)    // Must accomodate a frame for each interrupt plus a TA call + TEE call.
                                                // The memory can be insufficient if TAs ping pong each other.
                                                // Still not a security problem if implemented correctly.

#define PREDICTED_CONTEXT_SIZE  0x40            // TODO: Searching for a cleaner way of using sizeof in basic __asm__. Static asserted for manteinability.
#define XSTR(x)                 STR(x)          // Expand and stringify the argument (in case of a macro argument).
#define STR(x)                  #x              // Stringify the argument.

/**
 *  A separate call stack where to store (and recover) contexts for context switches.
 */
static struct Record {
    Context ctx;
    void (*ret)(const Context*, Context*);
} call_stack[CALL_STACK_SIZE];

/**
 *  The current size of the stack in number of store contexts. Initially there are not contexts stored.
 */
static size_t curr_size = 0;

/**
 *  The state the MPU is currently in.
 *  The MPU is initially configured for the CA.
 */
static MPUConfig curr_MPU_config = CA;


static void restore_MPU(Context *ctx);
void return_stub(void);
int return_catcher(void);

static int call_stack_push(Context *ctx, void (*ret)(const Context*, Context*));
static int call_stack_pop(Record *ctx);


/**
 *  Method to prepare context switches. Separates the concern of a return without leaks from the caller.
 *  This costs in a (potentially) slightly greater overhead.
 *  @warning This method is assumed to be called immediatly after an exception entry. LR and SP must remain untouched.
 *
 *  @param func_addr The sub routine that should be called.
 *  @return This function is supposed to perform the exception return.
 */
__attribute__((naked))
void scheduler_handle_context(void (*func_addr)(Context*)) {
    static_assert(offsetof(Context, auto_frame) == 0, "auto_frame doesn't have offset 0 in Context");
    static_assert(offsetof(Context, manual_frame) == 4, "manual_frame doesn't have offset 4 in Context");
    static_assert(sizeof(Context) <= PREDICTED_CONTEXT_SIZE, "Context is too big for the allocated frame.");

    // TODO: store everything.

    // Disable interrupts. Automatically re-enabled with the exception return.
    __asm__ ( "cpsie f" );

    // TODO: we can check the stack to avoid stack smashes.

    // Compute the auto frame location (frame allocated on exception entry).
    __asm__ (
        "test LR, #4\n"
        "ite eq\n"
        "mrseq r1, MSP\n"
        "mrsne r1, PSP\n"
    );

    __asm__ (
        "sub sp, sp, #" XSTR(PREDICTED_CONTEXT_SIZE) "\n\r"
        // Push auto_frame (r0) and the content of manual_frame.
        "stmia sp, {r1, r4-r11, LR}\n\t"
        "mov a1, SP\n\t" // Loads the context address as the first argument.

        // Finalize the initialization of the context and call the subroutine.
        "blx scheduler_handle_special_context\n\t"

        // Restore the context. This may have been changed by the sub routine.
        "ldmib sp, {r1, r4-r11, LR}\n\t"
        "add sp, sp, #" XSTR(PREDICTED_CONTEXT_SIZE) "\n\r"
    );

    // Restore PSP if needed. MSP should have been safely preserved by the trusted TEE. Otherwise we have bigger problems.
    __asm__ (
        "test LR, #4\n\t"
        "it ne\n\t"
        "msrne PSP, r1\n\t"
    );

    // Exception return.
    __asm__ ( "bx LR" );
}

void scheduler_handle_special_context(void (*func)(Context*), Context *ctx) {
    ctx->saved_specials = 0;
    ctx->MPU_config = curr_MPU_config;

    func(ctx);

    restore_MPU(ctx);
    restore_specials(ctx);
}

void scheduler_save_specials(Context *ctx, int specials) {
    ctx->saved_specials |= specials;

    if (0 != (specials & SPECIAL_IPSR)) {
        ctx->manual_frame.IPSR = _virtual_IPSR;
    }
}

/**
 *  Registers the context of the pre-empted process that must be resumed lately.
 *  The context is pushed in an internal stack.
 *
 *  @param auto_frame The address of the frame allocated during exception entry.
 *  @param component The component that will be called.
 *  @param address The address of the function that will be called. Should reside in the component accessible memory.
 *  @param ret  The address of the function to call with privileges after the component routine ends.
 *
 *  @return SCHEDULER_ERROR if there is no more space in the stack, SCHEDULER_OK otherwise.
 */
__attribute__((warn_unused_result("Fail of this function can result in wrong behaviour")))
int scheduler_schedule(Context* ctx, ComponentID component, void (*address)(), void (*ret)(const Context*,Context*)) {
    if (call_stack_push(ctx, ret) != SCHEDULER_OK) {
        return SCHEDULER_ERROR;
    }

    switch_context(ctx, component);
    set_exc_return(ctx, component);
    ctx->auto_frame->PC = address;
    ctx->auto_frame->LR = return_stub;

    return SCHEDULER_OK;
}

__attribute__((warn_unused_result("Fail of this function can result in wrong behaviour")))
int return_catcher(Context *ctx) {
    int ret = NOT_MATCHED;

    // The pre-call activation record.
    Record record;

    if (is_from_ret_stub(ctx->auto_frame->PC)) {
        if (SCHEDULER_OK == call_stack_pop(&record)) {
            record->ret(ctx, &record.ctx);
            *ctx = record.ctx;

            ret = MATCHED;
        } else {
            EMSG("Tentative to abuse the return stub.");
            soft_reset();
        }
    } else {
        ret = NOT_MATCHED;
    }

    return ret;
}





static void restore_MPU(Context *ctx) {

    if (curr_MPU_config != ctx->MPU_config) {
        // TODO: why the actual implementation changes most of the sections?
        reconfigure_MPU(ctx->MPU_config);
        curr_MPU_config = ctx->MPU_config;
    }

}

__attribute__((warn_unused_result("Fail of this function can result in wrong behaviour")))
static int call_stack_push(Context *ctx, void (*ret)(const Context*, Context*)) {
    size_t size = curr_size;

    if (size >= CALL_STACK_SIZE) {
        return SCHEDULER_ERROR;
    }

    // Push the context to the stack.
    call_stack[size].ctx = *ctx;
    call_stack[size].ret = ret;
    curr_size = size += 1;

    return SCHEDULER_OK;
}


__attribute__((warn_unused_result("Fail of this function can result in wrong behaviour")))
static int call_stack_pop(Record *ctx) {
    size_t size = curr_size;

    if (size == 0) {
        return SCHEDULER_ERROR;
    }

    curr_size = size -= 1;
    *ctx = call_stack[size];

    return SCHEDULER_OK;
}


__attribute__((naked, section("boot-nopriv")))
static void return_stub() {
    // TODO: using a EXC_RETURN as return value in Thread mode actually uses the value as an actual address
    //       to reserved memory. This triggers a MemManage exception or (UsageFault) that can be escalated to HardFault.
    //       I think this solution is cleaner as it would allow SVC calls to not need the additional argument.
	__asm__(
		/* Trigger HardFault to perform return sequence */
		/* Since we are executing unprivileged code, an access to the PPB will trigger a HardFault */
		".global RET_START\n" // define a globally visible label to be used in the exception return handler
		".global RET_END\n"  // define a globally visible label to be used in the exception return handler
		"RET_START:\n"
		"ldr r0, =0xe000e000\n"	// load SCB address
		"ldrt r0, [r0]\n"	// perfom unprivileged read
		".LOOP:\n"	// wait for HardFault
		"b .LOOP\n"
		"RET_END:\n"
	);
}
