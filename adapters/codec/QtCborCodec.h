// adapters/codec/QtCborCodec.h
//
// ICborCodec over QCborStreamWriter / QCborValue.
//
// Lives in an adapter purely so core/ stays Qt-free (§04). QtCore is LGPL and
// already in the build, so CBOR costs no new dependency.
//
// FORWARD COMPATIBILITY IS THE POINT: unknown map keys are skipped rather than
// rejected, so an older build receives a newer message type without crashing.
// That property matters a great deal once phase 5 starts adding fields.
// Decoders here must therefore never fail on an unrecognised key.

#pragma once

#include "core/protocol/AppEnvelope.h"

namespace anytime::codec {

class QtCborCodec final : public protocol::ICborCodec {
public:
    Result encode(const protocol::AppEnvelope&, ByteBuffer& out) override;
    Result decode(ByteSpan, protocol::AppEnvelope& out) override;
};

}  // namespace anytime::codec
