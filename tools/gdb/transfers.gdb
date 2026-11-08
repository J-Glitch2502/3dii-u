set pagination off
set breakpoint pending on
set print thread-events off
handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass
break gpu::display_transfer
commands
silent
printf "DT in %08x out %08x in_dims %ux%u out_dims %ux%u flags %08x\n", in_paddr, out_paddr, in_dims & 0xffff, in_dims >> 16, out_dims & 0xffff, out_dims >> 16, flags
continue
end
break gpu::texture_copy
commands
silent
printf "TC in %08x out %08x size %u gaps %08x %08x flags %08x\n", in_paddr, out_paddr, size, in_width_gap, out_width_gap, flags
continue
end
run
quit
