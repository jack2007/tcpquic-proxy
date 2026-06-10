#pragma once

#include "config.h"

#include "msquic.hpp"

#include <memory>

#if defined(_WIN32)

struct TqQuicCredentialHolder {
    TqQuicCredentialHolder();
    ~TqQuicCredentialHolder();

    TqQuicCredentialHolder(const TqQuicCredentialHolder&) = delete;
    TqQuicCredentialHolder& operator=(const TqQuicCredentialHolder&) = delete;

    bool Build(const TqConfig& cfg, QUIC_CREDENTIAL_FLAGS flags);

    const QUIC_CREDENTIAL_CONFIG& Config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> Impl_;
};

#endif
