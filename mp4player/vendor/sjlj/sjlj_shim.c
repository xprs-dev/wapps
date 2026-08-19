// Implementation of the setjmp/longjmp shim — see setjmp.h for rationale.
#include "setjmp.h"

int setjmp(jmp_buf env) {
  (void)env;
  return 0; // always the "saved context" return; protected code runs normally
}

__attribute__((noreturn)) void longjmp(jmp_buf env, int val) {
  (void)env;
  (void)val;
  __builtin_trap(); // only reached on a hard decode error; abort this call
}
