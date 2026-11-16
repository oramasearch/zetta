#!/usr/bin/env python3
"""Generate the communications churn demo dataset — a synthetic customer-communications
event log in the shape of a typical CRM and support warehouse table.

    python3 scripts/gen_comms_demo_dataset.py --out data/demo/comms

Writes three CSVs:

    communication_events.csv   the long event log, one row per communication
    accounts.csv               the account dimension, carrying the churn label
    account_features.csv       the flat rollup a warehouse query would return

Why this exists
---------------
A churn demo is only convincing against data that has the problems real communications
data has. This generates an invented B2B subscription business whose event log mixes
several source systems (CRM tasks and cases, email, contact-center and sales-call
transcripts, chat, an NPS survey), each with its own join quality and timestamp
semantics. Every source name, share, rate, segment and company here is invented.

What is deliberately realistic, because it is what breaks demos
---------------------------------------------------------------
* `crm_account_id` is null at a per-source rate — from ~0.1% on cases to ~50% on
  sales-call transcripts. Every account-level aggregate is therefore over a BIASED sample
  of the conversation, and the bias differs by channel. Generating a clean join would
  hide the single most important caveat in this kind of data.
* `current`-structured sources (`crm.case`, the chat messages) carry today's state under
  a creation-date timestamp, and `event_updated_datetime` is the pipeline load date, not
  the source change date. Featurizing one as-of a past cutoff reads the future. They are
  generated that way on purpose, so the leakage is demonstrable rather than accidental.
* Volume tracks account size, not risk. A large customer produces more of everything. If
  event COUNT carried the signal, a model would learn that big accounts churn and the
  demo would fall over on the first question about a small one. `seats` is emitted as an
  explicit feature so a model can control for size instead of being fooled by it.

The signal is a combination, not a column
-----------------------------------------
No single field predicts churn. Sustained support escalation, a detractor NPS, buyer
silence after prior sales engagement, and billing friction compound; each alone is weak.
The label is DRAWN from a logistic probability on the latent rather than thresholded, so
the overlap between churners and stayers is real rather than injected — some quiet
accounts leave and some visibly struggling ones stay. **A model that scores
near-perfectly on this is a bug in this file, not a good model**: the link caps the best
achievable AUC at roughly 0.85, and no single column should exceed ~0.70 on its own.

Deterministic: the same --seed yields byte-identical files.

No third-party imports (ADR-17 — this is an offline generator, never a build/CI
dependency, and it should run on any Python 3).
"""

import argparse
import csv
import datetime as dt
import json
import math
import os

# --- deterministic mixing -----------------------------------------------------------
# Counter-based (SplitMix64), so a row's values depend only on (seed, stream, index) and
# not on generation order — the file is reproducible AND a single row can be reasoned
# about in isolation.

MASK = (1 << 64) - 1


def _mix(value):
    value = (value + 0x9E3779B97F4A7C15) & MASK
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK
    return value ^ (value >> 31)


def draw(seed, stream, index):
    return _mix((_mix(seed ^ ((stream * 0xD6E8FEB86659FD93) & MASK)) + index) & MASK)


def uniform(seed, stream, index):
    return (draw(seed, stream, index) >> 11) / float(1 << 53)


# --- the modelled table -------------------------------------------------------------
# Invented shares and null rates, chosen so the sources differ the way real ones do.

SOURCES = [
    # event_source, event_system, structure, share, null_account_rate
    ("crm.task", "CRM", "completed", 0.300, 0.020),
    ("contact_center.transcript", "Contact Center", "completed", 0.210, 0.150),
    ("crm.case", "CRM", "current", 0.180, 0.001),
    ("crm.email_message", "CRM", "completed", 0.130, 0.050),
    ("sales_call.transcript", "Sales Calls", "completed", 0.060, 0.500),
    ("crm.opportunity_history", "CRM", "completed", 0.050, 0.010),
    ("crm.account_history", "CRM", "completed", 0.040, 0.0),
    ("phone.transcript", "Phone", "completed", 0.015, 0.100),
    ("chat.message", "Chat", "current", 0.009, 0.060),
    ("chat_assistant.message", "Chat", "current", 0.004, 0.400),
    ("survey.nps", "Survey", "completed", 0.002, 0.0),
]

# An invented sales organization: three segments, each split by region and size.
SEGMENTS = [
    ("Enterprise", "Enterprise - North"),
    ("Enterprise", "Enterprise - Strategic"),
    ("Enterprise", "Enterprise - South"),
    ("Mid-Market", "Mid-Market - North"),
    ("Mid-Market", "Commercial - North"),
    ("Mid-Market", "SMB - North"),
    ("Growth", "SMB - South"),
    ("Growth", "Commercial - South"),
    ("Growth", "Enterprise - South"),
]

COMPANIES = [
    "Alder Analytics", "Birchway Studios", "Cobalt Clinics",
    "Driftwood Retail", "Elmstone Labs", "Fernhill Foods",
    "Granite Media", "Hollowbrook Health", "Ivory Lane Legal",
    "Jasper Point Energy", "Kettle Creek Farms", "Larkspur Learning",
    "Maplecrest Finance", "Nettlefield Robotics", "Oakmere Hotels",
    "Pebble Bay Insurance", "Quillon Publishing", "Rowan Gate Logistics",
    "Saltmarsh Software", "Thistle Row Telecom",
]

CHURN_REASONS = [
    "Price / cost", "Product reliability", "Support experience",
    "Went to competitor", "Team downsizing", "Merger / acquisition",
]

# Short on purpose: the demo trains on engineered numeric features. The text is here so a
# person reading a timeline sees something recognisable, not so a model reads it.
CALM = [
    ("Check in", "AGENT: Following up on the rollout. CUSTOMER: All good, the team is on it."),
    ("Data sync", "AGENT: Your integration is syncing normally now. CUSTOMER: Great, thanks."),
    ("Quarterly review", "AGENT: Usage is up across your workspaces. CUSTOMER: We are happy with it."),
    ("Training", "AGENT: Walked the new admins through the app. CUSTOMER: That helped."),
]
# Keyed by WHICH trouble the exchange expresses, so the words on the page and the latent
# that produced them agree. A reader scrolling an account's timeline sees the story the
# features encode.
STRAINED = {
    "reliability": [
        ("Escalation | Report errors", "CUSTOMER: Third time the totals came out wrong. AGENT: Escalating to engineering."),
        ("Service outage", "CUSTOMER: Our dashboards have been down since Friday and nobody called back. AGENT: Sorry about the delay."),
        ("Recurring fault", "CUSTOMER: Same sync failed again after the fix. AGENT: I am raising this with the platform team."),
    ],
    "billing": [
        ("Billing dispute", "CUSTOMER: We were invoiced for seats we removed in March. AGENT: Opening a billing case."),
        ("Invoice correction", "CUSTOMER: This bill still has the old seat count on it. AGENT: I will get a credit issued."),
    ],
    "backlog": [
        ("Support experience", "CUSTOMER: We have been waiting nine days on the ticket. AGENT: Let me find the ticket."),
        ("No response", "CUSTOMER: Nobody has come back to me on the last two tickets. AGENT: I am sorry, let me pull those up."),
    ],
    "quiet": [
        ("Contract question", "CUSTOMER: What is our out clause? We are reviewing vendors this quarter. AGENT: I will get the CSM to reach out."),
        ("Declined meeting", "AGENT: Checking in on the QBR. CUSTOMER: Let us skip this quarter."),
    ],
}

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def account_id(index):
    """A stable, obviously-synthetic 18-character CRM-style id.

    Recognisably fake so nobody mistakes a demo account for a real one, with a fixed
    length and alphabet like the ids CRMs issue.
    """
    out = "ACCDEMO"
    value = (0x5DEECE66D + index * 0x9E3779B9) & MASK
    for position in range(11):
        out += ALPHABET[value % 36]
        value = _mix((value // 36) + position)
    return out


def stamp(day, within):
    hour = 8 + within % 11
    minute = (within // 11) % 60
    second = (within // 660) % 60
    return "%s %02d:%02d:%02d.000" % (day.isoformat(), hour, minute, second)


def generate(accounts, history_days, as_of, horizon_days, seed, clean_join):
    events, dimension, features = [], [], []
    start = as_of - dt.timedelta(days=history_days)
    ordinal = 0
    unjoinable = 0
    current_rows = 0

    for index in range(accounts):
        aid = account_id(index)
        name = COMPANIES[index % len(COMPANIES)]
        name += " LLC" if index < len(COMPANIES) else " %d" % (index // len(COMPANIES) + 1)
        segment, sub_segment = SEGMENTS[index % len(SEGMENTS)]

        # Size drives VOLUME, not risk. Emitted as a feature so the model can control for
        # it rather than confuse it with distress.
        seats = 5 + int(uniform(seed, 1, index) ** 2.4 * 1800)
        tenure_days = 90 + int(uniform(seed, 2, index) * 1800)

        # FOUR independent latent troubles, each continuous. Each one is read by a
        # DIFFERENT channel below, and the label depends on all four — which is what makes
        # the combination necessary rather than decorative. A single global "distress"
        # multiplier would make every feature a noisy copy of the same number, and the
        # loudest copy (escalation count) would give the label away on its own.
        reliability = uniform(seed, 3, index)   # product failing      -> support escalations
        billing = uniform(seed, 4, index)       # invoice friction     -> billing disputes
        quiet = uniform(seed, 5, index)         # buyer disengagement  -> sales silence
        backlog = uniform(seed, 6, index)       # unresolved queue     -> chat + NPS

        distress = (0.30 * reliability + 0.24 * billing + 0.26 * quiet + 0.28 * backlog)
        if sub_segment.startswith("SMB"):
            distress += 0.06          # smaller commercial accounts churn more, as in life
        if segment == "Enterprise":
            distress -= 0.05
        distress += (uniform(seed, 7, index) - 0.5) * 0.34   # unexplained variance

        # The label is DRAWN from a probability, not thresholded and then flipped. A
        # logistic link on the latent gives real overlap — some quiet accounts leave and
        # some visibly struggling ones stay — without injecting artificial noise on top,
        # and it caps how well any model can possibly do (~0.85 AUC) at a level that is
        # honest for churn.
        churn_probability = 1.0 / (1.0 + math.exp(-9.0 * (distress - 0.78)))
        churned = uniform(seed, 8, index) < churn_probability
        churn_day = as_of + dt.timedelta(
            days=1 + int(uniform(seed, 9, index) * (horizon_days - 1)))

        # Volume per account, from account size. Accounts in a table like this average
        # hundreds of communications over eighteen months. Generating a dozen would make every 90-day count mostly Poisson
        # noise, and no combination of features could recover the latent — the dataset
        # would look honest and be useless.
        base_events = 40 + int(math.log2(seats + 1) * 28)
        support_90d = escal_90d = escal_prior = billing_180d = unresolved_90d = 0
        sales_90d = sales_prior = chat_fail_90d = 0
        last_sales_day = start
        channels_90d = set()

        for event in range(base_events):
            stream = 100 + event
            # Recency-weighted: more conversation lately than eighteen months ago.
            when = uniform(seed, stream, index) ** 0.62
            day = start + dt.timedelta(days=int(when * history_days))

            pick, cumulative, chosen = uniform(seed, stream + 5000, index), 0.0, SOURCES[-1]
            for candidate in SOURCES:
                cumulative += candidate[3]
                if pick <= cumulative:
                    chosen = candidate
                    break
            source, system, structure, _share, null_rate = chosen

            recent = as_of - dt.timedelta(days=90) < day <= as_of
            prior = as_of - dt.timedelta(days=180) < day <= as_of - dt.timedelta(days=90)
            # Trouble shows up more strongly as the decision date approaches.
            weight = 1.5 if recent else (1.1 if prior else 0.75)

            is_support = system in ("Contact Center", "Phone") or source == "crm.case"
            is_sales = source in ("sales_call.transcript", "crm.opportunity_history")
            is_chat = system == "Chat"

            # WHICH trouble this exchange expresses, drawn independently per latent, and
            # the subject follows from it. Choosing the subject at random would leave
            # "Billing dispute" uncorrelated with the billing latent — the column would
            # look like a signal and read nothing.
            #
            # The rates are the honesty knob: raise one and its column starts predicting
            # the label by itself, which is not what churn looks like in real data.
            # A weighted categorical draw, not a cascade. Testing the latents in order
            # with elif starves whichever is last — the third latent only gets the events
            # the first two declined — so its column reads noise while its weight in the
            # label stays high, and no model can find it.
            if is_chat:
                candidates = [("backlog", backlog * 0.42)]
            elif is_sales:
                candidates = [("quiet", quiet * 0.24)]
            else:
                candidates = [("reliability", reliability * 0.34),
                              ("billing", billing * 0.30),
                              ("backlog", backlog * 0.34)]
            total = sum(rate for _name, rate in candidates)
            trouble = None
            if uniform(seed, stream + 9000, index) < total * weight:
                choice = uniform(seed, stream + 9100, index) * total
                running = 0.0
                for name, rate in candidates:
                    running += rate
                    if choice <= running:
                        trouble = name
                        break
            strained = trouble is not None

            # The join gap, at the per-source rate. These events are real
            # conversation that simply cannot be attributed to an account.
            joinable = clean_join or uniform(seed, stream + 13000, index) >= null_rate

            if trouble is not None:
                subject, text = STRAINED[trouble][
                    draw(seed, stream + 17000, index) % len(STRAINED[trouble])]
            else:
                subject, text = CALM[draw(seed, stream + 17000, index) % len(CALM)]
            within = draw(seed, stream + 21000, index)

            events.append({
                "event_id": "evt_%d" % ordinal,
                "event_system": system,
                "event_source": source,
                "event_structure_type": structure,
                "event_type": "support" if is_support else ("sales" if is_sales else "service"),
                "event_start_datetime": stamp(day, within),
                # For a `current` row this is the PIPELINE's load date, not the source's
                # change date — the trap described at the top of this file. Keeping the two
                # visibly different is what lets a demo show why the cutoff must be chosen.
                "event_updated_datetime": stamp(as_of, 3600) if structure == "current"
                                             else stamp(day, within),
                "crm_account_id": aid if joinable else "",
                "related_case_id": "CASEDEMO%05d" % (ordinal % 100000) if is_support else "",
                "communications_subject": subject,
                "communications_text": text,
            })
            ordinal += 1
            if structure == "current":
                current_rows += 1
            if not joinable:
                # An unattributable event contributes to no account's rollup. That is the
                # point: the features below are computed over what can actually be joined,
                # like any honest query against this table.
                unjoinable += 1
                continue

            if recent:
                channels_90d.add(system)
                support_90d += is_support
                escal_90d += trouble == "reliability"
                unresolved_90d += trouble == "backlog"
                sales_90d += is_sales
                chat_fail_90d += is_chat and trouble is not None
            if prior:
                escal_prior += trouble == "reliability"
                sales_prior += is_sales
            if is_sales and day <= as_of:
                # A disengaging buyer stops taking sales calls. Suppress the LAST touch
                # rather than flag it, so `days_since_last_sales_touch` reads real silence
                # instead of being another copy of the strained flag.
                if not (recent and uniform(seed, stream + 25000, index) < quiet * 0.80):
                    last_sales_day = max(last_sales_day, day)
            if trouble == "billing" and as_of - dt.timedelta(days=180) < day <= as_of:
                billing_180d += 1

        # NPS: most accounts never respond. Distress shifts the distribution down; it does
        # not determine the score.
        nps = -1
        unhappy = backlog * 0.60 + reliability * 0.45
        # Unhappy customers answer surveys more often than contented ones do.
        if uniform(seed, 30, index) < 0.34 + unhappy * 0.34:
            shifted = min(max(uniform(seed, 31, index) - unhappy * 0.80, 0.0), 0.999)
            nps = int(shifted * 11)

        dimension.append({
            "crm_account_id": aid,
            "crm_account_name": name,
            "segment": segment,
            "sub_segment": sub_segment,
            "seats": seats,
            "tenure_days": tenure_days,
            "churned": 1 if churned else 0,
            "churn_date": churn_day.isoformat() if churned else "",
            "last_churn_reason": CHURN_REASONS[draw(seed, 32, index) % len(CHURN_REASONS)]
                                 if churned else "",
        })

        # +1 on both sides so an account with no prior activity does not produce an
        # infinite trend, and a drop to zero still reads as a drop.
        def ratio(now, before):
            return round((now + 1.0) / (before + 1.0), 4)

        features.append({
            "account_index": index,          # the integer entity key the model ranks by
            "crm_account_id": aid,
            "segment": segment,
            "sub_segment": sub_segment,
            "seats": seats,
            "tenure_days": tenure_days,
            "support_events_90d": support_90d,
            "escalations_90d": escal_90d,
            "unresolved_contacts_90d": unresolved_90d,
            "escalation_trend": ratio(escal_90d, escal_prior),
            "billing_disputes_180d": billing_180d,
            "nps_score": nps,
            "nps_is_detractor": 1 if 0 <= nps <= 6 else 0,
            "days_since_last_sales_touch": (as_of - last_sales_day).days,
            "sales_touch_trend": ratio(sales_90d, sales_prior),
            "chat_deflection_failures_90d": chat_fail_90d,
            "distinct_channels_90d": len(channels_90d),
            "churned": 1 if churned else 0,
        })

    facts = {
        "accounts": len(dimension),
        "events": len(events),
        "churned": sum(row["churned"] for row in dimension),
        "events_without_account": unjoinable,
        "current_structured_events": current_rows,
    }
    return events, dimension, features, facts


def write_csv(path, rows):
    if not rows:
        return
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="data/demo/comms")
    parser.add_argument("--accounts", type=int, default=2000)
    parser.add_argument("--history-days", type=int, default=540)
    parser.add_argument("--as-of", default="2026-06-30",
                        help="the decision date; the label horizon starts the day after")
    parser.add_argument("--horizon-days", type=int, default=90)
    parser.add_argument("--seed", type=int, default=20260803)
    parser.add_argument("--clean-join", action="store_true",
                        help="drop the per-source null-account rates. Makes a tidier "
                             "walkthrough and hides the caveat that matters most — so "
                             "this should be a deliberate choice, not a default.")
    args = parser.parse_args()

    as_of = dt.date.fromisoformat(args.as_of)
    events, dimension, features, facts = generate(
        args.accounts, args.history_days, as_of, args.horizon_days, args.seed,
        args.clean_join)

    os.makedirs(args.out, exist_ok=True)
    write_csv(os.path.join(args.out, "communication_events.csv"), events)
    write_csv(os.path.join(args.out, "accounts.csv"), dimension)
    write_csv(os.path.join(args.out, "account_features.csv"), features)

    manifest = {
        "generator": "scripts/gen_comms_demo_dataset.py",
        "modelled_on": "an invented CRM and support communications event table",
        "seed": args.seed,
        "as_of_date": args.as_of,
        "horizon_days": args.horizon_days,
        "facts": facts,
        "churn_rate": round(facts["churned"] / max(facts["accounts"], 1), 4),
        "unjoinable_rate": round(facts["events_without_account"] / max(facts["events"], 1), 4),
        "label_link": "logistic on the latent: p = sigmoid(9 * (distress - 0.78)); the "
                      "label is drawn from p, so the overlap is real rather than injected",
        "note": "Synthetic and invented: sources, rates, segments, companies and text. "
                "No real customer data.",
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")

    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
