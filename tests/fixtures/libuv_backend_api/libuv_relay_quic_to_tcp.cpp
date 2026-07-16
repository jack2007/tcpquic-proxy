// Audited minimal excerpt from the completed libuv-q2t production lane.
void TqUvQueueQuicToTcpFixture(
    TqUvRelayState& relay,
    const TqDecompressResult& result) {
    TqBufferAcquireFailure failure{};
    auto owner = TqAllocateRelayBuffer(
        &relay.BufferBudget, result.OutputProduced, &failure);
}
