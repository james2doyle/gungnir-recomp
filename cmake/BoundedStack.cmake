include_guard(GLOBAL)

# Runtime code must not reserve large locals. The Android shell runs the
# runtime on the SDL thread, whose stack is about one megabyte; a single
# oversized frame (the game-data identity gate kept a 1 MiB scratch buffer as
# a local) eats that budget whole and turns the next ordinary call into a
# stack overflow that only shows up on the device. Bounding the frame at
# configure time keeps the class of bug out of the tree.
set(GUNGNIR_MAX_STACK_FRAME 16384 CACHE STRING
    "Largest stack frame in bytes a runtime function may reserve")

function(gungnir_bound_stack_frames target)
    # One option carries both the bound and the error promotion: GCC refuses
    # the plain -Wframe-larger-than= form when it is also named in -Werror=.
    target_compile_options(${target} PRIVATE
        "-Werror=frame-larger-than=${GUNGNIR_MAX_STACK_FRAME}")
endfunction()
