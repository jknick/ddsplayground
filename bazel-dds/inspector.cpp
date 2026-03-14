// inspector.cpp – spies on RTPS discovery for all participants/endpoints AND
// echoes every message on every user topic, annotated with sender topic,
// receiver topic, endpoint GUIDs, and payload.

#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "dds/dds.h"
#include "ping_pong.h"   // PingPong_Msg_desc + BazResponse_Msg_desc

// ── GUID helpers ─────────────────────────────────────────────────────────────

static std::string fmt_guid(const dds_guid_t &g)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x:%02x%02x%02x%02x:%02x%02x%02x%02x:%02x%02x%02x%02x",
        g.v[0],  g.v[1],  g.v[2],  g.v[3],
        g.v[4],  g.v[5],  g.v[6],  g.v[7],
        g.v[8],  g.v[9],  g.v[10], g.v[11],
        g.v[12], g.v[13], g.v[14], g.v[15]);
    return buf;
}

static const char *instance_state_str(uint32_t s)
{
    if (s & DDS_ALIVE_INSTANCE_STATE)              return "ALIVE";
    if (s & DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE) return "DISPOSED";
    return "NOT_ALIVE_NO_WRITERS";
}

// ── Endpoint registry (populated by discovery drain helpers) ─────────────────

struct EndpointRecord {
    std::string guid;
    std::string participant_guid;
    std::string topic;
    bool        is_publisher;
};

static std::map<std::string, EndpointRecord> g_endpoints;

// ── Discovery drain helpers ───────────────────────────────────────────────────

static void drain_participants(dds_entity_t rd)
{
    static constexpr int N = 16;
    void             *raw[N];
    dds_sample_info_t inf[N];
    memset(raw, 0, sizeof(raw));

    dds_return_t n = dds_take(rd, raw, inf, N, N);
    for (dds_return_t i = 0; i < n; ++i) {
        const auto *p = static_cast<const dds_builtintopic_participant_t *>(raw[i]);
        std::cout
            << "┌─ [PARTICIPANT " << instance_state_str(inf[i].instance_state) << "]\n"
            << "│  GUID : " << fmt_guid(p->key) << "\n"
            << "└──────────────────────────────────────────────────────────\n\n"
            << std::flush;
    }
    if (n > 0) dds_return_loan(rd, raw, n);
}

static void drain_endpoints(dds_entity_t rd, bool is_publisher)
{
    const char *role = is_publisher ? "PUBLISHER" : "SUBSCRIBER";

    static constexpr int N = 16;
    void             *raw[N];
    dds_sample_info_t inf[N];
    memset(raw, 0, sizeof(raw));

    dds_return_t n = dds_take(rd, raw, inf, N, N);
    for (dds_return_t i = 0; i < n; ++i) {
        const auto *ep    = static_cast<const dds_builtintopic_endpoint_t *>(raw[i]);
        const char *state = instance_state_str(inf[i].instance_state);
        std::string guid  = fmt_guid(ep->key);
        std::string pguid = fmt_guid(ep->participant_key);
        const char *topic = ep->topic_name ? ep->topic_name : "";
        const char *type  = ep->type_name  ? ep->type_name  : "";

        std::cout
            << "┌─ [" << role << " " << state << "]\n"
            << "│  Endpoint GUID    : " << guid  << "\n"
            << "│  Participant GUID : " << pguid << "\n"
            << "│  Topic            : " << topic << "\n"
            << "│  Type             : " << type  << "\n"
            << "└──────────────────────────────────────────────────────────\n\n"
            << std::flush;

        if (inf[i].instance_state & DDS_ALIVE_INSTANCE_STATE)
            g_endpoints[guid] = {guid, pguid, topic, is_publisher};
        else
            g_endpoints.erase(guid);
    }
    if (n > 0) dds_return_loan(rd, raw, n);
}

// ── Generic payload echo ──────────────────────────────────────────────────────
// fmt_payload(sample_ptr) → human-readable string for the message payload.

static void drain_msg(dds_entity_t rd,
                      std::function<std::string(const void *)> fmt_payload)
{
    static constexpr int N = 16;
    void             *raw[N];
    dds_sample_info_t inf[N];
    memset(raw, 0, sizeof(raw));

    dds_return_t n = dds_take(rd, raw, inf, N, N);
    for (dds_return_t i = 0; i < n; ++i) {
        if (!inf[i].valid_data) continue;

        // ── Sender info ───────────────────────────────────────────────────────
        std::string sender_ep    = "(unknown)";
        std::string sender_part  = "(unknown)";
        std::string sender_topic = "(unknown)";
        dds_builtintopic_endpoint_t *pub =
            dds_get_matched_publication_data(rd, inf[i].publication_handle);
        if (pub) {
            sender_ep    = fmt_guid(pub->key);
            sender_part  = fmt_guid(pub->participant_key);
            sender_topic = pub->topic_name ? pub->topic_name : "(unknown)";
            dds_builtintopic_free_endpoint(pub);
        }

        // ── Receiver info ─────────────────────────────────────────────────────
        char recv_buf[256] = {};
        dds_get_name(dds_get_topic(rd), recv_buf, sizeof(recv_buf));
        std::string recv_topic = recv_buf;

        std::vector<std::pair<std::string, std::string>> receivers;
        for (const auto &kv : g_endpoints) {
            if (!kv.second.is_publisher && kv.second.topic == recv_topic)
                receivers.push_back({kv.second.guid, kv.second.participant_guid});
        }

        // ── Print ─────────────────────────────────────────────────────────────
        std::cout
            << "┌─ [MSG  payload=" << fmt_payload(raw[i]) << "]\n"
            << "│  Sender topic     : " << sender_topic << "\n"
            << "│  Sender endpoint  : " << sender_ep    << "\n"
            << "│  Sender particip. : " << sender_part  << "\n"
            << "│  Receiver topic   : " << recv_topic   << "\n";

        if (receivers.empty()) {
            std::cout << "│  Receiver         : (none discovered yet)\n";
        } else {
            for (size_t r = 0; r < receivers.size(); ++r) {
                std::cout
                    << "│  Receiver[" << r << "] ep   : " << receivers[r].first  << "\n"
                    << "│  Receiver[" << r << "] part : " << receivers[r].second << "\n";
            }
        }
        std::cout
            << "└──────────────────────────────────────────────────────────\n\n"
            << std::flush;
    }
    if (n > 0) dds_return_loan(rd, raw, n);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::cout
        << "╔══════════════════════════════════════════════════════════╗\n"
        << "║          RTPS Discovery Inspector (CycloneDDS)           ║\n"
        << "╚══════════════════════════════════════════════════════════╝\n"
        << "Domain  : default\n"
        << "Echoing : all user-topic messages with sender/receiver info\n\n"
        << std::flush;

    dds_entity_t dp = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (dp < 0) {
        std::cerr << "dds_create_participant: " << dds_strretcode(-dp) << '\n';
        return 1;
    }

    // ── Built-in topic readers (discovery) ────────────────────────────────────
    dds_entity_t par_rd = dds_create_reader(dp, DDS_BUILTIN_TOPIC_DCPSPARTICIPANT,  nullptr, nullptr);
    dds_entity_t pub_rd = dds_create_reader(dp, DDS_BUILTIN_TOPIC_DCPSPUBLICATION,  nullptr, nullptr);
    dds_entity_t sub_rd = dds_create_reader(dp, DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION, nullptr, nullptr);
    if (par_rd < 0 || pub_rd < 0 || sub_rd < 0) {
        std::cerr << "Failed to open built-in topic readers.\n";
        return 1;
    }

    // ── Reliable QoS (matches ping/pong/baz writers) ──────────────────────────
    dds_qos_t *qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));

    // ── User-topic readers ────────────────────────────────────────────────────
    struct TopicReader {
        dds_entity_t                        rd;
        std::function<std::string(const void *)> fmt;
    };

    auto make_rd = [&](const dds_topic_descriptor_t *desc, const char *name) {
        dds_entity_t t = dds_create_topic(dp, desc, name, nullptr, nullptr);
        return dds_create_reader(dp, t, qos, nullptr);
    };

    std::vector<TopicReader> topic_readers = {
        {
            make_rd(&PingPong_Msg_desc, "PingPongTopic"),
            [](const void *p) {
                return std::to_string(static_cast<const PingPong_Msg *>(p)->value);
            }
        },
        {
            make_rd(&PingPong_Msg_desc, "PongBazTopic"),
            [](const void *p) {
                return std::to_string(static_cast<const PingPong_Msg *>(p)->value);
            }
        },
        {
            make_rd(&BazResponse_Msg_desc, "BazPongTopic"),
            [](const void *p) {
                const auto *m = static_cast<const BazResponse_Msg *>(p);
                return std::string(m->text ? m->text : "");
            }
        },
    };

    dds_delete_qos(qos);

    for (const auto &tr : topic_readers) {
        if (tr.rd < 0) {
            std::cerr << "Failed to create a user-topic reader.\n";
            return 1;
        }
    }

    // ── Single wait-set: IDs 0-2 discovery, 3+ user topics ───────────────────
    dds_entity_t ws = dds_create_waitset(dp);

    dds_waitset_attach(ws, dds_create_readcondition(par_rd, DDS_ANY_STATE), 0);
    dds_waitset_attach(ws, dds_create_readcondition(pub_rd, DDS_ANY_STATE), 1);
    dds_waitset_attach(ws, dds_create_readcondition(sub_rd, DDS_ANY_STATE), 2);

    for (dds_attach_t id = 0; id < (dds_attach_t)topic_readers.size(); ++id) {
        dds_entity_t cnd = dds_create_readcondition(topic_readers[id].rd, DDS_ANY_STATE);
        dds_waitset_attach(ws, cnd, 3 + id);
    }

    // Drain any state already cached
    drain_participants(par_rd);
    drain_endpoints(pub_rd, true);
    drain_endpoints(sub_rd, false);

    std::cout << "Waiting for events...\n\n" << std::flush;

    // ── Event loop ────────────────────────────────────────────────────────────
    dds_attach_t triggered[16];
    while (true) {
        dds_return_t n = dds_waitset_wait(ws, triggered, 16, DDS_INFINITY);
        if (n < 0) {
            std::cerr << "waitset error: " << dds_strretcode(-n) << '\n';
            break;
        }
        for (dds_return_t i = 0; i < n; ++i) {
            switch (triggered[i]) {
                case 0: drain_participants(par_rd);     break;
                case 1: drain_endpoints(pub_rd, true);  break;
                case 2: drain_endpoints(sub_rd, false); break;
                default: {
                    auto idx = triggered[i] - 3;
                    drain_msg(topic_readers[idx].rd, topic_readers[idx].fmt);
                    break;
                }
            }
        }
    }

    dds_delete(dp);
    return 0;
}
