#pragma once

// Installs Ctrl+C / SIGINT handling for graceful process shutdown.
void TqInstallInterruptHandler();

// Blocks until an interrupt has been requested.
void TqWaitForInterrupt();
