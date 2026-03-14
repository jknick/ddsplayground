// inspector.cpp – libpcap RTPS wire sniffer for the ping/pong/baz/hoge mesh.
//
// Captures all RTPS/UDP traffic on a network interface, decodes SEDP endpoint
// discovery to build a GUID→topic map, then prints every user-data message with
// its topic name, decoded payload, and writer GUID.
//
// Topology snooped:
//   ping ──PingPongTopic(int32)──► pong ──PongBazTopic(int32)──► baz
//                                    ▲                              │
//                                    └────BazPongTopic(string)──────┘
//   hoge also subscribes to PingPongTopic.
//
// Usage:  sudo ./inspector [--reannounce|--traffic|--both] [--verbose] [interface] [domain_id]
//           --reannounce  print only SPDP/SEDP discovery and re-announcement messages
//           --traffic     print only user-data DDS messages
//           --both        print everything (default)
//           --verbose     print per-packet diagnostics to stderr (capture counts, RTPS magic checks)
//           interface     default: lo0
//           domain_id     default: 0

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <pcap/pcap.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

// ── Print-mode filter ─────────────────────────────────────────────────────────

enum PrintMode { PRINT_BOTH, PRINT_REANNOUNCE, PRINT_TRAFFIC };
static PrintMode g_print_mode = PRINT_BOTH;
static bool      g_verbose    = false;

// Counters for --verbose stats
static uint64_t g_cnt_captured = 0;
static uint64_t g_cnt_rtps     = 0;
static uint64_t g_cnt_sedp     = 0;
static uint64_t g_cnt_data     = 0;

// ── RTPS protocol constants ───────────────────────────────────────────────────

static const uint8_t RTPS_MAGIC[4] = {'R', 'T', 'P', 'S'};

// Submessage IDs (RTPS 2.3 §8.3.3)
enum SubMsgId : uint8_t {
    SM_PAD             = 0x01,
    SM_ACKNACK         = 0x06,
    SM_HEARTBEAT       = 0x07,
    SM_GAP             = 0x09,
    SM_INFO_TS         = 0x0C,
    SM_INFO_SRC        = 0x0E,
    SM_INFO_REPLY_IP4  = 0x0F,
    SM_INFO_DST        = 0x10,
    SM_INFO_REPLY      = 0x11,
    SM_DATA            = 0x15,
    SM_DATA_FRAG       = 0x16,
    SM_NACK_FRAG       = 0x19,
    SM_HB_FRAG         = 0x1A,
};

// Well-known writer entity IDs (big-endian, always)
static const uint8_t EID_SPDP_WRITER[4]     = {0x00, 0x01, 0x00, 0xC2};
static const uint8_t EID_SEDP_PUB_WRITER[4] = {0x00, 0x00, 0x03, 0xC2};
static const uint8_t EID_SEDP_SUB_WRITER[4] = {0x00, 0x00, 0x04, 0xC2};

// CDR parameter IDs used in SEDP announcements
static constexpr uint16_t PID_SENTINEL      = 0x0001;
static constexpr uint16_t PID_TOPIC_NAME    = 0x0005;
static constexpr uint16_t PID_TYPE_NAME     = 0x0007;
static constexpr uint16_t PID_ENDPOINT_GUID = 0x005A;

// ── GUID types and helpers ────────────────────────────────────────────────────

struct GuidPrefix { uint8_t v[12]; };
struct EntityId   { uint8_t v[4];  };

static std::string fmt_prefix(const GuidPrefix &p)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x:%02x%02x%02x%02x:%02x%02x%02x%02x",
        p.v[0], p.v[1], p.v[2],  p.v[3],
        p.v[4], p.v[5], p.v[6],  p.v[7],
        p.v[8], p.v[9], p.v[10], p.v[11]);
    return buf;
}

static std::string fmt_entity(const EntityId &e)
{
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x|%02x",
        e.v[0], e.v[1], e.v[2], e.v[3]);
    return buf;
}

// Full GUID as "prefix:entity"
static std::string fmt_guid(const GuidPrefix &prefix, const EntityId &entity)
{
    return fmt_prefix(prefix) + ":" + fmt_entity(entity);
}

// ── Endpoint registry (populated by SEDP decoding) ───────────────────────────

struct EndpointInfo {
    std::string topic;
    std::string type_name;
    bool        is_writer;
};

// key = full GUID string from fmt_guid()
static std::map<std::string, EndpointInfo> g_endpoints;

// ── Byte-order helpers ────────────────────────────────────────────────────────

static inline uint16_t u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint16_t u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t u32_le(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24));
}
static inline uint32_t u32_be(const uint8_t *p)
{
    return (uint32_t)(((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]);
}
static inline uint16_t u16(const uint8_t *p, bool le) { return le ? u16_le(p) : u16_be(p); }
static inline uint32_t u32(const uint8_t *p, bool le) { return le ? u32_le(p) : u32_be(p); }

// ── CDR helpers ───────────────────────────────────────────────────────────────

// Read a CDR string starting at buf[off]: 4-byte length (incl. null) + chars.
// Returns "" on error.  *end_out is set to the next aligned offset after the string.
static std::string cdr_string(const uint8_t *buf, size_t buf_len,
                               size_t off, bool le, size_t *end_out = nullptr)
{
    if (off + 4 > buf_len) return "";
    uint32_t slen = u32(buf + off, le);
    off += 4;
    if (slen == 0 || off + slen > buf_len) return "";
    std::string s(reinterpret_cast<const char *>(buf + off), slen > 0 ? slen - 1 : 0);
    if (end_out) *end_out = off + ((slen + 3) & ~3u); // 4-byte aligned
    return s;
}

// ── SEDP CDR parameter-list decoder ──────────────────────────────────────────
// Parse a PL_CDR payload from a SEDP DATA submessage.
// Extracts topic name, type name, and endpoint GUID; registers in g_endpoints.

static void decode_sedp(const GuidPrefix &sender_prefix, const EntityId &writer_eid,
                        const uint8_t *payload, size_t len, bool is_pub)
{
    if (len < 4) return;
    // Bytes 0-1: CDR representation ID. Bit 0 of low byte = little-endian.
    bool le = (payload[1] & 0x01) != 0;
    size_t off = 4; // skip rep_id (2) + options (2)

    std::string topic, type_name, guid_str;
    GuidPrefix  ep_prefix{};
    EntityId    ep_entity{};
    bool        have_ep_guid = false;

    while (off + 4 <= len) {
        uint16_t pid  = u16(payload + off,     le);
        uint16_t plen = u16(payload + off + 2, le);
        off += 4;
        if (pid == PID_SENTINEL) break;
        size_t param_end = off + plen;
        if (param_end > len) break;

        if (pid == PID_TOPIC_NAME) {
            topic = cdr_string(payload, param_end, off, le);
        } else if (pid == PID_TYPE_NAME) {
            type_name = cdr_string(payload, param_end, off, le);
        } else if (pid == PID_ENDPOINT_GUID && plen >= 16) {
            memcpy(ep_prefix.v, payload + off,      12);
            memcpy(ep_entity.v, payload + off + 12,  4);
            have_ep_guid = true;
        }
        off = param_end; // plen is already padded to 4 bytes
    }

    // Fall back: construct GUID from sender prefix + writer entity if PID absent
    if (!have_ep_guid) {
        ep_prefix = sender_prefix;
        ep_entity = writer_eid;
    }

    if (topic.empty()) return;
    guid_str = fmt_guid(ep_prefix, ep_entity);
    bool reannounce = (g_endpoints.count(guid_str) > 0);
    g_endpoints[guid_str] = {topic, type_name, is_pub};

    if (g_print_mode != PRINT_TRAFFIC) {
        const char *kind = is_pub ? "PUBLICATION " : "SUBSCRIPTION";
        std::cout
            << "┌─ [SEDP " << kind << (reannounce ? " RE-ANNOUNCE" : " NEW        ") << "]\n"
            << "│  GUID  : " << guid_str  << "\n"
            << "│  Topic : " << topic     << "\n"
            << "│  Type  : " << type_name << "\n";
        if (is_pub && g_print_mode != PRINT_REANNOUNCE)
            std::cout << "│  → Traffic tracking active for this writer\n";
        std::cout
            << "└──────────────────────────────────────────────────────────\n\n"
            << std::flush;
    }
}

// ── Application payload decoders ──────────────────────────────────────────────
// Each takes the full serialized payload (starts with 4-byte CDR header).

static std::string decode_ping_pong_msg(const uint8_t *pl, size_t len)
{
    if (len < 8) return "(too short)";
    bool le = (pl[1] & 0x01) != 0;
    int32_t val = (int32_t)u32(pl + 4, le);
    return "value=" + std::to_string(val);
}

static std::string decode_baz_response_msg(const uint8_t *pl, size_t len)
{
    if (len < 8) return "(too short)";
    bool le = (pl[1] & 0x01) != 0;
    std::string s = cdr_string(pl, len, 4, le);
    return "text=\"" + s + "\"";
}

static std::string decode_payload(const std::string &topic,
                                  const uint8_t *pl, size_t len)
{
    if (topic == "PingPongTopic" || topic == "PongBazTopic")
        return decode_ping_pong_msg(pl, len);
    if (topic == "BazPongTopic")
        return decode_baz_response_msg(pl, len);
    // Unknown topic: first 24 bytes as hex
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    size_t show = std::min(len, (size_t)24);
    for (size_t i = 0; i < show; ++i)
        hex << std::setw(2) << (int)pl[i];
    if (len > 24) hex << "…";
    return hex.str();
}

// ── RTPS packet parser ────────────────────────────────────────────────────────

static void handle_rtps(const uint8_t *data, size_t len,
                        const char *src_ip, uint16_t src_port, uint16_t dst_port,
                        const struct timeval &ts)
{
    // RTPS wire header: 4-magic + 2-version + 2-vendorId + 12-guidPrefix = 20 bytes
    if (len < 20 || memcmp(data, RTPS_MAGIC, 4) != 0) {
        if (g_verbose)
            std::fprintf(stderr, "[verbose] no RTPS magic  src=%s:%u dst=:%u len=%zu  first4=%02x%02x%02x%02x\n",
                src_ip, src_port, dst_port, len,
                len>0?data[0]:0, len>1?data[1]:0, len>2?data[2]:0, len>3?data[3]:0);
        return;
    }
    ++g_cnt_rtps;

    GuidPrefix sender_prefix;
    memcpy(sender_prefix.v, data + 8, 12);

    // Format timestamp as HH:MM:SS.uuuuuu
    char ts_buf[32];
    struct tm *tm_info = localtime(&ts.tv_sec);
    std::strftime(ts_buf, sizeof(ts_buf), "%H:%M:%S", tm_info);
    char ts_full[48];
    std::snprintf(ts_full, sizeof(ts_full), "%s.%06ld", ts_buf, (long)ts.tv_usec);

    size_t off = 20;
    while (off + 4 <= len) {
        uint8_t  sm_id    = data[off];
        uint8_t  sm_flags = data[off + 1];
        bool     le       = (sm_flags & 0x01) != 0; // E flag
        uint16_t sm_len   = u16(data + off + 2, le);
        size_t   body_off = off + 4;
        // sm_len == 0 means "extends to end of message"
        size_t   next_off = (sm_len == 0) ? len : (off + 4 + sm_len);
        if (next_off > len) next_off = len;

        if (g_verbose && sm_id != SM_DATA)
            std::fprintf(stderr, "[verbose] submsg id=0x%02x flags=0x%02x len=%u\n",
                sm_id, sm_flags, sm_len);

        if (sm_id == SM_DATA) {
            // DATA body layout (§9.4.5.3):
            //   extraFlags(2) + octetsToInlineQos(2) +
            //   readerId(4)   + writerId(4)           +
            //   writerSN(8)   → 20 bytes minimum
            if (body_off + 20 > len) { off = next_off; continue; }

            bool has_qos  = (sm_flags & 0x02) != 0;
            bool has_data = (sm_flags & 0x04) != 0;

            uint16_t oct_to_iqos = u16(data + body_off + 2, le);
            EntityId reader_eid, writer_eid;
            memcpy(reader_eid.v, data + body_off + 4,  4);
            memcpy(writer_eid.v, data + body_off + 8,  4);

            // Sequence number: high int32 + low uint32
            int32_t  sn_hi = (int32_t)u32(data + body_off + 12, le);
            uint32_t sn_lo =           u32(data + body_off + 16, le);
            int64_t  seq   = ((int64_t)sn_hi << 32) | sn_lo;

            // Start of inline QoS / serialized payload
            // octetsToInlineQos counts from end of that field → after extraFlags+octetsToInlineQos
            size_t qos_off = body_off + 4 + oct_to_iqos;

            // Skip inline QoS parameter list if present
            if (has_qos && qos_off + 4 <= next_off) {
                size_t q = qos_off;
                while (q + 4 <= next_off) {
                    uint16_t pid  = u16(data + q,     le);
                    uint16_t plen = u16(data + q + 2, le);
                    q += 4 + plen;
                    if (pid == PID_SENTINEL) break;
                }
                qos_off = q;
            }

            size_t pl_off = qos_off;
            size_t pl_len = (pl_off < next_off) ? (next_off - pl_off) : 0;

            // Classify by writer entity ID
            bool is_spdp     = (memcmp(writer_eid.v, EID_SPDP_WRITER,     4) == 0);
            bool is_sedp_pub = (memcmp(writer_eid.v, EID_SEDP_PUB_WRITER, 4) == 0);
            bool is_sedp_sub = (memcmp(writer_eid.v, EID_SEDP_SUB_WRITER, 4) == 0);

            if (g_verbose)
                std::fprintf(stderr,
                    "[verbose] DATA  writer=%02x%02x%02x%02x  has_data=%d  has_qos=%d"
                    "  pl_len=%zu  spdp=%d sedp_pub=%d sedp_sub=%d\n",
                    writer_eid.v[0], writer_eid.v[1], writer_eid.v[2], writer_eid.v[3],
                    has_data, has_qos, pl_len,
                    is_spdp, is_sedp_pub, is_sedp_sub);

            if (is_spdp) {
                // SPDP: participant discovery – brief notice
                if (g_print_mode != PRINT_TRAFFIC)
                    std::cout
                        << "┌─ [SPDP  " << ts_full
                        << "  " << src_ip << ":" << src_port << " → :" << dst_port << "]\n"
                        << "│  Participant : " << fmt_prefix(sender_prefix) << "\n"
                        << "└──────────────────────────────────────────────────────────\n\n"
                        << std::flush;

            } else if ((is_sedp_pub || is_sedp_sub) && has_data && pl_len > 0) {
                ++g_cnt_sedp;
                decode_sedp(sender_prefix, writer_eid,
                            data + pl_off, pl_len, is_sedp_pub);

            } else if (!is_sedp_pub && !is_sedp_sub && has_data && pl_len > 0) {
                // User data – print always; resolve topic from SEDP if known
                ++g_cnt_data;
                if (g_print_mode != PRINT_REANNOUNCE) {
                    std::string writer_guid = fmt_guid(sender_prefix, writer_eid);
                    auto it = g_endpoints.find(writer_guid);
                    std::string topic   = (it != g_endpoints.end()) ? it->second.topic : "(unknown – SEDP not yet seen)";
                    std::string payload = decode_payload(it != g_endpoints.end() ? it->second.topic : "",
                                                         data + pl_off, pl_len);
                    std::cout
                        << "┌─ [DATA  " << ts_full
                        << "  " << src_ip << ":" << src_port << " → :" << dst_port << "]\n"
                        << "│  Writer GUID : " << writer_guid << "\n"
                        << "│  Reader EID  : " << fmt_entity(reader_eid) << "\n"
                        << "│  Topic       : " << topic << "\n"
                        << "│  Seq#        : " << seq   << "\n"
                        << "│  Payload     : " << payload << "\n"
                        << "└──────────────────────────────────────────────────────────\n\n"
                        << std::flush;
                }
            }
            // ACKNACK, GAP, other INFO_* → silently ignored to keep output clean
        } else if (sm_id == SM_HEARTBEAT && body_off + 28 <= len) {
            // HEARTBEAT: check if it's from an SEDP writer – hints that we missed the exchange
            EntityId hb_writer;
            memcpy(hb_writer.v, data + body_off + 4, 4);
            bool sedp_hb = (memcmp(hb_writer.v, EID_SEDP_PUB_WRITER, 4) == 0 ||
                            memcmp(hb_writer.v, EID_SEDP_SUB_WRITER, 4) == 0);
            if (sedp_hb && g_endpoints.empty() && g_print_mode != PRINT_TRAFFIC)
                std::cout
                    << "⚠  [SEDP HEARTBEAT received but SEDP DATA was missed]\n"
                    << "   Inspector started after discovery completed.\n"
                    << "   Topic names unknown until next SEDP re-announcement.\n\n"
                    << std::flush;
        }

        off = next_off;
    }
}

// ── libpcap link-layer stripper + pcap callback ───────────────────────────────
// Per-interface context passed via the pcap user-data pointer.

struct IfaceCtx {
    int  datalink;
    char name[32];
};

static void packet_handler(uint8_t *user,
                           const struct pcap_pkthdr *hdr,
                           const uint8_t *pkt)
{
    const IfaceCtx *ctx = reinterpret_cast<const IfaceCtx *>(user);
    const uint8_t *ip   = nullptr;
    size_t         cap  = hdr->caplen;

    if (ctx->datalink == DLT_EN10MB) {
        if (cap < 14) return;
        uint16_t etype = u16_be(pkt + 12);
        if (etype == 0x8100 && cap >= 18) { // 802.1Q VLAN tag
            etype = u16_be(pkt + 16);
            ip = pkt + 18; cap -= 18;
        } else {
            ip = pkt + 14; cap -= 14;
        }
        if (etype != 0x0800) return; // not IPv4
    } else if (ctx->datalink == DLT_NULL) {
        // BSD loopback: 4-byte AF family in host byte order
        if (cap < 4) return;
        uint32_t family;
        memcpy(&family, pkt, 4);
        if (family != AF_INET) return;
        ip = pkt + 4; cap -= 4;
    } else if (ctx->datalink == DLT_LOOP) {
        // Like DLT_NULL but big-endian
        if (cap < 4) return;
        if (u32_be(pkt) != AF_INET) return;
        ip = pkt + 4; cap -= 4;
    } else {
        return; // unsupported link type
    }

    if (!ip || cap < 20) return;
    if ((ip[0] >> 4) != 4) return;        // not IPv4
    size_t ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > cap) return;
    if (ip[9] != IPPROTO_UDP) return;

    char src_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, ip + 12, src_ip, sizeof(src_ip));

    const uint8_t *udp     = ip + ihl;
    size_t         udp_cap = cap - ihl;
    if (udp_cap < 8) return;

    uint16_t src_port   = u16_be(udp);
    uint16_t dst_port   = u16_be(udp + 2);
    uint16_t udp_pl_len = u16_be(udp + 4);
    if (udp_pl_len < 8) return;
    size_t rtps_len = std::min((size_t)(udp_pl_len - 8), udp_cap - 8);

    ++g_cnt_captured;
    if (g_verbose) {
        std::fprintf(stderr, "[verbose] pkt #%lu  [%s]  %s:%u → :%u  udp_payload=%u bytes\n",
            (unsigned long)g_cnt_captured, ctx->name, src_ip, src_port, dst_port, udp_pl_len - 8);
        if (g_cnt_captured % 10 == 0)
            std::fprintf(stderr, "[verbose] stats: captured=%lu  rtps=%lu  sedp=%lu  data=%lu  endpoints_known=%zu\n",
                (unsigned long)g_cnt_captured, (unsigned long)g_cnt_rtps,
                (unsigned long)g_cnt_sedp,     (unsigned long)g_cnt_data,
                g_endpoints.size());
    }
    handle_rtps(udp + 8, rtps_len, src_ip, src_port, dst_port, hdr->ts);
}

// ── RTPS port range for a given domain ───────────────────────────────────────
// PB=7400, DG=250: discovery_mc=PB+DG*d, user_mc=PB+DG*d+1, unicast starts at +10.

static void build_filter(int /*domain_id*/, char *out, size_t out_len)
{
    // CycloneDDS uses OS-assigned ephemeral ports for unicast (SEDP + user data),
    // so we can't predict them. Capture all UDP and rely on the RTPS magic check
    // in software to discard non-RTPS packets.
    std::snprintf(out, out_len, "udp");
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    // Strip optional flags from argv
    int pos = 1;
    while (pos < argc && argv[pos][0] == '-' && argv[pos][1] == '-') {
        std::string flag = argv[pos++];
        if      (flag == "--reannounce") g_print_mode = PRINT_REANNOUNCE;
        else if (flag == "--traffic")    g_print_mode = PRINT_TRAFFIC;
        else if (flag == "--both")       g_print_mode = PRINT_BOTH;
        else if (flag == "--verbose")    g_verbose    = true;
        else {
            std::cerr << "Unknown flag: " << flag << "\n"
                      << "Usage: inspector [--reannounce|--traffic|--both] [--verbose] [iface ...] [domain_id]\n";
            return 1;
        }
    }

    // Remaining positional args: zero or more interface names, then optional domain_id.
    // If none given, default to capturing on both lo0 (unicast) and en0 (multicast).
    std::vector<std::string> ifaces;
    int domain_id = 0;
    while (pos < argc) {
        std::string arg = argv[pos++];
        // A pure-integer arg is the domain ID
        if (!arg.empty() && arg.find_first_not_of("0123456789") == std::string::npos)
            domain_id = std::stoi(arg);
        else
            ifaces.push_back(arg);
    }
    if (ifaces.empty()) {
        ifaces = {"lo0", "en0"}; // lo0 for unicast, en0 for SPDP multicast
    }

    const char *mode_str = (g_print_mode == PRINT_REANNOUNCE) ? "reannounce only"
                         : (g_print_mode == PRINT_TRAFFIC)    ? "traffic only"
                                                              : "both";
    std::cout
        << "╔══════════════════════════════════════════════════════════╗\n"
        << "║          RTPS Wire Inspector  (libpcap)                  ║\n"
        << "╚══════════════════════════════════════════════════════════╝\n"
        << "Domain    : " << domain_id << "\n"
        << "Mode      : " << mode_str  << "\n"
        << std::flush;

    char filter_str[128];
    build_filter(domain_id, filter_str, sizeof(filter_str));
    std::cout << "Filter    : " << filter_str << "\n\n" << std::flush;

    // Open a pcap handle on each requested interface
    struct OpenHandle {
        pcap_t  *pcap;
        IfaceCtx ctx;
    };
    std::vector<OpenHandle> handles;

    for (const auto &iface : ifaces) {
        char errbuf[PCAP_ERRBUF_SIZE] = {};
        pcap_t *h = pcap_open_live(iface.c_str(), 65535, /*promisc=*/1, /*ms=*/1, errbuf);
        if (!h) {
            std::cerr << "Warning: skipping " << iface << ": " << errbuf << "\n";
            continue;
        }
        struct bpf_program fp{};
        if (pcap_compile(h, &fp, filter_str, 1, PCAP_NETMASK_UNKNOWN) < 0 ||
            pcap_setfilter(h, &fp) < 0) {
            std::cerr << "Warning: BPF error on " << iface << ": " << pcap_geterr(h) << "\n";
            pcap_close(h);
            continue;
        }
        pcap_freecode(&fp);

        OpenHandle oh;
        oh.pcap         = h;
        oh.ctx.datalink = pcap_datalink(h);
        std::strncpy(oh.ctx.name, iface.c_str(), sizeof(oh.ctx.name) - 1);
        oh.ctx.name[sizeof(oh.ctx.name) - 1] = '\0';
        handles.push_back(oh);
        std::cout << "Capturing on " << iface << "\n";
    }
    std::cout << "\n" << std::flush;

    if (handles.empty()) {
        std::cerr << "No interfaces could be opened. Run as root (sudo).\n";
        return 1;
    }

    // Multiplex all handles with select() + pcap_dispatch (single-threaded)
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = -1;
        for (auto &oh : handles) {
            int fd = pcap_get_selectable_fd(oh.pcap);
            if (fd >= 0) { FD_SET(fd, &fds); if (fd > maxfd) maxfd = fd; }
        }
        struct timeval tv = {0, 100000}; // 100 ms timeout
        if (maxfd >= 0) select(maxfd + 1, &fds, nullptr, nullptr, &tv);

        for (auto &oh : handles)
            pcap_dispatch(oh.pcap, /*count=*/-1, packet_handler,
                          reinterpret_cast<uint8_t *>(&oh.ctx));
    }

    for (auto &oh : handles) pcap_close(oh.pcap);
    return 0;
}
