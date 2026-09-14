#include "fixed_mpmc_ring.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {
struct Payload {
    std::uint32_t producer = 0;
    std::uint32_t sequence = 0;
};

void basic_test() {
    FixedMpmcRing<Payload, 8> ring;
    Payload out{};
    assert(!ring.try_dequeue(&out));

    for (std::uint32_t i = 0; i < 8; ++i)
        assert(ring.try_enqueue(Payload{1, i}));
    assert(!ring.try_enqueue(Payload{1, 99}));

    for (std::uint32_t i = 0; i < 8; ++i) {
        assert(ring.try_dequeue(&out));
        assert(out.producer == 1);
        assert(out.sequence == i);
    }
    assert(!ring.try_dequeue(&out));

    ring.reset();
    assert(ring.try_enqueue(Payload{2, 7}));
    assert(ring.try_dequeue(&out));
    assert(out.producer == 2 && out.sequence == 7);
}

void concurrent_test() {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kPerProducer = 20000;
    constexpr std::uint32_t kTotal = kProducers * kPerProducer;

    FixedMpmcRing<Payload, 64> ring;
    std::atomic<bool> start{false};
    std::atomic<std::uint32_t> producers_done{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (std::uint32_t producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (std::uint32_t sequence = 0; sequence < kPerProducer; ++sequence) {
                const Payload payload{producer, sequence};
                while (!ring.try_enqueue(payload))
                    std::this_thread::yield();
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    std::vector<std::uint8_t> seen(kTotal, 0);
    start.store(true, std::memory_order_release);

    std::uint32_t consumed = 0;
    while (consumed < kTotal) {
        Payload payload{};
        if (!ring.try_dequeue(&payload)) {
            // The first dequeue probe may race with a producer that has
            // reserved the head cell but has not published its sequence yet.
            // Observe producer completion with acquire semantics, then retry:
            // if every producer is done, all prior cell publications happen-
            // before this thread and the queue must contain the remaining item.
            if (producers_done.load(std::memory_order_acquire) < kProducers) {
                std::this_thread::yield();
                continue;
            }
            assert(ring.try_dequeue(&payload));
        }

        assert(payload.producer < kProducers);
        assert(payload.sequence < kPerProducer);
        const std::uint32_t index = payload.producer * kPerProducer + payload.sequence;
        assert(seen[index] == 0);
        seen[index] = 1;
        ++consumed;
    }

    for (auto& producer : producers)
        producer.join();

    assert(producers_done.load(std::memory_order_acquire) == kProducers);
    for (const auto flag : seen)
        assert(flag == 1);

    Payload out{};
    assert(!ring.try_dequeue(&out));
}
} // namespace

int main() {
    basic_test();
    concurrent_test();
    std::cout << "PASS: fixed MPMC ring basic/full/reset + 4-producer stress\n";
    return 0;
}
