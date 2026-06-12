# DGX perf 热点分析

- 时间: 2026-06-12T09:34:42+08:00
- 场景: proxy-1x1 (tcpquic-proxy 单 QUIC + 单 curl)
- QUIC 连接: 1 | 并行 curl: 1
- 采样: 25s @ 999Hz, call-graph dwarf
- 本机: 169.254.250.230 | 对端: 169.254.59.196

## 内核 socket 参数
```
# local spark-1619
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.core.rmem_default = 4194304
net.core.wmem_default = 4194304
net.ipv4.tcp_rmem = 4096	1048576	134217728
net.ipv4.tcp_wmem = 4096	1048576	134217728

# peer 169.254.59.196
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.core.rmem_default = 4194304
net.core.wmem_default = 4194304
net.ipv4.tcp_rmem = 4096	1048576	134217728
net.ipv4.tcp_wmem = 4096	1048576	134217728
```


## 吞吐
```
speed_download=0
```

## Client 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 42
#
# Samples: 1K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 3863894568
#
# Overhead       Samples  Symbol                                                                                                                                                   IPC   [IPC Coverage]
# ........  ............  .......................................................................................................................................................  ....................
#
    21.45%           325  [k] __arch_copy_to_user                                                                                                                                  -      -            
            |
            ---simple_copy_to_iter
               __skb_datagram_iter
               |          
               |--20.85%--__skb_datagram_iter
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
                --0.60%--skb_copy_datagram_iter
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

     6.04%            97  [.] __aarch64_ldadd8_sync                                                                                                                                -      -            
            |          
             --5.24%--QuicBindingReceive
                       CxPlatSocketReceiveCoalesced
                       CxPlatSocketContextIoEventComplete
                       CxPlatProcessEvents
                       CxPlatWorkerThread
                       start_thread
                       thread_start

     4.60%            61  [k] _raw_spin_unlock_irqrestore                                                                                                                          -      -            
            |          
            |--2.05%--wake_up_q
            |          futex_wake
            |          do_futex
            |          __arm64_sys_futex
            |          invoke_syscall
            |          el0_svc_common.constprop.0
            |          do_el0_svc
            |          el0_svc
            |          el0t_64_sync_handler
            |          el0t_64_sync
            |          futex_wake (inlined)
            |          |          
            |           --1.77%--___pthread_cond_broadcast (inlined)
            |                     QuicWorkerThreadWake
            |                     QuicWorkerQueueConnection
            |                     QuicBindingDeliverPackets
            |                     QuicBindingReceive
            |                     CxPlatSocketReceiveCoalesced
            |                     CxPlatSocketContextIoEventComplete
            |                     CxPlatProcessEvents
            |                     CxPlatWorkerThread
            |                     start_thread
            |                     thread_start
            |          
            |--0.86%--__folio_batch_add_and_move
            |          folio_add_lru
            |          |          
            |           --0.69%--filemap_add_folio
            |                     __filemap_get_folio
            |                     ext4_da_write_begin
            |                     generic_perform_write
            |                     ext4_buffered_write_iter
            |                     ext4_file_write_iter
            |                     __kernel_write_iter
            |                     dump_user_range
            |                     elf_core_dump
            |                     coredump_write
            |                     vfs_coredump
            |                     get_signal
            |                     do_signal
            |                     do_notify_resume
            |                     el0_svc
            |                     el0t_64_sync_handler
            |                     el0t_64_sync
            |                     __pthread_kill_implementation
            |                     __GI_raise (inlined)
            |                     __GI_abort (inlined)
            |                     __libc_message_impl
            |                     malloc_printerr
            |                     _int_malloc
            |                     __GI___libc_malloc (inlined)
            |                     operator new(unsigned long)
            |                     0x1dfa9c17a3b04f
            |                     0x1dfa9c17a3b04f
            |                     0x68c1babd27f497
            |                     std::_Function_handler<TqTunnelStartResult (TunnelRequest const&, int), (anonymous namespace)::RunSinglePeerClient(TqConfig const&)::{lambda(TunnelRequest const&, int)#1}>::_M_invoke(std::_Any_data const&, TunnelRequest const&, int&&)
            |                     0x12e
            |                     0x12e
            |          
             --0.44%--free_frozen_page_commit

     4.57%            70  [.] aes_gcm_dec_256_kernel                                                                                                                               -      -            
            |
            ---armv8_aes_gcm_decrypt
               ossl_gcm_aad_update
               0x5aa

     3.70%            53  [k] __slab_free                                                                                                                                          -      -            
            |          
             --3.29%--kmem_cache_free
                       |          
                        --3.25%--skb_free_head
                                  skb_release_data
                                  |          
                                   --3.21%--kfree_skb_list_reason
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 59K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 142028260902
#
# Overhead       Samples  Symbol                                                                                                                           IPC   [IPC Coverage]
# ........  ............  ...............................................................................................................................  ....................
#
    11.13%          5649  [.] TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)                                                        -      -            
            |
            ---TqLinuxRelayWorker::Run()
               0xe7c08b2d1adf
               |          
               |--5.67%--0x47e7c08b07595b
               |          0x47e7c08b07595b
               |          thread_start
               |          
                --5.11%--0x56e7c08b07595b
                          0x56e7c08b07595b
                          thread_start

     8.38%          4251  [.] TqLinuxRelayBufferPool::AcquireWorker()                                                                                      -      -            
            |
            ---TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xe7c08b2d1adf
               |          
               |--4.19%--0x47e7c08b07595b
               |          0x47e7c08b07595b
               |          thread_start
               |          
                --3.96%--0x56e7c08b07595b
                          0x56e7c08b07595b
                          thread_start

     5.94%          3013  [.] TqRelayBufferSlot::Data()                                                                                                    -      -            
            |
            ---TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xe7c08b2d1adf
               |          
               |--2.99%--0x56e7c08b07595b
               |          0x56e7c08b07595b
               |          thread_start
               |          
                --2.84%--0x47e7c08b07595b
                          0x47e7c08b07595b
                          thread_start

     5.91%          3015  [k] el0_svc                                                                                                                      -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync
               |          
                --5.55%--__GI___readv
                          __GI___readv
                          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                          TqLinuxRelayWorker::Run()
                          0xe7c08b2d1adf
                          |          
                          |--2.79%--0x47e7c08b07595b
                          |          0x47e7c08b07595b
                          |          thread_start
                          |          
                           --2.66%--0x56e7c08b07595b
                                     0x56e7c08b07595b
                                     thread_start

     4.26%          2164  [k] __import_iovec                                                                                                               -      -            
            |          
             --4.14%--import_iovec
                       |          
                        --4.14%--vfs_readv
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
                                  0xe7c08b2d1adf
                                  |          
                                  |--2.10%--0x47e7c08b07595b
                                  |          0x47e7c08b07595b
                                  |          thread_start
                                  |          
                                   --1.92%--0x56e7c08b07595b
                                             0x56e7c08b07595b
                                             thread_start

     4.21%          2138  [.] TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)                                                                    -      -            
            |
            ---TqBufferHandle::~TqBufferHandle()
               TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xe7c08b2d1adf
               |          
               |--2.08%--0x47e7c08b07595b
               |          0x47e7c08b07595b
               |          thread_start
               |          
                --2.01%--0x56e7c08b07595b
                          0x56e7c08b07595b
                          thread_start

     3.24%          1643  [k] fdget_pos                                                                                                                    -      -            
            |          
             --3.18%--do_readv
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
                       0xe7c08b2d1adf
                       |          
                       |--1.66%--0x47e7c08b07595b
                       |          0x47e7c08b07595b
                       |          thread_start
                       |          
                        --1.47%--0x56e7c08b07595b
                                  0x56e7c08b07595b
                                  thread_start

     2.99%          1519  [.] __GI___readv                                                                                                                 -      -            
            |          
             --2.93%--__GI___readv
                       TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                       TqLinuxRelayWorker::Run()
                       0xe7c08b2d1adf
                       |          
                       |--1.47%--0x47e7c08b07595b
                       |          0x47e7c08b07595b
                       |          thread_start
                       |          
                        --1.40%--0x56e7c08b07595b
                                  0x56e7c08b07595b
```

