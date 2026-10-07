// File inputs (SPEC §4.6): what the app gets after the boot checks pass.
#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace docuconf {

/// A TLS key pair in the kubernetes.io/tls layout. After a successful load
/// the certificate parses, matches the key, is valid with at least
/// minRemaining left, covers every dnsName and, with requireCA, chains to
/// ca.crt.
struct TlsKeyPair {
    bool present = false;
    std::string dir;              // the directory, after DOCUCONF_FILE_ROOT
    std::string certificate_pem;  // tls.crt
    std::string key_pem;          // tls.key
    std::string ca_pem;           // ca.crt, when present

    std::string certificate_path() const { return dir + "/tls.crt"; }
    std::string key_path() const { return dir + "/tls.key"; }
    std::string ca_path() const { return dir + "/ca.crt"; }
};

/// One or more PEM CA certificates.
struct CaBundle {
    bool present = false;
    std::string path;
    std::string pem;
    std::size_t certificates = 0;
};

/// A PKCS#12 or JKS keystore that opened with its password variable.
struct Keystore {
    bool present = false;
    std::string path;
    std::string data;  // the raw bytes
};

/// A text file that matched its pattern and length limits.
struct TextFile {
    bool present = false;
    std::string path;
    std::string content;
};

/// Opaque bytes within maxSize.
struct BinaryFile {
    bool present = false;
    std::string path;
    std::string data;
};

/// A structured config file (JSON, YAML or TOML), checked against T's JSON
/// Schema and bound to T with nlohmann::json's from_json.
template <class T>
struct ConfigFile {
    bool present = false;
    std::string path;
    T value{};

    const T& operator*() const { return value; }
    const T* operator->() const { return &value; }
};

}  // namespace docuconf
