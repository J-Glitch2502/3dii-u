set pagination off
set breakpoint pending on
handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass
break f_00103cf8
run
delete
set $c = (CPU*)$rdi
watch -l $c->pc if $c->pc == 0
continue
bt 8
x/14i $pc-48
info registers rbx rbp r12 r13 r14 r15
p/x $c->r
quit
