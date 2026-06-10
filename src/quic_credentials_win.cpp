#include "quic_credentials_win.h"

#if defined(_WIN32)

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include <windows.h>
#include <wincrypt.h>

struct TqQuicCredentialHolder::Impl {
    QUIC_CREDENTIAL_CONFIG Config{};
    QUIC_CERTIFICATE_HASH_STORE CertHashStore{};
    HCERTSTORE CertStore{nullptr};
    PCCERT_CONTEXT CertContext{nullptr};
    std::string TempPfxPath;
};

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool DecodeSha1Hex(const std::string& text, uint8_t out[20]) {
    std::string hex;
    hex.reserve(text.size());
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            hex.push_back(c);
        } else if (c != ':' && c != ' ' && c != '-') {
            return false;
        }
    }
    if (hex.size() != 40) {
        return false;
    }
    for (size_t i = 0; i < 20; ++i) {
        const int hi = HexValue(hex[i * 2]);
        const int lo = HexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string ResolveOpenSslExecutable() {
    char buffer[MAX_PATH]{};
    const DWORD envLen = GetEnvironmentVariableA("OPENSSL_BIN", buffer, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) {
        std::string candidate = buffer;
        if (!candidate.empty() && candidate.back() != '\\' && candidate.back() != '/') {
            candidate.push_back('\\');
        }
        candidate += "openssl.exe";
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }

    const char* candidates[] = {
        "C:\\Program Files\\OpenSSL-Win64\\bin\\openssl.exe",
        "C:\\Program Files\\Git\\usr\\bin\\openssl.exe",
    };
    for (const char* path : candidates) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    return "openssl.exe";
}

bool RunCommand(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

bool RunOpenSslPkcs12Export(const std::string& certPath, const std::string& keyPath, const std::string& pfxPath) {
    const std::string openssl = ResolveOpenSslExecutable();
    const std::string command =
        "cmd /c \"\"" + openssl + "\" pkcs12 -export -out \"" + pfxPath + "\" -inkey \"" + keyPath +
        "\" -in \"" + certPath + "\" -passout pass: -keypbe NONE -certpbe NONE -nomaciter\"";
    return RunCommand(command);
}

} // namespace

TqQuicCredentialHolder::TqQuicCredentialHolder() : Impl_(std::make_unique<Impl>()) {}

TqQuicCredentialHolder::~TqQuicCredentialHolder() {
    if (Impl_ == nullptr) {
        return;
    }
    if (Impl_->CertContext != nullptr) {
        CertFreeCertificateContext(Impl_->CertContext);
        Impl_->CertContext = nullptr;
    }
    if (Impl_->CertStore != nullptr) {
        CertCloseStore(Impl_->CertStore, FALSE);
        Impl_->CertStore = nullptr;
    }
    if (!Impl_->TempPfxPath.empty()) {
        DeleteFileA(Impl_->TempPfxPath.c_str());
        Impl_->TempPfxPath.clear();
    }
}

const QUIC_CREDENTIAL_CONFIG& TqQuicCredentialHolder::Config() const {
    return Impl_->Config;
}

bool TqQuicCredentialHolder::Build(const TqConfig& cfg, QUIC_CREDENTIAL_FLAGS flags) {
    uint8_t shaHash[20]{};
    if (DecodeSha1Hex(cfg.QuicCert, shaHash)) {
        HCERTSTORE store = CertOpenStore(
            CERT_STORE_PROV_SYSTEM_A,
            0,
            0,
            CERT_SYSTEM_STORE_CURRENT_USER,
            "MY");
        if (store == nullptr) {
            std::fprintf(stderr, "tcpquic-proxy: CertOpenStore(MY) failed, 0x%lx\n", GetLastError());
            return false;
        }

        CRYPT_HASH_BLOB hashBlob{};
        hashBlob.cbData = sizeof(shaHash);
        hashBlob.pbData = shaHash;
        PCCERT_CONTEXT found = CertFindCertificateInStore(
            store,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            0,
            CERT_FIND_SHA1_HASH,
            &hashBlob,
            nullptr);
        if (found == nullptr) {
            CertCloseStore(store, FALSE);
            std::fprintf(stderr, "tcpquic-proxy: certificate hash not found in CurrentUser MY store\n");
            return false;
        }

        DWORD keySpec = 0;
        BOOL callerFree = FALSE;
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE keyHandle = 0;
        if (!CryptAcquireCertificatePrivateKey(
                found,
                CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
                nullptr,
                &keyHandle,
                &keySpec,
                &callerFree)) {
            const DWORD error = GetLastError();
            CertFreeCertificateContext(found);
            CertCloseStore(store, FALSE);
            std::fprintf(stderr, "tcpquic-proxy: certificate private key unavailable, 0x%lx\n", error);
            return false;
        }
        if (callerFree && keyHandle != 0) {
            NCryptFreeObject(keyHandle);
        }

        Impl_->CertStore = store;
        Impl_->CertContext = found;
        Impl_->Config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
        Impl_->Config.Flags = flags;
        Impl_->Config.CertificateContext = (QUIC_CERTIFICATE*)Impl_->CertContext;
        (void)cfg.QuicKey;
        (void)cfg.QuicCa;
        return true;
    }

    char tempDir[MAX_PATH]{};
    char tempFile[MAX_PATH]{};
    if (GetTempPathA(MAX_PATH, tempDir) == 0 ||
        GetTempFileNameA(tempDir, "tqp", 0, tempFile) == 0) {
        return false;
    }

    Impl_->TempPfxPath = std::string(tempFile) + ".pfx";
    DeleteFileA(tempFile);

    if (!RunOpenSslPkcs12Export(cfg.QuicCert, cfg.QuicKey, Impl_->TempPfxPath)) {
        std::fprintf(stderr, "tcpquic-proxy: failed to export Windows PFX from PEM credentials\n");
        return false;
    }

    std::vector<uint8_t> pfxBytes;
    {
        std::ifstream input(Impl_->TempPfxPath, std::ios::binary);
        if (!input) {
            return false;
        }
        pfxBytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    if (pfxBytes.empty()) {
        return false;
    }

    CRYPT_DATA_BLOB blob{};
    blob.cbData = static_cast<DWORD>(pfxBytes.size());
    blob.pbData = pfxBytes.data();
    HCERTSTORE store = PFXImportCertStore(&blob, L"", CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
    if (store == nullptr) {
        std::fprintf(stderr, "tcpquic-proxy: PFXImportCertStore failed, 0x%lx\n", GetLastError());
        return false;
    }

    PCCERT_CONTEXT found = CertFindCertificateInStore(
        store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_FIND_HAS_PRIVATE_KEY,
        nullptr,
        nullptr);
    if (found == nullptr) {
        CertCloseStore(store, FALSE);
        std::fprintf(stderr, "tcpquic-proxy: certificate with private key not found in PFX\n");
        return false;
    }

    PCCERT_CONTEXT context = CertDuplicateCertificateContext(found);
    CertFreeCertificateContext(found);
    if (context == nullptr) {
        CertCloseStore(store, FALSE);
        return false;
    }

    Impl_->CertStore = store;
    Impl_->CertContext = context;

    Impl_->Config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
    Impl_->Config.Flags = flags;
    Impl_->Config.CertificateContext = (QUIC_CERTIFICATE*)Impl_->CertContext;
    (void)cfg.QuicCa;
    return true;
}

#endif
