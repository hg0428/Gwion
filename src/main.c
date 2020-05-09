#include <signal.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "gwion.h"
#include "arg.h"

static void sig(int unused NUSED) {
#ifdef BUILD_ON_WINDOWS
  exit(EXIT_FAILURE);
#else
  pthread_kill(pthread_self(), SIGTERM);
  pthread_exit(NULL);
#endif
}

#ifdef __AFL_HAVE_MANUAL_CONTROL

#define BUFSIZE 4

static void afl_run(const Gwion gwion) {
  __AFL_INIT();
  char buf[BUFSIZE];
  struct GwText_ text = { .mp=gwion->mp };
  while (__AFL_LOOP(256)) {
    ssize_t sz;
    while((sz = read(0, buf, BUFSIZE-1)) > 0) {
buf[sz] = '\0';
      text_add(&text, buf);
    }
    if(compile_string(gwion, "afl", text.str)) {
      push_global(gwion, "[afl]");
      gwion_run(gwion);
      pop_global(gwion);
   }
    text_reset(&text);
  }
  text_release(&text);
}
#define gwion_run(a) { afl_run(a); return 0; }
#endif

int main(int argc, char** argv) {
  Arg arg = { .arg={.argc=argc, .argv=argv}, .loop=-1 };
  signal(SIGINT, sig);
  signal(SIGTERM, sig);
  struct Gwion_ gwion = {};
  const m_bool ini = gwion_ini(&gwion, &arg);
  arg_release(&arg);
  if(ini > 0)
    gwion_run(&gwion);
  gwion_end(&gwion);
  THREAD_RETURN(EXIT_SUCCESS);
}
