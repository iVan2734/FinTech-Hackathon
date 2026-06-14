// Generator velikog, realnog dataseta + merenje efikasnosti.
//
// Build: CMake target `gen_data`.
// Run:   ./build/gen_data [persons] [companies] [transactions] [seed] [dataDir]
// Default: 60 osoba, 140 firmi, 8000 transakcija, seed 12345, ./data
//
// Pravi POZNATU "ground truth" strukturu:
//   - deo osoba je sankcionisan
//   - deo firmi je (direktno ili kroz lanac) u vlasnistvu sankcionisanih
//   - transakcije su izmesane: cisto<->cisto i ka/unutar sankcionisanog klastera
// Zatim odmah pokrene recomputeEdges + monteCarlo i ISPISE timing + rezultat.
#include "RiskEngine.h"
#include "Storage.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static constexpr double BLOCK_THRESHOLD = 0.60;

int main(int argc, char** argv) {
    int numPersons  = (argc > 1) ? std::stoi(argv[1]) : 60;
    int numCompanies= (argc > 2) ? std::stoi(argv[2]) : 140;
    int numTx       = (argc > 3) ? std::stoi(argv[3]) : 8000;
    unsigned seed   = (argc > 4) ? std::stoul(argv[4]) : 12345u;
    std::string dataDir = (argc > 5) ? argv[5] : "data";

    std::mt19937 rng(seed);
    auto U = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    auto I = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };

    Graph g;
    const char* cities[] = {"Beograd, RS","Novi Sad, RS","Wien, AT","Limassol, CY",
                            "Valletta, MT","Moscow, RU","Nicosia, CY","Zurich, CH",
                            "London, UK","Gotham, US","Dubai, AE","Riga, LV"};
    const int NC = sizeof(cities)/sizeof(cities[0]);

    // ---- Osobe (deo sankcionisan) ----
    std::vector<std::string> persons;
    int sanctionedCount = 0;
    for (int i = 0; i < numPersons; ++i) {
        Company p;
        p.id = "p" + std::to_string(i);
        p.name = "Osoba " + std::to_string(i);
        p.kind = "person";
        p.location = cities[I(0, NC - 1)];
        p.sanctioned = (U(0, 1) < 0.18);           // ~18% sankcionisano
        if (p.sanctioned) ++sanctionedCount;
        p.maliciousness = p.sanctioned ? U(0.3, 0.6) : U(0.0, 0.15);
        g.companies[p.id] = p;
        persons.push_back(p.id);
    }

    // ---- Firme (vlasnistvo: osobe i ranije firme -> DAG, dozvoljava lance) ----
    std::vector<std::string> firms;
    for (int i = 0; i < numCompanies; ++i) {
        Company c;
        c.id = "c" + std::to_string(i);
        c.name = "Firma " + std::to_string(i);
        c.kind = "business";
        c.location = cities[I(0, NC - 1)];
        c.maliciousness = U(0.0, 0.25);

        int nOwners = I(1, 3);
        double remaining = 1.0;
        for (int k = 0; k < nOwners; ++k) {
            bool last = (k == nOwners - 1);
            double stake = last ? remaining : U(0.1, remaining * 0.8);
            stake = std::min(stake, remaining);
            remaining -= stake;
            // ~35% vlasnik je ranija firma (pravi lance), inace osoba
            std::string owner = (!firms.empty() && U(0, 1) < 0.35)
                                    ? firms[I(0, (int)firms.size() - 1)]
                                    : persons[I(0, (int)persons.size() - 1)];
            c.owners.push_back(Owner{owner, stake});
            if (remaining <= 0.01) break;
        }
        g.companies[c.id] = c;
        firms.push_back(c.id);
    }

    // ---- Transakcije ----
    auto pickFirm   = [&]() { return firms[I(0, (int)firms.size() - 1)]; };
    auto pickPerson = [&]() { return persons[I(0, (int)persons.size() - 1)]; };

    long long ts = 1700000000000LL;
    for (int i = 0; i < numTx; ++i) {
        Transaction t;
        bool p2b = (U(0, 1) < 0.35);
        t.from = p2b ? pickPerson() : pickFirm();
        t.to = pickFirm();
        if (t.from == t.to) { --i; continue; }
        t.type = p2b ? "P2B" : "B2B";
        // iznosi: vecina mala, rep velikih + ponekad "strukturirani" okrugli
        double amt = (U(0, 1) < 0.15) ? std::round(U(9, 60)) * 1000.0 : U(50, 15000);
        t.amount = amt;
        t.id = "tx" + std::to_string(i + 1);
        t.ts = ts + (long long)i * 1000;
        t.risk = RiskEngine::computeRisk(g, t);
        t.status = RiskEngine::decide(t.risk);
        g.txs.push_back(std::move(t));
    }

    // ---- Snimi dataset ----
    Storage::saveCompanies(dataDir, g);
    Storage::saveTransactions(dataDir, g);

    // ---- Ground truth: koliko firmi je sankcionisano-kontrolisano ----
    int hitCompanies = 0;
    for (auto& f : firms)
        if (RiskEngine::isSanctionedControlled(g, f)) ++hitCompanies;

    int blockedTx = 0, reviewTx = 0;
    for (auto& t : g.txs) {
        if (t.status == "blocked") ++blockedTx;
        else if (t.status == "review") ++reviewTx;
    }

    // ---- Merenje efikasnosti ----
    auto t0 = Clock::now();
    RiskEngine::recomputeEdges(g);
    auto t1 = Clock::now();
    auto R = RiskEngine::monteCarlo(g, 20000, 0.015);
    auto t2 = Clock::now();

    Storage::saveEdges(dataDir, g);

    std::printf("\n================= DATASET =================\n");
    std::printf("  osobe:          %d  (sankcionisanih: %d)\n", numPersons, sanctionedCount);
    std::printf("  firme:          %d  (sankc.-kontrolisanih: %d)\n", numCompanies, hitCompanies);
    std::printf("  transakcije:    %d  (review: %d, blokiranih: %d)\n",
                numTx, reviewTx, blockedTx);
    std::printf("  jedinstvene grane (edges): %zu\n", g.edges.size());

    std::printf("\n================ EFIKASNOST ===============\n");
    std::printf("  recomputeEdges (propagacija + tezine): %8.2f ms\n", ms(t0, t1));
    std::printf("  monteCarlo 20k iteracija:              %8.2f ms\n", ms(t1, t2));
    std::printf("  ukupno:                                %8.2f ms\n", ms(t0, t2));

    std::printf("\n================ MONTE CARLO + CLT ========\n");
    std::printf("  p_hat = %.4f   CI95 = [%.4f, %.4f]   d = %.5f\n",
                R.pHat, R.ciLow, R.ciHigh, R.d);
    std::printf("  n = %ld   n_req(d=%.3f) = %ld   dovoljno = %s\n",
                R.n, R.targetD, R.nReq, R.enough ? "DA" : "NE");

    std::printf("\n  --- TOP 10 rizicnih grana (manual review) ---\n");
    for (int i = 0; i < (int)R.items.size() && i < 10; ++i) {
        auto& it = R.items[i];
        std::printf("   %2d. %-9s -> %-9s  w=%.2f  hits=%5d  tx=%d  blok=%d  hit=%s\n",
                    i + 1, it.from.c_str(), it.to.c_str(), it.weight, it.hits,
                    it.txCount, it.blockedCount, it.isHit ? "DA" : "ne");
    }

    // ---- Provera ispravnosti: sve top grane MORAJU biti hit ----
    int badTop = 0;
    for (int i = 0; i < (int)R.items.size(); ++i)
        if (!R.items[i].isHit) ++badTop;
    std::printf("\n  provera: ne-hit grana u review listi = %d  (ocekivano 0)  -> %s\n",
                badTop, badTop == 0 ? "OK" : "GRESKA");
    std::printf("===========================================\n\n");

    return 0;
}
