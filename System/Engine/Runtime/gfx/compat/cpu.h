/* The graphics layer's Cpu is this runtime's SwCpu (sw_rt.h). */
#pragma once
#include "sw_rt.h"

typedef SwCpu Cpu;
typedef void (*GuestFn)(Cpu *);
