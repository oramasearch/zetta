# Runbook — connecting Snowflake

Getting a Snowflake source into zetta with a **programmatic access token** (PAT), end to end: what
Snowflake requires before a PAT will authenticate at all, how to register the connection over MCP,
and where the governance gate stops you on purpose.

Written from doing it on 2026-07-27. Both failures below were hit for real, in this order, and
neither error message says what to actually do — which is the reason this page exists.

Related: [connector-query.md](connector-query.md) (the capability) ·
[adding-a-connector.md](adding-a-connector.md) (teaching zetta a new source) ·
[distributed-connector-ingest.md](distributed-connector-ingest.md) (the cross-node fetch).

---

## 0. What you need

| | |
|---|---|
| Account identifier | the host prefix, e.g. `ADPEVGY-OLB41490` → `https://adpevgy-olb41490.snowflakecomputing.com` |
| A PAT | Snowsight → Settings → Authentication → *Programmatic access tokens* |
| A warehouse | required for token auth; a query with none will fail even with valid credentials |
| A **network policy** | see §1 — a PAT will not authenticate without one, and this is not optional |

---

## 1. Snowflake requires a network policy ON THE USER

**This is the first wall, and the error does not name the fix.**

```
HTTP 401  code 390432  "Fail : Network policy is required."
```

A PAT will not authenticate until a network policy is attached to the **user** it authenticates as.
An account-level policy alone is not enough.

Note the chicken-and-egg: you cannot use the PAT to create the policy that enables the PAT. Run this
from a **browser worksheet**, which authenticates by password/SSO and is unaffected:

```sql
CREATE NETWORK POLICY IF NOT EXISTS steve_pat_policy
  ALLOWED_IP_LIST = ('203.0.113.7');   -- your egress IP; see below

ALTER USER STEVE SET NETWORK_POLICY = steve_pat_policy;

-- confirm it landed on the USER, not just the account
SHOW PARAMETERS LIKE 'NETWORK_POLICY' IN USER STEVE;
```

Find the egress IP the node will actually present — not the office IP, the one the machine leaves
from:

```sh
curl -s https://api.ipify.org
```

**Distinguishing the two policy failures matters**, because they look alike and have different
fixes:

| Error | Means | Fix |
|---|---|---|
| `390432` "Network policy is required" | **no** policy attached to the principal | attach one (above) |
| a message naming your IP | a policy exists and your IP is **not** in it | add the IP to `ALLOWED_IP_LIST` |

Start with `('0.0.0.0/0')` only to prove the path, and narrow it the moment it works:

```sql
ALTER NETWORK POLICY steve_pat_policy SET ALLOWED_IP_LIST = ('203.0.113.7');
```

Leaving it open means any host on the internet holding that token can authenticate. If the token
carries a privileged role, that is the whole account.

---

## 2. A PAT cannot assume a privileged role

**The second wall**, and the message actively misleads — it suggests the role is not granted, when
the role is granted and simply not available *to a token*:

```
HTTP 400  code 390186
"Role 'ACCOUNTADMIN' specified in the connect string is not granted to this user,
 or is not permitted for the credentials being used."
```

Snowflake blocks PATs from privileged roles. Find what the token actually resolves to by asking for
**no role at all** — the answer is the token's default:

```sh
curl -s -X POST "https://<account>.snowflakecomputing.com/api/v2/statements" \
  -H "Authorization: Bearer $SNOWFLAKE_PAT" \
  -H "X-Snowflake-Authorization-Token-Type: PROGRAMMATIC_ACCESS_TOKEN" \
  -H "Content-Type: application/json" -H "Accept: application/json" \
  -d '{"statement":"SELECT CURRENT_USER(), CURRENT_ROLE(), CURRENT_WAREHOUSE()","timeout":60}'
```

Then set that role in the connection. On the account this was written against, a block copied from
Snowsight said `ACCOUNTADMIN` and the token was scoped to `SYSADMIN` — copy the role from the token,
not from the sample config.

**Prefer a purpose-made role over whatever the token defaults to.** A connector needs read on the
tables it pulls, and nothing else:

```sql
CREATE ROLE IF NOT EXISTS zetta_reader;
GRANT USAGE ON WAREHOUSE AMARO_CI_WH TO ROLE zetta_reader;
GRANT USAGE ON DATABASE ADMIN_DB TO ROLE zetta_reader;
GRANT USAGE ON SCHEMA ADMIN_DB.PUBLIC TO ROLE zetta_reader;
GRANT SELECT ON ALL TABLES IN SCHEMA ADMIN_DB.PUBLIC TO ROLE zetta_reader;
GRANT ROLE zetta_reader TO USER STEVE;
```

---

## 3. Register the connection in zetta (MCP)

Save it once, by name, so the credential never travels with a query:

```sh
curl -s -X POST http://127.0.0.1:4800/mcp \
  -H "Authorization: Bearer $ZETTA_TOKEN" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{
        "name":"create_connection",
        "arguments":{
          "name":"snowflake-prod",
          "connector":{
            "type":"snowflake",
            "account":"ADPEVGY-OLB41490",
            "auth":"pat",
            "token":"'"$SNOWFLAKE_PAT"'",
            "user":"STEVE",
            "role":"SYSADMIN",
            "warehouse":"AMARO_CI_WH",
            "database":"ADMIN_DB",
            "schema":"PUBLIC"
          }}}}'
```

The connection is replicated to every node — which is what lets each node fetch its own stripe of a
distributed query straight from the source.

**The token is never returned by any read surface.** Verify that rather than trusting it:

```sh
# list_connections shows the config and NOT the credential
curl -s -X POST http://127.0.0.1:4800/mcp -H "Authorization: Bearer $ZETTA_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_connections","arguments":{}}}'
```

---

## 4. Pull rows into a dataset

```sh
curl -s -X POST http://127.0.0.1:4800/mcp \
  -H "Authorization: Bearer $ZETTA_TOKEN" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
        "name":"create_dataset_from_query",
        "arguments":{
          "connection":"snowflake-prod",
          "table_name":"accounts",
          "query":"SELECT * FROM ADMIN_DB.PUBLIC.ACCOUNTS",
          "max_rows":100000
        }}}'
```

Arguments worth knowing (the whole set is on the capability's schema): `table_name` names the new
table — **not** `name`, which is rejected as an unknown property; `database` targets a database
(default `datasets`); `partitioned` shards across the cluster (default true); `max_rows` caps the
pull (default 100,000).

Only a single `SELECT` or `WITH` runs. That is enforced, not advisory.

---

## 5. The consent gate stops you here, on purpose

```
code: consent_required
"'create_dataset_from_query' touches a cloud provider and did NOT execute;
 consent challenge <uuid> was issued and must be approved by a different,
 approval-capable principal."
```

This is the system working. Reaching an external provider with your credentials is gated, and the
approval must come from a **different principal** — the caller can never approve its own cloud
action. Approvals are per-action and never remembered.

To proceed: approve in the headed app's approval surface, or with an approver key via
`approve_consent`. `list_consent_requests` shows what is pending.

---

## 6. Verifying, in the order that isolates the failure

Each step rules out the layer below it, so a failure tells you where you are:

```sh
# 1. the account host resolves and answers at all
curl -s -o /dev/null -w '%{http_code}\n' -X POST \
  "https://<account>.snowflakecomputing.com/api/v2/statements" \
  -H "Authorization: Bearer bogus" \
  -H "X-Snowflake-Authorization-Token-Type: PROGRAMMATIC_ACCESS_TOKEN" \
  -H "Content-Type: application/json" -d '{"statement":"SELECT 1"}'
  # 401 = reachable. Anything else is DNS/network, not auth.

# 2. the token authenticates, and what role it really has (no role specified)
#    390432 -> §1   ·   a row back -> note CURRENT_ROLE()

# 3. that role can see the data
#    SHOW DATABASES  /  SELECT COUNT(*) FROM <db>.INFORMATION_SCHEMA.TABLES

# 4. zetta holds the connection
#    list_connections

# 5. the pull, which will stop at the consent gate (§5)
```

---

## Security practice

- **Rotate a token that has been pasted anywhere it persists** — a chat transcript, a ticket, a
  shell history. Revoke the old one; do not merely stop using it.
- **Scope the network policy to real egress IPs.** `0.0.0.0/0` is a proving step, not a
  configuration.
- **Give the connector a read-only role.** The default role on a PAT is usually far more than a
  connector needs.
- **Never commit a token.** The connection record holds it, and no read surface returns it; a local
  `~/.snowflake/connections.toml` (mode `600`) is for direct CLI use and belongs outside the repo.
