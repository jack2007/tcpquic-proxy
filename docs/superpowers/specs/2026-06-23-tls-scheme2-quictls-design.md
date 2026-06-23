# TLS Scheme 2 With Vendored quictls Design

## Summary

tcpquic-proxy will use the certificate model documented in `docs/tls-cert.md` scheme 2: the client validates the server certificate, and the server does not validate a client certificate. The implementation must behave the same on Linux, Windows, and macOS by using the vendored msquic + quictls backend only, with no system crypto/TLS backend and no Windows Schannel certificate path.

## Requirements

- QUIC payload encryption remains provided by msquic/TLS 1.3.
- Client mode must not require or load `client.crt` or `client.key`.
- Client mode must require a private CA PEM file via `--ca` or `tls.ca`.
- Client credential must use `QUIC_CREDENTIAL_TYPE_NONE`.
- Client credential flags must include `QUIC_CREDENTIAL_FLAG_CLIENT` and `QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE`.
- Client credential must set `CaCertificateFile` to the configured CA PEM path.
- Client credential must not set `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`.
- Server mode must require `server.crt` and `server.key` via `--cert/--key` or `tls.cert/tls.key`.
- Server mode must not require `--ca` or `tls.ca`.
- Server credential must use `QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE`.
- Server credential flags must not include `QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION`.
- Linux, Windows, and macOS must all use vendored quictls.
- The project must not support Windows Schannel or system OpenSSL/libcrypto for this certificate mode.
- CMake must force `QUIC_TLS_LIB=quictls` and `QUIC_USE_SYSTEM_LIBCRYPTO=OFF`.
- CLI help, runtime JSON docs, and README examples must match the new certificate requirements.

## Non-Goals

- Do not add mTLS as a runtime option in this change.
- Do not add `NO_CERTIFICATE_VALIDATION` as a supported production mode.
- Do not add OS certificate store integration.
- Do not change SOCKS5, HTTP CONNECT, ACL, compression, relay, reconnect, or speed-test behavior except where command examples include TLS arguments.

## Current State

The current code has historically treated certificate settings as symmetric:

- `src/config/config.cpp` requires `--cert`, `--key`, and `--ca` for both client and server.
- `src/protocol/quic_session.cpp` builds `QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE` for both roles on quictls paths.
- Non-Windows server credential flags include `QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION`.
- `src/platform/quic_credentials_win.cpp` supports Schannel certificate context loading, including certificate store/hash behavior. The file is not part of the desired TLS scheme and should not be compiled into the active proxy target.
- README and config-guide examples still describe mTLS in several places.

The new design replaces this with a role-specific credential model and a single supported TLS backend.

## Architecture

### TLS Backend Policy

The build and runtime policy is:

```text
Linux   -> vendored msquic + quictls + vendored crypto
Windows -> vendored msquic + quictls + vendored crypto
macOS   -> vendored msquic + quictls + vendored crypto
```

The implementation should keep source code focused on this supported path. Windows Schannel credential helper code is not part of the supported scheme and should be removed from the active build. `quic_session` should not carry a runtime credential-holder branch for Schannel; Windows should use the same PEM/quictls `TqCredentialConfig` path as Linux and macOS.

### Credential Construction

Credential construction should be role-specific:

```text
client:
  Type              = QUIC_CREDENTIAL_TYPE_NONE
  Flags             = QUIC_CREDENTIAL_FLAG_CLIENT
                    | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE
  CaCertificateFile = cfg.QuicCa

server:
  Type              = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE
  Flags             = QUIC_CREDENTIAL_FLAG_NONE
  CertificateFile   = { cfg.QuicCert, cfg.QuicKey }
```

On macOS, if msquic/quictls still requires `QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION` to make `CaCertificateFile` behave like Linux, that flag should be applied only to the client credential. It must not cause fallback to SecTrust or system roots for this project’s private CA flow.

### Configuration Semantics

CLI mode:

```text
client required:
  --peer or --client-config/--config peers
  --ca

client accepted but ignored for TLS identity:
  --cert
  --key

server required:
  --listen
  --cert
  --key

server accepted but unused for client authentication:
  --ca
```

Runtime JSON mode:

```json
{
  "tls": {
    "ca": "certs/ca.crt"
  }
}
```

is valid for client mode.

```json
{
  "tls": {
    "cert": "certs/server.crt",
    "key": "certs/server.key"
  }
}
```

is valid for server mode.

`tls.cert` and `tls.key` in client configs should remain parseable for compatibility but should not be required or used to build client credentials. `tls.ca` in server configs should remain parseable for compatibility but should not be required or used to authenticate clients.

## Error Handling

- Client mode without `--ca` or `tls.ca` fails argument validation with an error naming `--ca`.
- Server mode without `--cert` fails argument validation with an error naming `--cert`.
- Server mode without `--key` fails argument validation with an error naming `--key`.
- Client credential load failures should indicate `ConfigurationOpen/LoadCredential` failure as they do today.
- No code path should silently add `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`.

## Documentation

Update these documents:

- `docs/tls-cert.md`: already defines scheme 2 and quictls-only backend policy.
- `docs/config_guide.md`: client JSON example uses only `tls.ca`; server JSON example uses only `tls.cert/tls.key`; config key table reflects role-specific requirements.
- `docs/config_guide_cn.md`: same as English guide.
- `src/README.md`: architecture, quick start, CLI table, security section, and capability list no longer say mTLS.
- `README.md`: same certificate semantics, especially if it still contains older `--quic-*` examples.

## Testing

Add focused tests before implementation:

- Config parser accepts client CLI with `--peer ... --ca ca.crt` and empty `QuicCert/QuicKey`.
- Config parser rejects client CLI without `--ca`.
- Config parser accepts server CLI with `--listen ... --cert server.crt --key server.key` and empty `QuicCa`.
- Runtime JSON parser accepts client config with only `tls.ca`.
- Runtime JSON parser accepts server config with only `tls.cert/tls.key`.
- Usage text mentions client TLS requires `--ca` and server TLS requires `--cert/--key`.
- QUIC credential test confirms client type/flags/CA file and server type/flags/cert file.
- Source scan confirms no active use of `TqQuicCredentialHolder`, `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`, or `QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION` in proxy credential construction.

Regression verification:

```bash
rtk cmake --build build --target tcpquic_config_router_test tcpquic_quic_session_reconnect_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

If time permits, also run:

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
```

## Compatibility

The change intentionally breaks the old assumption that clients must carry a certificate and private key. Existing deployments that still pass client `--cert/--key` should continue to parse, but those files are no longer used for TLS identity. Existing server commands that pass `--ca` should continue to parse, but the file is no longer used unless a future mTLS mode is added.

## Risks

- Windows source still contains Schannel helper code. The implementation must remove it from the active target build and ensure proxy credential construction uses the same quictls PEM path as Linux/macOS.
- Root CMake currently forces `QUIC_TLS_LIB=quictls`; the implementation must also force `QUIC_USE_SYSTEM_LIBCRYPTO=OFF` so local cache values cannot switch msquic to system crypto.
- macOS certificate validation behavior must stay on the quictls PEM CA path, not the platform trust store path.
- Documentation and examples must be updated together with behavior to avoid users provisioning client private keys unnecessarily.

## Approval State

The selected certificate model is already documented in `docs/tls-cert.md`: scheme 2, client single-way validation of the server certificate. The new additional constraint is that Linux, Windows, and macOS all use vendored quictls and do not use system crypto/TLS backends.
