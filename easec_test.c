#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Redirect original main function so we can provide our own test runner main */
#define main interpreter_main
#include "easec.c"
#undef main

/* Assertion Helper Functions */
void assert_int(Env* env, const char* name, long long expected) {
    Value val = env_get(env, name);
    if (val.type != VAL_INT) {
        fprintf(stderr, "\nFAIL: '%s' is not VAL_INT (type is %d)\n", name, val.type);
        exit(1);
    }
    if (val.as.integer != expected) {
        fprintf(stderr, "\nFAIL: '%s' expected %lld, got %lld\n", name, expected, val.as.integer);
        exit(1);
    }
}

void assert_bool(Env* env, const char* name, int expected) {
    Value val = env_get(env, name);
    if (val.type != VAL_BOOL) {
        fprintf(stderr, "\nFAIL: '%s' is not VAL_BOOL\n", name);
        exit(1);
    }
    if (val.as.boolean != expected) {
        fprintf(stderr, "\nFAIL: '%s' expected %s, got %s\n", name, expected ? "true" : "false", val.as.boolean ? "true" : "false");
        exit(1);
    }
}

void assert_float(Env* env, const char* name, double expected) {
    Value val = env_get(env, name);
    if (val.type != VAL_FLOAT) {
        fprintf(stderr, "\nFAIL: '%s' is not VAL_FLOAT\n", name);
        exit(1);
    }
    double diff = val.as.floating - expected;
    if (diff < 0) diff = -diff;
    if (diff > 0.00001) {
        fprintf(stderr, "\nFAIL: '%s' expected %g, got %g\n", name, expected, val.as.floating);
        exit(1);
    }
}

void assert_string(Env* env, const char* name, const char* expected) {
    Value val = env_get(env, name);
    if (val.type != VAL_OBJ || val.as.obj->type != OBJ_STRING) {
        fprintf(stderr, "\nFAIL: '%s' is not OBJ_STRING\n", name);
        exit(1);
    }
    const char* str = ((ObjString*)val.as.obj)->chars;
    if (strcmp(str, expected) != 0) {
        fprintf(stderr, "\nFAIL: '%s' expected '%s', got '%s'\n", name, expected, str);
        exit(1);
    }
}

/* Test Runner Infrastructure */
void run_test(const char* name, const char* source, void (*asserts)(Env*)) {
    printf("Running test: %-45s ... ", name);
    fflush(stdout);

    had_error = 0;
    had_runtime_error = 0;

    init_vm();
    Env* env = create_env(NULL);
    
    run_script(source, env);
    
    if (had_error || had_runtime_error) {
        fprintf(stderr, "FAIL (Compilation or Runtime Error occurred)\n");
        exit(1);
    }
    
    if (asserts) {
        asserts(env);
    }
    
    // Complete VM teardown to reset state and release memory
    pop_env();
    vm.gc_paused = 0;
    vm.next_gc = 0;
    Object* curr = vm.objects;
    while (curr) {
        curr->is_constant = 0;
        curr = curr->next;
    }
    gc_collect();
    
    safe_free(vm.env_stack);
    free_table(&vm.strings);
    for (int i = 0; i < vm.import_count; i++) {
        safe_free(vm.import_stack[i]);
    }
    safe_free(vm.import_stack);
    vm.import_stack = NULL;
    vm.import_count = 0;
    vm.import_capacity = 0;
    
    free_ast();
    
    if (bytes_allocated != 0) {
        fprintf(stderr, "FAIL (Memory leak detected: %zu bytes remaining)\n", bytes_allocated);
        exit(1);
    }

    printf("PASSED\n");
}

/* Assertion and Source Blocks for Individual Tests */

// Test 1: Math and Variables
void assert_math_vars(Env* env) {
    assert_int(env, "a", 15);
    assert_float(env, "b", 12.5);
    assert_bool(env, "c", 1);
    assert_bool(env, "d", 0);
    assert_string(env, "e", "Hello, World!");
}
const char* src_math_vars = 
    "var a 10 + 5\n"
    "var b 10.0 + 2.5\n"
    "var c a == 15\n"
    "var d a != 15\n"
    "var e \"Hello, \" + \"World!\"\n";

// Test 2: If / Else Logic & Reassignments
void assert_if_else(Env* env) {
    assert_int(env, "res1", 42);
    assert_int(env, "res2", 100);
}
const char* src_if_else = 
    "var x 10\n"
    "var res1 0\n"
    "if x > 5 [\n"
    "    res1 = 42\n"
    "] else [\n"
    "    res1 = 99\n"
    "]\n"
    "var y 2\n"
    "var res2 0\n"
    "if y > 5 [\n"
    "    res2 = 42\n"
    "] else [\n"
    "    res2 = 100\n"
    "]\n";

// Test 3: Loops
void assert_loops(Env* env) {
    assert_int(env, "counter", 5);
}
const char* src_loops = 
    "var counter 0\n"
    "repeat 5 [\n"
    "    counter = counter + 1\n"
    "]\n";

// Test 4: Jobs with 0 Parameters (Implicit self-call test)
void assert_zero_arg_jobs(Env* env) {
    assert_int(env, "value", 100);
}
const char* src_zero_arg_jobs = 
    "job get_hundred [\n"
    "    out 100\n"
    "]\n"
    "var value get_hundred\n";

// Test 5: Jobs with Parameters & Recursion
void assert_arg_jobs(Env* env) {
    assert_int(env, "sum", 30);
    assert_int(env, "fact", 120);
}
const char* src_arg_jobs = 
    "job add x, y [\n"
    "    out x + y\n"
    "]\n"
    "var sum add 10, 20\n"
    "job factorial n [\n"
    "    if n == 1 [\n"
    "        out 1\n"
    "    ]\n"
    "    out n * factorial (n - 1)\n"
    "]\n"
    "var fact factorial 5\n";

// Test 6: Arrays
void assert_arrays(Env* env) {
    assert_int(env, "val1", 20);
    assert_int(env, "val2", 99);
}
const char* src_arrays = 
    "array my_arr 10, 20, 30\n"
    "var val1 array get my_arr 1\n"
    "array set my_arr 1 99\n"
    "var val2 array get my_arr 1\n";

// Test 7: Dictionaries
void assert_dicts(Env* env) {
    assert_string(env, "v1", "Easec");
    assert_string(env, "v2", "Updated");
}
const char* src_dicts = 
    "dictionary my_dict name: \"Easec\", score: 100\n"
    "var v1 dictionary get my_dict name\n"
    "dictionary set my_dict name \"Updated\"\n"
    "var v2 dictionary get my_dict name\n";

// Test 8: Time Sleep & Get
void assert_time(Env* env) {
    assert_bool(env, "time_passed", 1);
}
const char* src_time = 
    "var start time get\n"
    "time sleep 50\n"
    "var end time get\n"
    "var time_passed end >= start\n";

// Test 9: Files & Import System Mechanics
void assert_files_and_imports(Env* env) {
    assert_int(env, "res", 42);
}
const char* src_files_and_imports = 
    "file create \"temp_lib.easec\" \"job add_two a, b [ out a + b ]\"\n"
    "import \"temp_lib.easec\" as math\n"
    "var res math.add_two 40, 2\n"
    "file delete \"temp_lib.easec\"\n";


int main(int argc, char** argv) {
    printf("=============================================\n");
    printf("        STARTING EASEC TEST SUITE            \n");
    printf("=============================================\n");

    run_test("Variables and Arithmetic", src_math_vars, assert_math_vars);
    run_test("Branching Logic (If/Else)", src_if_else, assert_if_else);
    run_test("Loops (Repeat)", src_loops, assert_loops);
    run_test("0-Arity Autocall Jobs", src_zero_arg_jobs, assert_zero_arg_jobs);
    run_test("Parameterized & Recursive Jobs", src_arg_jobs, assert_arg_jobs);
    run_test("Arrays (Decl, Get, Set)", src_arrays, assert_arrays);
    run_test("Dictionaries (Decl, Get, Set)", src_dicts, assert_dicts);
    run_test("Time Utility (Get and Sleep)", src_time, assert_time);
    run_test("Filesystem & Module Imports", src_files_and_imports, assert_files_and_imports);

    printf("=============================================\n");
    printf("       ALL TESTS PASSED SUCCESSFULLY!        \n");
    printf("=============================================\n");
    return 0;
}