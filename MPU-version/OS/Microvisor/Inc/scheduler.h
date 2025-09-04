#ifndef MICROVISOR_SCHEDULER_H_INCLUDED
#define MICROVISOR_SCHEDULER_H_INCLUDED

#include <stdint.h>

#define SCHEDULER_OK    0xDEADBEEF  // Success return status
#define SCHEDULER_ERROR 0xABADBABE  // Error return status

/**
 *  Stores the Context to perform context switch.
 *  @warning FP extension is not supported.
 */
typedef struct Context {
    union {
        struct Regs {
            intptr_t r0, r1, r2, r3, r12, LR, PC, xPSR;
        } __attribute__((packed)); // Packed since struct padding in the C standard is implementation defined.
        struct {
            intptr_t words[sizeof(Regs) / sizeof(intptr_t)];
        } __attribute__((packed)); // Packed since struct padding in the C standard is implementation defined.
    } *auto_frame;
    union {
        struct Regs {
            intptr_t r4, r5, r6, r7, r8, r9, r10, r11, EXC_RETURN;
        } __attribute__((packed)); // Packed since struct padding in the C standard is implementation defined.
        struct {
            intptr_t words[sizeof(Regs) / sizeof(intptr_t)];
        } __attribute__((packed)); // Packed since struct padding in the C standard is implementation defined.
    } manual_frame;
    int saved_specials;
} Context;


#define SCHEDULER_HANDLE(func)\
    __asm__ (\
        "ldr r0, =" #func "\n\r"\
        "bx scheduler_handle_context\n\r"\
    )


/**
 *  Method to prepare context switches. Separates the concern of a return without leaks from the caller.
 *  This costs in a (potentially) slightly greater overhead.
 *  @warning This method is assumed to be called immediatly after an exception entry. LR and SP must remain untouched.
 *
 *  @param func_addr The sub routine that should be called.
 *  @return This function is supposed to perform the exception return.
 */
void scheduler_handle_context(void *func_addr);

/**
 *  @brief  Registers the context of the pre-empted process that must be resumed lately.
 *          The context is pushed in an internal stack.
 *
 *  @param ctx The address of the context allocated during exception entry.
 *
 *  @return SCHEDULER_ERROR if there is no more space in the stack, SCHEDULER_OK otherwise.
 */
int scheduler_call(Context *ctx);

/**
 *  @brief  Resumes the last pushed context.
 *          The context is popped from the stack.
 *
 *  @param ctx The address of the context allocated during exception entry.
 *
 *  @return SCHEDULER_ERROR if no context was saved in the stack, SCHEDULER_OK otherwise.
 */
int scheduler_return(Context *ctx);




#endif /* MICROVISOR_SCHEDULER_H_INCLUDED */
