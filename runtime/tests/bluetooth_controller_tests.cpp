#include "bluetooth/bt_controller.h"
#include "bluetooth/bt_sdp.h"
#include "bluetooth/bt_slot_owner.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using Bluetooth::BdAddr;
using Bluetooth::Controller;
using Bluetooth::LinkState;

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

struct FakeHost : Bluetooth::RemoteHost {
    std::array<bool, Bluetooth::kRemoteSlots> attached{};
    std::array<bool, Bluetooth::kRemoteSlots> wake{};
    std::array<std::deque<Bytes>, Bluetooth::kRemoteSlots> input;
    std::vector<std::pair<size_t, Bytes>> output;
    std::array<LinkState, Bluetooth::kRemoteSlots> states{};
    std::vector<size_t> guestDisconnects;
    bool inquiry = false;

    bool IsAttached(size_t slot) const override { return attached[slot]; }
    bool ConsumeWake(size_t slot) override {
        const bool w = wake[slot];
        wake[slot] = false;
        return w;
    }
    bool PopInputReport(size_t slot, Bytes& report) override {
        if (input[slot].empty()) return false;
        report = input[slot].front();
        input[slot].pop_front();
        return true;
    }
    void SendOutputReport(size_t slot, const uint8_t* report, size_t size) override {
        output.emplace_back(slot, Bytes(report, report + size));
    }
    void OnLinkStateChanged(size_t slot, LinkState state) override { states[slot] = state; }
    void OnGuestDisconnect(size_t slot) override { guestDisconnects.push_back(slot); }
    void OnGuestInquiry(bool active) override { inquiry = active; }
    const char* DeviceName(size_t) const override { return "Nintendo RVL-CNT-01"; }
};

uint16_t Le16(const Bytes& b, size_t i) {
    return static_cast<uint16_t>(b[i] | (b[i + 1] << 8));
}

void PutLe16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

Bytes Command(uint16_t opcode, Bytes params) {
    Bytes packet;
    PutLe16(packet, opcode);
    packet.push_back(static_cast<uint8_t>(params.size()));
    packet.insert(packet.end(), params.begin(), params.end());
    return packet;
}

struct Guest {
    Controller& controller;
    uint64_t now = 1000;

    std::vector<Bytes> Run(uint16_t opcode, Bytes params = {}) {
        const Bytes packet = Command(opcode, std::move(params));
        controller.ExecuteCommand(packet.data(), packet.size(), now);
        return Events();
    }
    std::vector<Bytes> Events() {
        std::vector<Bytes> events;
        Bytes event;
        while (controller.PopEvent(event)) events.push_back(event);
        return events;
    }
    std::vector<Bytes> Acl() {
        std::vector<Bytes> packets;
        Bytes packet;
        while (controller.PopAcl(packet)) packets.push_back(packet);
        return packets;
    }
    void Tick() {
        now += 1000;
        controller.Tick(now);
    }
    void SendL2cap(uint16_t handle, uint16_t cid, const Bytes& payload) {
        Bytes packet;
        PutLe16(packet, static_cast<uint16_t>(handle | 0x2000));
        PutLe16(packet, static_cast<uint16_t>(payload.size() + 4));
        PutLe16(packet, static_cast<uint16_t>(payload.size()));
        PutLe16(packet, cid);
        packet.insert(packet.end(), payload.begin(), payload.end());
        controller.ReceiveAcl(packet.data(), packet.size(), now);
    }
    void Signal(uint16_t handle, uint8_t code, uint8_t ident, const Bytes& data) {
        Bytes payload = {code, ident};
        PutLe16(payload, static_cast<uint16_t>(data.size()));
        payload.insert(payload.end(), data.begin(), data.end());
        SendL2cap(handle, 0x0001, payload);
    }
};

bool IsCommandComplete(const Bytes& e, uint16_t opcode) {
    return e.size() >= 6 && e[0] == 0x0E && Le16(e, 3) == opcode && e[5] == 0x00;
}

bool IsCommandStatusOk(const Bytes& e, uint16_t opcode) {
    return e.size() == 6 && e[0] == 0x0F && e[2] == 0x00 && Le16(e, 4) == opcode;
}

// Splits a signalling ACL packet from the remote into (code, ident, data).
struct Signal {
    uint8_t code = 0;
    uint8_t ident = 0;
    Bytes data;
};

std::vector<Signal> Signals(const std::vector<Bytes>& acl) {
    std::vector<Signal> out;
    for (const Bytes& packet : acl) {
        if (packet.size() < 8 || Le16(packet, 6) != 0x0001) continue;
        size_t i = 8;
        while (i + 4 <= packet.size()) {
            Signal s;
            s.code = packet[i];
            s.ident = packet[i + 1];
            const uint16_t length = Le16(packet, i + 2);
            s.data.assign(packet.begin() + static_cast<std::ptrdiff_t>(i + 4),
                          packet.begin() + static_cast<std::ptrdiff_t>(i + 4 + length));
            out.push_back(s);
            i += 4 + length;
        }
    }
    return out;
}

// Opens one HID channel the remote requested and completes both configurations.
uint16_t AcceptRemoteChannel(Guest& guest, uint16_t handle, uint16_t psm, uint16_t guestCid) {
    guest.Events();
    const auto requests = Signals(guest.Acl());
    CHECK(requests.size() == 1);
    if (requests.size() != 1) return 0;
    CHECK(requests[0].code == 0x02 && Le16(requests[0].data, 0) == psm);
    const uint16_t remoteCid = Le16(requests[0].data, 2);
    Bytes rsp;
    PutLe16(rsp, guestCid);
    PutLe16(rsp, remoteCid);
    PutLe16(rsp, 0);
    PutLe16(rsp, 0);
    guest.Signal(handle, 0x03, requests[0].ident, rsp);
    const auto config = Signals(guest.Acl());
    CHECK(config.size() == 1 && config[0].code == 0x04 && Le16(config[0].data, 0) == guestCid);
    Bytes cfg;
    PutLe16(cfg, remoteCid);
    PutLe16(cfg, 0);
    cfg.insert(cfg.end(), {0x01, 0x02, 0xA0, 0x02});
    guest.Signal(handle, 0x04, 0x40, cfg);
    const auto cfgRsp = Signals(guest.Acl());
    CHECK(cfgRsp.size() == 1 && cfgRsp[0].code == 0x05 && cfgRsp[0].ident == 0x40 &&
          Le16(cfgRsp[0].data, 0) == guestCid && Le16(cfgRsp[0].data, 4) == 0);
    Bytes ourCfgRsp;
    PutLe16(ourCfgRsp, remoteCid);
    PutLe16(ourCfgRsp, 0);
    PutLe16(ourCfgRsp, 0);
    if (!config.empty()) guest.Signal(handle, 0x05, config[0].ident, ourCfgRsp);
    return remoteCid;
}

void TestBootAndConnection() {
    FakeHost host;
    Controller controller(host);
    Guest guest{controller};
    controller.Open(guest.now);

    auto e = guest.Run(0x0C03);
    CHECK(e.size() == 1 && IsCommandComplete(e[0], 0x0C03));
    e = guest.Run(0x1005);
    CHECK(e.size() == 1 && IsCommandComplete(e[0], 0x1005) && Le16(e[0], 6) == 339 && Le16(e[0], 9) == 10);
    e = guest.Run(0x1009);
    CHECK(e.size() == 1 && IsCommandComplete(e[0], 0x1009) && e[0][11] == 0xFF);
    e = guest.Run(0x1001);
    CHECK(e.size() == 1 && IsCommandComplete(e[0], 0x1001) && Le16(e[0], 7) == 0x40A7);
    e = guest.Run(0x1003);
    CHECK(e.size() == 1 && IsCommandComplete(e[0], 0x1003));
    e = guest.Run(0x0C0D, {0, 0, 0, 0, 0, 0, 1});
    CHECK(e.size() == 2 && e[0][0] == 0x15 && e[0][2] == 4 && IsCommandComplete(e[1], 0x0C0D) &&
          Le16(e[1], 8) == 4);
    Bytes slot0Key;
    if (!e.empty() && e[0].size() >= 25) slot0Key.assign(e[0].begin() + 9, e[0].begin() + 25);
    e = guest.Run(0xFC4F, {0x00});
    CHECK(e.size() == 1 && IsCommandComplete(e[0], 0xFC4F));

    // Attached remote, page scan off: nothing yet.
    host.attached[0] = true;
    host.wake[0] = true;
    guest.Tick();
    CHECK(guest.Events().empty());

    e = guest.Run(0x0C1A, {0x02});
    CHECK(e.size() == 1 && IsCommandComplete(e[0], 0x0C1A));
    guest.Tick();
    e = guest.Events();
    CHECK(e.size() == 1 && e[0][0] == 0x04 && e[0][7] == 0x00);
    const BdAddr addr = Controller::RemoteAddress(0);

    Bytes accept(addr.begin(), addr.end());
    accept.push_back(0x00);
    e = guest.Run(0x0409, accept);
    CHECK(e.size() == 3 && IsCommandStatusOk(e[0], 0x0409) && e[1][0] == 0x12 && e[2][0] == 0x03 &&
          e[2][2] == 0x00 && Le16(e[2], 3) == 0x0100);
    CHECK(host.states[0] == LinkState::Connecting);

    // Authentication with the stored key.
    e = guest.Run(0x0411, {0x00, 0x01});
    CHECK(e.size() == 2 && IsCommandStatusOk(e[0], 0x0411) && e[1][0] == 0x17);
    Bytes reply(addr.begin(), addr.end());
    reply.insert(reply.end(), slot0Key.begin(), slot0Key.end());
    e = guest.Run(0x040B, reply);
    CHECK(e.size() == 2 && IsCommandComplete(e[0], 0x040B) && e[1][0] == 0x06 && e[1][2] == 0x00);

    guest.Tick();
    const uint16_t controlCid = AcceptRemoteChannel(guest, 0x0100, 0x0011, 0x0041);
    CHECK(host.states[0] == LinkState::Connecting);
    guest.Tick();
    const uint16_t interruptCid = AcceptRemoteChannel(guest, 0x0100, 0x0013, 0x0042);
    CHECK(controlCid != 0 && interruptCid != 0);
    CHECK(host.states[0] == LinkState::Ready);

    // Input passthrough.
    host.input[0].push_back({0x30, 0x00, 0x08});
    guest.Tick();
    guest.Events();
    auto acl = guest.Acl();
    CHECK(acl.size() == 1);
    if (acl.size() == 1) {
        const Bytes& p = acl[0];
        CHECK(Le16(p, 0) == 0x2100 && Le16(p, 6) == 0x0042 && p[8] == 0xA1 && p[9] == 0x30 && p[11] == 0x08);
    }

    // Output passthrough on the interrupt channel, and a SET_REPORT on control.
    guest.SendL2cap(0x0100, interruptCid, {0xA2, 0x11, 0x10});
    guest.SendL2cap(0x0100, controlCid, {0x52, 0x15, 0x00});
    CHECK(host.output.size() == 2 && host.output[0].second == Bytes({0x11, 0x10}) &&
          host.output[1].second == Bytes({0x15, 0x00}));
    acl = guest.Acl();
    CHECK(acl.size() == 1 && acl[0].size() == 9 && acl[0][8] == 0x00);
    guest.Tick();
    e = guest.Events();
    CHECK(e.size() == 1 && e[0][0] == 0x13 && e[0][2] == 1 && Le16(e[0], 3) == 0x0100 && Le16(e[0], 5) == 2);

    // The game drops the remote: a real one powers off.
    e = guest.Run(0x0406, {0x00, 0x01, 0x13});
    CHECK(e.size() == 2 && IsCommandStatusOk(e[0], 0x0406) && e[1][0] == 0x05 && e[1][5] == 0x16);
    CHECK(host.guestDisconnects.size() == 1 && host.states[0] == LinkState::Down);
    guest.Tick();
    CHECK(guest.Events().empty());
}

void TestInquiryAndPaging() {
    FakeHost host;
    Controller controller(host);
    Guest guest{controller};
    controller.Open(guest.now);
    guest.Run(0x0C45, {0x01});
    host.attached[1] = true;
    host.wake[1] = true;
    guest.Tick(); // consumes the wake; page scan is off
    auto e = guest.Run(0x0401, {0x00, 0x8B, 0x9E, 0x03, 0x01});
    CHECK(e.size() == 1 && IsCommandStatusOk(e[0], 0x0401) && host.inquiry);
    guest.Tick();
    e = guest.Events();
    CHECK(e.size() == 1 && e[0][0] == 0x22 && e[0][8] == 0x01);
    guest.Tick();
    e = guest.Events();
    CHECK(e.size() == 1 && e[0][0] == 0x01);
    CHECK(!host.inquiry);
    Bytes create;
    const BdAddr addr = Controller::RemoteAddress(1);
    create.insert(create.end(), addr.begin(), addr.end());
    create.insert(create.end(), {0x18, 0xCC, 0x01, 0x00, 0x00, 0x00, 0x00});
    e = guest.Run(0x0405, create);
    CHECK(e.size() == 2 && IsCommandStatusOk(e[0], 0x0405) && e[1][0] == 0x03 && e[1][2] == 0x00 &&
          Le16(e[1], 3) == 0x0101);

    // Paging a remote that is not there times out.
    const BdAddr missing = Controller::RemoteAddress(2);
    Bytes create2(missing.begin(), missing.end());
    create2.insert(create2.end(), {0x18, 0xCC, 0x01, 0x00, 0x00, 0x00, 0x00});
    e = guest.Run(0x0405, create2);
    CHECK(e.size() == 1);
    guest.now += 6'000'000;
    guest.Tick();
    e = guest.Events();
    CHECK(e.size() == 1 && e[0][0] == 0x03 && e[0][2] == 0x04);

    // The guest opens SDP itself on a paged remote.
    Bytes sdpReq;
    PutLe16(sdpReq, 0x0001);
    PutLe16(sdpReq, 0x0050);
    guest.Signal(0x0101, 0x02, 0x07, sdpReq);
    const auto rsp = Signals(guest.Acl());
    CHECK(rsp.size() == 2 && rsp[0].code == 0x03 && Le16(rsp[0].data, 4) == 0 && rsp[1].code == 0x04);
}

void TestSdp() {
    // ServiceAttributeRequest for the whole HID record.
    const Bytes request = {0x04, 0x00, 0x01, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF,
                           0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00};
    const Bytes response = Bluetooth::Sdp::HandleRequest(request.data(), request.size(), 4096);
    CHECK(response.size() > 10 && response[0] == 0x05);
    if (response.size() > 10) {
        const uint16_t count = static_cast<uint16_t>((response[5] << 8) | response[6]);
        CHECK(count == 0x01CC + 3 && response[7] == 0x36 && response[8] == 0x01 && response[9] == 0xCC);
        CHECK(response.back() == 0x00);
    }
    CHECK(Bluetooth::Sdp::WiiRemoteHidDescriptor().size() == 0xD9);

    // Continuation with a small MTU reassembles to the same list.
    Bytes assembled;
    Bytes cont = {0x00};
    for (int guard = 0; guard < 64; ++guard) {
        Bytes req = {0x04, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x40,
                     0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF};
        req.insert(req.end(), cont.begin(), cont.end());
        req[4] = static_cast<uint8_t>(req.size() - 5);
        const Bytes part = Bluetooth::Sdp::HandleRequest(req.data(), req.size(), 48);
        CHECK(part.size() <= 48 && part[0] == 0x05);
        const uint16_t count = static_cast<uint16_t>((part[5] << 8) | part[6]);
        assembled.insert(assembled.end(), part.begin() + 7, part.begin() + 7 + count);
        cont.assign(part.begin() + 7 + count, part.end());
        if (cont.size() == 1 && cont[0] == 0) break;
    }
    CHECK(assembled.size() == 0x01CC + 3);

    // Device ID record via ServiceSearchAttribute.
    const Bytes search = {0x06, 0x00, 0x03, 0x00, 0x0F, 0x35, 0x03, 0x19, 0x12, 0x00, 0xFF, 0xFF,
                          0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00};
    const Bytes found = Bluetooth::Sdp::HandleRequest(search.data(), search.size(), 4096);
    CHECK(found.size() > 12 && found[0] == 0x07 && found[7] == 0x35 && found[9] == 0x36 && found[10] == 0x00 &&
          found[11] == 0x5A);
}

// Same routing as VirtualRemote's CompositeHost: slot 0 is the real remote when one is attached, else the
// virtual one, and SlotOwnership forces a reconnect when that changes under a live link.
struct SwitchingHost : Bluetooth::RemoteHost {
    FakeHost virt, real;
    Bluetooth::SlotOwnership owner;
    std::vector<LinkState> realStates;
    Bluetooth::SlotBackend Backend(size_t slot) const {
        return real.attached[slot] ? Bluetooth::SlotBackend::Real : Bluetooth::SlotBackend::Virtual;
    }
    Bluetooth::RemoteHost& For(size_t slot) { return real.attached[slot] ? static_cast<Bluetooth::RemoteHost&>(real) : virt; }
    bool IsAttached(size_t slot) const override {
        if (!owner.Matches(slot, Backend(slot))) return false;
        return real.attached[slot] || virt.attached[slot];
    }
    bool ConsumeWake(size_t slot) override { return For(slot).ConsumeWake(slot); }
    bool PopInputReport(size_t slot, Bytes& report) override { return For(slot).PopInputReport(slot, report); }
    void SendOutputReport(size_t slot, const uint8_t* report, size_t size) override {
        For(slot).SendOutputReport(slot, report, size);
    }
    void OnLinkStateChanged(size_t slot, LinkState state) override {
        const Bluetooth::SlotBackend lost = owner.OnStateChanged(slot, state, Backend(slot));
        if (lost == Bluetooth::SlotBackend::Real) { real.OnLinkStateChanged(slot, state); realStates.push_back(state); return; }
        if (lost == Bluetooth::SlotBackend::Virtual) { virt.OnLinkStateChanged(slot, state); return; }
        if (real.attached[slot]) realStates.push_back(state);
        For(slot).OnLinkStateChanged(slot, state);
    }
    void OnGuestDisconnect(size_t slot) override { For(slot).OnGuestDisconnect(slot); }
    void OnGuestInquiry(bool active) override { real.OnGuestInquiry(active); }
    const char* DeviceName(size_t) const override { return "Nintendo RVL-CNT-01"; }
};

// Boots the controller with page scan on and slot 0 attached, then completes the remote-initiated connection.
// Returns the connection-complete status (0 = ok).
bool ConnectSlot0(Guest& guest, Bytes& key, bool bootFirst) {
    if (bootFirst) {
        guest.Run(0x0C03);
        auto e = guest.Run(0x0C0D, {0, 0, 0, 0, 0, 0, 1});
        if (!e.empty() && e[0].size() >= 25) key.assign(e[0].begin() + 9, e[0].begin() + 25);
        guest.Run(0x0C1A, {0x02});
    }
    std::vector<Bytes> e;
    for (int i = 0; i < 40; ++i) {  // the remote pages the guest (after its retry delay if it just dropped)
        guest.now += 100'000;
        guest.Tick();
        e = guest.Events();
        if (!e.empty()) break;
    }
    if (e.size() != 1 || e[0][0] != 0x04) return false;
    const BdAddr addr = Controller::RemoteAddress(0);
    Bytes accept(addr.begin(), addr.end());
    accept.push_back(0x00);
    e = guest.Run(0x0409, accept);
    if (e.size() != 3 || e[2][0] != 0x03 || e[2][2] != 0x00) return false;
    const uint16_t handle = Le16(e[2], 3);
    guest.Run(0x0411, {static_cast<uint8_t>(handle), static_cast<uint8_t>(handle >> 8)});
    Bytes reply(addr.begin(), addr.end());
    reply.insert(reply.end(), key.begin(), key.end());
    guest.Run(0x040B, reply);
    guest.Tick();
    AcceptRemoteChannel(guest, handle, 0x0011, 0x0041);
    guest.Tick();
    AcceptRemoteChannel(guest, handle, 0x0013, 0x0042);
    return true;
}

void TestRealRemoteReplacesVirtualLink() {
    SwitchingHost host;
    Controller controller(host);
    Guest guest{controller};
    controller.Open(guest.now);
    host.virt.attached[0] = true;
    host.virt.wake[0] = true;
    Bytes key;
    CHECK(ConnectSlot0(guest, key, true));
    CHECK(host.virt.states[0] == LinkState::Ready);
    CHECK(host.owner.Owner(0) == Bluetooth::SlotBackend::Virtual);

    // A physical Wii Remote connects to the PC mid-session and takes slot 0.
    host.real.attached[0] = true;
    host.real.wake[0] = true;
    guest.Tick();
    auto e = guest.Events();
    // The virtual link is torn down (Disconnection Complete) instead of silently continuing on the real remote.
    CHECK(!e.empty() && e[0][0] == 0x05);
    CHECK(host.virt.states[0] == LinkState::Down);
    CHECK(host.owner.Owner(0) == Bluetooth::SlotBackend::None);
    CHECK(host.real.guestDisconnects.empty());  // the real remote is not told to power off

    // The real remote then connects and initialises from scratch; its host sees the link come up.
    CHECK(ConnectSlot0(guest, key, false));
    CHECK(host.real.states[0] == LinkState::Ready);
    CHECK(host.owner.Owner(0) == Bluetooth::SlotBackend::Real);
    host.real.input[0].push_back({0x30, 0x00, 0x08});
    guest.Tick();
    guest.Events();
    const auto acl = guest.Acl();
    CHECK(acl.size() == 1 && acl[0].size() > 11 && acl[0][9] == 0x30 && acl[0][11] == 0x08);  // real passthrough

    // The real remote leaves: the link drops again and the real host hears it go down.
    host.real.attached[0] = false;
    guest.Tick();
    e = guest.Events();
    CHECK(!e.empty() && e[0][0] == 0x05);
    CHECK(host.real.states[0] == LinkState::Down);
    CHECK(host.owner.Owner(0) == Bluetooth::SlotBackend::None);
}

void TestSlotOwnershipUnit() {
    Bluetooth::SlotOwnership o;
    using B = Bluetooth::SlotBackend;
    CHECK(o.Matches(0, B::Virtual) && o.Matches(0, B::Real));
    CHECK(o.OnStateChanged(0, LinkState::Connecting, B::Virtual) == B::None);
    CHECK(o.OnStateChanged(0, LinkState::Ready, B::Real) == B::None);  // first claim wins
    CHECK(o.Owner(0) == B::Virtual && o.Matches(0, B::Virtual) && !o.Matches(0, B::Real));
    CHECK(o.OnStateChanged(0, LinkState::Down, B::Real) == B::Virtual);
    CHECK(o.Owner(0) == B::None && o.Matches(0, B::Real));
    CHECK(o.Matches(3, B::Real) && o.OnStateChanged(3, LinkState::Ready, B::Real) == B::None);
}

} // namespace

int main() {
    TestBootAndConnection();
    TestInquiryAndPaging();
    TestSdp();
    TestSlotOwnershipUnit();
    TestRealRemoteReplacesVirtualLink();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("bluetooth controller tests passed\n");
    return EXIT_SUCCESS;
}
