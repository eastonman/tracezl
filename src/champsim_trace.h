#ifndef CHAMPSIM_TRACE_H
#define CHAMPSIM_TRACE_H

#include <cstdint>

#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4

// ChampSim trace instruction format
// Size: 8 + 1 + 1 + 2 + 4 + 16 + 32 = 64 bytes
struct trace_instr_format_t {
    uint64_t ip;           // instruction pointer (program counter) value
    uint8_t is_branch;     // is this branch
    uint8_t branch_taken;  // if so, is this taken
    uint8_t destination_registers[NUM_INSTR_DESTINATIONS];  // output registers
    uint8_t source_registers[NUM_INSTR_SOURCES];            // input registers
    uint64_t destination_memory[NUM_INSTR_DESTINATIONS];    // output memory
    uint64_t source_memory[NUM_INSTR_SOURCES];              // input memory
};

#endif  // CHAMPSIM_TRACE_H
