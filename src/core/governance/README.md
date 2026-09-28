# Governance implementation

Permission vocabulary includes topics:read, topics:write and events:write;
subscription rights remain separate. Legacy pub/sub write atoms retain their
existing operation mappings. Mutation audit actions are schema-validated.
The audit projection uses checksummed compressed segments, a compact id index,
legacy-file migration, and retention that preserves summary events (#1154).
See [pub/sub CRUD](../../../docs/pubsub/crud.md).
