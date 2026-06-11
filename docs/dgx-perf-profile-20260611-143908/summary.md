# DGX perf 热点分析

- 时间: 2026-06-11T14:39:45+08:00
- 场景: proxy-1x1 (tcpquic-proxy 单 QUIC + 单 curl)
- 采样: 25s @ 999Hz, call-graph dwarf
- 本机: 169.254.250.230 | 对端: 169.254.59.196

## 吞吐
```
speed_download=1060808447
```

## Client 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 30
#
# Samples: 75  of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 7243977
#
# Overhead  Shared Object       Symbol                                 IPC   [IPC Coverage]
# ........  ..................  .....................................  ....................
#
    53.26%  [kernel.kallsyms]   [k] fdget                              -      -            
            |
            ---fdget
               |          
               |--52.50%--do_epoll_wait
               |          do_epoll_pwait.part.0
               |          __arm64_sys_epoll_pwait
               |          invoke_syscall
               |          el0_svc_common.constprop.0
               |          do_el0_svc
               |          el0_svc
               |          el0t_64_sync_handler
               |          el0t_64_sync
               |          __GI_epoll_pwait (inlined)
               |          TqLinuxRelayWorker::Run()
               |          0xedb93bb31adf
               |          |          
               |          |--51.63%--0x21edb93b8d595b
               |          |          0x21edb93b8d595b
               |          |          thread_start
               |          |          
               |           --0.87%--0x30edb93b8d595b
               |                     0x30edb93b8d595b
               |                     thread_start
               |          
                --0.75%--do_epoll_pwait.part.0
                          __arm64_sys_epoll_pwait
                          invoke_syscall
                          el0_svc_common.constprop.0
                          do_el0_svc
                          el0_svc
                          el0t_64_sync_handler
                          el0t_64_sync
                          __GI_epoll_pwait (inlined)
                          TqLinuxRelayWorker::Run()
                          0xedb93bb31adf
                          0x76edb93b8d595b
                          0x76edb93b8d595b
                          thread_start

    41.52%  libc.so.6           [.] __GI___pthread_enable_asynccancel  -      -            
            |
            ---__GI___pthread_enable_asynccancel
               __GI_epoll_pwait (inlined)
               TqLinuxRelayWorker::Run()
               0xedb93bb31adf
               0x76edb93b8d595b
               0x76edb93b8d595b
               thread_start

     1.03%  libc.so.6           [.] __pthread_mutex_cond_lock          -      -            
            |
            ---__pthread_mutex_cond_lock
               __pthread_cond_wait_common (inlined)
               ___pthread_cond_clockwait64 (inlined)
               ___pthread_cond_clockwait64 (inlined)
               TqTunnelReaper::ReaperLoop()
               0xedb93bb31adf
               0x7eedb93b8d595b
               0x7eedb93b8d595b
               thread_start

     0.90%  [kernel.kallsyms]   [k] schedule_debug.isra.0              -      -            
            |
            ---schedule_debug.isra.0
               __schedule
               schedule
               schedule_hrtimeout_range_clock
               schedule_hrtimeout_range
               ep_poll
               do_epoll_wait
               do_epoll_pwait.part.0
               __arm64_sys_epoll_pwait
               invoke_syscall
               el0_svc_common.constprop.0
               do_el0_svc
               el0_svc
               el0t_64_sync_handler
               el0t_64_sync
               __GI_epoll_pwait (inlined)
               TqLinuxRelayWorker::Run()
               0xedb93bb31adf
               0x30edb93b8d595b
               0x30edb93b8d595b
               thread_start

     0.81%  libc.so.6           [.] __aarch64_cas4_acq                 -      -            
            |
            ---__aarch64_cas4_acq
               __GI___pthread_disable_asynccancel
               __GI___pthread_disable_asynccancel
               __GI_epoll_pwait (inlined)
               TqLinuxRelayWorker::Run()
               0xedb93bb31adf
               0x21edb93b8d595b
               0x21edb93b8d595b
               thread_start

     0.81%  [kernel.kallsyms]   [k] ktime_add_safe                     -      -            
            |
            ---ktime_add_safe
               schedule_hrtimeout_range
               ep_poll
               do_epoll_wait
               do_epoll_pwait.part.0
               __arm64_sys_epoll_pwait
               invoke_syscall
               el0_svc_common.constprop.0
               do_el0_svc
```

## Server 热点 Top
```
```

