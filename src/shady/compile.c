#include "shady/cli.h"
#include "compile.h"

#include "parser/parser.h"
#include "shady_scheduler_src.h"
#include "transform/internal_constants.h"
#include "portability.h"
#include "ir_private.h"
#include "util.h"

#include <stdbool.h>

#ifdef SPV_PARSER_PRESENT
#include "../frontends/spirv/s2s.h"
#endif

#define KiB * 1024
#define MiB * 1024 KiB

CompilerConfig default_compiler_config() {
    return (CompilerConfig) {
        .allow_frontend_syntax = false,
        .dynamic_scheduling = true,
        .per_thread_stack_size = 4 KiB,
        .per_subgroup_stack_size = 1 KiB,

        .target_spirv_version = {
            .major = 1,
            .minor = 4
        },

        .logging = {
            // most of the time, we are not interested in seeing generated & internal code in the debug output
            .skip_internal = true,
            .skip_generated = true,
        },

        .specialization = {
            .subgroup_size = 8,
            .entry_point = NULL
        }
    };
}

ArenaConfig default_arena_config() {
    return (ArenaConfig) {
        .is_simt = true,
        .validate_builtin_types = false,

        .memory = {
            .word_size = IntTy32,
            .ptr_size = IntTy64,
        }
    };
}

#define mod (*pmod)

CompilationResult run_compiler_passes(CompilerConfig* config, Module** pmod) {
    ArenaConfig aconfig = get_module_arena(mod)->config;

    aconfig.specializations.subgroup_size = config->specialization.subgroup_size;

    Module* old_mod;

    IrArena* old_arena = NULL;
    IrArena* tmp_arena = NULL;

    generate_dummy_constants(config, mod);

    aconfig.name_bound = true;
    RUN_PASS(bind_program)
    RUN_PASS(normalize)

    aconfig.check_types = true;
    RUN_PASS(infer_program)

    aconfig.validate_builtin_types = true;
    RUN_PASS(normalize_builtins);

    aconfig.allow_fold = true;
    RUN_PASS(opt_inline_jumps)

    RUN_PASS(lcssa)
    RUN_PASS(reconvergence_heuristics)

    RUN_PASS(setup_stack_frames)
    RUN_PASS(lower_cf_instrs)
    if (!config->hacks.force_join_point_lifting) {
        RUN_PASS(mark_leaf_functions)
    }

    RUN_PASS(lower_callf)
    RUN_PASS(opt_inline)

    RUN_PASS(lift_indirect_targets)

    RUN_PASS(opt_stack)

    RUN_PASS(lower_tailcalls)
    RUN_PASS(lower_switch_btree)
    RUN_PASS(opt_restructurize)
    RUN_PASS(opt_inline_jumps)

    aconfig.specializations.subgroup_mask_representation = SubgroupMaskInt64;
    RUN_PASS(lower_mask)
    RUN_PASS(lower_memcpy)
    RUN_PASS(lower_subgroup_ops)
    RUN_PASS(lower_stack)

    RUN_PASS(lower_lea)
    RUN_PASS(lower_generic_ptrs)
    RUN_PASS(lower_physical_ptrs)
    RUN_PASS(lower_subgroup_vars)
    RUN_PASS(lower_memory_layout)

    if (config->lower.decay_ptrs) {
        RUN_PASS(lower_decay_ptrs)
    }

    RUN_PASS(lower_int)

    if (config->lower.simt_to_explicit_simd) {
        aconfig.is_simt = false;
        RUN_PASS(simt2d)
    }

    if (config->specialization.entry_point) {
        specialize_arena_config(&aconfig, mod, config);
        RUN_PASS(specialize_for_entry_point)
    }
    RUN_PASS(lower_fill)

    return CompilationNoError;
}

#undef mod

CompilationResult parse_files(CompilerConfig* config, size_t num_files, const char** file_names, const char** files_contents, Module* mod) {
    ParserConfig pconfig = {
        .front_end = config->allow_frontend_syntax
    };

    for (size_t i = 0; i < num_files; i++) {
        if (file_names && string_ends_with(file_names[i], ".spv")) {
#ifdef SPV_PARSER_PRESENT
            size_t size;
            unsigned char* data;
            bool ok = read_file(file_names[i], &size, &data);
            assert(ok);
            parse_spirv_into_shady(mod, size, (uint32_t*) data);
#else
            assert(false && "SPIR-V front-end missing in this version");
#endif
        } else {
            const char* file_contents;

            if (files_contents) {
                file_contents = files_contents[i];
            } else {
                assert(file_names);
                bool ok = read_file(file_names[i], NULL, &file_contents);
                assert(ok);

                if (file_contents == NULL) {
                    error_print("file does not exist\n");
                    exit(InputFileDoesNotExist);
                }
            }

            debugv_print("Parsing: \n%s\n", file_contents);

            parse(pconfig, file_contents, mod);

            if (!files_contents)
                free((void*) file_contents);
        }
    }

    if (config->dynamic_scheduling) {
        debugv_print("Parsing builtin scheduler code");
        parse(pconfig, shady_scheduler_src, mod);
    }

    // Free the read files
    for (size_t i = 0; i < num_files; i++)

    return CompilationNoError;
}
