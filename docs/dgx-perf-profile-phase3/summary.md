# DGX perf 热点分析

- 时间: 2026-06-11T18:37:33+08:00
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
# Total Lost Samples: 9
#
# Samples: 964  of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 1663969086
#
# Overhead       Samples  Symbol                                                                                       IPC   [IPC Coverage]
# ........  ............  ...........................................................................................  ....................
#
    25.17%           174  [k] __arch_copy_to_user                                                                      -      -            
            |
            ---simple_copy_to_iter
               __skb_datagram_iter
               |          
               |--23.89%--__skb_datagram_iter
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
                --1.29%--skb_copy_datagram_iter
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

     7.36%            41  [.] aes_gcm_dec_256_kernel                                                                   -      -            
            |
            ---armv8_aes_gcm_decrypt
               ossl_gcm_aad_update
               0x5aa

     4.79%            34  [k] __slab_free                                                                              -      -            
            |          
            |--4.14%--kmem_cache_free
            |          |          
            |           --4.01%--skb_free_head
            |                     skb_release_data
            |                     kfree_skb_list_reason
            |                     skb_release_data
            |                     __consume_stateless_skb
            |                     skb_consume_udp
            |                     udpv6_recvmsg
            |                     inet6_recvmsg
            |                     sock_recvmsg
            |                     ____sys_recvmsg
            |                     ___sys_recvmsg
            |                     do_recvmmsg
            |                     __arm64_sys_recvmmsg
            |                     invoke_syscall
            |                     el0_svc_common.constprop.0
            |                     do_el0_svc
            |                     el0_svc
            |                     el0t_64_sync_handler
            |                     el0t_64_sync
            |                     recvmmsg_syscall (inlined)
            |                     __recvmmsg
            |                     CxPlatSocketReceiveCoalesced
            |                     CxPlatSocketContextIoEventComplete
            |                     CxPlatProcessEvents
            |                     CxPlatWorkerThread
            |                     start_thread
            |                     thread_start
            |          
             --0.65%--kmem_cache_free_bulk.part.0
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

     4.68%            34  [k] __skb_datagram_iter                                                                      -      -            
            |          
            |--2.40%--skb_copy_datagram_iter
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
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 25K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 64868502116
#
# Overhead       Samples  Symbol                                                                                           IPC   [IPC Coverage]
# ........  ............  ...............................................................................................  ....................
#
    11.12%          2579  [.] TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)                        -      -            
            |
            ---TqLinuxRelayWorker::Run()
               0xf300494c1adf
               |          
               |--9.94%--0x10f3004926595b
               |          0x10f3004926595b
               |          thread_start
               |          
                --1.09%--0x4af3004926595b
                          0x4af3004926595b
                          thread_start

     7.94%          1841  [.] TqLinuxRelayBufferPool::AcquireWorker()                                                      -      -            
            |
            ---TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xf300494c1adf
               |          
               |--7.20%--0x10f3004926595b
               |          0x10f3004926595b
               |          thread_start
               |          
                --0.71%--0x4af3004926595b
                          0x4af3004926595b
                          thread_start

     6.23%          1451  [k] el0_svc                                                                                      -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync
               |          
               |--5.59%--__GI___readv
               |          __GI___readv
               |          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               |          TqLinuxRelayWorker::Run()
               |          0xf300494c1adf
               |          |          
               |          |--5.12%--0x10f3004926595b
               |          |          0x10f3004926595b
               |          |          thread_start
               |          |          
               |           --0.46%--0x4af3004926595b
               |                     0x4af3004926595b
               |                     thread_start
               |          
                --0.63%--__GI_epoll_pwait (inlined)
                          |          
                           --0.63%--TqLinuxRelayWorker::Run()
                                     0xf300494c1adf
                                     |          
                                      --0.41%--0x10f3004926595b
                                                0x10f3004926595b
                                                thread_start

     4.77%          1106  [k] __import_iovec                                                                               -      -            
            |          
             --4.64%--import_iovec
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
                       0xf300494c1adf
                       |          
                       |--4.12%--0x10f3004926595b
                       |          0x10f3004926595b
                       |          thread_start
                       |          
                        --0.48%--0x4af3004926595b
                                  0x4af3004926595b
                                  thread_start

     4.32%          1002  [.] TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)                                    -      -            
            |
            ---TqBufferHandle::~TqBufferHandle()
               TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xf300494c1adf
               |          
                --3.96%--0x10f3004926595b
                          0x10f3004926595b
                          thread_start

     3.42%           793  [.] TqRelayBufferSlot::Data()                                                                    -      -            
            |
            ---TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xf300494c1adf
               |          
               |--3.01%--0x10f3004926595b
               |          0x10f3004926595b
               |          thread_start
               |          
                --0.41%--0x4af3004926595b
                          0x4af3004926595b
                          thread_start

     2.97%           689  [.] __GI___readv                                                                                 -      -            
            |          
             --2.93%--__GI___readv
                       TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                       TqLinuxRelayWorker::Run()
                       0xf300494c1adf
                       |          
                        --2.65%--0x10f3004926595b
                                  0x10f3004926595b
                                  thread_start

     2.52%           585  [k] fdget_pos                                                                                    -      -            
            |          
             --2.44%--do_readv
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
                       0xf300494c1adf
                       |          
                        --2.26%--0x10f3004926595b
                                  0x10f3004926595b
                                  thread_start

     2.51%           583  [.] TqBufferHandle::~TqBufferHandle()                                                            -      -            
            |
```

