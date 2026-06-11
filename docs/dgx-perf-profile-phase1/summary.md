# DGX perf 热点分析

- 时间: 2026-06-11T16:15:40+08:00
- 场景: proxy-1x1 (tcpquic-proxy 单 QUIC + 单 curl)
- 采样: 25s @ 999Hz, call-graph dwarf
- 本机: 169.254.250.230 | 对端: 169.254.59.196

## 吞吐
```
speed_download=338369707
```

## Client 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 33
#
# Samples: 15K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 34176329726
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                                                                                                                                                  IPC   [IPC Coverage]
# ........  ............  ......................................................................................................................................................................................................................................................................................................................................................................  ....................
#
    31.99%          4745  [.] aes_gcm_dec_256_kernel                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---armv8_aes_gcm_decrypt
               ossl_gcm_aad_update
               0x5aa

     6.45%          1008  [.] CxPlatHashtableEnumerateNext                                                                                                                                                                                                                                                                                                                                        -      -            
            |
            ---QuicStreamSetGetFlowControlSummary
               BbrCongestionControlUpdateBlockedState
               |          
               |--3.41%--QuicLossDetectionOnPacketSent
               |          QuicPacketBuilderFinalize
               |          QuicSendFlush
               |          QuicConnDrainOperations
               |          QuicWorkerProcessConnection
               |          QuicWorkerLoop
               |          QuicWorkerThread
               |          start_thread
               |          thread_start
               |          
                --3.04%--QuicLossDetectionProcessAckBlocks
                          QuicLossDetectionProcessAckFrame
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

     4.89%           729  [.] __memcpy_sve                                                                                                                                                                                                                                                                                                                                                        -      -            
            |          
            |--3.14%--TqLinuxRelayWorker::CopyQuicReceiveBatchToEvent(unsigned long, QUIC_BUFFER const*, unsigned int, bool)
            |          TqLinuxRelayWorker::OnStreamEventWithBinding(MsQuicStream*, QUIC_STREAM_EVENT*, TqLinuxRelayWorker::StreamRelayBinding*)
            |          MsQuicStream::MsQuicCallback(QUIC_HANDLE*, MsQuicStream*, QUIC_STREAM_EVENT*)
            |          QuicStreamRecvFlush
            |          QuicConnDrainOperations
            |          QuicWorkerProcessConnection
            |          QuicWorkerLoop
            |          QuicWorkerThread
            |          start_thread
            |          thread_start
            |          
             --1.51%--QuicRecvBufferCopyIntoChunks
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

     2.69%           393  [.] __aarch64_ldadd8_acq_rel                                                                                                                                                                                                                                                                                                                                            -      -            
            |          
            |--2.19%--CxPlatRefDecrement
            |          |          
            |           --2.16%--QuicConnRecvFrames
            |                     QuicConnRecvDatagramBatch
            |                     QuicConnRecvDatagrams
            |                     QuicConnFlushRecv
            |                     QuicConnDrainOperations
            |                     QuicWorkerProcessConnection
            |                     QuicWorkerLoop
            |                     QuicWorkerThread
            |                     start_thread
            |                     thread_start
            |          
             --0.50%--CxPlatRefIncrement
                       |          
                        --0.46%--QuicStreamSetGetStreamForPeer
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

     2.16%           317  [.] 0x0000000000000578                                                                                                                                                                                                                                                                                                                                                  -      -            
            |
            ---__kernel_clock_gettime
               |          
                --1.93%--0x2ee0dace75bccb
                          CxPlatTimeUs64

     1.80%           240  [k] __arch_copy_to_user                                                                                                                                                                                                                                                                                                                                                 -      -            
            |          
             --1.80%--simple_copy_to_iter
                       __skb_datagram_iter
                       |          
                        --1.71%--__skb_datagram_iter
                                  skb_copy_datagram_iter
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

     1.75%           236  [.] __aarch64_cas4_acq                                                                                                                                                                                                                                                                                                                                                  -      -            
            |          
             --1.46%--lll_mutex_lock_optimized (inlined)
                       ___pthread_mutex_lock (inlined)
                       |          
                        --0.44%--TqLinuxRelayBufferPool::Acquire()
                                  |          
                                   --0.40%--TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                                             TqLinuxRelayWorker::Run()
                                             0xe0dace981adf

     1.40%           189  [.] __GI___pthread_mutex_unlock_usercnt                                                                                                                                                                                                                                                                                                                                 -      -            
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 26K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 27160918495
#
# Overhead       Samples  Symbol                                                                                                                                                                       IPC   [IPC Coverage]
# ........  ............  ...........................................................................................................................................................................  ....................
#
     1.64%           445  [k] finish_task_switch.isra.0                                                                                                                                                -      -            
            |          
             --1.62%--__schedule
                       schedule
                       |          
                        --1.61%--schedule_hrtimeout_range_clock
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
                                   --1.60%--CxPlatProcessEvents
                                             CxPlatWorkerThread
                                             start_thread
                                             thread_start

     1.44%           141  [.] aes_gcm_enc_256_kernel                                                                                                                                                   -      -            
            |
            ---armv8_aes_gcm_encrypt
               ossl_gcm_aad_update
               0x5aa

     0.79%           139  [.] __aarch64_cas4_acq                                                                                                                                                       -      -            
            |          
             --0.52%--lll_mutex_lock_optimized (inlined)
                       ___pthread_mutex_lock (inlined)

     0.61%            57  [.] CxPlatHashtableEnumerateNext                                                                                                                                             -      -            
            |
            ---QuicStreamSetGetFlowControlSummary
               |          
                --0.60%--BbrCongestionControlUpdateBlockedState
                          QuicLossDetectionOnPacketSent
                          QuicPacketBuilderFinalize
                          QuicSendFlush
                          QuicConnDrainOperations
                          QuicWorkerProcessConnection
                          QuicWorkerLoop
                          QuicWorkerThread
                          start_thread
                          thread_start

     0.53%            89  [k] el0_svc                                                                                                                                                                  -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync

     0.46%            56  [.] __GI___pthread_mutex_unlock_usercnt                                                                                                                                      -      -            
            |          
             --0.44%--__GI___pthread_mutex_unlock_usercnt

     0.43%           105  [.] __aarch64_ldadd8_sync                                                                                                                                                    -      -            
     0.41%            98  [k] fdget                                                                                                                                                                    -      -            


# Samples: 216K of event 'armv8_pmuv3_1/cycles/P'
# Event count (approx.): 826327249363
#
# Overhead       Samples  Symbol                                                                                                                                                                       IPC   [IPC Coverage]
# ........  ............  ...........................................................................................................................................................................  ....................
#
    18.56%         39401  [.] __aarch64_swp4_rel                                                                                                                                                       -      -            
            |          
            |--17.65%--lll_mutex_unlock_optimized (inlined)
            |          __GI___pthread_mutex_unlock_usercnt
            |          |          
            |          |--8.87%--TqLinuxRelayBufferPool::Acquire()
            |          |          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
            |          |          TqLinuxRelayWorker::Run()
            |          |          0xfef8b0c91adf
            |          |          |          
            |          |          |--1.14%--0x42fef8b0a3595b
            |          |          |          0x42fef8b0a3595b
            |          |          |          thread_start
            |          |          |          
            |          |          |--1.14%--0x26fef8b0a3595b
            |          |          |          0x26fef8b0a3595b
            |          |          |          thread_start
            |          |          |          
            |          |          |--1.11%--0x5afef8b0a3595b
            |          |          |          0x5afef8b0a3595b
            |          |          |          thread_start
            |          |          |          
            |          |          |--1.11%--0x69fef8b0a3595b
            |          |          |          0x69fef8b0a3595b
            |          |          |          thread_start
            |          |          |          
            |          |          |--1.10%--0x34fef8b0a3595b
            |          |          |          0x34fef8b0a3595b
            |          |          |          thread_start
            |          |          |          
            |          |          |--1.10%--0x6ffef8b0a3595b
            |          |          |          0x6ffef8b0a3595b
            |          |          |          thread_start
            |          |          |          
            |          |          |--1.10%--0x45fef8b0a3595b
            |          |          |          0x45fef8b0a3595b
            |          |          |          thread_start
            |          |          |          
            |          |           --1.08%--0x6efef8b0a3595b
            |          |                     0x6efef8b0a3595b
            |          |                     thread_start
            |          |          
            |           --8.27%--TqLinuxRelayBufferPool::Release(TqRelayBuffer*)
            |                     std::_Sp_counted_deleter<TqRelayBuffer*, TqLinuxRelayBufferPool::Acquire()::{lambda(TqRelayBuffer*)#1}, std::allocator<void>, (__gnu_cxx::_Lock_policy)2>::_M_dispose()
            |                     |          
            |                      --8.27%--TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
            |                                TqLinuxRelayWorker::Run()
            |                                0xfef8b0c91adf
            |                                |          
            |                                |--1.07%--0x42fef8b0a3595b
            |                                |          0x42fef8b0a3595b
            |                                |          thread_start
            |                                |          
            |                                |--1.06%--0x6ffef8b0a3595b
            |                                |          0x6ffef8b0a3595b
            |                                |          thread_start
            |                                |          
            |                                |--1.06%--0x6efef8b0a3595b
            |                                |          0x6efef8b0a3595b
            |                                |          thread_start
            |                                |          
            |                                |--1.04%--0x45fef8b0a3595b
            |                                |          0x45fef8b0a3595b
            |                                |          thread_start
            |                                |          
            |                                |--1.03%--0x26fef8b0a3595b
            |                                |          0x26fef8b0a3595b
            |                                |          thread_start
            |                                |          
            |                                |--1.02%--0x69fef8b0a3595b
```

