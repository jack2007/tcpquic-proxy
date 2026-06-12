# DGX perf 热点分析

- 时间: 2026-06-12T09:45:42+08:00
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
speed_download=657749480
```

## Client 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 6
#
# Samples: 17K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 45750896582
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                                                            IPC   [IPC Coverage]
# ........  ............  ................................................................................................................................................................................................................................................................................  ....................
#
    23.74%          4088  [k] __arch_copy_to_user                                                                                                                                                                                                                                                           -      -            
            |          
             --23.73%--simple_copy_to_iter
                       __skb_datagram_iter
                       |          
                       |--22.99%--__skb_datagram_iter
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
                        --0.74%--skb_copy_datagram_iter
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

     6.87%          1178  [k] __slab_free                                                                                                                                                                                                                                                                   -      -            
            |          
            |--6.35%--kmem_cache_free
            |          |          
            |           --6.28%--skb_free_head
            |                     skb_release_data
            |                     |          
            |                      --6.18%--kfree_skb_list_reason
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
             --0.41%--kmem_cache_free_bulk.part.0
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

     4.37%           698  [.] aes_gcm_dec_256_kernel                                                                                                                                                                                                                                                        -      -            
            |
            ---armv8_aes_gcm_decrypt
               ossl_gcm_aad_update
               0x5aa

     4.15%           705  [k] check_heap_object                                                                                                                                                                                                                                                             -      -            
            |          
             --4.00%--__check_object_size.part.0
                       __check_object_size
                       |          
                        --3.96%--simple_copy_to_iter
                                  __skb_datagram_iter
                                  |          
                                   --3.73%--__skb_datagram_iter
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
```

## Server 热点 Top
```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 70K of event 'armv8_pmuv3_0/cycles/P'
# Event count (approx.): 113254316417
#
# Overhead       Samples  Symbol                                                                                                                                                                                                                                                                             IPC   [IPC Coverage]
# ........  ............  .................................................................................................................................................................................................................................................................................  ....................
#
    23.94%         10564  [.] aes_gcm_enc_256_kernel                                                                                                                                                                                                                                                         -      -            
            |
            ---armv8_aes_gcm_encrypt
               ossl_gcm_aad_update
               |          
                --23.93%--0x5aa

    10.08%          4433  [.] CxPlatHashtableEnumerateNext                                                                                                                                                                                                                                                   -      -            
            |
            ---QuicStreamSetGetFlowControlSummary
               |          
                --10.04%--BbrCongestionControlUpdateBlockedState
                          |          
                           --10.01%--QuicLossDetectionOnPacketSent
                                     QuicPacketBuilderFinalize
                                     QuicSendFlush
                                     QuicConnDrainOperations
                                     QuicWorkerProcessConnection
                                     QuicWorkerLoop
                                     QuicWorkerThread
                                     start_thread
                                     thread_start

     2.29%          1009  [k] __arch_copy_from_user                                                                                                                                                                                                                                                          -      -            
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

     1.96%           858  [.] QuicStreamSetGetFlowControlSummary                                                                                                                                                                                                                                             -      -            
            |          
             --1.95%--BbrCongestionControlUpdateBlockedState
                       |          
                        --1.92%--QuicLossDetectionOnPacketSent
                                  QuicPacketBuilderFinalize
                                  QuicSendFlush
                                  QuicConnDrainOperations
                                  QuicWorkerProcessConnection
                                  QuicWorkerLoop
                                  QuicWorkerThread
                                  start_thread
                                  thread_start

     1.86%           823  [.] __memcpy_sve                                                                                                                                                                                                                                                                   -      -            
            |          
             --1.68%--QuicStreamCopyFromSendRequests
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

     1.33%           592  [.] 0x0000000000000578                                                                                                                                                                                                                                                             -      -            
            |
            ---__kernel_clock_gettime

     1.29%           584  [k] handle_softirqs                                                                                                                                                                                                                                                                -      -            
            |
            ---__do_softirq
               ____do_softirq
               call_on_irq_stack
               do_softirq_own_stack
               |          
                --1.29%--__irq_exit_rcu
                          irq_exit_rcu
                          |          
                           --1.24%--el0_interrupt
                                     __el0_irq_handler_common
                                     el0t_64_irq_handler
                                     el0t_64_irq
                                     |          
                                      --0.52%--aes_gcm_enc_256_kernel
                                                armv8_aes_gcm_encrypt
                                                ossl_gcm_aad_update
                                                0x5aa

     1.16%           517  [.] QuicPacketBuilderFinalize                                                                                                                                                                                                                                                      -      -            
            |
            ---QuicSendFlush
               |          
                --1.16%--QuicConnDrainOperations
                          QuicWorkerProcessConnection
                          QuicWorkerLoop
                          QuicWorkerThread
                          start_thread
                          thread_start

     0.98%           707  [k] _raw_spin_unlock_irqrestore                                                                                                                                                                                                                                                    -      -            
     0.78%           408  [.] __aarch64_cas4_acq                                                                                                                                                                                                                                                             -      -            
            |          
             --0.41%--lll_mutex_lock_optimized (inlined)
                       ___pthread_mutex_lock (inlined)

     0.77%           341  [.] CRYPTO_gcm128_encrypt_ctr32                                                                                                                                                                                                                                                    -      -            
            |
            ---generic_aes_gcm_cipher_update
               gcm_cipher_internal
               ossl_gcm_stream_update
               EVP_EncryptUpdate
               CxPlatEncrypt
               QuicPacketBuilderFinalize
               QuicSendFlush
               |          
                --0.77%--QuicConnDrainOperations
                          QuicWorkerProcessConnection
```

