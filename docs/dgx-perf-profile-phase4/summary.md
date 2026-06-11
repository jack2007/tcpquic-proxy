# DGX perf 热点分析

- 时间: 2026-06-11T23:26:54+08:00
- 场景: proxy-1x1 (tcpquic-proxy 单 QUIC + 单 curl)
- 采样: 25s @ 999Hz, call-graph dwarf
- 本机: 169.254.250.230 | 对端: 169.254.59.196

## 吞吐
```
speed_download=994824394
```

## Client 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 4
#
# Samples: 24K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 68060339533
#
# Overhead       Samples  Symbol                                                                                           IPC   [IPC Coverage]
# ........  ............  ...............................................................................................  ....................
#
    11.31%          2748  [.] TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)                                    -      -            
            |
            ---TqBufferHandle::~TqBufferHandle()
               TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xf4af63471adf
               |          
                --11.25%--start_thread
                          thread_start

     8.55%          2076  [k] el0_svc                                                                                      -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync
               |          
               |--4.48%--__GI___readv
               |          __GI___readv
               |          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               |          TqLinuxRelayWorker::Run()
               |          0xf4af63471adf
               |          |          
               |           --4.47%--start_thread
               |                     thread_start
               |          
                --4.07%--__GI_epoll_pwait (inlined)
                          TqLinuxRelayWorker::Run()
                          0xf4af63471adf
                          |          
                           --4.04%--start_thread
                                     thread_start

     8.07%          1959  [.] TqLinuxRelayBufferPool::AcquireWorker()                                                      -      -            
            |
            ---TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               TqLinuxRelayWorker::Run()
               0xf4af63471adf
               |          
                --8.03%--start_thread
                          thread_start

     7.80%          1893  [.] TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)                        -      -            
            |
            ---TqLinuxRelayWorker::Run()
               0xf4af63471adf
               |          
                --7.76%--start_thread
                          thread_start

     4.19%          1017  [k] __import_iovec                                                                               -      -            
            |          
             --4.09%--import_iovec
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
                       0xf4af63471adf
                       |          
                        --4.06%--start_thread
                                  thread_start

     3.89%           943  [k] arch_counter_get_cntpct                                                                      -      -            
            |
            ---ktime_get_ts64
               |          
               |--1.97%--select_estimate_accuracy
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
               |          0xf4af63471adf
               |          |          
               |           --1.97%--start_thread
               |                     thread_start
               |          
                --1.91%--__arm64_sys_epoll_pwait
                          invoke_syscall
                          el0_svc_common.constprop.0
                          do_el0_svc
                          el0_svc
                          el0t_64_sync_handler
                          el0t_64_sync
                          __GI_epoll_pwait (inlined)
                          TqLinuxRelayWorker::Run()
                          0xf4af63471adf
                          |          
                           --1.91%--start_thread
                                     thread_start

     2.87%           698  [.] TqLinuxRelayBufferPool::PendingBytes() const                                                 -      -            
            |          
             --2.87%--TqLinuxRelayBufferPool::AcquireWorker()
                       TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                       TqLinuxRelayWorker::Run()
                       0xf4af63471adf
                       |          
                        --2.86%--start_thread
                                  thread_start

     2.48%           602  [k] get_random_u16                                                                               -      -            
            |          
             --2.38%--invoke_syscall
                       el0_svc_common.constprop.0
                       do_el0_svc
                       el0_svc
                       el0t_64_sync_handler
                       el0t_64_sync
                       |          
                       |--1.70%--__GI_epoll_pwait (inlined)
                       |          TqLinuxRelayWorker::Run()
                       |          0xf4af63471adf
                       |          start_thread
                       |          thread_start
                       |          
                        --0.68%--__GI___readv
                                  __GI___readv
                                  TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                                  TqLinuxRelayWorker::Run()
                                  0xf4af63471adf
                                  |          
                                   --0.67%--start_thread
                                             thread_start

```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 2K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 4312232409
#
# Overhead       Samples  Symbol                                            IPC   [IPC Coverage]
# ........  ............  ................................................  ....................
#
     2.52%            41  [.] aes_gcm_enc_256_kernel                        -      -            
            |
            ---armv8_aes_gcm_encrypt
               ossl_gcm_aad_update
               0x5aa

     0.82%            14  [.] CxPlatHashtableEnumerateNext                  -      -            
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



# Samples: 170K of event 'armv8_pmuv3_1/cycles/P'
# Event count (approx.): 649194859880
#
# Overhead       Samples  Symbol                                                                                           IPC   [IPC Coverage]
# ........  ............  ...............................................................................................  ....................
#
    22.81%         38712  [.] __aarch64_ldadd8_relax                                                                       -      -            
            |          
            |--12.60%--TqLinuxRelayBufferPool::AcquireWorker()
            |          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
            |          TqLinuxRelayWorker::Run()
            |          0xf16c677d1adf
            |          |          
            |          |--2.01%--0x64f16c6757595b
            |          |          0x64f16c6757595b
            |          |          thread_start
            |          |          
            |          |--1.99%--0x58f16c6757595b
            |          |          0x58f16c6757595b
            |          |          thread_start
            |          |          
            |          |--1.95%--0x5df16c6757595b
            |          |          0x5df16c6757595b
            |          |          thread_start
            |          |          
            |          |--1.79%--0x2ef16c6757595b
            |          |          0x2ef16c6757595b
            |          |          thread_start
            |          |          
            |          |--1.74%--0x62f16c6757595b
            |          |          0x62f16c6757595b
            |          |          thread_start
            |          |          
            |          |--1.59%--0x4cf16c6757595b
            |          |          0x4cf16c6757595b
            |          |          thread_start
            |          |          
            |           --1.52%--0x7f16c6757595b
            |                     0x7f16c6757595b
            |                     thread_start
            |          
             --10.21%--TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot*)
                       TqBufferHandle::~TqBufferHandle()
                       TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
                       TqLinuxRelayWorker::Run()
                       0xf16c677d1adf
                       |          
                       |--1.63%--0x58f16c6757595b
                       |          0x58f16c6757595b
                       |          thread_start
                       |          
                       |--1.61%--0x64f16c6757595b
                       |          0x64f16c6757595b
                       |          thread_start
                       |          
                       |--1.52%--0x5df16c6757595b
                       |          0x5df16c6757595b
                       |          thread_start
                       |          
                       |--1.43%--0x2ef16c6757595b
                       |          0x2ef16c6757595b
                       |          thread_start
                       |          
                       |--1.40%--0x62f16c6757595b
                       |          0x62f16c6757595b
                       |          thread_start
                       |          
                       |--1.37%--0x4cf16c6757595b
                       |          0x4cf16c6757595b
                       |          thread_start
                       |          
                        --1.26%--0x7f16c6757595b
                                  0x7f16c6757595b
                                  thread_start

     7.61%         12862  [k] el0_svc                                                                                      -      -            
            |
            ---el0t_64_sync_handler
               el0t_64_sync
               |          
               |--4.51%--__GI___readv
               |          __GI___readv
               |          TqLinuxRelayWorker::DrainTcpReadable(TqLinuxRelayWorker::RelayState*)
               |          TqLinuxRelayWorker::Run()
               |          0xf16c677d1adf
               |          |          
               |          |--0.91%--0x2ef16c6757595b
               |          |          0x2ef16c6757595b
               |          |          thread_start
               |          |          
               |          |--0.88%--0x4cf16c6757595b
               |          |          0x4cf16c6757595b
               |          |          thread_start
               |          |          
               |          |--0.76%--0x62f16c6757595b
               |          |          0x62f16c6757595b
               |          |          thread_start
               |          |          
               |          |--0.57%--0x64f16c6757595b
               |          |          0x64f16c6757595b
               |          |          thread_start
               |          |          
               |          |--0.55%--0x58f16c6757595b
               |          |          0x58f16c6757595b
               |          |          thread_start
               |          |          
               |           --0.55%--0x5df16c6757595b
               |                     0x5df16c6757595b
               |                     thread_start
               |          
                --3.10%--__GI_epoll_pwait (inlined)
                          TqLinuxRelayWorker::Run()
                          0xf16c677d1adf
                          |          
                          |--0.54%--0x58f16c6757595b
                          |          0x58f16c6757595b
                          |          thread_start
```

