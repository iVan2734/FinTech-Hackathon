# FinTech — P2B/B2B Risk Console

Hakaton starter: detekcija rizicnih P2B/B2B transakcija. Sumnjive transakcije se
blokiraju i salju na **manual review**. Reviewer dobija listu grana izmedju firmi
rangiranu **Monte Carlo** principom (po prekalkulisanim verovatnocama rizika), pa
brzo vidi gde je problem najverovatniji.

## Arhitektura
- **Backend (C++17):** `backend/src/` — graf ucesnika i transakcija, `RiskEngine`
  (prekalkulisanje rizika + Monte Carlo uzorkovanje), HTTP API (cpp-httplib).
- **Frontend (HTML/JS):** `frontend/` — login, pokretanje transakcije, manual review.
- Biblioteke (`backend/include/httplib.h`, `json.hpp`) su vendored single-header.

## Build & run
```bash
cmake -S . -B build
cmake --build build
./build/fintech_server          # pokreni iz root foldera (mountuje ./frontend)
```
Otvori **http://localhost:8080**.

Demo nalozi: `user / user` (obican korisnik), `reviewer / reviewer` (manual review).

## API
| Metoda | Putanja | Opis |
|--------|---------|------|
| POST | `/api/login` | `{username, password}` → `{token, role}` |
| GET  | `/api/companies` | lista ucesnika |
| POST | `/api/transaction` | `{from, to, amount, type, flag}` → odluka + rizik |
| GET  | `/api/review?iters=N` | Monte Carlo rangirane grane |
| POST | `/api/review/decision` | `{txId, decision: "cleared"\|"fraud"}` |

## Kako radi rizik
`RiskEngine::computeRisk` racuna verovatnocu po iznosu, nepoznatom partneru, tipu
(P2B/B2B) i strukturiranim iznosima. Transakcije iznad praga (0.60) se blokiraju.
`monteCarloReview` bira grane proporcionalno riziku (roulette-wheel) i radi kratke
slucajne setnje niz lance firmi — grane koje iskoce najcesce idu na vrh review liste.
# FinTech-Hackathon
