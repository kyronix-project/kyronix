set pagination off
set confirm off

define profiler_start
    call (void)prof_start()
    printf "[profiler] Started (250 Hz sampling)\n"
end

define profiler_stop
    call (void)prof_stop()
    printf "[profiler] Stopped\n"
end

define profiler_reset
    call (void)prof_reset()
    printf "[profiler] Reset\n"како
end

define profiler_status
    call (int)prof_is_active()
    printf "[profiler] Active: %d\n", $rax
end

define profiler_show
    printf "[profiler] Dumping profile to serial...\n"
    call (void)prof_print()
    printf "[profiler] Done. Check output above.\n"
end

define profiler_help
    printf "k9 kernel Profiler Commands:\n"
    printf "  profiler_start    - Start profiling (250 Hz sampling)\n"
    printf "  profiler_stop     - Stop profiling\n"
    printf "  profiler_reset    - Reset sample buffer\n"
    printf "  profiler_status   - Show profiler status (active?, samples)\n"
    printf "  profiler_show     - Print full profile to serial console\n"
    printf "  profiler_help     - This help\n"
    printf "\n"
    printf "Workflow:\n"
    printf "  1. (gdb) profiler_start\n"
    printf "  2. (gdb) continue          # let kernel run\n"
    printf "  3. (gdb) <Ctrl+C>          # stop when done\n"
    printf "  4. (gdb) profiler_show     # view results\n"
    printf "\n"
    printf "Or from kernel shell:\n"
    printf "  $ echo start > /proc/profile\n"
    printf "  $ cat /proc/profile\n"
end

printf "k9 Profiler loaded. Type 'profiler_help' for commands.\n"
