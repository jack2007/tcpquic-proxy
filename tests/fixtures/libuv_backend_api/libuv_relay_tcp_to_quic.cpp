// Audited minimal excerpts from the completed libuv-t2q production lane.
void TqUvQueueTcpToQuicFixture(TqUvRelayState& relay, std::size_t size) {
    TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
    auto input = TqAllocateRelayBuffer(
        &relay.TcpReadBufferBudget, size, &failure);
    auto direct = TqAllocateRelayBuffer(&relay.TcpReadBufferBudget, size);
}
