#include "genexe.h"
#include "gencall.h"
#include "genfun.h"
#include "genname.h"
#include "genobj.h"
#include "genopt.h"
#include "genprim.h"
#include "../reach/paint.h"
#include "../pkg/package.h"
#include "../pkg/program.h"
#include "../plugin/plugin.h"
#include "../type/assemble.h"
#include "../type/lookup.h"
#include "../../libponyrt/mem/pool.h"
#include "ponyassert.h"
#include <string.h>

#ifdef PLATFORM_IS_POSIX_BASED
#  include <unistd.h>
#endif

static LLVMValueRef create_main(compile_t* c, reach_type_t* t,
  LLVMValueRef ctx)
{
  // Create the main actor and become it.
  LLVMValueRef args[2];
  args[0] = ctx;
  args[1] = LLVMConstBitCast(((compile_type_t*)t->c_type)->desc,
    c->descriptor_ptr);
  LLVMValueRef actor = gencall_runtime(c, "pony_create", args, 2, "");

  args[0] = ctx;
  args[1] = actor;
  gencall_runtime(c, "pony_become", args, 2, "");

  return actor;
}

static LLVMValueRef make_lang_features_init(compile_t* c)
{
  char* triple = c->opt->triple;

  LLVMTypeRef boolean;

  if(target_is_ppc(triple) && target_is_ilp32(triple) &&
    target_is_macosx(triple))
    boolean = c->i32;
  else
    boolean = c->i8;

  LLVMTypeRef desc_ptr_ptr = LLVMPointerType(c->descriptor_ptr, 0);

  uint32_t desc_table_size = reach_max_type_id(c->reach);

  LLVMTypeRef f_params[4];
  f_params[0] = boolean;
  f_params[1] = boolean;
  f_params[2] = desc_ptr_ptr;
  f_params[3] = c->intptr;

  LLVMTypeRef lfi_type = LLVMStructTypeInContext(c->context, f_params, 4,
    false);

  LLVMBasicBlockRef this_block = LLVMGetInsertBlock(c->builder);
  LLVMBasicBlockRef entry_block = LLVMGetEntryBasicBlock(codegen_fun(c));
  LLVMValueRef inst = LLVMGetFirstInstruction(entry_block);

  if(inst != NULL)
    LLVMPositionBuilderBefore(c->builder, inst);
  else
    LLVMPositionBuilderAtEnd(c->builder, entry_block);

  LLVMValueRef lfi_object = LLVMBuildAlloca(c->builder, lfi_type, "");

  LLVMPositionBuilderAtEnd(c->builder, this_block);

  LLVMValueRef field = LLVMBuildStructGEP(c->builder, lfi_object, 0, "");
  LLVMBuildStore(c->builder, LLVMConstInt(boolean, 1, false), field);

  field = LLVMBuildStructGEP(c->builder, lfi_object, 1, "");
  LLVMBuildStore(c->builder, LLVMConstInt(boolean, 1, false), field);

  field = LLVMBuildStructGEP(c->builder, lfi_object, 2, "");
  LLVMBuildStore(c->builder, LLVMBuildBitCast(c->builder, c->desc_table,
    desc_ptr_ptr, ""), field);

  field = LLVMBuildStructGEP(c->builder, lfi_object, 3, "");
  LLVMBuildStore(c->builder, LLVMConstInt(c->intptr, desc_table_size, false),
    field);

  return LLVMBuildBitCast(c->builder, lfi_object, c->void_ptr, "");
}

LLVMValueRef gen_main(compile_t* c, reach_type_t* t_main, reach_type_t* t_env)
{
  LLVMTypeRef params[3];
  params[0] = c->i32;
  params[1] = LLVMPointerType(LLVMPointerType(c->i8, 0), 0);
  params[2] = LLVMPointerType(LLVMPointerType(c->i8, 0), 0);

  LLVMTypeRef ftype = LLVMFunctionType(c->i32, params, 3, false);
  LLVMValueRef func = LLVMAddFunction(c->module, "main", ftype);

  codegen_startfun(c, func, NULL, NULL, NULL, false);

  LLVMBasicBlockRef start_fail_block = codegen_block(c, "start_fail");
  LLVMBasicBlockRef post_block = codegen_block(c, "post");

  LLVMValueRef args[5];
  args[0] = LLVMGetParam(func, 0);
  LLVMSetValueName(args[0], "argc");

  args[1] = LLVMGetParam(func, 1);
  LLVMSetValueName(args[1], "argv");

  args[2] = LLVMGetParam(func, 2);
  LLVMSetValueName(args[2], "envp");

  // Initialise the pony runtime with argc and argv, getting a new argc.
  args[0] = gencall_runtime(c, "pony_init", args, 2, "argc");

  // Create the main actor and become it.
  LLVMValueRef ctx = gencall_runtime(c, "pony_ctx", NULL, 0, "");
  codegen_setctx(c, ctx);
  LLVMValueRef main_actor = create_main(c, t_main, ctx);

  // Create an Env on the main actor's heap.
  reach_method_t* m = reach_method(t_env, TK_NONE, c->str__create, NULL);

  LLVMValueRef env_args[4];
  env_args[0] = gencall_alloc(c, t_env);
  env_args[1] = args[0];
  env_args[2] = LLVMBuildBitCast(c->builder, args[1], c->void_ptr, "");
  env_args[3] = LLVMBuildBitCast(c->builder, args[2], c->void_ptr, "");
  codegen_call(c, ((compile_method_t*)m->c_method)->func, env_args, 4, true);
  LLVMValueRef env = env_args[0];

  // Run primitive initialisers using the main actor's heap.
  if(c->primitives_init != NULL)
    LLVMBuildCall(c->builder, c->primitives_init, NULL, 0, "");

  // Create a type for the message.
  LLVMTypeRef f_params[4];
  f_params[0] = c->i32;
  f_params[1] = c->i32;
  f_params[2] = c->void_ptr;
  f_params[3] = LLVMTypeOf(env);
  LLVMTypeRef msg_type = LLVMStructTypeInContext(c->context, f_params, 4,
    false);
  LLVMTypeRef msg_type_ptr = LLVMPointerType(msg_type, 0);

  // Allocate the message, setting its size and ID.
  uint32_t index = reach_vtable_index(t_main, c->str_create);
  size_t msg_size = (size_t)LLVMABISizeOfType(c->target_data, msg_type);
  args[0] = LLVMConstInt(c->i32, ponyint_pool_index(msg_size), false);
  args[1] = LLVMConstInt(c->i32, index, false);
  LLVMValueRef msg = gencall_runtime(c, "pony_alloc_msg", args, 2, "");
  LLVMValueRef msg_ptr = LLVMBuildBitCast(c->builder, msg, msg_type_ptr, "");

  // Set the message contents.
  LLVMValueRef env_ptr = LLVMBuildStructGEP(c->builder, msg_ptr, 3, "");
  LLVMBuildStore(c->builder, env, env_ptr);

  // Trace the message.
  args[0] = ctx;
  gencall_runtime(c, "pony_gc_send", args, 1, "");

  args[0] = ctx;
  args[1] = LLVMBuildBitCast(c->builder, env, c->object_ptr, "");
  args[2] = LLVMBuildBitCast(c->builder, ((compile_type_t*)t_env->c_type)->desc,
    c->descriptor_ptr, "");
  args[3] = LLVMConstInt(c->i32, PONY_TRACE_IMMUTABLE, false);
  gencall_runtime(c, "pony_traceknown", args, 4, "");

  args[0] = ctx;
  gencall_runtime(c, "pony_send_done", args, 1, "");

  // Send the message.
  args[0] = ctx;
  args[1] = main_actor;
  args[2] = msg;
  args[3] = msg;
  args[4] = LLVMConstInt(c->i1, 1, false);
  gencall_runtime(c, "pony_sendv_single", args, 5, "");

  // Start the runtime.
  args[0] = LLVMConstInt(c->i1, 0, false);
  args[1] = LLVMConstNull(LLVMPointerType(c->i32, 0));
  args[2] = make_lang_features_init(c);
  LLVMValueRef start_success = gencall_runtime(c, "pony_start", args, 3, "");

  LLVMBuildCondBr(c->builder, start_success, post_block, start_fail_block);

  LLVMPositionBuilderAtEnd(c->builder, start_fail_block);

  const char error_msg_str[] = "Error: couldn't start runtime!";

  args[0] = codegen_string(c, error_msg_str, sizeof(error_msg_str) - 1);
  gencall_runtime(c, "puts", args, 1, "");
  LLVMBuildBr(c->builder, post_block);

  LLVMPositionBuilderAtEnd(c->builder, post_block);

  // Run primitive finalisers. We create a new main actor as a context to run
  // the finalisers in, but we do not initialise or schedule it.
  if(c->primitives_final != NULL)
  {
    LLVMValueRef final_actor = create_main(c, t_main, ctx);
    LLVMBuildCall(c->builder, c->primitives_final, NULL, 0, "");
    args[0] = final_actor;
    gencall_runtime(c, "ponyint_destroy", args, 1, "");
  }

  args[0] = ctx;
  args[1] = LLVMConstNull(c->object_ptr);
  gencall_runtime(c, "pony_become", args, 2, "");

  LLVMValueRef rc = gencall_runtime(c, "pony_get_exitcode", NULL, 0, "");
  LLVMValueRef minus_one = LLVMConstInt(c->i32, (unsigned long long)-1, true);
  rc = LLVMBuildSelect(c->builder, start_success, rc, minus_one, "");

  // Return the runtime exit code.
  LLVMBuildRet(c->builder, rc);

  codegen_finishfun(c);

  // External linkage for main().
  LLVMSetLinkage(func, LLVMExternalLinkage);

  return func;
}

#if defined(PLATFORM_IS_LINUX) || defined(PLATFORM_IS_BSD)
static const char* env_cc_or_pony_compiler(bool* out_fallback_linker)
{
  const char* cc = getenv("CC");
  if(cc == NULL)
  {
    *out_fallback_linker = true;
    return PONY_COMPILER;
  }
  return cc;
}
#endif

static bool link_exe(compile_t* c, ast_t* program,
  const char* file_o)
{
  errors_t* errors = c->opt->check.errors;

  const char* ponyrt = c->opt->runtimebc ? "" :
#if defined(PLATFORM_IS_WINDOWS)
    "libponyrt.lib";
#elif defined(PLATFORM_IS_LINUX)
    c->opt->pic ? "-lponyrt-pic" : "-lponyrt";
#else
    "-lponyrt";
#endif

#if defined(PLATFORM_IS_MACOSX)
  char* arch = strchr(c->opt->triple, '-');

  if(arch == NULL)
  {
    errorf(errors, NULL, "couldn't determine architecture from %s",
      c->opt->triple);
    return false;
  }

  const char* file_exe =
    suffix_filename(c, c->opt->output, "", c->filename, "");

  if(c->opt->verbosity >= VERBOSITY_MINIMAL)
    fprintf(stderr, "Linking %s\n", file_exe);

  program_lib_build_args(program, c->opt, "-L", NULL, "", "", "-l", "");
  const char* lib_args = program_lib_args(program);

  size_t arch_len = arch - c->opt->triple;
  size_t ld_len = 128 + arch_len + strlen(file_exe) + strlen(file_o) +
    strlen(lib_args);
  const char* linker = "ld";
  if (c->opt->linker) {
    linker = c->opt->linker;
    ld_len += strlen(c->opt->linker);
  }
  char* ld_cmd = (char*)ponyint_pool_alloc_size(ld_len);

  snprintf(ld_cmd, ld_len,
    "%s -execute -no_pie -arch %.*s "
    "-macosx_version_min 10.8 -o %s %s %s %s -lSystem",
           linker, (int)arch_len, c->opt->triple, file_exe, file_o,
           lib_args, ponyrt
    );

  if(c->opt->verbosity >= VERBOSITY_TOOL_INFO)
    fprintf(stderr, "%s\n", ld_cmd);

  if(system(ld_cmd) != 0)
  {
    errorf(errors, NULL, "unable to link: %s", ld_cmd);
    ponyint_pool_free_size(ld_len, ld_cmd);
    return false;
  }

  ponyint_pool_free_size(ld_len, ld_cmd);

  if(!c->opt->strip_debug)
  {
    size_t dsym_len = 16 + strlen(file_exe);
    char* dsym_cmd = (char*)ponyint_pool_alloc_size(dsym_len);

    snprintf(dsym_cmd, dsym_len, "rm -rf %s.dSYM", file_exe);
    system(dsym_cmd);

    snprintf(dsym_cmd, dsym_len, "dsymutil %s", file_exe);

    if(system(dsym_cmd) != 0)
      errorf(errors, NULL, "unable to create dsym");

    ponyint_pool_free_size(dsym_len, dsym_cmd);
  }

#elif defined(PLATFORM_IS_LINUX) || defined(PLATFORM_IS_BSD)
  const char* file_exe =
    suffix_filename(c, c->opt->output, "", c->filename, "");

  if(c->opt->verbosity >= VERBOSITY_MINIMAL)
    fprintf(stderr, "Linking %s\n", file_exe);

  program_lib_build_args(program, c->opt, "-L", "-Wl,-rpath,",
    "-Wl,--start-group ", "-Wl,--end-group ", "-l", "");
  const char* lib_args = program_lib_args(program);

  const char* arch = c->opt->link_arch != NULL ? c->opt->link_arch : PONY_ARCH;
  bool fallback_linker = false;
  const char* linker = c->opt->linker != NULL ? c->opt->linker :
    env_cc_or_pony_compiler(&fallback_linker);
  const char* mcx16_arg = target_is_ilp32(c->opt->triple) ? "" : "-mcx16";
  const char* fuseld = target_is_linux(c->opt->triple) ? "-fuse-ld=gold" : "";
  const char* ldl = target_is_linux(c->opt->triple) ? "-ldl" : "";
  const char* atomic = target_is_linux(c->opt->triple) ? "-latomic" : "";
  const char* staticbin = c->opt->staticbin ? "-static" : "";
  const char* dtrace_args =
#if defined(PLATFORM_IS_BSD) && defined(USE_DYNAMIC_TRACE)
   "-Wl,--whole-archive -ldtrace_probes -Wl,--no-whole-archive -lelf";
#else
    "";
#endif
  const char* lexecinfo =
#if (defined(PLATFORM_IS_LINUX) && !defined(__GLIBC__))
   "-lexecinfo";
#else
    "";
#endif

  size_t ld_len = 512 + strlen(file_exe) + strlen(file_o) + strlen(lib_args)
                  + strlen(arch) + strlen(mcx16_arg) + strlen(fuseld)
                  + strlen(ldl) + strlen(atomic) + strlen(staticbin)
                  + strlen(dtrace_args) + strlen(lexecinfo);

  char* ld_cmd = (char*)ponyint_pool_alloc_size(ld_len);

  snprintf(ld_cmd, ld_len, "%s -o %s -O3 -march=%s "
    "%s "
#ifdef PONY_USE_LTO
    "-flto -fuse-linker-plugin "
#endif
// The use of NDEBUG instead of PONY_NDEBUG here is intentional.
#ifndef NDEBUG
    // Allows the implementation of `pony_assert` to correctly get symbol names
    // for backtrace reporting.
    "-rdynamic "
#endif
    "%s %s %s %s %s -lpthread %s %s %s -lm %s",
    linker, file_exe, arch, mcx16_arg, atomic, staticbin, fuseld, file_o,
    lib_args, dtrace_args, ponyrt, ldl, lexecinfo
    );

  if(c->opt->verbosity >= VERBOSITY_TOOL_INFO)
    fprintf(stderr, "%s\n", ld_cmd);

  if(system(ld_cmd) != 0)
  {
    if((c->opt->verbosity >= VERBOSITY_MINIMAL) && fallback_linker)
    {
      fprintf(stderr,
        "Warning: environment variable $CC undefined, using %s as the linker\n",
        PONY_COMPILER);
    }

    errorf(errors, NULL, "unable to link: %s", ld_cmd);
    ponyint_pool_free_size(ld_len, ld_cmd);
    return false;
  }

  ponyint_pool_free_size(ld_len, ld_cmd);
#elif defined(PLATFORM_IS_WINDOWS)
  vcvars_t vcvars;

  if(!vcvars_get(c, &vcvars, errors))
  {
    errorf(errors, NULL, "unable to link: no vcvars");
    return false;
  }

  const char* file_exe = suffix_filename(c, c->opt->output, "", c->filename,
    ".exe");
  if(c->opt->verbosity >= VERBOSITY_MINIMAL)
    fprintf(stderr, "Linking %s\n", file_exe);

  program_lib_build_args(program, c->opt,
    "/LIBPATH:", NULL, "", "", "", ".lib");
  const char* lib_args = program_lib_args(program);

  char ucrt_lib[MAX_PATH + 12];
  if (strlen(vcvars.ucrt) > 0)
    snprintf(ucrt_lib, MAX_PATH + 12, "/LIBPATH:\"%s\"", vcvars.ucrt);
  else
    ucrt_lib[0] = '\0';

  size_t ld_len = 256 + strlen(file_exe) + strlen(file_o) +
    strlen(vcvars.kernel32) + strlen(vcvars.msvcrt) + strlen(lib_args);
  char* ld_cmd = (char*)ponyint_pool_alloc_size(ld_len);

  char* linker = vcvars.link;
  if (c->opt->linker != NULL && strlen(c->opt->linker) > 0)
    linker = c->opt->linker;

  while (true)
  {
    size_t num_written = snprintf(ld_cmd, ld_len,
      "cmd /C \"\"%s\" /DEBUG /NOLOGO /MACHINE:X64 /ignore:4099 "
      "/OUT:%s "
      "%s %s "
      "/LIBPATH:\"%s\" "
      "/LIBPATH:\"%s\" "
      "%s %s %s \"",
      linker, file_exe, file_o, ucrt_lib, vcvars.kernel32,
      vcvars.msvcrt, lib_args, vcvars.default_libs, ponyrt
    );

    if (num_written < ld_len)
      break;

    ponyint_pool_free_size(ld_len, ld_cmd);
    ld_len += 256;
    ld_cmd = (char*)ponyint_pool_alloc_size(ld_len);
  }

  if(c->opt->verbosity >= VERBOSITY_TOOL_INFO)
    fprintf(stderr, "%s\n", ld_cmd);

  if (system(ld_cmd) == -1)
  {
    errorf(errors, NULL, "unable to link: %s", ld_cmd);
    ponyint_pool_free_size(ld_len, ld_cmd);
    return false;
  }

  ponyint_pool_free_size(ld_len, ld_cmd);
#endif

  return true;
}

bool genexe(compile_t* c, ast_t* program)
{
  errors_t* errors = c->opt->check.errors;

  // The first package is the main package. It has to have a Main actor.
  const char* main_actor = c->str_Main;
  const char* env_class = c->str_Env;
  const char* package_name = c->filename;

  if((c->opt->bin_name != NULL) && (strlen(c->opt->bin_name) > 0))
    c->filename = c->opt->bin_name;

  ast_t* package = ast_child(program);
  ast_t* main_def = ast_get(package, main_actor, NULL);

  if(main_def == NULL)
  {
    errorf(errors, NULL, "no Main actor found in package '%s'", package_name);
    return false;
  }

  // Generate the Main actor and the Env class.
  ast_t* main_ast = type_builtin(c->opt, main_def, main_actor);
  ast_t* env_ast = type_builtin(c->opt, main_def, env_class);

  deferred_reification_t* main_create = lookup(c->opt, main_ast, main_ast,
    c->str_create);

  if(main_create == NULL)
  {
    ast_free(main_ast);
    ast_free(env_ast);
    return false;
  }

  deferred_reify_free(main_create);

  if(c->opt->verbosity >= VERBOSITY_INFO)
    fprintf(stderr, " Reachability\n");
  reach(c->reach, main_ast, c->str_create, NULL, c->opt);
  reach(c->reach, env_ast, c->str__create, NULL, c->opt);

  if(c->opt->limit == PASS_REACH)
  {
    ast_free(main_ast);
    ast_free(env_ast);
    return true;
  }

  if(c->opt->verbosity >= VERBOSITY_INFO)
    fprintf(stderr, " Selector painting\n");
  paint(&c->reach->types);

  plugin_visit_reach(c->reach, c->opt, true);

  if(c->opt->limit == PASS_PAINT)
  {
    ast_free(main_ast);
    ast_free(env_ast);
    return true;
  }

  if(!gentypes(c))
  {
    ast_free(main_ast);
    ast_free(env_ast);
    return false;
  }

  if(c->opt->verbosity >= VERBOSITY_ALL)
    reach_dump(c->reach);

  reach_type_t* t_main = reach_type(c->reach, main_ast);
  reach_type_t* t_env = reach_type(c->reach, env_ast);

  ast_free(main_ast);
  ast_free(env_ast);

  if((t_main == NULL) || (t_env == NULL))
    return false;

  gen_main(c, t_main, t_env);

  plugin_visit_compile(c, c->opt);

  if(!genopt(c, true))
    return false;

  if(c->opt->runtimebc)
  {
    if(!codegen_merge_runtime_bitcode(c))
      return false;

    // Rerun the optimiser without the Pony-specific optimisation passes.
    // Inlining runtime functions can screw up these passes so we can't
    // run the optimiser only once after merging.
    if(!genopt(c, false))
      return false;
  }

  const char* file_o = genobj(c);

  if(file_o == NULL)
    return false;

  if(c->opt->limit < PASS_ALL)
    return true;

  if(!link_exe(c, program, file_o))
    return false;

#ifdef PLATFORM_IS_WINDOWS
  _unlink(file_o);
#else
  unlink(file_o);
#endif

  return true;
}
