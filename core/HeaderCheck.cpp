// core/HeaderCheck.cpp
//
// Compiles every public header in core/ so the compiler validates them even
// before their .cpp files exist.
//
// Without this, a header with a missing #include or a typo sits undetected until
// the phase that finally implements it — which is exactly when you least want to
// be fixing declarations. Keeping this file in the build means every header is
// syntax-checked on every compile, from day one.
//
// Add a line here whenever you add a header under core/. Delete this file only
// when every header below has a real .cpp of its own in the build.

// domain — each included directly, so a header that only compiles because
// something else included its dependencies first is caught here.
#include "core/domain/Primitives.h"
#include "core/domain/Enums.h"
#include "core/domain/Identifiers.h"
#include "core/domain/GlobalStructures.h"
#include "core/domain/Types.h"
#include "core/domain/Result.h"
#include "core/domain/Entities.h"

#include "core/domain/constants/CryptoConstants.h"
#include "core/domain/constants/DeliveryConstants.h"
#include "core/domain/constants/DiscoveryConstants.h"
#include "core/domain/constants/NetworkConstants.h"
#include "core/domain/constants/ProtocolConstants.h"
#include "core/domain/constants/RequestConstants.h"

#include "core/ports/IClock.h"
#include "core/ports/IEventSink.h"
#include "core/ports/IIdentityStore.h"
#include "core/ports/IRepositories.h"
#include "core/ports/ISecureSession.h"
#include "core/ports/ITransport.h"

#include "core/protocol/AppEnvelope.h"
#include "core/protocol/DedupCache.h"
#include "core/protocol/Fragmenter.h"
#include "core/protocol/Packet.h"
#include "core/protocol/PacketCodec.h"

#include "core/services/AccountService.h"
#include "core/services/ChatService.h"
#include "core/services/DeliveryService.h"
#include "core/services/DiscoveryService.h"
#include "core/services/MeshEngine.h"
#include "core/services/RequestService.h"
#include "core/services/RoutingPolicy.h"
#include "core/services/SessionManager.h"
#include "core/services/TransportRegistry.h"

#include "core/AppCore.h"

namespace anytime::header_check {

// A few static assertions that pin down assumptions the protocol relies on.
// If any of these ever fails, something load-bearing changed silently.

static_assert(kPeerIdBytes == 8,
              "PeerId is 8 bytes on the wire — see docs/ARCHITECTURE.md section 07");
static_assert(kPublicKeyBytes == 32, "X25519 and Ed25519 public keys are 32 bytes");
static_assert(kSignatureBytes == 64, "Ed25519 signatures are 64 bytes");

static_assert(protocol::kFixedHeaderBytes == 12,
              "packet header geometry changed — update the codec and the docs");
static_assert(protocol::kInitialTtl == 7, "initial TTL follows bitchat's figure");

static_assert(sizeof(kRetryBackoffMillis) / sizeof(kRetryBackoffMillis[0])
                  == kMaxSendAttempts,
              "the retry ladder must have exactly one entry per allowed attempt");

static_assert(kShortLabelChars < kPeerIdBytes * 2,
              "the display discriminator must be shorter than the full peer id");

static_assert(kRssiVeryStrongDbm > kRssiStrongDbm,
              "RSSI band thresholds must descend — less negative is stronger");

static_assert(protocol::kFragmentPayloadBytes > protocol::kFragmentHeaderBytes,
              "a fragment must carry more payload than its own header");

}  // namespace anytime::header_check
