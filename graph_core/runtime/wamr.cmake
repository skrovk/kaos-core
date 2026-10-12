# Shared pinned engine profile and reviewed build-copy correction.
set(CORE_WAMR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../sim_env/.deps/wamr"
    CACHE PATH "Pinned WAMR source")
get_filename_component(CORE_WAMR_ROOT "${CORE_WAMR_ROOT}" REALPATH)
execute_process(COMMAND git -C "${CORE_WAMR_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE CORE_WAMR_REV OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CORE_WAMR_GIT_RESULT)
if(NOT CORE_WAMR_GIT_RESULT EQUAL 0 OR
   NOT CORE_WAMR_REV STREQUAL "b124f70345d712bead5c0c2393acb2dc583511de")
    message(FATAL_ERROR "P3 requires WAMR b124f70345d712bead5c0c2393acb2dc583511de")
endif()
set(WAMR_ROOT_DIR "${CORE_WAMR_ROOT}")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 0)
set(WAMR_BUILD_THREAD_MGR 1)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_LIB_WASI_THREADS 0)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_ALLOC_WITH_USAGE 1)
set(WAMR_BUILD_DUMP_CALL_STACK 0)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
# WAMR's host default uses typed i64/f64 dereferences on arbitrary 32-bit
# operand cells and guest byte addresses. x86 permits the machine accesses,
# but C still requires natural alignment. Select the existing safe helpers
# for integer/float cells, numeric operators and guest memory on both targets
# (ESP32 already selects this path). Keep sanitizer alignment checks enabled.
file(READ "${WAMR_ROOT_DIR}/core/config.h" CORE_WAMR_CONFIG)
string(FIND "${CORE_WAMR_CONFIG}" "#ifndef WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS"
    CORE_WAMR_UNALIGNED_OFFSET)
if(CORE_WAMR_UNALIGNED_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned WAMR unaligned-access selection changed; review safe helpers")
endif()
add_compile_definitions(WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS=0)
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")

# Pinned WAMR's final exec-env deletion assumes there are no readers once its
# own guest threads stop. Other node workers still search every cluster during
# termination/traps. Protect deletion with the same cluster lock as lookup,
# then release it before cluster destruction takes the global list lock.
# Only the build copy changes. Exact source matching makes dependency upgrades
# require review of this local correction instead of silently reapplying it.
set(CORE_WAMR_THREAD_MANAGER
    "${WAMR_ROOT_DIR}/core/iwasm/libraries/thread-mgr/thread_manager.c")
file(READ "${CORE_WAMR_THREAD_MANAGER}" CORE_WAMR_THREAD_SOURCE)
set(CORE_WAMR_OLD_DELETE [=[/* The caller should lock cluster->lock for thread safety */
bool
wasm_cluster_del_exec_env(WASMCluster *cluster, WASMExecEnv *exec_env)
{
    return wasm_cluster_del_exec_env_internal(cluster, exec_env, true);
}]=])
set(CORE_WAMR_NEW_DELETE [=[/* KaOS pinned-profile correction: other clusters' workers can search this
 * list during termination even after this cluster's own execution stopped. */
bool
wasm_cluster_del_exec_env(WASMCluster *cluster, WASMExecEnv *exec_env)
{
    bool ret, empty;
    os_mutex_lock(&cluster->lock);
    ret = wasm_cluster_del_exec_env_internal(cluster, exec_env, false);
    empty = cluster->exec_env_list.len == 0;
    os_mutex_unlock(&cluster->lock);
    if (empty)
        wasm_cluster_destroy(cluster);
    return ret;
}]=])
string(FIND "${CORE_WAMR_THREAD_SOURCE}" "${CORE_WAMR_OLD_DELETE}" CORE_WAMR_DELETE_OFFSET)
if(CORE_WAMR_DELETE_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned WAMR final-cluster deletion changed; review the synchronization correction")
endif()
string(REPLACE "${CORE_WAMR_OLD_DELETE}" "${CORE_WAMR_NEW_DELETE}"
    CORE_WAMR_THREAD_SOURCE "${CORE_WAMR_THREAD_SOURCE}")
# Allocation failure leaves vector->lock untouched, but the common exit path
# destroys the vector. Initialize it so a tight arena cannot free garbage.
set(CORE_WAMR_OLD_VECTOR "Vector proc_nodes;\n    void *node;")
string(FIND "${CORE_WAMR_THREAD_SOURCE}" "${CORE_WAMR_OLD_VECTOR}" CORE_WAMR_VECTOR_OFFSET)
if(CORE_WAMR_VECTOR_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned WAMR traversal vector changed; review OOM correction")
endif()
string(REPLACE "${CORE_WAMR_OLD_VECTOR}" "Vector proc_nodes = { 0 };\n    void *node;"
    CORE_WAMR_THREAD_SOURCE "${CORE_WAMR_THREAD_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/thread_manager.c" "${CORE_WAMR_THREAD_SOURCE}")
list(FIND WAMR_RUNTIME_LIB_SOURCE "${CORE_WAMR_THREAD_MANAGER}" CORE_WAMR_SOURCE_INDEX)
if(CORE_WAMR_SOURCE_INDEX EQUAL -1)
    message(FATAL_ERROR "Pinned WAMR source list changed; cannot replace thread_manager.c")
endif()
list(REMOVE_ITEM WAMR_RUNTIME_LIB_SOURCE "${CORE_WAMR_THREAD_MANAGER}")
list(APPEND WAMR_RUNTIME_LIB_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/thread_manager.c")

# The classic interpreter packs 32-bit cells ahead of pointer-bearing label
# blocks and subsequent frames. Four-byte alignment is insufficient on x86_64;
# preserve pointer alignment on both targets without disabling UBSan checks.
set(CORE_WAMR_INTERP "${WAMR_ROOT_DIR}/core/iwasm/interpreter")
file(READ "${CORE_WAMR_INTERP}/wasm_interp.h" CORE_WAMR_HEADER)
set(CORE_WAMR_OLD_ALIGN "return align_uint(frame_size, 4);")
string(FIND "${CORE_WAMR_HEADER}" "${CORE_WAMR_OLD_ALIGN}" CORE_WAMR_ALIGN_OFFSET)
if(CORE_WAMR_ALIGN_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned WAMR frame alignment changed; review correction")
endif()
string(REPLACE "${CORE_WAMR_OLD_ALIGN}"
    "return align_uint(frame_size, sizeof(void *));" CORE_WAMR_HEADER "${CORE_WAMR_HEADER}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wasm_interp.h" "${CORE_WAMR_HEADER}")
file(READ "${CORE_WAMR_INTERP}/wasm_interp_classic.c" CORE_WAMR_CLASSIC)
set(CORE_WAMR_OLD_CELLS [=[            cell_num_of_local_stack = cur_func->param_cell_num
                                      + cur_func->local_cell_num
                                      + max_stack_cell_num;]=])
set(CORE_WAMR_NEW_CELLS [=[            /* Keep the label stack aligned after the 32-bit cells. */
            max_stack_cell_num +=
                (sizeof(void *) / 4 - (cur_func->param_cell_num
                 + cur_func->local_cell_num + max_stack_cell_num)
                 % (sizeof(void *) / 4)) % (sizeof(void *) / 4);
            cell_num_of_local_stack = cur_func->param_cell_num
                                      + cur_func->local_cell_num
                                      + max_stack_cell_num;]=])
string(FIND "${CORE_WAMR_CLASSIC}" "${CORE_WAMR_OLD_CELLS}" CORE_WAMR_CELLS_OFFSET)
if(CORE_WAMR_CELLS_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned WAMR label stack layout changed; review correction")
endif()
string(REPLACE "${CORE_WAMR_OLD_CELLS}" "${CORE_WAMR_NEW_CELLS}"
    CORE_WAMR_CLASSIC "${CORE_WAMR_CLASSIC}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wasm_interp_classic.c" "${CORE_WAMR_CLASSIC}")
list(REMOVE_ITEM WAMR_RUNTIME_LIB_SOURCE "${CORE_WAMR_INTERP}/wasm_interp_classic.c")
list(APPEND WAMR_RUNTIME_LIB_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/wasm_interp_classic.c")
