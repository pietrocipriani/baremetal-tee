#include "internal_client_api.h"
#include "it.h"
#include "scheduler.h"


static void init_context();
static void init_context_ret(const Context *ctx, Context *prev);

static void finalize_context();
static void finalize_context_ret(const Context *ctx, Context *prev);

static void open_session();
static void open_session_ret(const Context *ctx, Context *prev);

static void finalize_session();
static void finalize_session_ret(const Context *ctx, Context *prev);

static void invoke_command();
static void invoke_command_ret(const Context *ctx, Context *prev);


/**
 * @brief Function to call a Trusted Application (TA) Client API from the Client Application (CA)
 * This function is called by the SVC Handler when the SVC number corresponds to a TA Client API
 * and it is responsible to manage the switch of context from the non secure world to the secure world,
 * including the MPU reconfiguration and the stack pointer change
 * 
 * @param auto_frame: pointer to the auto frame (stack frame) 
 * @param manual_frame: pointer to the manual frame (stack frame)
 */
// TODO: the documentation doesn't consider safe the use of naked with standard C, only with basic __asm__.
// TODO: rewrite for readability and maintenibility.
void internal_client_api(Context* ctx, uint8_t svc_num) {
    // TODO: maybe TEE_FAILED as default?
	uintptr_t ret_val = TEE_FAILED; // Declare a variable to store the return value
	int ta_num = 1; // Declare a variable to store the TA number (default value is 1, but can also be 2)

    if (svc_num > 4) {
        ret_val = TEE_FAILED;
        goto exit;
    }

	// Declare a variable to store the internal operation (used to pass the parameters from the client to the core)
	internal_operation_t *internal_op = {0};

	// The internal operation is passed only in the case of the following functions:
	// FUNCTION_INIT_CONTEXT, FUNCTION_OPEN_SESSION, FUNCTION_INVOKE_COMMAND
	// In the other cases (FUNCTION_FINALIZE_CONTEXT, FUNCTION_CLOSE_SESSION), the TA number is passed directly
	if(svc_num == FUNCTION_INIT_CONTEXT || svc_num == FUNCTION_OPEN_SESSION || svc_num == FUNCTION_INVOKE_COMMAND) {

		// Extract the internal operation from r4 (first element of the manual frame)
		// which contains all the information passed by the TA (UUID, parameters, etc.)
        // TODO: aka the context?
		internal_op = (internal_operation_t*)(ctx->manual_frame.r4);


		//Check validity of the manual frame pointer
		if(internal_op == NULL || internal_op < CA_MEMORY_START_ADDR || internal_op + sizeof(internal_operation_t) > CA_MEMORY_END_ADDR) {
			ret_val = TEE_FAILED;
			goto exit;
		}

		// Extract the TA number from the internal operation
		ta_num = internal_op->id;
	} else {
		// Extract the TA number from the manual frame (passed in r4)
		ta_num = (int) ctx->manual_frame.r4;
	}

	//Define which parameters which should use.
	TEE_Param * ta_params_ptr = NULL;
	if(ta_num == 1) {
		// If the TA number is 1, use the ta_params_1 structure
		ta_params_ptr = ta_params_1;
	} else if (ta_num == 2) {
		// If the TA number is 2, use the ta_params_2 structure
		ta_params_ptr = ta_params_2;
	} else {
		// If the TA number is not valid, return with an error
		ret_val = TEE_FAILED;
		goto exit;
	}


	// Extract the command ID from the manual frame (passed in r5)
	// This is the command_id is used only when the SVC number is equal to FUNCTION_INVOKE_COMMAND (4)
	int command_id = ctx->manual_frame.r5;

	// Check if the SVC number is equal to FUNCTION_OPEN_SESSION (2) or FUNCTION_INVOKE_COMMAND (4)
	// In these cases, the parameters are passed by TA inside the memory area used by the internal operation
	// They need to be extracted and stored in the format used by the TA (TA_Param)
	if((svc_num == FUNCTION_OPEN_SESSION)  || (svc_num == FUNCTION_INVOKE_COMMAND)) {

		//We still have the privileges so we must ensure that the memory we are accessing and copying around
		// is valid and inside the CA memory area and the TA memory area. We disable interrupts to avoid TOCTOU
        // TODO: magic numbers?
		for(int i=0; i<4; i++) {
            // internal_op parameter carry the payload as a void pointer so we need to type cast it to TEEC_Parameter
            // TODO: why they cannot be typed? Why the cast?
			TEEC_Parameter *ca_params = (TEEC_Parameter*)internal_op->params[i];

			int type = TEE_PARAM_GET_TYPES(internal_op->paramTypes, i);

			if(ca_params) {
				//Check if both structures are inside the CA memory area
				if(ca_params < CA_MEMORY_START_ADDR || ca_params + sizeof(TEEC_Parameter) > CA_MEMORY_END_ADDR) {
					ret_val = TEE_FAILED;
					goto exit;
				}

                // TODO: function
                // TODO: check the parent is in readable memory to avoid faults.
                // TODO: just to understand... the idea is that one between memref and tmpref must be passed, right?
                // TODO: what are flags and offset used for?
                // TODO: find out a newer GP specification... the 2010 one defines TEEC_Parameter as a union.

				// Copy the memory reference params from the TEEC_Param structure to TA_Param structure
				if(ca_params->memref.parent != NULL) {
					//Check if the various memory pointers are inside the CA memory area
					if(ca_params->memref.parent->buffer < CA_MEMORY_START_ADDR ||
					   ca_params->memref.parent->size > (CA_MEMORY_END_ADDR - (uintptr_t) ca_params->memref.parent->buffer)) {
						ret_val = TEE_FAILED;
						goto exit;
					}

					ta_params_ptr[i].memref.buffer = ca_params->memref.parent->buffer;
					ta_params_ptr[i].memref.size = ca_params->memref.parent->size;
				}

				// Copy the temporary memory reference params from the TEEC_Param structure to TA_Param structure
				if(ca_params->tmpref.buffer != NULL) {
					// Check if the temporary memory reference is valid
					if(ca_params->tmpref.buffer < CA_MEMORY_START_ADDR ||
					   ca_params->tmpref.size > (CA_MEMORY_END_ADDR - (uintptr_t) ca_params->tmpref.buffer)) {
						ret_val = TEE_FAILED;
						goto exit;
					}
					ta_params_ptr[i].memref.buffer = ca_params->tmpref.buffer;
					ta_params_ptr[i].memref.size = ca_params->tmpref.size;
				}

				//Check if the value is valid

				// Copy integer params from the TEEC_Param structure to TA_Param structure only if they are input or in-out params
				if(type == TEE_PARAM_TYPE_VALUE_INPUT || type ==  TEE_PARAM_TYPE_VALUE_INOUT) {
					ta_params_ptr[i].value.a = ca_params->value.a;
					ta_params_ptr[i].value.b = ca_params->value.b;
				}
			}
		}
	}

	// Call the TA function (API offered by the TA accordingly the GlobalPlatform TEE specification) based on the SVC number and the TA number
	// You can see that the name of the function is composed by:
	// - the prefix TA_
	// - and the name of the function in the TEE Client API
	// - 1 or 2
	// The last part refers to the TA number.
	// As it is not possible to have the same function with the same name at the same time for both TAs (linker error),
	// a simple solution was to append the TA number to the function name while still allowing the TAs to use
	// the function without the suffix. How is that possible? The makefile is configured to replace the functions name
	// in the TA source code: for the TA1, the function TA_CreateEntryPoint is replaced with TA_CreateEntryPoint1,
	// for the TA2, the function TA_CreateEntryPoint is replaced with TA_CreateEntryPoint2. This applies to all 5 the GP Client
	// API functions
    // TODO: readability: functions.
    // TODO: check if the TEE must enforce the types of TA (single or multiple) and if it must enforce the order of operation,
    // or if it is a responsability of the TA itself.
    // TODO: the post-response logic should be managed by the TEE, not at TA level as the TA should not have access to the CA buffer.
	switch(svc_num)
	{
		case FUNCTION_INIT_CONTEXT:
            init_context();
			break;
		case FUNCTION_FINALIZE_CONTEXT:
            finalize_context();
			break;

		case FUNCTION_OPEN_SESSION:
            open_session();
			break;

		case FUNCTION_CLOSE_SESSION:
            finalize_session();
			break;

		case FUNCTION_INVOKE_COMMAND:
			break;

		default:
			// None of the Client API functions was called, return with an error
			ret_val = TEE_FAILED;
			goto exit;
	}
}


static void init_context(Context *ctx) {
    // Call TA_CreateEntryPoint, corresponding to the TEEC_InitializeContext() function of the TEE Client API
    // TODO: This doesn't correspond with TEEC_InitializeContext. It can happen with the Session creation.
    
    ComponentID component = COMPONENT_TA?;
    void (*f)() fun = TA_CreateEntryPoint?;

    scheduler_schedule(ctx, component, fun, init_context_ret);
}
static void init_context_ret(const Context *ctx, Context *prev) {
    prev->manual_frame.r4 = ctx->manual_frame.r4;
}

static void finalize_context(Context *ctx) {
    ComponentID component = COMPONENT_TA?;
    void (*f)() fun = TA_DestroyEntryPoint?;

    scheduler_schedule(ctx, component, fun, finalize_context_ret);
}
static void finalize_context_ret(const Context *ctx, Context *prev) {
    prev->manual_frame.r4 = TEE_SUCCESS;
}

static void open_session(Context *ctx) {
    ComponentID component = COMPONENT_TA?;
    void (*f)() fun = TA_OpenSessionEntryPoint?;

    scheduler_schedule(ctx, component, fun, open_session_ret);

    // Call TEE_openTASession, corresponding to TEEC_OpenSession() function
	// TODO: ask if it was an implementation choice to not pass the session
    ctx->auto_frame->r0 = internal_op->paramTypes;  // paramTypes
    ctx->auto_frame->r1 = ta_params_ptr;            // params
    ctx->auto_frame->r2 = NULL;                     // sessionContext
}
static void open_session_ret(const Context *ctx, Context *prev) {
    // TODO: parameters are inout.
	// No out params are used inside this fuction, so there is no need to trasfer back the modifications
	// to the TEEC_Param structure. Just need to reset the TA_Params structure
	for(int i=0; i<4; i++) {
		ta_params_ptr[i].memref.buffer = NULL;
		ta_params_ptr[i].memref.size = 0;
		ta_params_ptr[i].value.a = 0;
		ta_params_ptr[i].value.b = 0;
	}

    // TODO: Populate the session pointer.
    prev->manual_frame.r4 = TEE_SUCCESS;
}

static void finalize_session(Context *ctx) {
    ComponentID component = COMPONENT_TA?;
    void (*f)() fun = TA_CloseSessionEntryPoint?;

    scheduler_schedule(ctx, component, fun, open_session_ret);
    ctx->auto_frame->r0 = NULL;  // sessionContext

    // Call TA_closeSessionEntryPoint, corresponding to TEEC_CloseSession() function
	// TODO: ask if it was an implementation choice to not pass the session
    (ta_num == 1) ? TA_CloseSessionEntryPoint1(NULL) : TA_CloseSessionEntryPoint2(NULL);
			// The function does not have any return value, so we set the return value to TEE_SUCCESS
			ret_val = TEE_SUCCESS;

}
static void finalize_session_ret(const Context *ctx, Context *prev) {
    prev->manual_frame.r4 = TEE_SUCCESS;
}

static void invoke_command() {
                // Call TA_invokeCommandEntryPoint, correspond to TEEC_InvokeCommand() function
			// TODO: ask if it was an implementation choice to not pass the session
			ctx->auto_frame->PC = (ta_num == 1) ? TA_InvokeCommandEntryPoint1 : TA_InvokeCommandEntryPoint2;
            ctx->auto_frame->r0 = internal_op->paramTypes;
            ctx->auto_frame->r1 = ta_params_ptr;
            ctx->auto_frame->r2 = NULL;

			if (ta_num == 1)
				ret_val = TA_InvokeCommandEntryPoint1(NULL, command_id, internal_op->paramTypes, ta_params_ptr);
			else
				ret_val = TA_InvokeCommandEntryPoint2(NULL, command_id, internal_op->paramTypes, ta_params_ptr);

			// Copy back the parameters from the TA_Param structure to the TEEC_Param structure
			// This is done only for the output and in-out parameters

			//Disable interrupts since we enter a critical section
            // TODO: Can they be disabled in unprivileged mode?
			__disable_irq();
			for(int i=0; i<4; i++) {
				TEEC_Parameter *ca_params = (TEEC_Parameter*)internal_op->params[i];
				int type = TEE_PARAM_GET_TYPES(internal_op->paramTypes, i);

				if(ca_params) {
					if(ca_params < CA_MEMORY_START_ADDR || ca_params + sizeof(TEEC_Parameter) > CA_MEMORY_END_ADDR) {
						ret_val = TEE_FAILED;
						goto exit;
					}

					// Copy the integer params from the TA_Param structure to TEEC_Param structure
					if(type == TEE_PARAM_TYPE_VALUE_OUTPUT || type ==  TEE_PARAM_TYPE_VALUE_INOUT) {
						ca_params->value.a = ta_params_ptr[i].value.a;
						ca_params->value.b = ta_params_ptr[i].value.b;
					}

					// Copy only the size of the memory reference params from the TA_Param structure to TEEC_Param structure
					// The buffer is already allocated in the client application memory and the TA should not modify it
                    // TODO: in case this logic is moved to the TEE, the buffer returned by the TA must be checked to avoid TEE memory leaks.
					if(ta_params_ptr[i].memref.buffer != NULL && (type == TEE_PARAM_TYPE_MEMREF_OUTPUT || type == TEE_PARAM_TYPE_MEMREF_INOUT)) {

						if (ca_params->memref.parent != NULL) {
							// Check if the memory reference is valid
							if(ca_params->memref.parent < CA_MEMORY_START_ADDR ||
								ca_params->memref.parent + sizeof(TEEC_SharedMemory) > CA_MEMORY_END_ADDR) {
								ret_val = TEE_FAILED;
								goto exit;
							}
                            // TODO: I don't think this is consistent with the GP specification.
                            // The documentation for this project states that this is a partial TEE implementation.
                            // Understand if this is acceptable for the project scope.
							ca_params->memref.parent->size = ta_params_ptr[i].memref.size;
							ca_params->memref.size = ta_params_ptr[i].memref.size;
						} else {
							ca_params->tmpref.size = ta_params_ptr[i].memref.size;
						}
					}
				}
			}

			// Reset the TA_Params structure
			for(int i=0; i<4; i++) {
				ta_params_ptr[i].memref.buffer = NULL;
				ta_params_ptr[i].memref.size = 0;
				ta_params_ptr[i].value.a = 0;
				ta_params_ptr[i].value.b = 0;
			}

}
static void invoke_command_ret(const Context *ctx, Context *prev);

