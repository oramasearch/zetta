#!/usr/bin/env python3
"""Generate a 500-account UC-1 churn dataset DIRECTORY (dataset.json + accounts.jsonl
+ events.jsonl) for the churn demo. Learnable-but-noisy signal so
the trained model has a CREDIBLE AUC (not the 1.0 of the 10-row fixture):
payment_failed is the dominant driver (matching the engine featurizer), with Poisson
count noise + logistic label noise creating class overlap. Includes the Contoso TC-4
entity-resolution siblings. Stdlib only, seeded, reproducible."""
import json, os, math, random, statistics
from collections import defaultdict
from datetime import date, timedelta

random.seed(42)
AS_OF = date(2026, 3, 1)
HORIZON = 90
N_GENERAL = 494  # + 6 Contoso siblings = 500

PLAN_TIERS = ["starter", "business", "enterprise"]
INDUSTRIES = ["logistics", "fleet", "transportation", "manufacturing", "retail", "construction", "field-services"]
REGIONS = ["us", "ca", "us-strategic"]

accounts, events = [], []
evt_seq = 0

def pois(lam):  # Knuth Poisson
    L, k, p = math.exp(-lam), 0, 1.0
    while True:
        k += 1; p *= random.random()
        if p <= L:
            return k - 1

def ts_before():
    d = AS_OF - timedelta(days=random.randint(1, 180))
    return f"{d.isoformat()}T{random.randint(0,23):02d}:{random.randint(0,59):02d}:00Z"

def add_event(account_id, etype, channel, value=0, mention=""):
    global evt_seq
    evt_seq += 1
    events.append({"event_id": f"evt_{evt_seq:06d}", "account_id": account_id,
                   "event_ts": ts_before(), "event_type": etype, "channel": channel,
                   "mention": mention, "value": value})

def emit_events(account_id, latent, mention_name=""):
    for _ in range(pois(0.2 + 4.4 * latent * latent)):   # payment_failed (dominant; latent^2 sharpens the tail)
        add_event(account_id, "payment_failed", "billing", round(random.uniform(200, 5000), 2))
    for _ in range(pois(0.3 + 3.0 * latent)):         # support_ticket
        add_event(account_id, "support_ticket", "web", random.randint(1, 5))
    for _ in range(pois(6.0 * (1.0 - 0.5 * latent) + 1)):
        add_event(account_id, "login", "app")
    for _ in range(pois(4.0 * (1.0 - 0.4 * latent) + 1)):
        add_event(account_id, "feature_use", "app")
    for _ in range(pois(1.5)):
        add_event(account_id, "csm_contact", "phone")
    for _ in range(pois(2.0)):
        add_event(account_id, "comms", random.choice(["email", "crm"]), mention=mention_name)

def make_account(account_id, name, brand, entity, latent, forced_churn=None, mention_name=""):
    plan = random.choices(PLAN_TIERS, weights=[3, 4, 3])[0]
    base = {"starter": 800, "business": 4000, "enterprise": 15000}[plan]
    mrr = round(base * random.uniform(0.6, 1.6) * (1.0 - 0.15 * latent), 2)
    seats = max(1, int({"starter": 15, "business": 90, "enterprise": 350}[plan] * random.uniform(0.5, 1.5)))
    tenure = max(30, int(random.uniform(90, 2200) * (1.0 - 0.2 * latent)))
    prob = 1.0 / (1.0 + math.exp(-(-3.0 + 3.7 * latent + random.gauss(0, 0.5))))  # ~18% base rate, label noise
    churned = (random.random() < prob) if forced_churn is None else forced_churn
    churn_date = (AS_OF + timedelta(days=random.randint(1, HORIZON))).isoformat() if churned else ""
    renewal = (AS_OF + timedelta(days=random.randint(20, 240))).isoformat()
    accounts.append({"account_id": account_id, "name": name, "brand": brand, "entity": entity,
                     "plan_tier": plan, "industry": random.choice(INDUSTRIES), "region": random.choice(REGIONS),
                     "mrr": mrr, "seats": seats, "tenure_days": tenure, "renewal_date": renewal,
                     "churned": churned, "churn_date": churn_date})
    emit_events(account_id, latent, mention_name)

# Contoso TC-4 siblings — share brand, distinct entities, one churned; comms carry the mention
for ent, nm, ch in [("contoso_freight", "Contoso Freight", False), ("contoso_ground", "Contoso Ground", False),
                    ("contoso_express", "Contoso Express", False), ("contoso_office", "Contoso Office", True),
                    ("contoso_logistics", "Contoso Logistics", False), ("contoso_services", "Contoso Services", False)]:
    make_account(f"acct_{ent}", nm, "Contoso", ent, 0.75 if ch else random.uniform(0.1, 0.4), forced_churn=ch, mention_name=nm)

for i in range(N_GENERAL):
    make_account(f"acct_{i:04d}", f"Account {i:04d}", "", f"entity_{i:04d}", random.random())

manifest = {"name": "churn-demo-500",
            "description": "500-account synthetic UC-1 churn dataset for the churn demo. Learnable-but-noisy signal (payment_failed dominant) plus the Contoso TC-4 entity-resolution siblings. No real customer data.",
            "entity_field": "account_id", "time_field": "event_ts", "label_field": "churned",
            "as_of_date": AS_OF.isoformat(), "churn_horizon_days": HORIZON,
            "label_definition": "churned == true iff the account canceled / did not renew within churn_horizon_days after as_of_date."}

OUT = os.environ.get("OUT", os.path.expanduser("~/.zetta-local/datasets/churn-demo-500"))
os.makedirs(OUT, exist_ok=True)
json.dump(manifest, open(f"{OUT}/dataset.json", "w"), indent=2)
with open(f"{OUT}/accounts.jsonl", "w") as f:
    for a in accounts:
        f.write(json.dumps(a) + "\n")
with open(f"{OUT}/events.jsonl", "w") as f:
    for e in events:
        f.write(json.dumps(e) + "\n")

n_churn = sum(a["churned"] for a in accounts)
pf = defaultdict(int)
for e in events:
    if e["event_type"] == "payment_failed":
        pf[e["account_id"]] += 1
by = defaultdict(list)
for a in accounts:
    by[a["churned"]].append(pf.get(a["account_id"], 0))
print(f"OUT={OUT}")
print(f"accounts={len(accounts)} churned={n_churn} ({100*n_churn/len(accounts):.1f}%) events={len(events)}")
for k in (False, True):
    print(f"  churned={k}: mean payment_failed={statistics.mean(by[k]):.2f}  n={len(by[k])}")
