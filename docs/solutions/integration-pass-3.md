# Solution integration: pass three

Refs #1122 and #1128. This pass starts from the integration branch after
passes one and two (#1152 and #1159), preserving S-1 through S-9 and S-12.

## Merge ledger

1. S-10 Fraud & Financial-Crime Decisioning (#1155): preserve four domain
   checks, request-time feature parity, fraud-specific score output, complete
   policy defaults, authorization and case payloads, protected experimental
   cells, monitor observations, and all registry and journey tests. The
   correctness gate contains 72 checks; both ordered inventories retain every
   check from earlier passes before adding S-10 after S-9.

Module branches are joined with merge commits. Per-file line counts are
audited after each merge. No formatter is run. The integration adds no new
JSON or wire format; generated schemas remain derived from the native code.

## Proof contract

The delivery proof is a fresh native static build and the full `zetta-tests`
suite. Every solution registry and journey test must pass. Only the known
#959 and #1124 multi-member families may be waived.
