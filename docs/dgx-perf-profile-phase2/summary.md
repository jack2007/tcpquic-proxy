# DGX perf 热点分析

- 时间: 2026-06-11T16:56:06+08:00
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
# Total Lost Samples: 8
#
# Samples: 2K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 4570294310
#
# Overhead       Samples  Symbol                                                                     IPC   [IPC Coverage]
# ........  ............  .........................................................................  ....................
#
    29.52%           533  [k] __arch_copy_to_user                                                    -      -            
            |
            ---simple_copy_to_iter
               __skb_datagram_iter
               |          
               |--28.12%--__skb_datagram_iter
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
                --1.40%--skb_copy_datagram_iter
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

     5.83%           111  [k] __slab_free                                                            -      -            
            |          
            |--5.34%--kmem_cache_free
            |          |          
            |           --5.30%--skb_free_head
            |                     skb_release_data
            |                     |          
            |                      --5.15%--kfree_skb_list_reason
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
             --0.49%--kmem_cache_free_bulk.part.0
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

     5.67%            95  [.] __aarch64_ldadd8_sync                                                  -      -            
            |          
             --5.07%--QuicBindingReceive
                       CxPlatSocketReceiveCoalesced
                       CxPlatSocketContextIoEventComplete
                       CxPlatProcessEvents
                       CxPlatWorkerThread
                       start_thread
                       thread_start

     4.52%            84  [k] __skb_datagram_iter                                                    -      -            
            |          
            |--2.78%--skb_copy_datagram_iter
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
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 8K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 9145638661
#
# Overhead       Samples  Symbol                                                                                           IPC   [IPC Coverage]
# ........  ............  ...............................................................................................  ....................
#
     2.07%            68  [.] TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)                        -      -            
            |
            ---TqLinuxRelayWorker::Run()
               0xf0e1c1481adf
               |          
               |--0.86%--0x68f0e1c122595b
               |          0x68f0e1c122595b
               |          thread_start
               |          
                --0.60%--0x78f0e1c122595b
                          0x78f0e1c122595b
                          thread_start

     1.48%            57  [k] el0_svc                                                                                      -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync
               |          
               |--0.70%--__GI_epoll_pwait (inlined)
               |          TqLinuxRelayWorker::Run()
               |          0xf0e1c1481adf
               |          
                --0.69%--__GI___readv
                          __GI___readv
                          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                          TqLinuxRelayWorker::Run()
                          0xf0e1c1481adf

     1.45%            44  [k] arch_counter_get_cntpct                                                                      -      -            
            |          
             --1.42%--ktime_get_ts64
                       |          
                       |--0.79%--select_estimate_accuracy
                       |          ep_poll
                       |          do_epoll_wait
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
                       |          0xf0e1c1481adf
                       |          
                        --0.63%--__arm64_sys_epoll_pwait
                                  invoke_syscall
                                  el0_svc_common.constprop.0
                                  do_el0_svc
                                  el0_svc
                                  el0t_64_sync_handler
                                  el0t_64_sync
                                  __GI_epoll_pwait (inlined)
                                  TqLinuxRelayWorker::Run()
                                  0xf0e1c1481adf

     1.22%            40  [.] TqLinuxRelayBufferPool::AcquireWorker()                                                      -      -            
            |
            ---TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xf0e1c1481adf
               |          
                --0.43%--0x68f0e1c122595b
                          0x68f0e1c122595b
                          thread_start

     0.91%            30  [.] CxPlatHashtableEnumerateNext                                                                 -      -            
            |
            ---QuicStreamSetGetFlowControlSummary
               BbrCongestionControlUpdateBlockedState
               |          
                --0.88%--QuicLossDetectionOnPacketSent
                          QuicPacketBuilderFinalize
                          QuicSendFlush
                          QuicConnDrainOperations
                          QuicWorkerProcessConnection
                          QuicWorkerLoop
                          QuicWorkerThread
                          start_thread
                          thread_start

     0.89%            30  [k] __import_iovec                                                                               -      -            
            |          
             --0.79%--import_iovec
                       |          
                        --0.79%--vfs_readv
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
                                  0xf0e1c1481adf

     0.81%            27  [.] aes_gcm_enc_256_kernel                                                                       -      -            
            |
            ---armv8_aes_gcm_encrypt
               ossl_gcm_aad_update
               0x5aa

     0.76%            28  [k] __arch_copy_to_user                                                                          -      -            
            |          
             --0.71%--simple_copy_to_iter
                       __skb_datagram_iter
                       skb_copy_datagram_iter
                       tcp_recvmsg_locked
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
                       0xf0e1c1481adf

     0.71%            24  [.] TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)                                    -      -            
            |
            ---TqBufferHandle::~TqBufferHandle()
               TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
```

