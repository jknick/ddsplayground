// baz.cpp
//
// Subscribes to PongBazTopic (ints forwarded by pong).
// For each received value, publishes "baz:<value>" on BazPongTopic.

#include <cstring>
#include <iostream>
#include <string>

#include "dds/dds.h"
#include "ping_pong.h"

static constexpr int kMaxSamples = 16;

int main()
{
    // ── Participant ──────────────────────────────────────────────────────────
    dds_entity_t dp = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (dp < 0) {
        std::cerr << "[baz] dds_create_participant: " << dds_strretcode(-dp) << '\n';
        return 1;
    }

    // ── QoS ─────────────────────────────────────────────────────────────────
    dds_qos_t *qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));

    // ── PongBazTopic reader (int from pong) ──────────────────────────────────
    dds_entity_t in_topic = dds_create_topic(
        dp, &PingPong_Msg_desc, "PongBazTopic", nullptr, nullptr);
    dds_entity_t reader = dds_create_reader(dp, in_topic, qos, nullptr);
    if (reader < 0) {
        std::cerr << "[baz] reader: " << dds_strretcode(-reader) << '\n';
        return 1;
    }

    // ── BazPongTopic writer (string response back to pong) ───────────────────
    dds_entity_t out_topic = dds_create_topic(
        dp, &BazResponse_Msg_desc, "BazPongTopic", nullptr, nullptr);
    dds_entity_t writer = dds_create_writer(dp, out_topic, qos, nullptr);
    if (writer < 0) {
        std::cerr << "[baz] writer: " << dds_strretcode(-writer) << '\n';
        return 1;
    }

    dds_delete_qos(qos);

    // ── Wait-set ─────────────────────────────────────────────────────────────
    dds_entity_t ws  = dds_create_waitset(dp);
    dds_entity_t cnd = dds_create_readcondition(reader, DDS_ANY_STATE);
    dds_waitset_attach(ws, cnd, 0);

    std::cout << "[baz] Ready. Waiting for messages from pong...\n\n" << std::flush;

    void            *samples[kMaxSamples];
    dds_sample_info_t infos[kMaxSamples];

    while (true) {
        dds_waitset_wait(ws, nullptr, 0, DDS_INFINITY);

        memset(samples, 0, sizeof(samples));
        dds_return_t n = dds_take(reader, samples, infos, kMaxSamples, kMaxSamples);
        for (dds_return_t i = 0; i < n; ++i) {
            if (!infos[i].valid_data) continue;
            const auto *msg = static_cast<const PingPong_Msg *>(samples[i]);

            std::string response = "baz:" + std::to_string(msg->value);
            std::cout << "[baz] Received " << msg->value
                      << " → responding \"" << response << "\"\n" << std::flush;

            // Publish the string response
            BazResponse_Msg reply{};
            reply.text = const_cast<char *>(response.c_str());  // serialized immediately
            dds_write(writer, &reply);
        }
        if (n > 0) dds_return_loan(reader, samples, n);
    }

    dds_delete(dp);
    return 0;
}
