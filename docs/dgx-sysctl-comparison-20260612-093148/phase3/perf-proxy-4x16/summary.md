# DGX perf 热点分析

- 时间: 2026-06-12T09:35:52+08:00
- 场景: proxy-4x16 (tcpquic-proxy quic=16 + 4 并行 curl)
- QUIC 连接: 16 | 并行 curl: 4
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
# Total Lost Samples: 25
#
# Samples: 1K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 931621815
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                                        IPC   [IPC Coverage]
# ........  ............  ............................................................................................................................................................................................................................................................  ....................
#
    11.54%            30  [.] aes_gcm_dec_256_kernel                                                                                                                                                                                                                                    -      -            
            |
            ---armv8_aes_gcm_decrypt
               ossl_gcm_aad_update
               0x5aa

    10.68%            31  [k] __arch_copy_to_user                                                                                                                                                                                                                                       -      -            
            |          
             --10.54%--simple_copy_to_iter
                       __skb_datagram_iter
                       |          
                        --10.21%--__skb_datagram_iter
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

     7.96%            28  [.] __memset_zva64                                                                                                                                                                                                                                            -      -            
            |
            ---TqRelayBufferSlot::TqRelayBufferSlot(unsigned long)
               TqLinuxRelayBufferPool::Reserve(unsigned long)
               TqLinuxRelayWorker::RegisterRelayWithId(TqLinuxRelayRegistration const&)
               TqRelayStart(int, MsQuicStream*, ITqCompressor*, ITqDecompressor*, TqRelayHandle*, TqTuningConfig const&, TqCompressAlgo)
               TqTunnelContext::StartRelay(unsigned char)
               TqStartClientTunnel(MsQuicConnection*, TunnelRequest const&, int, TqConfig const&)
               std::_Function_handler<TqTunnelStartResult (TunnelRequest const&, int), (anonymous namespace)::RunSinglePeerClient(TqConfig const&)::{lambda(TunnelRequest const&, int)#1}>::_M_invoke(std::_Any_data const&, TunnelRequest const&, int&&)
               (anonymous namespace)::TqHandleHttpConnectClient(int, std::function<TqTunnelStartResult (TunnelRequest const&, int)> const&)
               TqThreadPool::WorkerLoop()
               0xfae5b1011adf
               |          
               |--2.65%--0x1dfae5b0db595b
               |          0x1dfae5b0db595b
               |          thread_start
               |          
               |--1.62%--0x54fae5b0db595b
               |          0x54fae5b0db595b
               |          thread_start
               |          
               |--1.23%--0x77fae5b0db595b
               |          0x77fae5b0db595b
               |          thread_start
               |          
               |--0.97%--0x58fae5b0db595b
               |          0x58fae5b0db595b
               |          thread_start
               |          
               |--0.69%--0x15fae5b0db595b
               |          0x15fae5b0db595b
               |          thread_start
               |          
                --0.67%--0x2bfae5b0db595b
                          0x2bfae5b0db595b
                          thread_start

     4.21%            36  [k] _raw_spin_unlock_irqrestore                                                                                                                                                                                                                               -      -            
            |          
            |--2.03%--sock_def_readable
            |          tcp_data_ready
            |          |          
            |           --2.00%--tcp_data_queue
            |                     tcp_rcv_established
            |                     tcp_v4_do_rcv
            |                     tcp_v4_rcv
            |                     ip_protocol_deliver_rcu
            |                     ip_local_deliver_finish
            |                     ip_local_deliver
            |                     ip_rcv_finish
            |                     ip_rcv
            |                     __netif_receive_skb_one_core
            |                     __netif_receive_skb
            |                     process_backlog
            |                     __napi_poll
            |                     net_rx_action
            |                     handle_softirqs
            |                     __do_softirq
            |                     ____do_softirq
            |                     call_on_irq_stack
            |                     do_softirq_own_stack
            |                     do_softirq
            |                     __local_bh_enable_ip
            |                     __dev_queue_xmit
            |                     neigh_hh_output
            |                     ip_finish_output2
            |                     __ip_finish_output
            |                     ip_finish_output
            |                     ip_output
            |                     __ip_queue_xmit
            |                     ip_queue_xmit
            |                     __tcp_transmit_skb
            |                     tcp_write_xmit
            |                     __tcp_push_pending_frames
            |                     tcp_push
            |                     tcp_sendmsg_locked
            |                     tcp_sendmsg
            |                     inet_sendmsg
            |                     __sock_sendmsg
            |                     |          
            |                      --1.89%--sock_write_iter
            |                                do_iter_readv_writev
            |                                vfs_writev
            |                                do_writev
            |                                __arm64_sys_writev
            |                                invoke_syscall
            |                                el0_svc_common.constprop.0
            |                                do_el0_svc
            |                                el0_svc
            |                                el0t_64_sync_handler
            |                                el0t_64_sync
            |                                __GI___writev
            |                                __GI___writev
            |                                TqLinuxRelayWorker::FlushTcpWrites(TqLinuxRelayWorker::RelayState*)
            |                                TqLinuxRelayWorker::ProcessQuicReceiveEvent(TqLinuxRelayEvent&)
            |                                TqLinuxRelayWorker::DrainEvents(unsigned long)
            |                                TqLinuxRelayWorker::Run()
            |                                0xfae5b1011adf
            |                                |          
            |                                 --1.53%--0x20fae5b0db595b
            |                                           0x20fae5b0db595b
            |                                           thread_start
            |          
            |--0.65%--__folio_batch_add_and_move
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 40K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 74008906218
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                                                                      IPC   [IPC Coverage]
# ........  ............  ..........................................................................................................................................................................................................................................................................................  ....................
#
    23.47%          7128  [.] aes_gcm_enc_256_kernel                                                                                                                                                                                                                                                                  -      -            
            |
            ---armv8_aes_gcm_encrypt
               ossl_gcm_aad_update
               0x5aa

     8.13%          2470  [.] CxPlatHashtableEnumerateNext                                                                                                                                                                                                                                                            -      -            
            |
            ---QuicStreamSetGetFlowControlSummary
               |          
                --8.12%--BbrCongestionControlUpdateBlockedState
                          |          
                           --8.09%--QuicLossDetectionOnPacketSent
                                     QuicPacketBuilderFinalize
                                     QuicSendFlush
                                     QuicConnDrainOperations
                                     QuicWorkerProcessConnection
                                     QuicWorkerLoop
                                     QuicWorkerThread
                                     start_thread
                                     thread_start

     2.27%           687  [k] __arch_copy_from_user                                                                                                                                                                                                                                                                   -      -            
            |          
             --2.23%--ip_generic_getfrag
                       __ip_append_data
                       ip_make_skb
                       udp_sendmsg
                       udpv6_sendmsg
                       inet6_sendmsg
                       __sock_sendmsg
                       ____sys_sendmsg
                       ___sys_sendmsg
                       __sys_sendmsg
                       __arm64_sys_sendmsg
                       invoke_syscall
                       el0_svc_common.constprop.0
                       do_el0_svc
                       el0_svc
                       el0t_64_sync_handler
                       el0t_64_sync
                       __libc_sendmsg
                       __libc_sendmsg
                       CxPlatSendDataSendSegmented
                       CxPlatSendDataSend
                       SocketSend
                       QuicBindingSend
                       QuicPacketBuilderSendBatch
                       QuicPacketBuilderFinalize
                       QuicSendFlush
                       QuicConnDrainOperations
                       QuicWorkerProcessConnection
                       QuicWorkerLoop
                       QuicWorkerThread
                       start_thread
                       thread_start

     1.78%           557  [.] __memcpy_sve                                                                                                                                                                                                                                                                            -      -            
            |          
             --1.62%--QuicStreamCopyFromSendRequests
                       QuicStreamWriteOneFrame
                       QuicStreamWriteStreamFrames
                       QuicStreamSendWrite
                       QuicSendFlush
                       QuicConnDrainOperations
                       QuicWorkerProcessConnection
                       QuicWorkerLoop
                       QuicWorkerThread
                       start_thread
                       thread_start

     1.49%           453  [.] QuicStreamSetGetFlowControlSummary                                                                                                                                                                                                                                                      -      -            
            |
            ---BbrCongestionControlUpdateBlockedState
               |          
                --1.47%--QuicLossDetectionOnPacketSent
                          QuicPacketBuilderFinalize
                          QuicSendFlush
                          QuicConnDrainOperations
                          QuicWorkerProcessConnection
                          QuicWorkerLoop
                          QuicWorkerThread
                          start_thread
                          thread_start

     1.43%           464  [.] __aarch64_cas4_acq                                                                                                                                                                                                                                                                      -      -            
            |          
             --0.81%--__GI___libc_malloc (inlined)
                       |          
                        --0.80%--QuicSentPacketPoolGetPacketMetadata
                                  QuicLossDetectionOnPacketSent
                                  QuicPacketBuilderFinalize
                                  QuicSendFlush
                                  QuicConnDrainOperations
                                  QuicWorkerProcessConnection
                                  QuicWorkerLoop
                                  QuicWorkerThread
                                  start_thread
                                  thread_start

     1.31%           406  [.] 0x0000000000000578                                                                                                                                                                                                                                                                      -      -            
            |
            ---__kernel_clock_gettime

     1.12%           356  [k] handle_softirqs                                                                                                                                                                                                                                                                         -      -            
            |
            ---__do_softirq
               ____do_softirq
               call_on_irq_stack
               do_softirq_own_stack
               __irq_exit_rcu
               irq_exit_rcu
               |          
                --1.05%--el0_interrupt
                          __el0_irq_handler_common
                          el0t_64_irq_handler
                          el0t_64_irq
                          |          
                           --0.47%--aes_gcm_enc_256_kernel
                                     armv8_aes_gcm_encrypt
                                     ossl_gcm_aad_update
                                     0x5aa

     1.09%           333  [.] QuicPacketBuilderFinalize                                                                                                                                                                                                                                                               -      -            
            |
            ---QuicSendFlush
               |          
                --1.09%--QuicConnDrainOperations
                          QuicWorkerProcessConnection
                          QuicWorkerLoop
                          QuicWorkerThread
                          start_thread
                          thread_start

     1.07%           436  [k] _raw_spin_unlock_irqrestore                                                                                                                                                                                                                                                             -      -            
            |          
             --0.52%--__folio_batch_add_and_move
                       folio_add_lru
                       folio_add_lru_vma
```

