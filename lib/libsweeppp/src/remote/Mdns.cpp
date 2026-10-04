// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/remote/Mdns.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Log.hpp"
#include "sweeppp/net/Udp.hpp"
#include "sweeppp/remote/Protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <mutex>
#include <thread>

namespace sweeppp::remote::mdns {
namespace {

using Labels = std::vector<std::string>;

constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeTxt = 16;
constexpr std::uint16_t kTypeAaaa = 28;
constexpr std::uint16_t kTypeSrv = 33;
constexpr std::uint16_t kTypeAny = 255;
constexpr std::uint16_t kClassIn = 1;
constexpr std::uint16_t kUnicastResponse = 0x8000;
constexpr std::uint16_t kCacheFlush = 0x8000;

constexpr std::size_t kMaxLabel = 63;
constexpr std::size_t kMaxName = 255;
constexpr int kMaxPointerHops = 32;
constexpr std::size_t kMaxRecords = 64;

const Labels& serviceLabels() {
    static const Labels kService{"_sweeppp", "_tcp", "local"};
    return kService;
}

bool sameLabel(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

bool sameName(const Labels& a, const Labels& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](const std::string& x, const std::string& y) { return sameLabel(x, y); });
}

/// The instance's labels: its own name, a single label whatever it holds,
/// then the service.
Labels instanceLabels(const std::string& instance) {
    Labels labels{instance.substr(0, kMaxLabel)};
    labels.insert(labels.end(), serviceLabels().begin(), serviceLabels().end());
    return labels;
}

Labels hostLabels(const std::string& host) {
    // A host name with dots in it is not ours to split: one label.
    std::string label = host.substr(0, kMaxLabel);
    std::ranges::replace(label, '.', '-');
    return {label, "local"};
}

// ---- writing --------------------------------------------------------------------

class Writer {
public:
    void u16(std::uint16_t value) {
        m_bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
        m_bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    }
    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value >> 16U));
        u16(static_cast<std::uint16_t>(value & 0xFFFFU));
    }
    void name(const Labels& labels) {
        for (const std::string& label : labels) {
            const std::size_t length = std::min(label.size(), kMaxLabel);
            m_bytes.push_back(static_cast<std::uint8_t>(length));
            m_bytes.insert(m_bytes.end(), label.begin(),
                           label.begin() + static_cast<std::ptrdiff_t>(length));
        }
        m_bytes.push_back(0);
    }
    void bytes(std::span<const std::uint8_t> data) {
        m_bytes.insert(m_bytes.end(), data.begin(), data.end());
    }

    /// A record whose data `body` writes, its length filled in after.
    template <typename Body>
    void record(const Labels& owner, std::uint16_t type, std::uint16_t klass, std::uint32_t ttl,
                Body body) {
        name(owner);
        u16(type);
        u16(klass);
        u32(ttl);
        const std::size_t lengthAt = m_bytes.size();
        u16(0);
        body(*this);
        const std::size_t length = m_bytes.size() - lengthAt - 2;
        m_bytes[lengthAt] = static_cast<std::uint8_t>(length >> 8U);
        m_bytes[lengthAt + 1] = static_cast<std::uint8_t>(length & 0xFFU);
    }

    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(m_bytes); }

private:
    std::vector<std::uint8_t> m_bytes;
};

// ---- reading --------------------------------------------------------------------

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> packet) : m_packet(packet) {}

    [[nodiscard]] bool u16(std::uint16_t& out) {
        if (m_at + 2 > m_packet.size()) {
            return false;
        }
        out = static_cast<std::uint16_t>((m_packet[m_at] << 8U) | m_packet[m_at + 1]);
        m_at += 2;
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& out) {
        std::uint16_t high = 0;
        std::uint16_t low = 0;
        if (!u16(high) || !u16(low)) {
            return false;
        }
        out = (std::uint32_t{high} << 16U) | low;
        return true;
    }

    /// A name at the cursor, following compression pointers -- only ever
    /// backwards, and only so many times, so no packet can loop it.
    [[nodiscard]] bool name(Labels& out) { return nameAt(m_at, out, true); }

    /// A name at `offset` inside the record data, leaving the cursor alone.
    [[nodiscard]] bool nameAt(std::size_t offset, Labels& out) {
        std::size_t at = offset;
        return nameAt(at, out, false);
    }

    [[nodiscard]] bool skip(std::size_t bytes) {
        if (m_at + bytes > m_packet.size()) {
            return false;
        }
        m_at += bytes;
        return true;
    }

    [[nodiscard]] std::size_t position() const noexcept { return m_at; }
    [[nodiscard]] std::span<const std::uint8_t> packet() const noexcept { return m_packet; }

private:
    bool nameAt(std::size_t& cursor, Labels& out, bool advance) {
        out.clear();
        std::size_t at = cursor;
        std::size_t total = 0;
        int hops = 0;
        bool jumped = false;
        while (true) {
            if (at >= m_packet.size()) {
                return false;
            }
            const std::uint8_t length = m_packet[at];
            if ((length & 0xC0U) == 0xC0U) {
                if (at + 1 >= m_packet.size() || ++hops > kMaxPointerHops) {
                    return false;
                }
                const std::size_t target = ((length & 0x3FU) << 8U) | m_packet[at + 1];
                if (target >= at) {
                    return false;
                }
                if (!jumped && advance) {
                    cursor = at + 2;
                }
                jumped = true;
                at = target;
                continue;
            }
            if ((length & 0xC0U) != 0) {
                return false;
            }
            if (length == 0) {
                if (!jumped && advance) {
                    cursor = at + 1;
                }
                return true;
            }
            total += length + 1U;
            if (total > kMaxName || at + 1 + length > m_packet.size()) {
                return false;
            }
            out.emplace_back(reinterpret_cast<const char*>(m_packet.data() + at + 1), length);
            at += 1U + length;
        }
    }

    std::span<const std::uint8_t> m_packet;
    std::size_t m_at = 0;
};

struct Header {
    std::uint16_t id = 0;
    std::uint16_t flags = 0;
    std::uint16_t questions = 0;
    std::uint16_t answers = 0;
    std::uint16_t authorities = 0;
    std::uint16_t additionals = 0;
};

bool readHeader(Reader& reader, Header& header) {
    return reader.u16(header.id) && reader.u16(header.flags) && reader.u16(header.questions) &&
           reader.u16(header.answers) && reader.u16(header.authorities) &&
           reader.u16(header.additionals);
}

std::string txtEntry(std::string_view key, std::string_view value) {
    std::string entry = std::format("{}={}", key, value);
    entry.resize(std::min<std::size_t>(entry.size(), 255));
    return entry;
}

} // namespace

std::vector<std::uint8_t> encodeQuery(std::uint16_t id) {
    Writer writer;
    writer.u16(id);
    writer.u16(0);
    writer.u16(1);
    writer.u16(0);
    writer.u16(0);
    writer.u16(0);
    writer.name(serviceLabels());
    writer.u16(kTypePtr);
    writer.u16(kClassIn | kUnicastResponse);
    return writer.take();
}

Result<Question> parseQuery(std::span<const std::uint8_t> packet) {
    Reader reader(packet);
    Header header;
    if (!readHeader(reader, header)) {
        return fail<Question>(ErrorCode::ProtocolError, "a DNS packet shorter than its header");
    }
    Question question{.id = header.id};
    // Answers are not questions, whatever else they carry.
    if ((header.flags & 0x8000U) != 0) {
        return question;
    }
    if (header.questions > kMaxRecords) {
        return fail<Question>(ErrorCode::ProtocolError, "{} questions", header.questions);
    }
    for (std::uint16_t i = 0; i < header.questions; ++i) {
        Labels name;
        std::uint16_t type = 0;
        std::uint16_t klass = 0;
        if (!reader.name(name) || !reader.u16(type) || !reader.u16(klass)) {
            return fail<Question>(ErrorCode::ProtocolError, "a malformed question");
        }
        const bool aboutUs =
            sameName(name, serviceLabels()) ||
            (name.size() == 4 && sameName(Labels(name.begin() + 1, name.end()), serviceLabels()));
        if (aboutUs &&
            (type == kTypePtr || type == kTypeSrv || type == kTypeTxt || type == kTypeAny)) {
            question.forUs = true;
        }
    }
    return question;
}

std::vector<std::uint8_t> encodeAnswer(const Advert& advert, std::uint16_t id, bool question,
                                       std::uint32_t ttl) {
    const Labels instance = instanceLabels(advert.instance);
    const Labels host = hostLabels(advert.host);

    Writer writer;
    writer.u16(id);
    writer.u16(0x8400); // a response, authoritative
    writer.u16(question ? 1 : 0);
    writer.u16(3);
    writer.u16(0);
    writer.u16(static_cast<std::uint16_t>(advert.ipv4.size() + advert.ipv6.size()));
    if (question) {
        writer.name(serviceLabels());
        writer.u16(kTypePtr);
        writer.u16(kClassIn);
    }

    writer.record(serviceLabels(), kTypePtr, kClassIn, ttl,
                  [&](Writer& out) { out.name(instance); });
    writer.record(instance, kTypeSrv, kClassIn | kCacheFlush, ttl, [&](Writer& out) {
        out.u16(0);
        out.u16(0);
        out.u16(advert.port);
        out.name(host);
    });
    writer.record(instance, kTypeTxt, kClassIn | kCacheFlush, ttl, [&](Writer& out) {
        for (const std::string& entry :
             {txtEntry("v", std::to_string(kProtocolVersion)), txtEntry("device", advert.device),
              txtEntry("auth", advert.authRequired ? "1" : "0"),
              txtEntry("busy", advert.busy ? "1" : "0"),
              txtEntry("shared", advert.shared ? "1" : "0")}) {
            out.bytes(std::array{static_cast<std::uint8_t>(entry.size())});
            out.bytes({reinterpret_cast<const std::uint8_t*>(entry.data()), entry.size()});
        }
    });
    for (const std::string& address : advert.ipv4) {
        std::array<std::uint8_t, 4> raw{};
        net::parseIpv4(address, raw);
        writer.record(host, kTypeA, kClassIn | kCacheFlush, ttl,
                      [&](Writer& out) { out.bytes(raw); });
    }
    for (const std::string& address : advert.ipv6) {
        std::array<std::uint8_t, 16> raw{};
        net::parseIpv6(address, raw);
        writer.record(host, kTypeAaaa, kClassIn | kCacheFlush, ttl,
                      [&](Writer& out) { out.bytes(raw); });
    }
    return writer.take();
}

Result<std::vector<Found>> parseAnswer(std::span<const std::uint8_t> packet) {
    using Servers = std::vector<Found>;
    Reader reader(packet);
    Header header;
    if (!readHeader(reader, header)) {
        return fail<Servers>(ErrorCode::ProtocolError, "a DNS packet shorter than its header");
    }
    if ((header.flags & 0x8000U) == 0) {
        return Servers{};
    }
    const std::size_t records =
        std::size_t{header.answers} + header.authorities + header.additionals;
    if (header.questions > kMaxRecords || records > kMaxRecords) {
        return fail<Servers>(ErrorCode::ProtocolError, "{} records", records);
    }
    for (std::uint16_t i = 0; i < header.questions; ++i) {
        Labels name;
        std::uint16_t type = 0;
        std::uint16_t klass = 0;
        if (!reader.name(name) || !reader.u16(type) || !reader.u16(klass)) {
            return fail<Servers>(ErrorCode::ProtocolError, "a malformed question");
        }
    }

    // By instance name, as the records arrive in no particular order.
    std::map<std::string, Found> found;
    for (std::size_t i = 0; i < records; ++i) {
        Labels owner;
        std::uint16_t type = 0;
        std::uint16_t klass = 0;
        std::uint32_t ttl = 0;
        std::uint16_t length = 0;
        if (!reader.name(owner) || !reader.u16(type) || !reader.u16(klass) || !reader.u32(ttl) ||
            !reader.u16(length)) {
            return fail<Servers>(ErrorCode::ProtocolError, "a malformed record");
        }
        const std::size_t data = reader.position();
        if (!reader.skip(length)) {
            return fail<Servers>(ErrorCode::ProtocolError, "a record longer than its packet");
        }
        const bool ours =
            owner.size() == 4 && sameName(Labels(owner.begin() + 1, owner.end()), serviceLabels());

        if (type == kTypePtr && sameName(owner, serviceLabels())) {
            Labels target;
            if (!reader.nameAt(data, target)) {
                return fail<Servers>(ErrorCode::ProtocolError, "a malformed PTR");
            }
            if (target.size() == 4) {
                found[target.front()].instance = target.front();
            }
        } else if (type == kTypeSrv && ours && length >= 7) {
            const std::span<const std::uint8_t> bytes = reader.packet().subspan(data, length);
            Labels target;
            if (!reader.nameAt(data + 6, target)) {
                return fail<Servers>(ErrorCode::ProtocolError, "a malformed SRV");
            }
            Found& server = found[owner.front()];
            server.instance = owner.front();
            server.port = static_cast<std::uint16_t>((bytes[4] << 8U) | bytes[5]);
            server.host = target.empty() ? std::string{} : target.front();
        } else if (type == kTypeTxt && ours) {
            Found& server = found[owner.front()];
            server.instance = owner.front();
            std::size_t at = 0;
            const std::span<const std::uint8_t> bytes = reader.packet().subspan(data, length);
            while (at < bytes.size()) {
                const std::size_t entryLength = bytes[at];
                if (at + 1 + entryLength > bytes.size()) {
                    return fail<Servers>(ErrorCode::ProtocolError, "a malformed TXT");
                }
                const std::string_view entry(reinterpret_cast<const char*>(bytes.data() + at + 1),
                                             entryLength);
                at += 1 + entryLength;
                const std::size_t equals = entry.find('=');
                const std::string_view key = entry.substr(0, equals);
                const std::string_view value = equals == std::string_view::npos
                                                   ? std::string_view{}
                                                   : entry.substr(equals + 1);
                if (key == "device") {
                    server.device = std::string(value);
                } else if (key == "auth") {
                    server.authRequired = value == "1";
                } else if (key == "busy") {
                    server.busy = value == "1";
                } else if (key == "shared") {
                    server.shared = value == "1";
                } else if (key == "v") {
                    std::from_chars(value.data(), value.data() + value.size(),
                                    server.protocolVersion);
                }
            }
        }
    }

    Servers servers;
    for (auto& [name, server] : found) {
        if (server.port != 0) {
            servers.push_back(std::move(server));
        }
    }
    return servers;
}

// ---------------------------------------------------------------- the advertiser

struct Advertiser::Impl {
    Advert advert;
    std::atomic<bool>* busy = nullptr;
    std::atomic<bool> stopping{false};
    std::vector<net::UdpSocket> sockets;
    std::thread thread;

    /// The advert as of now: this machine's addresses change as cables do.
    [[nodiscard]] Advert current() const {
        Advert now = advert;
        now.busy = busy->load();
        now.ipv4.clear();
        now.ipv6.clear();
        for (const net::NetworkInterface& iface : net::networkInterfaces()) {
            if (iface.loopback) {
                continue;
            }
            now.ipv4.insert(now.ipv4.end(), iface.ipv4.begin(), iface.ipv4.end());
            now.ipv6.insert(now.ipv6.end(), iface.ipv6.begin(), iface.ipv6.end());
        }
        now.ipv4.resize(std::min<std::size_t>(now.ipv4.size(), 8));
        now.ipv6.resize(std::min<std::size_t>(now.ipv6.size(), 8));
        return now;
    }

    /// To every interface, each socket in its own family's group.
    void announce(std::uint32_t ttl) {
        const std::vector<std::uint8_t> packet = encodeAnswer(current(), 0, false, ttl);
        for (net::UdpSocket& socket : sockets) {
            const std::string group(socket.family() == net::IpFamily::V4 ? kGroupV4 : kGroupV6);
            for (const net::NetworkInterface& iface : net::networkInterfaces()) {
                (void)socket.sendToGroup(packet, group, kPort, iface);
            }
        }
    }

    void run() {
        announce(120);
        while (!stopping.load()) {
            for (net::UdpSocket& socket : sockets) {
                auto received = socket.receive(std::chrono::milliseconds(100));
                if (!received || !*received) {
                    continue;
                }
                const net::Datagram& datagram = **received;
                auto question = parseQuery(datagram.bytes);
                if (!question || !question->forUs) {
                    continue;
                }
                // From the mDNS port it is a resolver like ours, and wants the
                // answer multicast; from any other, the asker is waiting for it
                // there (RFC 6762 section 6.7).
                if (datagram.sourcePort == kPort) {
                    announce(120);
                } else {
                    (void)socket.sendTo(encodeAnswer(current(), question->id, true, 10),
                                        datagram.sourceAddress, datagram.sourcePort);
                }
            }
        }
        announce(0);
    }
};

Advertiser::Advertiser(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {
}

Result<std::unique_ptr<Advertiser>> Advertiser::start(Advert advert) {
    auto impl = std::make_unique<Impl>();
    impl->advert = std::move(advert);
    for (const auto& [family, group] :
         {std::pair{net::IpFamily::V4, kGroupV4}, std::pair{net::IpFamily::V6, kGroupV6}}) {
        auto socket = net::UdpSocket::open(family, kPort, std::string(group));
        if (socket) {
            impl->sockets.push_back(std::move(*socket));
        } else {
            logInfo("remote", "not advertising over {}: {}",
                    family == net::IpFamily::V4 ? "IPv4" : "IPv6", socket.error().describe());
        }
    }
    if (impl->sockets.empty()) {
        return fail<std::unique_ptr<Advertiser>>(ErrorCode::Unavailable,
                                                 "no multicast socket could be opened");
    }
    std::unique_ptr<Advertiser> advertiser(new Advertiser(std::move(impl)));
    Impl& running = *advertiser->m_impl;
    running.busy = &advertiser->m_busy;
    running.thread = std::thread([&running] { running.run(); });
    return advertiser;
}

Advertiser::~Advertiser() {
    m_impl->stopping.store(true);
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
}

// ------------------------------------------------------------------- the browser

struct Browser::Impl {
    std::vector<net::UdpSocket> sockets;
    std::atomic<bool> stopping{false};
    std::atomic<bool> askNow{true};
    mutable std::mutex mutex;
    std::map<std::string, DiscoveredServer> servers;
    std::thread thread;

    static constexpr std::uint64_t kIntervalNs = 5'000'000'000;
    static constexpr std::uint64_t kForgetNs = 15'000'000'000;

    void ask() {
        const std::vector<std::uint8_t> query = encodeQuery(0);
        for (net::UdpSocket& socket : sockets) {
            const std::string group(socket.family() == net::IpFamily::V4 ? kGroupV4 : kGroupV6);
            for (const net::NetworkInterface& iface : net::networkInterfaces()) {
                (void)socket.sendToGroup(query, group, kPort, iface);
            }
        }
    }

    void heard(const net::Datagram& datagram, net::IpFamily family) {
        auto answer = parseAnswer(datagram.bytes);
        if (!answer) {
            return;
        }
        const std::uint64_t now = monotonicNs();
        const std::lock_guard lock(mutex);
        for (Found& found : *answer) {
            DiscoveredServer& server = servers[found.instance];
            // IPv4 over IPv6 when both answer: an address without a scope
            // carries over to another machine, and reads as one.
            const bool keepAddress = !server.endpoint.host.empty() && family == net::IpFamily::V6 &&
                                     server.endpoint.host.find(':') == std::string::npos;
            if (!keepAddress) {
                server.endpoint.host = datagram.sourceAddress;
            }
            server.endpoint.port = found.port;
            server.found = std::move(found);
            server.lastSeenNs = now;
        }
    }

    void run() {
        std::uint64_t lastAsked = 0;
        while (!stopping.load()) {
            const std::uint64_t now = monotonicNs();
            if (askNow.exchange(false) || now - lastAsked >= kIntervalNs) {
                ask();
                lastAsked = now;
            }
            for (net::UdpSocket& socket : sockets) {
                auto received = socket.receive(std::chrono::milliseconds(100));
                if (received && *received) {
                    heard(**received, socket.family());
                }
            }
        }
    }
};

Browser::Browser(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {
}

Result<std::unique_ptr<Browser>> Browser::start() {
    auto impl = std::make_unique<Impl>();
    for (const net::IpFamily family : {net::IpFamily::V4, net::IpFamily::V6}) {
        if (auto socket = net::UdpSocket::open(family, 0)) {
            impl->sockets.push_back(std::move(*socket));
        }
    }
    if (impl->sockets.empty()) {
        return fail<std::unique_ptr<Browser>>(ErrorCode::Unavailable,
                                              "no socket to look for servers with");
    }
    std::unique_ptr<Browser> browser(new Browser(std::move(impl)));
    Impl& running = *browser->m_impl;
    running.thread = std::thread([&running] { running.run(); });
    return browser;
}

Browser::~Browser() {
    m_impl->stopping.store(true);
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
}

void Browser::refresh() noexcept {
    m_impl->askNow.store(true);
}

std::vector<DiscoveredServer> Browser::servers() const {
    const std::uint64_t now = monotonicNs();
    const std::lock_guard lock(m_impl->mutex);
    std::vector<DiscoveredServer> recent;
    for (const auto& [name, server] : m_impl->servers) {
        if (now - server.lastSeenNs < Impl::kForgetNs) {
            recent.push_back(server);
        }
    }
    return recent;
}

} // namespace sweeppp::remote::mdns
