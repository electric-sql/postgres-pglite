#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Simple mobile CMA-like buffer semantics used by interactive_one.c
int get_buffer_size(int fd);       // capacity for channel fd
intptr_t get_buffer_addr(int fd);  // native pointer to buffer base + 1 for IO[]
void interactive_write(int size);  // set cma_rsize=size, reset cma_wsize=0
int interactive_read(void);        // return cma_wsize
void use_wire(int state);          // >0 wire mode, <=0 repl

// Expose variables similar to wasm build  
extern volatile int pgl_mobile_cma_wsize;  // External variable defined in pqcomm.c
extern volatile int cma_rsize;
extern volatile int channel;
extern volatile bool is_wire;
extern volatile bool is_repl;

#ifdef __cplusplus
}
#endif

