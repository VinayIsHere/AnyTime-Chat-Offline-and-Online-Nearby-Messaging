// tests/FragmenterTests.cpp
//
// Fragmentation lives in the core precisely so it is written and tested once
// rather than reimplemented per platform (docs/ARCHITECTURE.md section 11).
// The reassembler consumes untrusted input, so the bounds are tested as
// carefully as the happy path.

#include "tests/TestHarness.h"

#include "core/protocol/Fragmenter.h"
#include "core/protocol/PacketCodec.h"

namespace anytime::test {

using namespace anytime::protocol;

namespace {

PeerId peerFrom(Byte fill)
{
    PeerIdBytes bytes{};
    bytes.fill(fill);
    return PeerId{bytes};
}

ByteBuffer sequence(std::size_t length)
{
    ByteBuffer out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) out.push_back(static_cast<Byte>(i & 0xFF));
    return out;
}

}  // namespace

void runFragmenterTests(Registry& r)
{
    const PeerId sender = peerFrom(0x5A);
    const FragmentId fragmentId = Fragmenter::newFragmentId(sender, 12345);

    r.section("Fragmenter — splitting");

    {
        const ByteBuffer small = sequence(50);
        auto frames = Fragmenter::split(small, 180, fragmentId);
        r.check(frames.isOk(), "splits a small packet");
        r.check(frames.value().size() == 1, "a packet that already fits is not fragmented");
        r.check(frames.value().front() == small, "and is returned byte-for-byte unchanged");
    }

    {
        const ByteBuffer large = sequence(1000);
        auto frames = Fragmenter::split(large, 180, fragmentId);
        r.check(frames.isOk(), "splits a large packet");
        r.check(frames.value().size() > 1, "produces several fragments");

        bool allWithinBudget = true;
        for (const ByteBuffer& frame : frames.value()) {
            if (frame.size() > 180) { allWithinBudget = false; break; }
        }
        r.check(allWithinBudget, "every fragment respects the frame budget");
    }

    {
        auto frames = Fragmenter::split(sequence(100), 8, fragmentId);
        r.check(frames.isError(), "refuses a frame budget smaller than the fragment header");
    }

    {
        // REGRESSION: the packet wrapper has to fit INSIDE the transport budget.
        //
        // MeshEngine re-wraps every slice in its own directed Fragment packet, so
        // splitting against the full budget produced frames larger than the budget
        // they were fragmented to — 452 + 28 = 480 fits, but the old 480 + 28 = 508
        // did not. LoopbackTransport rejects such a frame outright, and on BLE it
        // sat two bytes below the 512-byte write that would have cost every
        // fragment a second chunk.
        //
        // The arithmetic is asserted end to end rather than pinned to a number, so
        // widening any header field fails here instead of on a handset.
        const std::size_t budget = kFragmentPayloadBytes;
        const std::size_t wrapperBytes =
            kFixedHeaderBytes + kRecipientFieldBytes + kPayloadLengthBytes;

        auto frames = Fragmenter::split(sequence(20 * 1024), budget - wrapperBytes, fragmentId);
        r.check(frames.isOk(), "splits against a budget that reserves the packet wrapper");
        r.check(frames.value().size() > 1, "and the sample really does fragment");

        bool allFit = true;
        for (const ByteBuffer& slice : frames.value()) {
            PacketToEncode wrapper;
            wrapper.type = PacketType::Fragment;
            wrapper.sender = sender;
            wrapper.recipient = peerFrom(0x77);  // directed: the recipient field is present
            wrapper.payload = slice;

            ByteBuffer wrapped;
            if (PacketCodec::encode(wrapper, wrapped).isError() || wrapped.size() > budget) {
                allFit = false;
                break;
            }
        }
        r.check(allFit, "every wrapped fragment fits the transport frame budget");
    }

    r.section("Fragmenter — reassembly round trip");

    {
        const ByteBuffer original = sequence(1000);
        auto frames = Fragmenter::split(original, 180, fragmentId);

        Reassembler reassembler;
        std::optional<ByteBuffer> completed;

        for (const ByteBuffer& frame : frames.value()) {
            auto result = reassembler.accept(sender, frame, 0);
            if (result.isOk() && result.value().has_value()) completed = result.value();
        }
        r.check(completed.has_value(), "reassembles once the last fragment arrives");
        r.check(completed.has_value() && *completed == original,
                "reassembled bytes match the original exactly");
        r.check(reassembler.pendingCount() == 0, "completed assembly is released");
    }

    {
        // Out-of-order delivery is normal on a mesh; order must not matter.
        const ByteBuffer original = sequence(700);
        auto frames = Fragmenter::split(original, 180, fragmentId);

        Reassembler reassembler;
        std::optional<ByteBuffer> completed;

        auto list = frames.value();
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            auto result = reassembler.accept(sender, *it, 0);
            if (result.isOk() && result.value().has_value()) completed = result.value();
        }
        r.check(completed.has_value() && *completed == original,
                "reassembles correctly from reversed fragments");
    }

    {
        // A repeated fragment must not corrupt the assembly. Only non-final
        // fragments are duplicated here: re-feeding the LAST one after the
        // assembly completed would correctly open a fresh assembly, which is
        // right behaviour rather than a duplicate.
        const ByteBuffer original = sequence(500);
        auto frames = Fragmenter::split(original, 180, fragmentId);
        auto list = frames.value();

        Reassembler reassembler;
        std::optional<ByteBuffer> completed;

        for (std::size_t i = 0; i + 1 < list.size(); ++i) {
            reassembler.accept(sender, list[i], 0);
            reassembler.accept(sender, list[i], 0);  // duplicate
            reassembler.accept(sender, list[i], 0);  // and again
        }
        auto final_ = reassembler.accept(sender, list.back(), 0);
        if (final_.isOk() && final_.value().has_value()) completed = final_.value();

        r.check(completed.has_value(), "duplicated fragments still reassemble");
        r.check(completed.has_value() && *completed == original,
                "duplicates do not corrupt the reassembled bytes");
    }

    r.section("Fragmenter — rejects hostile input");

    {
        Reassembler reassembler;
        const ByteBuffer runt{1, 2, 3};
        r.check(reassembler.accept(sender, runt, 0).isError(),
                "rejects a fragment shorter than its header");
    }

    {
        // index >= total is nonsense and must not index out of bounds.
        ByteBuffer bad;
        for (std::size_t i = 0; i < kFragmentIdBytes; ++i) bad.push_back(0xAB);
        bad.push_back(0x00); bad.push_back(0x05);  // index 5
        bad.push_back(0x00); bad.push_back(0x03);  // total 3
        bad.push_back(0xFF);

        Reassembler reassembler;
        r.check(reassembler.accept(sender, bad, 0).isError(),
                "rejects a fragment index beyond the total");
    }

    {
        ByteBuffer zeroTotal;
        for (std::size_t i = 0; i < kFragmentIdBytes; ++i) zeroTotal.push_back(0xCD);
        zeroTotal.push_back(0x00); zeroTotal.push_back(0x00);  // index 0
        zeroTotal.push_back(0x00); zeroTotal.push_back(0x00);  // total 0
        zeroTotal.push_back(0xFF);

        Reassembler reassembler;
        r.check(reassembler.accept(sender, zeroTotal, 0).isError(),
                "rejects a zero fragment count");
    }

    {
        // Assemblies a peer opens and never completes must age out, or a hostile
        // peer could hold memory indefinitely.
        const ByteBuffer original = sequence(1000);
        auto frames = Fragmenter::split(original, 180, fragmentId);

        Reassembler reassembler(128, 30'000, 1024 * 1024);
        reassembler.accept(sender, frames.value().front(), 0);
        r.check(reassembler.pendingCount() == 1, "incomplete assembly is held");

        reassembler.evictStale(60'000);
        r.check(reassembler.pendingCount() == 0, "stale assembly is evicted after the timeout");
    }

    {
        // The concurrency cap must hold against a flood of distinct assemblies.
        Reassembler reassembler(4, 30'000, 1024 * 1024);

        for (int i = 0; i < 10; ++i) {
            ByteBuffer frame;
            for (std::size_t b = 0; b < kFragmentIdBytes; ++b) {
                frame.push_back(static_cast<Byte>(i));  // distinct fragment id each time
            }
            frame.push_back(0x00); frame.push_back(0x00);  // index 0
            frame.push_back(0x00); frame.push_back(0x09);  // total 9 — never completed
            frame.push_back(0x42);
            reassembler.accept(sender, frame, 0);
        }
        r.check(reassembler.pendingCount() <= 4, "never exceeds the concurrent assembly cap");
    }
}

}  // namespace anytime::test
