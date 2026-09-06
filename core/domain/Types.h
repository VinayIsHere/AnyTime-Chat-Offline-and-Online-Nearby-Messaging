// core/domain/Types.h
//
// UMBRELLA HEADER. Everything outside core/domain includes this and nothing
// lower — it pulls in the primitives, the enums, every constants file and the
// shared structs in one place.
//
// ARCHITECTURE RULE (docs/ARCHITECTURE.md section 04): nothing under core/ may
// include Qt, SQLite, JNI or platform headers. This whole subtree is standard
// C++17 only, which is what makes the core unit-testable with no device and what
// keeps the iOS port to "write one radio class".
// Enforced by scripts/check-core-includes.sh.
//
// Layering below this file (acyclic, bottom to top):
//
//   Primitives.h         Byte, ByteSpan, Timestamp, TaggedId<>
//   Enums.h              every enumeration
//   constants/*.h        the tuning knobs, grouped by role
//   Identifiers.h        PeerId, ChatId, MessageId, LinkId, ...
//   GlobalStructures.h   cross-layer value structs
//
// Adding a constant? Put it in the constants/ file matching its role rather than
// next to the code that reads it — they are the system's tuning surface and are
// far easier to review together than scattered across twenty headers.

#pragma once

#include "core/domain/Primitives.h"
#include "core/domain/Enums.h"

#include "core/domain/constants/CryptoConstants.h"
#include "core/domain/constants/DeliveryConstants.h"
#include "core/domain/constants/DiscoveryConstants.h"
#include "core/domain/constants/NetworkConstants.h"
#include "core/domain/constants/ProtocolConstants.h"
#include "core/domain/constants/RequestConstants.h"

#include "core/domain/Identifiers.h"
#include "core/domain/GlobalStructures.h"
