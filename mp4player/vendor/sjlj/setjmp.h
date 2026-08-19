// setjmp/longjmp shim for wasm32-wasi.
//
// libvpx uses setjmp/longjmp for decoder error recovery. WASI has no real
// setjmp (it needs the WebAssembly exception-handling proposal, which the host
// runtime here — wasmtime 14 — does not implement). We can't perform a true
// non-local jump without it, so we shadow <setjmp.h> with this header (placed
// first on the include path for the vpx sources):
//
//   - setjmp() always returns 0 (the "direct" path), so the protected code runs
//     normally for well-formed input.
//   - longjmp() traps. It is only ever reached from vpx_internal_error() on a
//     corrupt/oversized frame — input our own encode/mux pipeline never emits.
//     A trap unwinds as a clean wasm trap that the host catches; it does not
//     corrupt the engine.
//
// This keeps the decoder real (the normal decode path is unchanged); only the
// rare hard-error recovery path degrades from "recover and report" to "abort
// this decode call".
#ifndef PLAYER_SJLJ_SHIM_SETJMP_H
#define PLAYER_SJLJ_SHIM_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

// Opaque, generously sized so it can stand in anywhere a jmp_buf is stored.
typedef struct { void *__opaque[8]; } jmp_buf[1];

int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);

#ifdef __cplusplus
}
#endif

#endif
