/* xbox-core: xemu's trace points compiled out */
#ifndef XB_XEMU_DSP_TRACE_H
#define XB_XEMU_DSP_TRACE_H
#define trace_dsp_read_peripheral(a, v) ((void)(a), (void)(v))
#define trace_dsp_write_peripheral(a, v) ((void)(a), (void)(v))
#define trace_dsp56k_execute_instruction(id, pc) ((void)(id), (void)(pc))
#define trace_dsp56k_execute_instruction_disasm(s) ((void)(s))
#define trace_event_get_state(x) 0
#define TRACE_DSP56K_EXECUTE_INSTRUCTION_DISASM 0
#endif
