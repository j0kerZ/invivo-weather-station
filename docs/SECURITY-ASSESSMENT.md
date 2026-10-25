# Security and safety assessment

I looked at this from two sides. First: what goes wrong for the farmer. Second: how I would attack it. From antivirus work I assume two things: every input is hostile, and the attacker can get their hands on the device.

## What I protect, and how much

- **The readings.** Farmers decide on watering and frost protection from them. A wrong number is worse than a missing one, and losing readings without anyone noticing is not OK.
- **Access.** Only the farm's own users see the readings, and nobody changes them from the dashboard.
- **Proportionate.** Weather readings are not personal data, so I use standard encryption and no customer-managed keys. The only personal data is the users' email addresses. Patient data would get more.

## Safety: when something breaks

No attacker here. Sensors fail, networks drop, the cloud has errors. The harm is a farmer acting on wrong or missing readings.

| What can go wrong | What I do |
|---|---|
| A sensor breaks or returns garbage | Reject the reading and send "no data". A gap is better than a wrong number |
| A sensor slowly drifts | Compare the two temperature sensors; compare with a reference station in the trial |
| The station is offline | It keeps 365 days and resends after reconnecting. Readings are late, not lost |
| The cloud can't store a message | Retry, then a dead-letter queue with an alarm. Never dropped silently |
| The same message arrives twice | Idempotent writes: the second one overwrites the first |
| Old readings look live | Show the age of each reading; mark a station offline |

## Security: how I would attack it

Someone doing harm on purpose. Often the result is the same as a safety failure (fake or missing readings), so security also protects safety.

| Attack | What stops it |
|---|---|
| Steal a station and dump its flash for the key (what I'd do with RE) | The key lives in a secure element and can't be read out. One certificate per station, so I revoke only that one |
| Pretend to be another station | A station can only publish to its own topic. The cloud takes the station ID from the topic, not from the message |
| Send crafted bytes to break the decoder, like a malicious file against a scanner | Strict parser: check lengths before reading, stop at the first error. Fuzz it |
| Push fake firmware | Updates are signed; the station installs only signed ones |
| Read the farm's readings | Sign-in on every call, users created by us, no passwords. Multi-tenant per farm when there's more than one farm |
| Change readings through the dashboard | The API is read-only |
| Misuse a leaked cloud function or role | Least privilege: each part can do only its one job |
| Poison a dependency or the CI | SAST and SCA in CI, an SBOM, CI actions pinned |
| Flood the API or the IoT endpoint | AWS's managed limits; rate limits and a WAF on the API before launch |

## How I would test it

Each part alone, then each connection, then everything together in the field.

| Part or connection | Test |
|---|---|
| Sensor → station | Fake sensor in unit tests; the real station in a climate chamber (cold, heat, condensation) |
| Station storage | Cut the power in the middle of a write |
| Station → cloud | A wrong certificate or a wrong topic must be refused; drop and slow the network |
| Message format | Station and cloud tested on the same example messages; fuzz the decoder |
| Cloud ingestion | Load test: many stations sending their backlog at once |
| API | No token, wrong user, write attempts: all must fail. Pen test before launch |
| Dashboard | Unit and browser tests; watch farmers use it |
| Everything | Field trial next to a reference station for a season |

On every change, CI runs the unit tests, the contract tests, infrastructure checks, SAST and SCA. Each release goes through a test environment first. The field trial and the pen test happen before launch.

## Regulation

From my research, three rules apply to this product in the EU:

| Rule | What it means here |
|---|---|
| Cyber Resilience Act | Security updates for at least 5 years, so signed updates are a must, not a nice-to-have. Exploited vulnerabilities reported within 24 h from 11 Sep 2026, so we need a contact and a process. An SBOM. Full rules from 11 Dec 2027 |
| Radio Equipment Directive (EN 18031) | The station has a cellular modem, so these cybersecurity rules already apply (since 1 Aug 2025) |
| GDPR | Only the users' email addresses. Kept in the EU (Stockholm region), nothing else collected |

Norway applies EU rules through the EEA agreement; when the CRA applies there is still to confirm.
