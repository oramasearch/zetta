# API implementation

Registry declarations generate REST routes, MCP tools and OpenAPI. Pub/sub CRUD
uses the same schemas and handlers on both transports, with coroutine long polls
and no thread per waiter. See [CRUD](../../../docs/pubsub/crud.md) and
[REST/OpenAPI](../../../docs/api/rest-openapi.md).
