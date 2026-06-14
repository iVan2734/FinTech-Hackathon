// Test primeri za logiku rizika. Build: ukljucen u CMake kao `risk_tests`.
// Pokretanje: ./build/risk_tests   (exit code != 0 ako neki test padne)
#include "RiskEngine.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static int g_pass = 0;

static void check(bool cond, const char* desc) {
    if (cond) { ++g_pass; std::printf("  \033[32mPASS\033[0m  %s\n", desc); }
    else      { ++g_fail; std::printf("  \033[31mFAIL\033[0m  %s\n", desc); }
}

// helperi za pravljenje malih grafova
static Company person(const std::string& id, bool sanc, double mal = 0.0) {
    Company c; c.id = id; c.name = id; c.kind = "person"; c.sanctioned = sanc; c.maliciousness = mal; return c;
}
static Company firm(const std::string& id, double mal, std::vector<Owner> owners) {
    Company c; c.id = id; c.name = id; c.kind = "business"; c.maliciousness = mal; c.owners = std::move(owners); return c;
}
static Transaction tx(const std::string& f, const std::string& t, double amt, const std::string& status) {
    Transaction x; x.id = f + "_" + t; x.from = f; x.to = t; x.amount = amt; x.type = "B2B"; x.status = status; return x;
}
static void add(Graph& g, Company c) { g.companies[c.id] = c; }

// =========================================================================
int main() {
    std::printf("\n=== T1: sankcionisani vlasnik > 20%% (direktno) ===\n");
    {
        Graph g;
        add(g, person("S", true));            // sankcionisan
        add(g, person("A", false));
        add(g, firm("B", 0.0, {{"S", 0.8}, {"A", 0.2}}));
        double sf = RiskEngine::sanctFrac(g, "B");
        std::printf("  sanctFrac(B) = %.3f\n", sf);
        check(std::abs(sf - 0.8) < 1e-9, "sanctFrac(B) == 0.80");
        check(RiskEngine::isSanctionedControlled(g, "B"), "B je hit (>20%)");
    }

    std::printf("\n=== T2: sankcionisani vlasnik ISPOD praga (10%%) ===\n");
    {
        Graph g;
        add(g, person("S", true));
        add(g, person("A", false));
        add(g, firm("B", 0.0, {{"S", 0.10}, {"A", 0.90}}));
        double sf = RiskEngine::sanctFrac(g, "B");
        std::printf("  sanctFrac(B) = %.3f\n", sf);
        check(std::abs(sf - 0.10) < 1e-9, "sanctFrac(B) == 0.10");
        check(!RiskEngine::isSanctionedControlled(g, "B"), "B NIJE hit (<20%)");
    }

    std::printf("\n=== T3: indirektno vlasnistvo kroz lanac (efektivni udeo) ===\n");
    {
        Graph g;
        add(g, person("S", true));
        add(g, person("A", false));
        add(g, firm("C3", 0.0, {{"S", 0.8}, {"A", 0.2}}));      // S drzi 80% C3
        add(g, firm("C4", 0.0, {{"C3", 0.5}, {"A", 0.5}}));     // C3 drzi 50% C4 -> S efektivno 40%
        add(g, firm("C5", 0.0, {{"C4", 0.3}, {"A", 0.7}}));     // C4 drzi 30% C5 -> S efektivno 12%
        double f4 = RiskEngine::sanctFrac(g, "C4");
        double f5 = RiskEngine::sanctFrac(g, "C5");
        std::printf("  sanctFrac(C4) = %.3f (ocek. 0.40), sanctFrac(C5) = %.3f (ocek. 0.12)\n", f4, f5);
        check(std::abs(f4 - 0.40) < 1e-9, "C4 efektivno 40%");
        check(RiskEngine::isSanctionedControlled(g, "C4"), "C4 je hit (40% > 20%)");
        check(std::abs(f5 - 0.12) < 1e-9, "C5 efektivno 12%");
        check(!RiskEngine::isSanctionedControlled(g, "C5"), "C5 NIJE hit (12% < 20%)");
    }

    std::printf("\n=== T4: istorija (blokirane tx) dize tezinu grane ===\n");
    {
        Graph g;
        add(g, person("A", false, 0.1));
        add(g, firm("X", 0.1, {{"A", 1.0}}));
        add(g, firm("Y", 0.1, {{"A", 1.0}}));
        add(g, firm("Z", 0.1, {{"A", 1.0}}));
        // grana A->X: 3 cisto;  grana A->Y: 3 blokirano
        g.txs = { tx("A","X",100,"approved"), tx("A","X",100,"approved"), tx("A","X",100,"approved"),
                  tx("A","Y",100,"blocked"),  tx("A","Y",100,"blocked"),  tx("A","Y",100,"blocked") };
        RiskEngine::recomputeEdges(g);
        double wX = g.currentEdgeWeight("A","X");
        double wY = g.currentEdgeWeight("A","Y");
        std::printf("  w(A->X cisto) = %.3f   w(A->Y blokirano) = %.3f\n", wX, wY);
        check(wY > wX, "blokirana grana ima vecu tezinu od ciste");
    }

    std::printf("\n=== T5: propagacija rizika kroz lanac transakcija ===\n");
    {
        Graph g;
        add(g, person("S", true));
        add(g, firm("R", 0.0, {{"S", 1.0}}));   // R je sankcionisano-kontrolisan (base=1)
        add(g, firm("M", 0.0, {{"S", 0.0}}));   // M cist, ali prima od R
        add(g, firm("ISO", 0.0, {}));           // izolovan cist
        g.txs = { tx("R","M",1000,"approved") };
        auto risk = RiskEngine::propagate(g);
        std::printf("  risk(R)=%.3f  risk(M)=%.3f  risk(ISO)=%.3f\n", risk["R"], risk["M"], risk["ISO"]);
        check(risk["M"] > risk["ISO"], "M (prima od rizicnog R) rizicniji od izolovanog");
        check(risk["R"] > 0.9, "R ostaje visoko rizican (sankcionisan vlasnik)");
    }

    std::printf("\n=== T6: Monte Carlo + CLT (p_true = 0.5, najgori slucaj) ===\n");
    {
        Graph g;
        add(g, person("S", true));
        add(g, person("A", false));
        add(g, firm("H", 0.0, {{"S", 1.0}}));   // hit firma
        add(g, firm("C", 0.0, {{"A", 1.0}}));   // cista firma
        // dve grane jednake tezine: jedna vodi u H (hit), druga u C (ne)
        // tezine namestamo preko istorije tako da budu ~jednake
        g.txs = { tx("A","H",100,"approved"), tx("A","C",100,"approved") };
        RiskEngine::recomputeEdges(g);
        // izjednaci tezine rucno radi cistog analitickog p_true = 0.5
        g.edges[Graph::edgeKey("A","H")].weight = 0.5;
        g.edges[Graph::edgeKey("A","C")].weight = 0.5;

        int n = 20000;
        auto R = RiskEngine::monteCarlo(g, n, 0.015, 50, /*seed=*/42);
        double dExpected = 1.96 * std::sqrt(0.25 / n);
        long nReqExpected = (long)std::ceil(1.96 * 1.96 * 0.25 / (0.015 * 0.015));
        std::printf("  p_hat = %.4f  (p_true = 0.5)\n", R.pHat);
        std::printf("  CI95  = [%.4f, %.4f]   d = %.5f\n", R.ciLow, R.ciHigh, R.d);
        std::printf("  d(n=20000) ocekivano = %.5f   |   n_req(d=0.015) = %ld (ocek. %ld)\n",
                    dExpected, R.nReq, nReqExpected);
        check(std::abs(R.d - dExpected) < 1e-9, "CLT d = 1.96*sqrt(0.25/n)");
        check(R.nReq == nReqExpected, "n_req = z^2*0.25/d^2 (~4269)");
        check(R.enough, "n=20000 >= n_req (dovoljno iteracija za d=0.015)");
        check(std::abs(R.pHat - 0.5) < R.d, "p_hat unutar CLT granice d od p_true=0.5");
        check(R.d < 0.015, "stvarni d kod 20k < ciljanih 0.015");
    }

    std::printf("\n=== REZIME: %d PASS, %d FAIL ===\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
