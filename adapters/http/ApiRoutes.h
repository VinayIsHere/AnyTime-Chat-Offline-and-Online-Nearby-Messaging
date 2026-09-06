// adapters/http/ApiRoutes.h
//
// Wires the local API surface (docs/ARCHITECTURE.md section 05) onto AppCore.
//
// This is the only file that knows both HTTP and the core services, which keeps
// the router generic and the services free of any HTTP concept.

#pragma once

namespace anytime {
class AppCore;
}

namespace anytime::http {

class Router;
class SseHub;

/// Registers every route. Paths are relative to the API root: the server has
/// already stripped both the secret prefix and "api/".
void registerApiRoutes(Router&, AppCore&, SseHub&);

}  // namespace anytime::http
