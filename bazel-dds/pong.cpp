// pong.cpp
//
// Topology:
//   ping  ──PingPongTopic(int)──►  pong  ──PongBazTopic(int)──►  baz
//                                   ▲                               │
//                                   └────BazPongTopic(string)───────┘
//
// Pong prints every integer it receives from ping AND every string it
// receives from baz.  It also forwards each ping value to baz.

#include <cstring>
#include <iostream>
#include <string>

#include "dds/dds.h"
#include "ping_pong.h"

// Return the topic name this reader is subscribed to.
static std::string receiver_topic(dds_entity_t reader)
{
    dds_entity_t t = dds_get_topic(reader);
    char buf[256] = {};
    dds_get_name(t, buf, sizeof(buf));
    return buf;
}

// Return the topic name the matched writer published on (from sample info).
static std::string sender_topic(dds_entity_t reader, dds_instance_handle_t pub_handle)
{
    dds_builtintopic_endpoint_t *ep =
        dds_get_matched_publication_data(reader, pub_handle);
    if (!ep) return "(unknown)";
    std::string name = ep->topic_name ? ep->topic_name : "(unknown)";
    dds_builtintopic_free_endpoint(ep);
    return name;
}

static constexpr int kMaxSamples = 16;

int main()
{
    // ── Participant ──────────────────────────────────────────────────────────
    dds_entity_t dp = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (dp < 0) {
        std::cerr << "[pong] dds_create_participant: " << dds_strretcode(-dp) << '\n';
        return 1;
    }

    // ── QoS ─────────────────────────────────────────────────────────────────
    dds_qos_t *qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));

    // ── PingPongTopic reader (from ping) ─────────────────────────────────────
    dds_entity_t ping_topic = dds_create_topic(
        dp, &PingPong_Msg_desc, "PingPongTopic", nullptr, nullptr);
    dds_entity_t ping_reader = dds_create_reader(dp, ping_topic, qos, nullptr);
    if (ping_reader < 0) {
        std::cerr << "[pong] ping reader: " << dds_strretcode(-ping_reader) << '\n';
        return 1;
    }

    // ── PongBazTopic writer (to baz) ─────────────────────────────────────────
    dds_entity_t baz_out_topic = dds_create_topic(
        dp, &PingPong_Msg_desc, "PongBazTopic", nullptr, nullptr);
    dds_entity_t baz_writer = dds_create_writer(dp, baz_out_topic, qos, nullptr);
    if (baz_writer < 0) {
        std::cerr << "[pong] baz writer: " << dds_strretcode(-baz_writer) << '\n';
        return 1;
    }

    // ── BazPongTopic reader (response from baz) ──────────────────────────────
    dds_entity_t baz_in_topic = dds_create_topic(
        dp, &BazResponse_Msg_desc, "BazPongTopic", nullptr, nullptr);
    dds_entity_t baz_reader = dds_create_reader(dp, baz_in_topic, qos, nullptr);
    if (baz_reader < 0) {
        std::cerr << "[pong] baz reader: " << dds_strretcode(-baz_reader) << '\n';
        return 1;
    }

    dds_delete_qos(qos);

    // ── Wait-set: wake on ping data OR baz response ──────────────────────────
    dds_entity_t ws       = dds_create_waitset(dp);
    dds_entity_t ping_cnd = dds_create_readcondition(ping_reader, DDS_ANY_STATE);
    dds_entity_t baz_cnd  = dds_create_readcondition(baz_reader,  DDS_ANY_STATE);
    dds_waitset_attach(ws, ping_cnd, 0);   // 0 → ping data
    dds_waitset_attach(ws, baz_cnd,  1);   // 1 → baz response

    std::cout << "[pong] Ready. Waiting for messages...\n\n" << std::flush;

    void            *samples[kMaxSamples];
    dds_sample_info_t infos[kMaxSamples];

    while (true) {
        dds_attach_t triggered[4];
        dds_return_t nt = dds_waitset_wait(ws, triggered, 4, DDS_INFINITY);

        for (dds_return_t t = 0; t < nt; ++t) {

            if (triggered[t] == 0) {
                // ── Ping message arrived ─────────────────────────────────────
                memset(samples, 0, sizeof(samples));
                dds_return_t n = dds_take(ping_reader, samples, infos, kMaxSamples, kMaxSamples);
                for (dds_return_t i = 0; i < n; ++i) {
                    if (!infos[i].valid_data) continue;
                    const auto *msg = static_cast<const PingPong_Msg *>(samples[i]);
                    std::cout
                        << "[pong] From ping  : " << msg->value
                        << "  (sender topic: " << sender_topic(ping_reader, infos[i].publication_handle)
                        << ", receiver topic: " << receiver_topic(ping_reader) << ")\n"
                        << std::flush;

                    // Forward the integer value to baz
                    PingPong_Msg fwd{};
                    fwd.value = msg->value;
                    dds_write(baz_writer, &fwd);
                }
                if (n > 0) dds_return_loan(ping_reader, samples, n);

            } else if (triggered[t] == 1) {
                // ── Baz response arrived ─────────────────────────────────────
                memset(samples, 0, sizeof(samples));
                dds_return_t n = dds_take(baz_reader, samples, infos, kMaxSamples, kMaxSamples);
                for (dds_return_t i = 0; i < n; ++i) {
                    if (!infos[i].valid_data) continue;
                    const auto *msg = static_cast<const BazResponse_Msg *>(samples[i]);
                    std::cout
                        << "[pong] From baz   : " << (msg->text ? msg->text : "")
                        << "  (sender topic: " << sender_topic(baz_reader, infos[i].publication_handle)
                        << ", receiver topic: " << receiver_topic(baz_reader) << ")\n"
                        << std::flush;
                }
                if (n > 0) dds_return_loan(baz_reader, samples, n);
            }
        }
    }

    dds_delete(dp);
    return 0;
}
