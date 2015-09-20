#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

void initialize_swap_slot (void);
size_t swap_frame_out (void *frame);
void swap_frame_in (size_t used_index, void *frame);

#endif /* vm/swap.h */
