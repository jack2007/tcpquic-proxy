# DGX perf 热点分析

- 时间: 2026-06-11T18:50:43+08:00
- 场景: proxy-1x1 (tcpquic-proxy 单 QUIC + 单 curl)
- 采样: 25s @ 999Hz, call-graph dwarf
- 本机: 169.254.250.230 | 对端: 169.254.59.196

## 吞吐
```
speed_download=0
```

## Client 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 3K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 7108140605
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                                        IPC   [IPC Coverage]
# ........  ............  ............................................................................................................................................................................................................................................................  ....................
#
    23.98%           711  [k] __arch_copy_to_user                                                                                                                                                                                                                                       -      -            
            |          
             --23.92%--simple_copy_to_iter
                       __skb_datagram_iter
                       |          
                       |--23.26%--__skb_datagram_iter
                       |          skb_copy_datagram_iter
                       |          udpv6_recvmsg
                       |          inet6_recvmsg
                       |          sock_recvmsg
                       |          ____sys_recvmsg
                       |          ___sys_recvmsg
                       |          do_recvmmsg
                       |          __arm64_sys_recvmmsg
                       |          invoke_syscall
                       |          el0_svc_common.constprop.0
                       |          do_el0_svc
                       |          el0_svc
                       |          el0t_64_sync_handler
                       |          el0t_64_sync
                       |          recvmmsg_syscall (inlined)
                       |          __recvmmsg
                       |          CxPlatSocketReceiveCoalesced
                       |          CxPlatSocketContextIoEventComplete
                       |          CxPlatProcessEvents
                       |          CxPlatWorkerThread
                       |          start_thread
                       |          thread_start
                       |          
                        --0.66%--skb_copy_datagram_iter
                                  udpv6_recvmsg
                                  inet6_recvmsg
                                  sock_recvmsg
                                  ____sys_recvmsg
                                  ___sys_recvmsg
                                  do_recvmmsg
                                  __arm64_sys_recvmmsg
                                  invoke_syscall
                                  el0_svc_common.constprop.0
                                  do_el0_svc
                                  el0_svc
                                  el0t_64_sync_handler
                                  el0t_64_sync
                                  recvmmsg_syscall (inlined)
                                  __recvmmsg
                                  CxPlatSocketReceiveCoalesced
                                  CxPlatSocketContextIoEventComplete
                                  CxPlatProcessEvents
                                  CxPlatWorkerThread
                                  start_thread
                                  thread_start

    13.26%           384  [k] __slab_free                                                                                                                                                                                                                                               -      -            
            |          
            |--12.18%--kmem_cache_free
            |          |          
            |           --11.96%--skb_free_head
            |                     skb_release_data
            |                     |          
            |                      --11.69%--kfree_skb_list_reason
            |                                skb_release_data
            |                                __consume_stateless_skb
            |                                skb_consume_udp
            |                                udpv6_recvmsg
            |                                inet6_recvmsg
            |                                sock_recvmsg
            |                                ____sys_recvmsg
            |                                ___sys_recvmsg
            |                                do_recvmmsg
            |                                __arm64_sys_recvmmsg
            |                                invoke_syscall
            |                                el0_svc_common.constprop.0
            |                                do_el0_svc
            |                                el0_svc
            |                                el0t_64_sync_handler
            |                                el0t_64_sync
            |                                recvmmsg_syscall (inlined)
            |                                __recvmmsg
            |                                CxPlatSocketReceiveCoalesced
            |                                CxPlatSocketContextIoEventComplete
            |                                CxPlatProcessEvents
            |                                CxPlatWorkerThread
            |                                start_thread
            |                                thread_start
            |          
             --1.02%--kmem_cache_free_bulk.part.0
                       kmem_cache_free_bulk
                       kfree_skb_list_reason
                       skb_release_data
                       __consume_stateless_skb
                       skb_consume_udp
                       udpv6_recvmsg
                       inet6_recvmsg
                       sock_recvmsg
                       ____sys_recvmsg
                       ___sys_recvmsg
                       do_recvmmsg
                       __arm64_sys_recvmmsg
                       invoke_syscall
                       el0_svc_common.constprop.0
                       do_el0_svc
                       el0_svc
                       el0t_64_sync_handler
                       el0t_64_sync
                       recvmmsg_syscall (inlined)
                       __recvmmsg
                       CxPlatSocketReceiveCoalesced
                       CxPlatSocketContextIoEventComplete
                       CxPlatProcessEvents
                       CxPlatWorkerThread
                       start_thread
                       thread_start

     7.25%           215  [k] __skb_datagram_iter                                                                                                                                                                                                                                       -      -            
            |          
            |--5.04%--skb_copy_datagram_iter
            |          udpv6_recvmsg
            |          inet6_recvmsg
            |          sock_recvmsg
            |          ____sys_recvmsg
            |          ___sys_recvmsg
            |          do_recvmmsg
            |          __arm64_sys_recvmmsg
            |          invoke_syscall
            |          el0_svc_common.constprop.0
            |          do_el0_svc
            |          el0_svc
            |          el0t_64_sync_handler
            |          el0t_64_sync
            |          recvmmsg_syscall (inlined)
            |          __recvmmsg
            |          CxPlatSocketReceiveCoalesced
            |          CxPlatSocketContextIoEventComplete
            |          CxPlatProcessEvents
            |          CxPlatWorkerThread
            |          start_thread
            |          thread_start
            |          
             --2.21%--__skb_datagram_iter
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 9K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 7964594641
#
# Overhead       Samples  Symbol                                                                                                                           IPC   [IPC Coverage]
# ........  ............  ...............................................................................................................................  ....................
#
     1.19%           128  [k] finish_task_switch.isra.0                                                                                                    -      -            
            |          
             --1.18%--__schedule
                       schedule
                       |          
                        --1.15%--schedule_hrtimeout_range_clock
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
                                  |          
                                   --1.14%--CxPlatProcessEvents
                                             CxPlatWorkerThread
                                             start_thread
                                             thread_start

     1.14%            31  [.] aes_gcm_enc_256_kernel                                                                                                       -      -            
            |
            ---armv8_aes_gcm_encrypt
               ossl_gcm_aad_update
               0x5aa

     0.98%            28  [.] TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)                                                        -      -            
            |
            ---TqLinuxRelayWorker::Run()
               0xfc48cc931adf

     0.83%            37  [k] el0_svc                                                                                                                      -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync
               |          
                --0.59%--__GI_epoll_pwait (inlined)
                          |          
                           --0.52%--TqLinuxRelayWorker::Run()
                                     0xfc48cc931adf

     0.72%            42  [k] __arch_copy_to_user                                                                                                          -      -            
            |          
             --0.48%--simple_copy_to_iter
                       __skb_datagram_iter
                       |          
                        --0.47%--skb_copy_datagram_iter
                                  |          
                                   --0.43%--tcp_recvmsg_locked
                                             tcp_recvmsg
                                             inet_recvmsg
                                             sock_recvmsg
                                             sock_read_iter
                                             do_iter_readv_writev
                                             vfs_readv
                                             do_readv
                                             __arm64_sys_readv
                                             invoke_syscall
                                             el0_svc_common.constprop.0
                                             do_el0_svc
                                             el0_svc
                                             el0t_64_sync_handler
                                             el0t_64_sync
                                             __GI___readv
                                             __GI___readv
                                             TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                                             TqLinuxRelayWorker::Run()
                                             0xfc48cc931adf

     0.68%            31  [k] fput                                                                                                                         -      -            
            |          
             --0.53%--do_epoll_wait
                       do_epoll_pwait.part.0
                       __arm64_sys_epoll_pwait
                       invoke_syscall
                       el0_svc_common.constprop.0
                       do_el0_svc
                       el0_svc
                       el0t_64_sync_handler
                       el0t_64_sync
                       __GI_epoll_pwait (inlined)
                       |          
                        --0.41%--TqLinuxRelayWorker::Run()
                                  0xfc48cc931adf

     0.62%            32  [k] _raw_spin_unlock_irq                                                                                                         -      -            
            |          
             --0.50%--ep_poll
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

     0.58%            32  [.] __aarch64_cas4_acq                                                                                                           -      -            
     0.55%            46  [k] fdget                                                                                                                        -      -            
     0.51%            16  [k] arch_counter_get_cntpct                                                                                                      -      -            
            |          
             --0.50%--ktime_get_ts64

     0.46%            14  [.] CxPlatHashtableEnumerateNext                                                                                                 -      -            
            |
            ---QuicStreamSetGetFlowControlSummary
               BbrCongestionControlUpdateBlockedState
               QuicLossDetectionOnPacketSent
               QuicPacketBuilderFinalize
               QuicSendFlush
               QuicConnDrainOperations
               QuicWorkerProcessConnection
               QuicWorkerLoop
               QuicWorkerThread
               start_thread
               thread_start

     0.41%            18  [k] get_random_u16                                                                                                               -      -            
            |          
             --0.40%--invoke_syscall
                       el0_svc_common.constprop.0
                       do_el0_svc
                       el0_svc
                       el0t_64_sync_handler
                       el0t_64_sync

     0.41%            19  [.] epoll_pwait                                                                                                                  -      -            


# Samples: 165K of event 'armv8_pmuv3_1/cycles/P'
# Event count (approx.): 630289836854
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                                              IPC   [IPC Coverage]
```

