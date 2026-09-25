// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The sync key layer's primitives (docs/user-data-parity.md S3) against
// known answers: the HPKE core against RFC 9180's base-mode vector for the
// Mac's exact suite (DHKEM(X25519, HKDF-SHA256) / HKDF-SHA256 /
// ChaCha20-Poly1305 — CryptoKit implements the RFC), the recovery and
// verification codes against an independent transcription of the Mac's
// Swift, and the envelope framings round trip and refuse what CryptoKit
// refuses.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "phi_key_crypto.h"

namespace keys = phi_client::keys;
using keys::Bytes;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

Bytes Hex(const std::string& hex) {
  Bytes out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

Bytes Text(const std::string& text) {
  return Bytes(text.begin(), text.end());
}

void TestHpkeKnownAnswer() {
  // RFC 9180 A.2.1 (mode 0, kem 0x20, kdf 1, aead 3), sequence 0.
  const std::string info_hex = "4f6465206f6e2061204772656369616e2055726e";
  const Bytes info_bytes = Hex(info_hex);
  const std::string info(info_bytes.begin(), info_bytes.end());
  // BoringSSL's X25519 "seed" is the ephemeral private key itself (skEm),
  // not the RFC's ikmE.
  const Bytes sk_e = Hex(
      "f4ec9b33b792c372c1d2c2063507b684ef925b8c75a42dbcbf57d63ccd381600");
  const Bytes sk_r = Hex(
      "8057991eef8f1f1af18f4a9491d16a1ce333f695d4db8e38da75975c4478e0fb");
  const Bytes pk_r = Hex(
      "4310ee97d88cc1f088a5576c77ab0cf5c3ac797f3d95139c6c84b5429c59662a");
  const Bytes enc = Hex(
      "1afa08d3dec047a643885163f1180476fa7ddb54c6a8029ea33f95796bf2ac4a");
  const Bytes aad = Hex("436f756e742d30");
  const Bytes pt = Hex(
      "4265617574792069732074727574682c20747275746820626561757479");
  const Bytes ct = Hex(
      "1c5250d8034ec2b784ba2cfd69dbdb8af406cfe3ff938e131f0def8c8b60b4db21993c"
      "62ce81883d2dd1b51a28");

  Check(keys::PublicKeyFor(sk_r) == pk_r, "X25519 public key from private");
  const std::optional<Bytes> sealed =
      keys::internal::HpkeSeal(pt, aad, pk_r, info, sk_e);
  Bytes expected = enc;
  expected.insert(expected.end(), ct.begin(), ct.end());
  Check(sealed && *sealed == expected, "HPKE seal matches RFC 9180 A.2.1");
  const std::optional<Bytes> opened =
      keys::internal::HpkeOpen(enc, ct, aad, sk_r, info);
  Check(opened && *opened == pt, "HPKE open matches RFC 9180 A.2.1");
  Check(!keys::internal::HpkeOpen(enc, ct, Bytes(), sk_r, info),
        "HPKE open refuses the wrong AAD");
}

void TestDeviceEnvelope() {
  const keys::DeviceKeyPair device = keys::GenerateDeviceKeyPair();
  const Bytes ark = keys::GenerateArk();
  Check(device.private_key.size() == 32 && device.public_key.size() == 32,
        "device key sizes");
  const std::optional<Bytes> envelope =
      keys::SealToPublicKey(ark, device.public_key);
  Check(envelope && envelope->size() == 1 + 32 + 32 + 16 &&
            (*envelope)[0] == 0x01,
        "device envelope framing [01][enc:32][ct+tag]");
  keys::CryptoError error = keys::CryptoError::kNone;
  std::optional<Bytes> opened =
      keys::OpenWithPrivateKey(*envelope, device.private_key, &error);
  Check(opened && *opened == ark && error == keys::CryptoError::kNone,
        "device envelope round trip");

  const keys::DeviceKeyPair other = keys::GenerateDeviceKeyPair();
  Check(!keys::OpenWithPrivateKey(*envelope, other.private_key, &error) &&
            error == keys::CryptoError::kDecryptFailed,
        "another device's key cannot open it");
  Bytes wrong_version = *envelope;
  wrong_version[0] = 0x02;
  Check(!keys::OpenWithPrivateKey(wrong_version, device.private_key, &error) &&
            error == keys::CryptoError::kBadVersion,
        "device envelope version byte checked");
  Check(!keys::OpenWithPrivateKey(Bytes(33, 1), device.private_key, &error) &&
            error == keys::CryptoError::kBadFormat,
        "device envelope too short");
}

void TestSymmetricEnvelope() {
  const Bytes key = keys::GenerateArk();
  const Bytes plain = Text("{\"v\":1,\"key\":\"x\",\"name\":\"Work\"}");
  const std::optional<Bytes> envelope = keys::SealWithSymmetric(plain, key);
  Check(envelope && envelope->size() == 1 + 12 + plain.size() + 16 &&
            (*envelope)[0] == 0x01,
        "symmetric envelope framing [01][nonce:12][ct][tag:16]");
  const std::optional<Bytes> again = keys::SealWithSymmetric(plain, key);
  Check(again && *again != *envelope, "symmetric nonces are fresh");
  keys::CryptoError error = keys::CryptoError::kNone;
  const std::optional<Bytes> opened =
      keys::OpenWithSymmetric(*envelope, key, &error);
  Check(opened && *opened == plain, "symmetric round trip");
  Bytes tampered = *envelope;
  tampered.back() ^= 1;
  Check(!keys::OpenWithSymmetric(tampered, key, &error) &&
            error == keys::CryptoError::kDecryptFailed,
        "symmetric tag checked");
  Check(!keys::OpenWithSymmetric(Bytes{0x01, 0x00}, key, &error) &&
            error == keys::CryptoError::kDecryptFailed,
        "symmetric envelope shorter than nonce+tag");
  Check(!keys::OpenWithSymmetric(Bytes{0x01}, key, &error) &&
            error == keys::CryptoError::kBadFormat,
        "symmetric envelope with no body");
}

void TestRecoveryKey() {
  // RFC 5869 A.1 is HKDF-SHA256 with a 42-byte output; the first 32 bytes
  // are what a 32-byte derivation with the same inputs yields.
  const Bytes ikm = Hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
  const Bytes salt = Hex("000102030405060708090a0b0c");
  Check(keys::DeriveRecoveryKey(ikm, salt).size() == 32, "recovery key size");
  Check(keys::DeriveRecoveryKey(ikm, salt) == keys::DeriveRecoveryKey(ikm, salt),
        "recovery key deterministic");
  Check(keys::DeriveRecoveryKey(ikm, salt) !=
            keys::DeriveRecoveryKey(ikm, Hex("00")),
        "recovery key depends on the salt");
}

void TestCodes() {
  Bytes counting;
  for (uint8_t i = 0; i < 16; ++i) {
    counting.push_back(i);
  }
  Check(keys::EncodeRecoveryCode(counting) ==
            "000G4-0R40M-30E20-9185G-R38E1-WY",
        "recovery code for 00..0f");
  Check(keys::EncodeRecoveryCode(Bytes(16, 0xFF)) ==
            "ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-ZZZZZ-WT",
        "recovery code for ff..ff");
  Check(keys::DecodeRecoveryCode("000G4-0R40M-30E20-9185G-R38E1-WY") ==
            std::optional<Bytes>(counting),
        "recovery code decodes");
  Check(keys::DecodeRecoveryCode("ooog4 or4om 3oe2o 9185g r38e1 wy") ==
            std::optional<Bytes>(counting),
        "recovery code: case, spaces and O→0 tolerated");
  Check(!keys::DecodeRecoveryCode("000G4-0R40M-30E20-9185G-R38E1-WZ"),
        "recovery code checksum checked");
  Check(!keys::DecodeRecoveryCode("000G4-0R40M-30E20-9185G-R38E1-W"),
        "recovery code length checked");
  Check(!keys::DecodeRecoveryCode("000G4-0R40M-30E20-9185G-R38EU-WY"),
        "recovery code: U dropped, so the length fails");
  const keys::RecoveryCode fresh = keys::GenerateRecoveryCode();
  Check(fresh.display.size() == 32 &&
            keys::DecodeRecoveryCode(fresh.display) ==
                std::optional<Bytes>(fresh.entropy),
        "a generated recovery code decodes to its entropy");

  const Bytes pk = Hex(
      "4310ee97d88cc1f088a5576c77ab0cf5c3ac797f3d95139c6c84b5429c59662a");
  Check(keys::VerificationCode(pk) == "KR3M-F74D", "verification code");
  Check(keys::DeviceKeyId(pk) == "ngdHnI2CeaQcFoA765PY6A", "device key id");
}

void TestEncodings() {
  Check(keys::Base64Encode(Text("any carnal pleas")) ==
            "YW55IGNhcm5hbCBwbGVhcw==",
        "base64 encode pads");
  Check(keys::Base64Decode("YW55IGNhcm5hbCBwbGVhcw==") ==
            std::optional<Bytes>(Text("any carnal pleas")),
        "base64 decode");
  Check(!keys::Base64Decode("YW55*"), "base64 decode refuses junk");
  Check(keys::Base64Decode("") == std::optional<Bytes>(Bytes()),
        "base64 of nothing");
  Check(keys::HexLower(Hex("00ff10ab")) == "00ff10ab", "hex");
}

}  // namespace

int main() {
  TestHpkeKnownAnswer();
  TestDeviceEnvelope();
  TestSymmetricEnvelope();
  TestRecoveryKey();
  TestCodes();
  TestEncodings();
  if (g_failures) {
    std::printf("key_crypto_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("key_crypto_test: OK\n");
  return 0;
}
