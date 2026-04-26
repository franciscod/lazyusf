#include <stdint.h>
#include <limits.h>

#define CPU_Interpreter             1
#define CPU_Recompiler              0

extern char filename[PATH_MAX+1];

void DisplayError (char * Message, ...);
void StopEmulation(void);
