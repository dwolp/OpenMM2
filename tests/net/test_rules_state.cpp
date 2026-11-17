// The host's rules message (net/RulesState.h, protocol 10): its encoding,
// its limits, and fixed-seed random and mutated payloads, which must never
// crash, read out of bounds (run under the sanitizers) or decode to a value
// the writer could not have produced.
#include "net/RulesState.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <random>

using namespace mm2;
using namespace mm2::net;

namespace {

RulesMsg raceMessage() {
    RulesMsg m;
    m.race = 3;
    m.seq = 17;
    m.evaluated = 123456;
    RuleDecisionMsg f;
    f.seq = 4;
    f.type = RuleDecision::Finished;
    f.player = 2;
    f.ms = 83250;
    m.decisions.push_back(f);
    RuleDecisionMsg t;
    t.seq = 5;
    t.type = RuleDecision::TimedOut;
    m.decisions.push_back(t);
    RuleDecisionMsg a;
    a.seq = 6;
    a.type = RuleDecision::AllCounted;
    m.decisions.push_back(a);
    m.firstHit = 64;
    m.hits = {1, 2, 3, 0, 1, 2, 3, 0};
    m.place = 2;
    m.racers = 3;
    m.icons = {{0, 1}, {2, 0}};
    m.results = {{2, 83250}, {0, kRuleDnfMs}};
    m.timedOut = true;
    m.allCounted = true;
    return m;
}

RulesMsg copsMessage() {
    RulesMsg m;
    m.race = 9;
    m.seq = 1;
    m.evaluated = 77;
    m.cops = true;
    RuleDecisionMsg taken;
    taken.seq = 1;
    taken.type = RuleDecision::GoldTaken;
    taken.player = 1;
    RuleDecisionMsg dropped;
    dropped.seq = 2;
    dropped.type = RuleDecision::GoldDropped;
    dropped.player = 1;
    dropped.position = {12.5f, -3.0f, 900.0f};
    dropped.flag = true;
    RuleDecisionMsg delivered;
    delivered.seq = 3;
    delivered.type = RuleDecision::GoldDelivered;
    delivered.player = 0;
    RuleDecisionMsg set;
    set.seq = 4;
    set.type = RuleDecision::NewSet;
    set.gold = {1, 2, 3};
    set.bank = {4, 5, 6};
    set.hideout = {7, 8, 9};
    RuleDecisionMsg limit;
    limit.seq = 5;
    limit.type = RuleDecision::Limit;
    limit.flag = true;
    limit.player = 0;
    limit.value = 225;
    m.decisions = {taken, dropped, delivered, set, limit};
    m.carrier = kInvalidPlayerId;
    m.goldActive = true;
    m.goldPosition = {1, 2, 3};
    m.bank = {4, 5, 6};
    m.setGold = {1, 2, 3};
    m.hideout = {7, 8, 9};
    m.scores = {{0, 225}, {1, 25}};
    m.over = true;
    return m;
}

std::optional<RulesMsg> roundTrip(const RulesMsg& m) {
    const auto bytes = encodePayload(m);
    RulesMsg out;
    if (!decodePayload(bytes, out))
        return std::nullopt;
    return out;
}

// Every value a decoded message holds is one a writer could have sent.
void expectSane(const RulesMsg& m) {
    EXPECT_LE(m.decisions.size(), kMaxRuleDecisions);
    EXPECT_LE(m.hits.size(), kMaxRuleHits);
    EXPECT_LE(m.icons.size(), kMaxPlayers);
    EXPECT_LE(m.results.size(), kMaxPlayers);
    EXPECT_LE(m.scores.size(), kMaxPlayers);
    auto finite = [](const Vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
               std::fabs(v.x) <= kMaxRulePosition && std::fabs(v.y) <= kMaxRulePosition &&
               std::fabs(v.z) <= kMaxRulePosition;
    };
    for (const auto& d : m.decisions) {
        EXPECT_LE(static_cast<int>(d.type), static_cast<int>(RuleDecision::Last));
        EXPECT_LE(d.ms, kRuleDnfMs);
        EXPECT_TRUE(finite(d.position) && finite(d.gold) && finite(d.bank) && finite(d.hideout));
        EXPECT_GE(d.value, 0);
        EXPECT_LE(d.value, kMaxRuleScore);
    }
    for (const auto& [id, place] : m.icons) {
        EXPECT_NE(id, kInvalidPlayerId);
        EXPECT_LE(place, kMaxPlayers);
    }
    for (const auto& [id, ms] : m.results) {
        EXPECT_NE(id, kInvalidPlayerId);
        EXPECT_LE(ms, kRuleDnfMs);
    }
    if (!m.cops) {
        EXPECT_GE(m.place, 1);
        EXPECT_LE(m.place, kMaxPlayers);
        EXPECT_GE(m.racers, 1);
        EXPECT_LE(m.racers, kMaxPlayers);
    }
    EXPECT_TRUE(finite(m.goldPosition) && finite(m.bank) && finite(m.setGold) && finite(m.hideout));
    for (const auto& [id, score] : m.scores) {
        EXPECT_NE(id, kInvalidPlayerId);
        EXPECT_GE(score, 0);
        EXPECT_LE(score, kMaxRuleScore);
    }
}

} // namespace

TEST(RulesState, RaceAndCopsMessagesRoundTrip) {
    const auto race = raceMessage();
    const auto r = roundTrip(race);
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, race);
    const auto cops = copsMessage();
    const auto c = roundTrip(cops);
    ASSERT_TRUE(c);
    EXPECT_EQ(*c, cops);
    // Both fit an event with room to spare at their limits.
    RulesMsg big = raceMessage();
    big.hits.assign(kMaxRuleHits, 200);
    big.decisions.assign(kMaxRuleDecisions, cops.decisions[3]);
    big.icons.clear();
    big.results.clear();
    for (std::uint8_t i = 0; i < kMaxPlayers; ++i) {
        big.icons.emplace_back(i, 3);
        big.results.emplace_back(i, 1000u * i);
    }
    EXPECT_LE(encodePayload(big).size(), kMaxEventPayload);
    EXPECT_TRUE(roundTrip(big));
}

TEST(RulesState, OutOfRangeMessagesAreRefused) {
    auto refused = [](RulesMsg m) { return !roundTrip(m).has_value(); };
    RulesMsg m = raceMessage();
    m.hits.assign(kMaxRuleHits + 1, 1);
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    const auto decision = m.decisions[0]; // a copy: assign() invalidates the elements a reference would name
    m.decisions.assign(kMaxRuleDecisions + 1, decision);
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    m.decisions[0].ms = kRuleDnfMs + 1;
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    m.decisions[0].player = kInvalidPlayerId; // a finish is somebody's
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    m.place = 0;
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    m.racers = 40;
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    m.icons.emplace_back(kInvalidPlayerId, 1);
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    m.icons[0].second = 200;
    EXPECT_TRUE(refused(m));
    m = raceMessage();
    m.results[0].second = kRuleDnfMs + 5;
    EXPECT_TRUE(refused(m));
    RulesMsg c = copsMessage();
    c.decisions[1].position.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(refused(c));
    c = copsMessage();
    c.decisions[3].bank.z = 1e9f;
    EXPECT_TRUE(refused(c));
    c = copsMessage();
    c.goldPosition.y = std::numeric_limits<float>::infinity();
    EXPECT_TRUE(refused(c));
    c = copsMessage();
    c.scores.emplace_back(kInvalidPlayerId, 3);
    EXPECT_TRUE(refused(c));
    // A score beyond the range is clamped by the writer, never read as more.
    c = copsMessage();
    c.scores[0].second = kMaxRuleScore * 2;
    const auto out = roundTrip(c);
    ASSERT_TRUE(out);
    EXPECT_EQ(out->scores[0].second, kMaxRuleScore);
    // A truncated message is refused.
    auto bytes = encodePayload(raceMessage());
    RulesMsg in;
    EXPECT_TRUE(decodePayload(bytes, in));
    bytes.resize(bytes.size() / 3);
    EXPECT_FALSE(decodePayload(bytes, in));
}

TEST(RulesState, RandomAndMutatedPayloadsDecodeSafely) {
    std::mt19937 rng(20261009);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> len(0, 600);
    int decoded = 0;
    for (int i = 0; i < 4000; ++i) {
        std::vector<std::byte> bytes(static_cast<std::size_t>(len(rng)));
        for (auto& b : bytes)
            b = static_cast<std::byte>(byte(rng));
        RulesMsg m;
        if (decodePayload(bytes, m)) {
            ++decoded;
            expectSane(m);
        }
    }
    const std::vector<std::vector<std::byte>> seeds{encodePayload(raceMessage()),
                                                    encodePayload(copsMessage())};
    for (int i = 0; i < 6000; ++i) {
        auto bytes = seeds[static_cast<std::size_t>(i) % seeds.size()];
        const int flips = 1 + i % 6;
        for (int k = 0; k < flips && !bytes.empty(); ++k) {
            const auto at = static_cast<std::size_t>(rng()) % bytes.size();
            bytes[at] ^= static_cast<std::byte>(1 << (rng() % 8));
        }
        if (i % 7 == 0)
            bytes.resize(static_cast<std::size_t>(rng()) % (bytes.size() + 1));
        RulesMsg m;
        if (decodePayload(bytes, m)) {
            ++decoded;
            expectSane(m);
        }
    }
    EXPECT_GT(decoded, 0);
}

TEST(RulesState, TheUnreliableStateCarriesARacesStateAlone) {
    // Protocol 14: RulesState on the State channel, the same message
    // without decisions or results; anything else is refused.
    RulesStateMsg st;
    st.rules = raceMessage();
    st.rules.decisions.clear();
    st.rules.results.clear();
    RulesStateMsg back;
    ASSERT_TRUE(decodeMessage(encodeMessage(st), back));
    EXPECT_EQ(back.rules, st.rules);
    RulesStateMsg withDecisions;
    withDecisions.rules = raceMessage();
    ASSERT_FALSE(withDecisions.rules.decisions.empty());
    EXPECT_FALSE(decodeMessage(encodeMessage(withDecisions), back));
    RulesStateMsg cops;
    cops.rules = copsMessage();
    cops.rules.decisions.clear();
    EXPECT_FALSE(decodeMessage(encodeMessage(cops), back));
    // Random and damaged packets decode safely, or not at all.
    std::mt19937 rng(14);
    const auto seed = encodeMessage(st);
    int decoded = 0;
    for (int i = 0; i < 3000; ++i) {
        auto bytes = seed;
        for (int k = 0; k < 1 + i % 5; ++k) {
            const auto at = 1 + static_cast<std::size_t>(rng()) % (bytes.size() - 1);
            bytes[at] ^= static_cast<std::byte>(1 << (rng() % 8));
        }
        if (i % 5 == 0)
            bytes.resize(1 + static_cast<std::size_t>(rng()) % bytes.size());
        RulesStateMsg m;
        if (decodeMessage(bytes, m)) {
            ++decoded;
            EXPECT_FALSE(m.rules.cops);
            EXPECT_TRUE(m.rules.decisions.empty());
            EXPECT_TRUE(m.rules.results.empty());
            expectSane(m.rules);
        }
    }
    EXPECT_GT(decoded, 0);
}
