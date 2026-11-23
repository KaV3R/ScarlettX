#pragma once

// Plants diagnostic breakpoints listed in SCARLETTX_BREAKPOINTS (hex
// addresses, comma separated) into the loaded guest image.
void InstallGuestBreakpointsFromEnv();
