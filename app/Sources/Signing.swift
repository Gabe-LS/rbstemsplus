// Signed releases: each release's payload.json carries a signature, payload.json.sig (ECDSA
// P-256 with SHA-256, DER-encoded, over payload.json's exact bytes), made by scripts/release.sh
// on the developer's Mac, where the release is built (GitHub builds nothing). The app trusts a payload.json only if one of the public keys compiled in here signed
// it: the everyday release key or the backup key, either is enough (docs/RELEASING.md).
//
// The keys come from keys/release.pub.pem and keys/backup.pub.pem: scripts/build-app.sh writes
// them into Keys.swift (compiledKeys) when it compiles the app. A test build trusts only the
// test key scripts/build.sh --test makes in build/test-keys/; a release build refuses to embed
// it. `make test` compiles an empty list: the tests pass their own keys.
import Foundation
import CryptoKit

/// A public key payload.json may be signed with, and its name for the log ("release",
/// "backup", "test").
struct TrustedKey {
    let name: String
    let key: P256.Signing.PublicKey

    /// From the base64 of the key's DER (SubjectPublicKeyInfo, as in a .pub.pem file without its
    /// first and last lines). nil unless it is a P-256 public key.
    init?(name: String, base64 der: String) {
        guard let d = Data(base64Encoded: der), let k = try? P256.Signing.PublicKey(derRepresentation: d) else { return nil }
        self.name = name; self.key = k
    }
    init(name: String, key: P256.Signing.PublicKey) { self.name = name; self.key = key }
}

/// The keys compiled into this app (Keys.swift, written by scripts/build-app.sh).
let trustedKeys: [TrustedKey] = compiledKeys.compactMap { TrustedKey(name: $0.name, base64: $0.der) }

/// The name of the key in `keys` that made `signature` over `data`, or nil if none did. The
/// signature is DER, as openssl writes it; an empty, truncated or malformed one is nil.
func signer(of data: Data, signature: Data, keys: [TrustedKey] = trustedKeys) -> String? {
    guard !signature.isEmpty, let sig = try? P256.Signing.ECDSASignature(derRepresentation: signature) else { return nil }
    return keys.first { $0.key.isValidSignature(sig, for: data) }?.name
}
