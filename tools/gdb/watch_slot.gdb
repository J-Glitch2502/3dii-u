set pagination off
set breakpoint pending on
handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass
break load_game
run
delete
set $slot = (unsigned*)(g_mem + 0x0ffffecc)
watch -l *$slot
commands
silent
printf "slot now %08x, thread %d\n", *$slot, $_thread
bt 4
continue
end
break rt_fatal
continue
quit
