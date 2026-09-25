// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The sync key layer (docs/user-data-parity.md S3) against an in-memory
// /keys/v1 that keeps the server's rules (409 on a second bootstrap, a
// revoked fingerprint refused forever, the last active device not
// revocable, first writer wins on a profile key): the Mac's
// AccountKeyManagerTests / ProfileKeyManagerTests / SyncKeyControllerTests
// scenarios, device to device.

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "json_lite.h"
#include "phi_sync_keys.h"

namespace keys = phi_client::keys;
using keys::Bytes;
using keys::KeyErrorKind;
using keys::KeyStatus;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

// --- The fake server -----------------------------------------------------------

struct FakeServer : keys::KeyApi {
  struct Device {
    Bytes public_key;
    std::string name;
    std::string platform;
    std::string status = "active";
    Bytes envelope;
    int64_t created_at_ms = 0;
  };
  std::optional<keys::AccountKeyState> account;
  std::map<std::string, Device> devices;
  std::map<std::string, keys::JoinRequest> joins;
  std::map<std::string, Bytes> profiles;
  std::vector<std::string> profile_order;
  int64_t clock_ms = 1'000'000;
  int next_join = 1;
  int fail_post_device = 0;   // the next N PostDevice calls answer 500
  bool fail_list_profiles = false;

  KeyStatus ListDevices(std::vector<keys::AccountDevice>* out) override {
    out->clear();
    for (const auto& [id, device] : devices) {
      out->push_back({id, device.name, device.platform, device.status,
                      device.created_at_ms, std::nullopt});
    }
    return KeyStatus::Ok();
  }
  KeyStatus PutAccount(const Bytes& salt, const std::string& kdf_version,
                       const std::string& kdf_params_json,
                       const Bytes& recovery_envelope, bool* created) override {
    if (account) {
      *created = false;
      return KeyStatus::Ok();
    }
    account = keys::AccountKeyState{salt, kdf_version, kdf_params_json,
                                    recovery_envelope, 1};
    *created = true;
    return KeyStatus::Ok();
  }
  KeyStatus GetAccount(std::optional<keys::AccountKeyState>* out) override {
    *out = account;
    return KeyStatus::Ok();
  }
  KeyStatus PostDevice(const std::string& id, const Bytes& public_key,
                       const std::string& name, const std::string& platform,
                       const Bytes& envelope) override {
    if (fail_post_device > 0) {
      --fail_post_device;
      return KeyStatus::Http(500, "boom");
    }
    const auto it = devices.find(id);
    if (it != devices.end() && it->second.status == "revoked") {
      return KeyStatus::Http(409, "{\"error\":\"device_revoked\"}");
    }
    Device& device = devices[id];
    device.public_key = public_key;
    device.name = name;
    device.platform = platform;
    device.status = "active";
    device.envelope = envelope;
    device.created_at_ms = clock_ms;
    return KeyStatus::Ok();
  }
  KeyStatus GetDeviceEnvelope(const std::string& id,
                              std::optional<Bytes>* out) override {
    const auto it = devices.find(id);
    if (it == devices.end() || it->second.status != "active") {
      out->reset();
    } else {
      *out = it->second.envelope;
    }
    return KeyStatus::Ok();
  }
  KeyStatus RevokeDevice(const std::string& id) override {
    int active = 0;
    for (const auto& [other, device] : devices) {
      active += device.status == "active";
    }
    if (active <= 1) {
      return KeyStatus::Error(KeyErrorKind::kLastActiveDevice);
    }
    devices[id].status = "revoked";
    return KeyStatus::Ok();
  }
  KeyStatus PostJoinRequest(const Bytes& public_key, const std::string& name,
                            const std::string& platform,
                            std::string* request_id) override {
    int pending = 0;
    for (const auto& [id, join] : joins) {
      pending += join.status == "pending";
    }
    if (pending >= 5) {
      return KeyStatus::Error(KeyErrorKind::kTooManyPending);
    }
    keys::JoinRequest join;
    join.request_id = "join-" + std::to_string(next_join++);
    join.requesting_public_key = public_key;
    join.name = name;
    join.platform = platform;
    join.status = "pending";
    join.created_at_ms = clock_ms;
    joins[join.request_id] = join;
    *request_id = join.request_id;
    return KeyStatus::Ok();
  }
  KeyStatus ListPendingJoinRequests(
      std::vector<keys::JoinRequestSummary>* out) override {
    out->clear();
    for (const auto& [id, join] : joins) {
      if (join.status == "pending") {
        out->push_back(join);
      }
    }
    return KeyStatus::Ok();
  }
  KeyStatus GetJoinRequest(const std::string& id,
                           keys::JoinRequest* out) override {
    const auto it = joins.find(id);
    if (it == joins.end()) {
      return KeyStatus::Error(KeyErrorKind::kNotFound);
    }
    *out = it->second;
    return KeyStatus::Ok();
  }
  KeyStatus ApproveJoinRequest(const std::string& id, const Bytes& envelope,
                               const std::string& resolved_by) override {
    auto it = joins.find(id);
    if (it == joins.end()) {
      return KeyStatus::Error(KeyErrorKind::kNotFound);
    }
    if (it->second.status != "pending") {
      return KeyStatus::Error(KeyErrorKind::kNotPending);
    }
    it->second.status = "approved";
    it->second.granted_ark_envelope = envelope;
    it->second.resolved_by_device_key_id = resolved_by;
    return KeyStatus::Ok();
  }
  KeyStatus DenyJoinRequest(const std::string& id) override {
    auto it = joins.find(id);
    if (it == joins.end()) {
      return KeyStatus::Error(KeyErrorKind::kNotFound);
    }
    if (it->second.status != "pending") {
      return KeyStatus::Error(KeyErrorKind::kNotPending);
    }
    it->second.status = "denied";
    return KeyStatus::Ok();
  }
  KeyStatus ListProfiles(std::vector<keys::ProfileSummary>* out) override {
    if (fail_list_profiles) {
      return KeyStatus::Error(KeyErrorKind::kTransport);
    }
    out->clear();
    for (const std::string& uuid : profile_order) {
      out->push_back({uuid, true, clock_ms});
    }
    return KeyStatus::Ok();
  }
  KeyStatus GetProfileKey(const std::string& uuid,
                          std::optional<Bytes>* envelope) override {
    const auto it = profiles.find(uuid);
    if (it == profiles.end()) {
      envelope->reset();
    } else {
      *envelope = it->second;
    }
    return KeyStatus::Ok();
  }
  KeyStatus PutProfileKey(const std::string& uuid, const Bytes& envelope,
                          bool* created) override {
    if (profiles.count(uuid)) {
      *created = false;
      return KeyStatus::Ok();
    }
    profiles[uuid] = envelope;
    profile_order.push_back(uuid);
    *created = true;
    return KeyStatus::Ok();
  }
  KeyStatus GetDomainKey(const std::string& domain,
                         std::optional<Bytes>* out) override {
    ++domain_gets;
    const auto it = domains.find(domain);
    if (it == domains.end() || hide_domain_once) {
      hide_domain_once = false;  // the other device's PUT lands after this GET
      out->reset();
    } else {
      *out = it->second;
    }
    return KeyStatus::Ok();
  }
  KeyStatus PutDomainKey(const std::string& domain, const Bytes& envelope,
                         bool* created) override {
    *created = !domains.count(domain);
    if (*created) {
      domains[domain] = envelope;
    }
    return KeyStatus::Ok();
  }
  std::map<std::string, Bytes> domains;
  bool hide_domain_once = false;
  int domain_gets = 0;
};

struct MemoryDeviceKeys : keys::DeviceKeyStore {
  std::optional<keys::DeviceKeyPair> pair;
  KeyStatus LoadOrCreate(keys::DeviceKeyPair* out) override {
    if (!pair) {
      pair = keys::GenerateDeviceKeyPair();
    }
    *out = *pair;
    return KeyStatus::Ok();
  }
  KeyStatus Rotate() override {
    pair = keys::GenerateDeviceKeyPair();
    return KeyStatus::Ok();
  }
};

struct MemoryState : keys::KeyStateStore {
  std::map<std::string, std::string> mappings;
  std::map<std::string, Bytes> parked;
  std::optional<keys::EnrollmentRecord> enrollment;
  std::map<std::string, std::string> ProfileMappings() const override {
    return mappings;
  }
  bool SetProfileMappings(const std::map<std::string, std::string>& m) override {
    mappings = m;
    return true;
  }
  std::optional<Bytes> PendingRegistration(const std::string& id) const override {
    const auto it = parked.find(id);
    return it == parked.end() ? std::nullopt : std::optional<Bytes>(it->second);
  }
  void SetPendingRegistration(const std::string& id, const Bytes& e) override {
    parked[id] = e;
  }
  void ClearPendingRegistration(const std::string& id) override {
    parked.erase(id);
  }
  std::optional<keys::EnrollmentRecord> Enrollment() const override {
    return enrollment;
  }
  bool SetEnrollment(const keys::EnrollmentRecord& record) override {
    enrollment = record;
    return true;
  }
};

struct TestCodec : keys::ProfilePayloadCodec {
  std::string Encode(const Bytes& key, const std::string& name) const override {
    return "{\"v\":1,\"key\":\"" + keys::Base64Encode(key) + "\",\"name\":\"" +
           name + "\"}";
  }
  bool Decode(const std::string& json, Bytes* key,
              std::string* name) const override {
    const json_lite::Value value = json_lite::Parser(json).Parse();
    if (value["v"].text != "1") {
      return false;
    }
    const std::optional<Bytes> decoded = keys::Base64Decode(value["key"].str());
    if (!decoded) {
      return false;
    }
    *key = *decoded;
    *name = value["name"].is_null() ? std::string() : value["name"].str();
    return true;
  }
};

// One device: its key store, its account-scoped state and the managers.
struct Device {
  Device(FakeServer& server, const std::string& name)
      : manager(server, device_keys, state, {name, "windows"}),
        approvals(server, manager),
        profiles(server, manager, state, codec),
        core(manager, profiles, state) {}
  MemoryDeviceKeys device_keys;
  MemoryState state;
  TestCodec codec;
  keys::AccountKeyManager manager;
  keys::DeviceApprovalService approvals;
  keys::ProfileKeyManager profiles;
  keys::SyncKeyCore core;
  std::string Id() {
    std::string id;
    manager.DeviceKeyId(&id);
    return id;
  }
};

const std::vector<keys::LocalProfile> kDefaultOnly = {{"Default", "Your Phi"}};

// --- Scenarios -------------------------------------------------------------------

void TestBootstrapAndSilentUnlock() {
  FakeServer server;
  Device a(server, "Box A");
  int unlocked = 0;
  a.manager.set_on_unlocked([&] { ++unlocked; });
  std::string code;
  Check(a.manager.Bootstrap(&code).ok(), "bootstrap");
  Check(keys::DecodeRecoveryCode(code).has_value(), "bootstrap returns a code");
  Check(a.manager.current_ark().has_value() && unlocked == 1,
        "bootstrap unlocks and announces once");
  Check(server.devices.count(a.Id()) && server.devices[a.Id()].platform ==
                                            "windows",
        "bootstrap registers this device");
  std::string again;
  Check(a.manager.Bootstrap(&again).kind == KeyErrorKind::kAlreadyInitialized,
        "a second bootstrap is refused");

  // A relaunch: same device key, fresh manager, silent unlock.
  keys::AccountKeyManager relaunched(server, a.device_keys, a.state,
                                     {"Box A", "windows"});
  keys::UnlockResult result = keys::UnlockResult::kNeedsJoin;
  Check(relaunched.UnlockAtStartup(&result).ok() &&
            result == keys::UnlockResult::kUnlocked &&
            relaunched.current_ark() == a.manager.current_ark(),
        "silent unlock opens this device's envelope");

  Device b(server, "Box B");
  Check(b.manager.UnlockAtStartup(&result).ok() &&
            result == keys::UnlockResult::kNeedsJoin,
        "an unregistered device needs to join");
  bool exists = false;
  Check(b.manager.AccountExists(&exists).ok() && exists, "account exists");
}

void TestParkedRegistration() {
  FakeServer server;
  Device a(server, "Box A");
  server.fail_post_device = 1;
  std::string code;
  Check(a.manager.Bootstrap(&code).ok() && !code.empty(),
        "bootstrap still returns the code when registration fails");
  Check(a.state.PendingRegistration(a.Id()).has_value() &&
            !server.devices.count(a.Id()),
        "the failed registration is parked");
  keys::AccountKeyManager relaunched(server, a.device_keys, a.state,
                                     {"Box A", "windows"});
  keys::UnlockResult result = keys::UnlockResult::kNeedsJoin;
  Check(relaunched.UnlockAtStartup(&result).ok() &&
            result == keys::UnlockResult::kUnlocked &&
            server.devices.count(a.Id()) &&
            !a.state.PendingRegistration(a.Id()).has_value(),
        "the next unlock finishes the parked registration");
}

void TestRecoveryJoin() {
  FakeServer server;
  Device a(server, "Box A");
  std::string code;
  a.manager.Bootstrap(&code);
  Device b(server, "Box B");
  Check(b.manager.JoinWithRecoveryCode("not a code").kind ==
            KeyErrorKind::kBadRecoveryCode,
        "a malformed code is refused");
  const std::string other =
      keys::EncodeRecoveryCode(keys::RandomBytes(keys::kRecoveryEntropyBytes));
  Check(b.manager.JoinWithRecoveryCode(other).kind ==
            KeyErrorKind::kBadRecoveryCode,
        "a well-formed wrong code is refused");
  Check(!b.manager.current_ark(), "no unlock on a wrong code");
  Check(b.manager.JoinWithRecoveryCode(code).ok() &&
            b.manager.current_ark() == a.manager.current_ark() &&
            server.devices.count(b.Id()),
        "the recovery code opens the same ARK and registers the device");
}

void TestApprovalJoin() {
  FakeServer server;
  Device a(server, "Box A");
  std::string code;
  a.manager.Bootstrap(&code);
  Device b(server, "Box B");
  keys::JoinTicket ticket;
  Check(b.manager.RequestJoinApproval(&ticket).ok(), "join request");
  Bytes b_public;
  b.manager.DevicePublicKey(&b_public);
  Check(ticket.verification_code == keys::VerificationCode(b_public),
        "the requester shows its key's verification code");
  // A retry withdraws this device's own stale ticket first.
  keys::JoinTicket retry;
  Check(b.manager.RequestJoinApproval(&retry).ok() &&
            server.joins[ticket.request_id].status == "denied" &&
            retry.verification_code == ticket.verification_code,
        "a new request withdraws the stale one; same key, same code");

  keys::JoinPoll poll;
  Check(b.manager.PollJoin(retry.request_id, nullptr, &poll).ok() &&
            poll.state == keys::JoinPollState::kPending &&
            poll.deadline_ms == server.clock_ms + keys::kJoinRequestLifetimeMs,
        "pending until approved, deadline created_at + 15 min");

  std::vector<keys::PendingApproval> pending;
  Check(a.approvals.ListPendingApprovals(&pending).ok() && pending.size() == 1 &&
            pending[0].verification_code == ticket.verification_code,
        "the approver sees the same verification code");
  Check(a.approvals.Approve(pending[0]).ok(), "approve");
  Check(server.joins[retry.request_id].resolved_by_device_key_id == a.Id(),
        "the approval names the approver");

  Check(b.manager.PollJoin(retry.request_id, [] { return true; }, &poll).kind ==
                KeyErrorKind::kCancelled &&
            !b.manager.current_ark(),
        "a withdrawn flow never consumes the approval");
  Check(b.manager.PollJoin(retry.request_id, nullptr, &poll).ok() &&
            poll.state == keys::JoinPollState::kApproved &&
            b.manager.current_ark() == a.manager.current_ark() &&
            server.devices.count(b.Id()),
        "the approved device opens the granted ARK and registers");

  Device c(server, "Box C");
  keys::JoinTicket denied;
  c.manager.RequestJoinApproval(&denied);
  a.approvals.Deny(denied.request_id);
  Check(c.manager.PollJoin(denied.request_id, nullptr, &poll).ok() &&
            poll.state == keys::JoinPollState::kDenied,
        "a denied request reads denied");
  Check(c.manager.CancelJoinApproval(denied.request_id).ok(),
        "withdrawing a terminal request is fine");

  Device locked(server, "Locked");
  Check(locked.approvals.Approve(pending[0]).kind == KeyErrorKind::kNotUnlocked,
        "a locked device cannot approve");
}

void TestRevokedFingerprintRotates() {
  FakeServer server;
  Device a(server, "Box A");
  std::string code;
  a.manager.Bootstrap(&code);
  Device b(server, "Box B");
  b.manager.JoinWithRecoveryCode(code);
  const std::string old_id = b.Id();
  server.devices[old_id].status = "revoked";
  b.manager.DiscardArk();
  Check(b.manager.JoinWithRecoveryCode(code).ok() && b.Id() != old_id &&
            server.devices[b.Id()].status == "active",
        "a revoked fingerprint is rotated and the join retried once");
}

void TestProfilesAcrossDevices() {
  FakeServer server;
  Device a(server, "Box A");
  std::string code;
  a.manager.Bootstrap(&code);

  // Not paired yet: nothing is registered and nothing is exposed.
  a.core.ResolveMappings(kDefaultOnly);
  Check(server.profiles.empty() && a.core.ExposedKeys().empty() &&
            a.core.needs_pairing(),
        "an unpaired device registers nothing");

  // The first device's wizard: register Default, complete, resolve.
  keys::PairingCandidates candidates;
  Check(a.core.StartPairing(kDefaultOnly, &candidates).ok() &&
            candidates.locals.size() == 1 && candidates.remotes.empty(),
        "first device: one local, no remotes");
  Check(a.core.ApplyDecisions({{keys::PairingDecision::Kind::kRegisterNew,
                                "Default", "", "Your Phi"}},
                              kDefaultOnly)
                .ok() &&
            a.core.IsPaired(),
        "the wizard completes enrollment");
  const std::map<std::string, keys::ResolvedKey> a_keys = a.core.ExposedKeys();
  Check(a_keys.size() == 1 && a_keys.at("Default").passphrase.size() == 64 &&
            !a.core.needs_pairing() &&
            a.core.last_outcome() == keys::MappingsOutcome::kMeasured,
        "the paired first device exposes Default's key");

  // The second device joins and adopts the account's profile.
  Device b(server, "Box B");
  b.manager.JoinWithRecoveryCode(code);
  Check(b.core.StartPairing(kDefaultOnly, &candidates).ok() &&
            candidates.locals.size() == 1 && candidates.remotes.size() == 1 &&
            candidates.remotes[0].name == std::optional<std::string>("Your Phi"),
        "second device: its Default and the account's named profile");
  Check(b.core.ApplyDecisions(
                  {{keys::PairingDecision::Kind::kAdopt, "Default",
                    candidates.remotes[0].uuid, ""}},
                  kDefaultOnly)
                .ok(),
        "adopt");
  const std::map<std::string, keys::ResolvedKey> b_keys = b.core.ExposedKeys();
  Check(b_keys.size() == 1 && b_keys.at("Default").uuid == a_keys.at("Default").uuid &&
            b_keys.at("Default").passphrase == a_keys.at("Default").passphrase,
        "both devices hand Chromium the same uuid and passphrase");

  // A later profile on A registers automatically (every remote claimed).
  const std::vector<keys::LocalProfile> two = {{"Default", "Your Phi"},
                                               {"Profile 1", "Work"}};
  a.core.ResolveMappings(two);
  Check(a.core.ExposedKeys().size() == 2 && server.profiles.size() == 2,
        "a paired device registers a new local profile");
  // B has one unmapped local and one unclaimed remote: 1:1 adoption.
  const std::vector<keys::LocalProfile> b_two = {{"Default", "Your Phi"},
                                                 {"Profile 2", "Job"}};
  b.core.ResolveMappings(b_two);
  Check(b.core.ExposedKeys().size() == 2 &&
            b.core.ExposedKeys().at("Profile 2").uuid ==
                a.core.ExposedKeys().at("Profile 1").uuid,
        "one unmapped local + one unclaimed remote adopt each other");

  // An unknown remote set holds everything.
  server.fail_list_profiles = true;
  b.core.ResolveMappings(b_two);
  Check(b.core.last_outcome() == keys::MappingsOutcome::kHeld &&
            b.core.ExposedKeys().size() == 2,
        "a failed listing holds the cache and the predicates");
  server.fail_list_profiles = false;

  // A mismatched enrollment (another device id) is not paired.
  b.state.enrollment->device_key_id = "someone-else";
  Check(!b.core.IsPaired() && b.core.ExposedKeys().empty(),
        "an enrollment for another device id is not paired");
}

void TestRegisterRefusesLiveMapping() {
  FakeServer server;
  Device a(server, "Box A");
  std::string code;
  a.manager.Bootstrap(&code);
  keys::ProfileKeyRecord record;
  Check(a.profiles.RegisterLocalProfile("Default", "Your Phi", &record).ok(),
        "register");
  Check(a.profiles.RegisterLocalProfile("Default", "Your Phi", &record).kind ==
            KeyErrorKind::kAlreadyMapped,
        "registering a live mapping again is refused");
  server.profiles.clear();
  server.profile_order.clear();
  Check(a.profiles.RegisterLocalProfile("Default", "Your Phi", &record).ok() &&
            server.profiles.size() == 1,
        "a mapping whose record is definitively gone may be replaced");
}

void TestRemoveThisDevice() {
  FakeServer server;
  Device a(server, "Box A");
  std::string code;
  a.manager.Bootstrap(&code);
  a.core.ApplyDecisions({{keys::PairingDecision::Kind::kRegisterNew, "Default",
                          "", "Your Phi"}},
                        kDefaultOnly);
  Check(a.core.RemoveThisDevice().kind == KeyErrorKind::kLastActiveDevice &&
            a.core.IsPaired() && !a.state.mappings.empty(),
        "the last active device cannot leave, and nothing local changes");

  Device b(server, "Box B");
  b.manager.JoinWithRecoveryCode(code);
  const std::string old_id = a.Id();
  Check(a.core.RemoveThisDevice().ok(), "remove with another device active");
  Check(server.devices[old_id].status == "revoked" && a.Id() != old_id &&
            !a.core.IsPaired() && a.state.mappings.empty() &&
            !a.manager.current_ark() && a.core.ExposedKeys().empty(),
        "revoked remotely, key rotated, unpaired, mappings and ARK gone");
}

void TestDomainKey() {
  FakeServer server;
  Device a(server, "Box A");
  std::string code;
  a.manager.Bootstrap(&code);
  Device b(server, "Box B");
  keys::DomainKeyManager locked(server, b.manager, b.codec);
  Bytes key;
  Check(locked.DomainKey(&key).kind == KeyErrorKind::kNotUnlocked,
        "no domain key without the ARK");
  b.manager.JoinWithRecoveryCode(code);

  keys::DomainKeyManager a_domain(server, a.manager, a.codec);
  Bytes a_key;
  Check(a_domain.DomainKey(&a_key).ok() && a_key.size() == 32 &&
            server.domains.count("phi"),
        "the first use mints and escrows the domain key");
  const int gets = server.domain_gets;
  Check(a_domain.DomainKey(&key).ok() && key == a_key &&
            server.domain_gets == gets,
        "cached against the ARK it came from");

  // B raced A: its GET saw nothing, its PUT lost (409) — it adopts A's key.
  server.hide_domain_once = true;
  keys::DomainKeyManager b_domain(server, b.manager, b.codec);
  Bytes b_key;
  Check(b_domain.DomainKey(&b_key).ok() && b_key == a_key,
        "a lost first-use race adopts the winner's key");
}

}  // namespace

int main() {
  TestBootstrapAndSilentUnlock();
  TestParkedRegistration();
  TestRecoveryJoin();
  TestApprovalJoin();
  TestRevokedFingerprintRotates();
  TestProfilesAcrossDevices();
  TestRegisterRefusesLiveMapping();
  TestRemoveThisDevice();
  TestDomainKey();
  if (g_failures) {
    std::printf("sync_keys_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("sync_keys_test: OK\n");
  return 0;
}
