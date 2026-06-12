# DGX perf 热点分析

- 时间: 2026-06-12T09:39:05+08:00
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
speed_download=338947351
```

## Client 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 9
#
# Samples: 23K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 59141648252
#
# Overhead       Samples  Symbol                                                                                                                                      IPC   [IPC Coverage]
# ........  ............  ..........................................................................................................................................  ....................
#
    16.55%          3966  [.] aes_gcm_dec_256_kernel                                                                                                                  -      -            
            |
            ---armv8_aes_gcm_decrypt
               ossl_gcm_aad_update
               0x5aa

     8.57%          1906  [k] __arch_copy_to_user                                                                                                                     -      -            
            |          
             --8.56%--simple_copy_to_iter
                       __skb_datagram_iter
                       |          
                       |--8.09%--__skb_datagram_iter
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
                        --0.47%--skb_copy_datagram_iter
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

     3.50%           903  [.] CxPlatHashtableEnumerateNext                                                                                                            -      -            
            |
            ---QuicStreamSetGetFlowControlSummary
               |          
                --3.50%--BbrCongestionControlUpdateBlockedState
                          |          
                          |--2.49%--QuicLossDetectionProcessAckBlocks
                          |          QuicLossDetectionProcessAckFrame
                          |          QuicConnRecvFrames
                          |          QuicConnRecvDatagramBatch
                          |          QuicConnRecvDatagrams
                          |          QuicConnFlushRecv
                          |          QuicConnDrainOperations
                          |          QuicWorkerProcessConnection
                          |          QuicWorkerLoop
                          |          QuicWorkerThread
                          |          start_thread
                          |          thread_start
                          |          
                           --1.01%--QuicLossDetectionOnPacketSent
                                     QuicPacketBuilderFinalize
                                     QuicSendFlush
                                     QuicConnDrainOperations
                                     QuicWorkerProcessConnection
                                     QuicWorkerLoop
                                     QuicWorkerThread
                                     start_thread
                                     thread_start

     3.39%           826  [.] __memcpy_sve                                                                                                                            -      -            
            |          
             --3.30%--QuicRecvBufferCopyIntoChunks
                       QuicRecvBufferWrite
                       QuicStreamProcessStreamFrame
                       QuicStreamRecv
                       QuicConnRecvFrames
                       QuicConnRecvDatagramBatch
                       QuicConnRecvDatagrams
                       QuicConnFlushRecv
                       QuicConnDrainOperations
                       QuicWorkerProcessConnection
                       QuicWorkerLoop
                       QuicWorkerThread
                       start_thread
                       thread_start

     2.98%           624  [.] TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)                                                                               -      -            
            |
            ---TqBufferHandle::~TqBufferHandle()
               TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xea59cd741adf
               |          
               |--2.26%--0x7cea59cd4e595b
               |          0x7cea59cd4e595b
               |          thread_start
               |          
                --0.65%--0x54ea59cd4e595b
                          0x54ea59cd4e595b
                          thread_start

     2.83%           595  [.] TqLinuxRelayBufferPool::AcquireWorker()                                                                                                 -      -            
            |
            ---TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xea59cd741adf
               |          
               |--2.08%--0x7cea59cd4e595b
               |          0x7cea59cd4e595b
               |          thread_start
               |          
                --0.68%--0x54ea59cd4e595b
                          0x54ea59cd4e595b
                          thread_start

     2.73%           656  [.] __aarch64_ldadd8_acq_rel                                                                                                                -      -            
            |          
             --2.52%--CxPlatRefDecrement
                       |          
                        --2.48%--QuicConnRecvFrames
                                  QuicConnRecvDatagramBatch
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 28K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 25948887267
#
# Overhead       Samples  Symbol                                                                                                                                   IPC   [IPC Coverage]
# ........  ............  .......................................................................................................................................  ....................
#
     1.78%           505  [k] finish_task_switch.isra.0                                                                                                            -      -            
            |          
             --1.76%--__schedule
                       schedule
                       |          
                        --1.74%--schedule_hrtimeout_range_clock
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
                                  CxPlatProcessEvents
                                  CxPlatWorkerThread
                                  start_thread
                                  thread_start

     0.52%            93  [k] el0_svc                                                                                                                              -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync

     0.51%            49  [.] aes_gcm_enc_256_kernel                                                                                                               -      -            
            |
            ---armv8_aes_gcm_encrypt
               ossl_gcm_aad_update
               0x5aa



# Samples: 215K of event 'armv8_pmuv3_1/cycles/P'
# Event count (approx.): 818884950525
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ............  ...............................................................................................................................................................................................................................................  ....................
#
    25.27%         53678  [.] __aarch64_ldadd8_relax                                                                                                                                                                                                                       -      -            
            |          
            |--13.94%--TqLinuxRelayBufferPool::AcquireWorker()
            |          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
            |          TqLinuxRelayWorker::Run()
            |          0xf667fb621adf
            |          |          
            |          |--3.48%--0x4bf667fb3c595b
            |          |          0x4bf667fb3c595b
            |          |          thread_start
            |          |          
            |          |--1.82%--0x25f667fb3c595b
            |          |          0x25f667fb3c595b
            |          |          thread_start
            |          |          
            |          |--1.80%--start_thread
            |          |          thread_start
            |          |          
            |          |--1.76%--0x4af667fb3c595b
            |          |          0x4af667fb3c595b
            |          |          thread_start
            |          |          
            |          |--1.74%--0xff667fb3c595b
            |          |          0xff667fb3c595b
            |          |          thread_start
            |          |          
            |          |--1.67%--0x1cf667fb3c595b
            |          |          0x1cf667fb3c595b
            |          |          thread_start
            |          |          
            |           --1.66%--0x46f667fb3c595b
            |                     0x46f667fb3c595b
            |                     thread_start
            |          
             --11.34%--TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)
                       TqBufferHandle::~TqBufferHandle()
                       TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                       TqLinuxRelayWorker::Run()
                       0xf667fb621adf
                       |          
                       |--2.85%--0x4bf667fb3c595b
                       |          0x4bf667fb3c595b
                       |          thread_start
                       |          
                       |--1.46%--0x25f667fb3c595b
                       |          0x25f667fb3c595b
                       |          thread_start
                       |          
                       |--1.43%--0xff667fb3c595b
                       |          0xff667fb3c595b
                       |          thread_start
                       |          
                       |--1.42%--0x1cf667fb3c595b
                       |          0x1cf667fb3c595b
                       |          thread_start
                       |          
                       |--1.40%--start_thread
                       |          thread_start
                       |          
                       |--1.40%--0x46f667fb3c595b
                       |          0x46f667fb3c595b
                       |          thread_start
                       |          
                        --1.37%--0x4af667fb3c595b
                                  0x4af667fb3c595b
                                  thread_start

     6.74%         14322  [.] TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)                                                                                                                                                                                    -      -            
            |
            ---TqBufferHandle::~TqBufferHandle()
               |          
                --6.74%--TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                          TqLinuxRelayWorker::Run()
                          0xf667fb621adf
                          |          
                          |--1.70%--0x4bf667fb3c595b
                          |          0x4bf667fb3c595b
                          |          thread_start
                          |          
                          |--0.87%--0xff667fb3c595b
                          |          0xff667fb3c595b
                          |          thread_start
                          |          
                          |--0.86%--0x25f667fb3c595b
                          |          0x25f667fb3c595b
                          |          thread_start
                          |          
                          |--0.85%--start_thread
                          |          thread_start
                          |          
                          |--0.83%--0x46f667fb3c595b
                          |          0x46f667fb3c595b
                          |          thread_start
                          |          
                          |--0.82%--0x4af667fb3c595b
                          |          0x4af667fb3c595b
                          |          thread_start
```

