// hoge.cpp – subscribes to PingPongTopic and prints every integer from ping.

#include <cstring>
#include <iostream>

#include "dds/dds.h"
#include "ping_pong.h"

static constexpr int kMaxSamples = 16;

int main()
{
    // ── Participant ──────────────────────────────────────────────────────────
    dds_entity_t dp = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
    if (dp < 0) {
        std::cerr << "[hoge] dds_create_participant: " << dds_strretcode(-dp) << '\n';
        return 1;
    }

    // ── QoS ─────────────────────────────────────────────────────────────────
    dds_qos_t *qos = dds_create_qos();
    dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(10));

    // ── PingPongTopic reader (from ping) ─────────────────────────────────────
    dds_entity_t topic = dds_create_topic(
        dp, &PingPong_Msg_desc, "PingPongTopic", nullptr, nullptr);
    dds_entity_t reader = dds_create_reader(dp, topic, qos, nullptr);
    dds_delete_qos(qos);
    if (reader < 0) {
        std::cerr << "[hoge] dds_create_reader: " << dds_strretcode(-reader) << '\n';
        return 1;
    }

    // ── Wait-set ─────────────────────────────────────────────────────────────
    dds_entity_t ws  = dds_create_waitset(dp);
    dds_entity_t cnd = dds_create_readcondition(reader, DDS_ANY_STATE);
    dds_waitset_attach(ws, cnd, 0);

    std::cout << "[hoge] Ready. Waiting for messages from ping...\n\n" << std::flush;

    void            *samples[kMaxSamples];
    dds_sample_info_t infos[kMaxSamples];

    while (true) {
        dds_waitset_wait(ws, nullptr, 0, DDS_INFINITY);

        memset(samples, 0, sizeof(samples));
        dds_return_t n = dds_take(reader, samples, infos, kMaxSamples, kMaxSamples);
        for (dds_return_t i = 0; i < n; ++i) {
            if (!infos[i].valid_data) continue;
            const auto *msg = static_cast<const PingPong_Msg *>(samples[i]);
            std::cout << "[hoge] Received: " << msg->value << '\n' << std::flush;
        }
        if (n > 0) dds_return_loan(reader, samples, n);
    }

    dds_delete(dp);
    return 0;
}
