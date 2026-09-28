# Landing S-1 through S-4 and S-12

Refs #1122 and #1128. The integration branch combines S-1 (#1135), S-2
(#1141), S-3 (#1140), S-4 (#1139), and S-12 (#1142), followed by `dev`.
Module branches retain merge commits and their original tests. This landing
does not complete the catalogue epic: S-5 through S-11 and the live lake
acceptance remain outside this work order.

## Shared code resolutions

- The correctness gate keeps all 51 checks. After S-1, module checks register
  in S-2, S-3, S-4, S-12 order. Both ordered-name tests assert the full list.
- M-1 keeps lead conversion, continuous-value regression, collections recovery,
  and claim referral shapes. The shared executor honors each shape's result
  database, table prefix, entity column, and score column; squared-error
  models retain the lifetime-value defaults.
- Foundation binding retains every module's correctness profile, decision
  source, policy, holdout, action payload, and monitor. S-1, S-3, and S-4 keep
  M-6 treatment output; S-12 also carries M-11 model metrics into monitoring.
- All module journeys and solution tests remain present. Documentation keeps
  each module's data, policy, refusal, and measurement contract.
- The `dev` merge retains pub/sub CRUD, incremental solution reconciliation,
  distributed entity resolution, API changes, and their tests. Generated
  schemas are rebuilt from the combined capability registry.

No new JSON or wire format is introduced by the integration itself. Caller
snapshot cutoffs, delivery credentials, and ROI economics remain required.

## Verification

Native static and shared-dependency suites are run separately with at most
six build jobs. The complete `zetta-tests` binary supplies the proof counts;
the only pre-authorized timing waivers are #959 and #1124. OpenAPI and boundary
schemas are regenerated with `ZETTA_WRITE_SCHEMAS=1`, then drift-checked.

Linux proof deferred: the work order's external Linux proof helper is absent.
Final native results are recorded here after the proof run.
