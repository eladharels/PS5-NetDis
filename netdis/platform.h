#pragma once

#include "sockets.h"

// Fills `out` with every TCP and UDP socket on the console and the process
// that has it open. Returns 0 on success.
int net_collect(socklist_t *out);

// Logs the details of how sockets were collected, for the Logs tab.
void net_diagnose(void);
