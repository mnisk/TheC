#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#include "xcc.h"
#include "elfutil.h"
#include "util.h"
#include "x86_64.h"

#define PROG_START   (0x80)

#if defined(__XV6)
// XV6
#include "../kernel/syscall.h"
#include "../kernel/traps.h"

#define START_ADDRESS    0x1000

#define SYSTEMCALL(no)  do { MOV_IM32_EAX(no); INT(T_SYSCALL); } while(0)

#define SYSCALL_EXIT   (SYS_exit)
#define SYSCALL_WRITE  (SYS_write)

#elif defined(__linux__)
// Linux

#include <sys/stat.h>

#define START_ADDRESS    (0x1000000 + PROG_START)

#define SYSTEMCALL(no)  do { MOV_IM32_EAX(no); SYSCALL(); } while(0)

#define SYSCALL_EXIT   (60 /*__NR_exit*/)
#define SYSCALL_WRITE  (1 /*__NR_write*/)

#else

#error Target not supported

#endif

#define LOAD_ADDRESS    START_ADDRESS

////////////////////////////////////////////////

void init_compiler(uintptr_t adr) {
  loc_vector = new_vector();
  struct_map = new_map();
  typedef_map = new_map();
  global = new_map();

  init_gen(adr);
}

void compile(FILE *fp, const char *filename) {
  init_lexer(fp, filename);
  Vector *node_vector = parse_program();

  for (int i = 0, len = node_vector->len; i < len; ++i)
    gen(node_vector->data[i]);
}

int main(int argc, char* argv[]) {
  const char *ofn = "a.out";
  int iarg;

  for (iarg = 1; iarg < argc; ++iarg) {
    if (strcmp(argv[iarg], "-o") == 0 && iarg + 1 < argc)
      ofn = argv[++iarg];
    else
      break;
  }

  init_compiler(LOAD_ADDRESS);

  // Test.
  {
    static Type tyVoid = {.type=TY_VOID};
    static Type tyChar = {.type=TY_CHAR};
    static Type tyInt = {.type=TY_INT};
    static Type tyLong = {.type=TY_LONG};
    static Type tyStr = {.type=TY_PTR, .u={.pa={.ptrof=&tyChar}}};

    static VarInfo vInt = {.name="", .type=&tyInt};
    static VarInfo vLong = {.name="", .type=&tyLong};
    static VarInfo vStr = {.name="", .type=&tyStr};

    Vector *exit_params = new_vector();
    vec_push(exit_params, &vInt);
    static Type tyExit = {.type=TY_FUNC, .u={.func={.ret=&tyVoid}}};
    tyExit.u.func.params = exit_params;
    define_global(&tyExit, "_exit", NULL);

    Vector *write_params = new_vector();
    vec_push(write_params, &vInt);
    vec_push(write_params, &vStr);
    vec_push(write_params, &vLong);
    static Type tyWrite = {.type=TY_FUNC, .u={.func={.ret=&tyInt}}};
    tyWrite.u.func.params = write_params;
    define_global(&tyWrite, "_write", NULL);
  }

  if (argc > iarg) {
    for (int i = iarg; i < argc; ++i) {
      const char *filename = argv[i];
      FILE *fp = fopen(filename, "r");
      if (fp == NULL)
        error("Cannot open file: %s\n", argv[i]);
      compile(fp, filename);
      fclose(fp);
    }
  } else {
    compile(stdin, "*stdin*");
  }

  // Test.
  {
    add_label("_start");
#if defined(__XV6)
    // XCV6 calls entry point according to ABI.
#elif __linux__
    MOV_IND8_RSP_RDI(0);
    LEA_OFS8_RSP_RSI(8);
#endif
    CALL("main");
    MOV_EAX_EDI();
    // Fall
    add_label("_exit");
    SYSTEMCALL(SYSCALL_EXIT);
    RET();

    add_label("_write");
    SYSTEMCALL(SYSCALL_WRITE);
    RET();
  }

  size_t memsz;
  size_t filesz = fixup_locations(&memsz);

  uintptr_t entry = label_adr("_start");
  if (entry == (uintptr_t)-1)
    error("Cannot find label: `%s'", "_start");

  FILE* fp = fopen(ofn, "wb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open output file: %s\n", ofn);
    return 1;
  }

  out_elf_header(fp, entry);
  out_program_header(fp, PROG_START, LOAD_ADDRESS, filesz, memsz);
  put_padding(fp, PROG_START);
  output_code(fp);
  fclose(fp);

#if !defined(__XV6) && defined(__linux__)
  if (chmod(ofn, 0755) == -1) {
    perror("chmod failed\n");
    return 1;
  }
#endif

  return 0;
}
