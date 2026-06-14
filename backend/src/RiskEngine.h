#pragma once
#include "Graph.h"
#include <random>
#include <cmath>
#include <algorithm>
#include <unordered_set>

// Logika rizika (vidi README / razgovor):
//  - sanctFrac:   efektivni sankcionisani vlasnicki udeo (pun obilazak
//                 vlasnickog grafa, indirektno vlasnistvo kroz lanac)
//  - baseRisk:    bazni rizik cvora (maliciousness + sanctFrac)
//  - propagate:   rizik se siri kroz SVE transakcije (pun obilazak grafa)
//  - recomputeEdges: tezine grana iz propagiranog rizika + istorije
//  - computeRisk: rizik konkretne nove transakcije
//  - monteCarlo:  procena p(hit) + Centralna granicna teorema (CLT)
//
// NAPOMENA: sve funkcije ocekuju da pozivalac drzi Graph::mtx.
class RiskEngine {
public:
    static constexpr double OWNERSHIP_HIT = 0.20;  // prag vlasnistva za hit
    static constexpr double Z95 = 1.96;            // z za 95% pouzdanost

    // ---- Efektivni sankcionisani udeo (rekurzivno kroz lanac vlasnistva) ----
    static double sanctFrac(const Graph& g, const std::string& id,
                            std::unordered_map<std::string, double>& memo,
                            std::unordered_set<std::string>& visiting) {
        auto m = memo.find(id);
        if (m != memo.end()) return m->second;
        auto it = g.companies.find(id);
        if (it == g.companies.end()) return 0.0;
        if (it->second.sanctioned) { memo[id] = 1.0; return 1.0; }
        if (visiting.count(id)) return 0.0;  // zastita od ciklusa u vlasnistvu
        visiting.insert(id);
        double s = 0.0;
        for (const auto& o : it->second.owners)
            s += o.stake * sanctFrac(g, o.ownerId, memo, visiting);
        visiting.erase(id);
        s = std::clamp(s, 0.0, 1.0);
        memo[id] = s;
        return s;
    }

    static double sanctFrac(const Graph& g, const std::string& id) {
        std::unordered_map<std::string, double> memo;
        std::unordered_set<std::string> visiting;
        return sanctFrac(g, id, memo, visiting);
    }

    // Cvor je "sankcionisano-kontrolisan" (hit) ako je sam sankcionisan ili
    // ga sankcionisani drze efektivno preko praga.
    static bool isSanctionedControlled(const Graph& g, const std::string& id) {
        if (g.isSanctioned(id)) return true;
        return sanctFrac(g, id) > OWNERSHIP_HIT;
    }

    // ---- Bazni rizik cvora ----
    static double baseRisk(const Graph& g, const std::string& id) {
        auto it = g.companies.find(id);
        if (it == g.companies.end()) return 0.30;
        if (it->second.sanctioned) return 1.0;
        return std::clamp(it->second.maliciousness + sanctFrac(g, id), 0.0, 1.0);
    }

    static double pairBaseRisk(const Graph& g, const std::string& a, const std::string& b) {
        return std::clamp(0.4 * baseRisk(g, a) + 0.6 * baseRisk(g, b), 0.0, 1.0);
    }

    // ---- Pun obilazak grafa: rizik se siri kroz sve transakcije ----
    static std::unordered_map<std::string, double>
    propagate(const Graph& g, int sweeps = 5, double alpha = 0.6) {
        std::unordered_map<std::string, double> base, risk;
        for (const auto& kv : g.companies) {
            double b = baseRisk(g, kv.first);
            base[kv.first] = b;
            risk[kv.first] = b;
        }
        for (int s = 0; s < sweeps; ++s) {
            std::unordered_map<std::string, double> num, den;
            for (const auto& t : g.txs) {
                double w = flagged(t.status) ? 1.0 : 0.25;
                num[t.to] += w * risk[t.from];
                den[t.to] += w;
            }
            std::unordered_map<std::string, double> next;
            for (const auto& kv : g.companies) {
                const std::string& v = kv.first;
                auto d = den.find(v);
                if (d != den.end() && d->second > 0.0) {
                    double inflow = num[v] / d->second;
                    next[v] = std::clamp(alpha * base[v] + (1.0 - alpha) * inflow, 0.0, 1.0);
                } else {
                    next[v] = base[v];
                }
            }
            risk = std::move(next);
        }
        return risk;
    }

    static constexpr double REVIEW_THRESHOLD = 0.60;  // >= ovoga -> manual review
    static constexpr double BLOCK_THRESHOLD = 0.85;   // >= ovoga (ili sankc.) -> tvrda blokada

    // Odluka: approved < review < blocked
    static std::string decide(double risk) {
        if (risk >= BLOCK_THRESHOLD) return "blocked";
        if (risk >= REVIEW_THRESHOLD) return "review";
        return "approved";
    }
    // Da li status znaci "oznacen/rizican" (za istoriju grana i propagaciju)
    static bool flagged(const std::string& s) {
        return s == "blocked" || s == "review" || s == "fraud";
    }

    static std::string pctStr(double x) {
        return std::to_string(static_cast<int>(std::lround(x * 100))) + "%";
    }

    struct Factor { std::string label; double value; };
    struct RiskBreakdown {
        double risk = 0.0;
        std::string status = "approved";  // approved | review | blocked
        bool sanctionedParty = false;
        std::vector<Factor> factors;     // numericki doprinosi
        std::vector<std::string> notes;  // kvalitativni razlozi (zasto)
    };

    // Komponente rizika (deljeno izmedju computeRisk i explain).
    // NAPOMENA: tip transakcije (P2B/B2B) NE utice na rizik.
    static void riskComponents(const Graph& g, const Transaction& t, bool& sanctioned,
                               double& base, double& amountF, double& structuring) {
        sanctioned = g.isSanctioned(t.from) || g.isSanctioned(t.to);
        double w = g.currentEdgeWeight(t.from, t.to);
        base = (w < 0.0) ? pairBaseRisk(g, t.from, t.to) : w;
        amountF = 0.40 / (1.0 + std::exp(-(t.amount - 10000.0) / 5000.0));
        structuring = (std::fmod(t.amount, 1000.0) == 0.0 && t.amount >= 9000.0) ? 0.10 : 0.0;
    }

    // ---- Rizik konkretne nove transakcije ----
    static double computeRisk(const Graph& g, const Transaction& t) {
        bool s; double base, a, st;
        riskComponents(g, t, s, base, a, st);
        if (s) return 1.0;
        return std::clamp(0.6 * base + a + st, 0.0, 1.0);
    }

    // ---- Objasnjenje: rizik + doprinosi + razlozi (za "zasto") ----
    static RiskBreakdown explain(const Graph& g, const Transaction& t) {
        bool s; double base, a, st;
        riskComponents(g, t, s, base, a, st);
        RiskBreakdown rb;
        if (s) {
            rb.risk = 1.0; rb.status = "blocked"; rb.sanctionedParty = true;
            rb.factors.push_back({"Sankcionisani ucesnik", 1.0});
            std::string who = g.isSanctioned(t.from) ? g.nameOf(t.from) : g.nameOf(t.to);
            rb.notes.push_back("Ucesnik \"" + who + "\" je na sankcionoj listi -> automatska blokada.");
            return rb;
        }
        rb.risk = std::clamp(0.6 * base + a + st, 0.0, 1.0);
        rb.status = decide(rb.risk);
        rb.factors.push_back({"Rizik relacije (vlasnistvo + istorija)", 0.6 * base});
        if (a > 0.005) rb.factors.push_back({"Visina iznosa", a});
        if (st > 0)    rb.factors.push_back({"Okrugao iznos tik iznad praga", st});

        double sfTo = sanctFrac(g, t.to), sfFrom = sanctFrac(g, t.from);
        if (sfTo > OWNERSHIP_HIT)
            rb.notes.push_back("Primalac \"" + g.nameOf(t.to) + "\" je kroz vlasnistvo pod kontrolom sankcionisanih (" + pctStr(sfTo) + ").");
        if (sfFrom > OWNERSHIP_HIT)
            rb.notes.push_back("Posiljalac \"" + g.nameOf(t.from) + "\" je kroz vlasnistvo pod kontrolom sankcionisanih (" + pctStr(sfFrom) + ").");
        auto e = g.edges.find(Graph::edgeKey(t.from, t.to));
        if (e != g.edges.end() && e->second.blockedCount > 0)
            rb.notes.push_back("Ranije transakcije po ovoj grani su blokirane (" +
                               std::to_string(e->second.blockedCount) + "/" + std::to_string(e->second.txCount) + ").");
        if (a > 0.20) rb.notes.push_back("Iznos je relativno visok.");
        if (rb.notes.empty())
            rb.notes.push_back(rb.status != "approved" ? "Kombinacija faktora prelazi prag rizika."
                                                       : "Nema znacajnih faktora rizika.");
        return rb;
    }

    // ---- OFFLINE: ponovo izgradi grane i tezine iz cele istorije ----
    static void recomputeEdges(Graph& g) {
        std::map<std::string, Edge> edges;
        for (const auto& t : g.txs) {
            Edge& e = edges[Graph::edgeKey(t.from, t.to)];
            e.from = t.from; e.to = t.to;
            e.txCount += 1;
            e.sumAmount += t.amount;
            if (flagged(t.status)) e.blockedCount += 1;
            e.lastUpdated = std::max(e.lastUpdated, t.ts);
        }
        auto risk = propagate(g);
        auto rget = [&](const std::string& id) {
            auto it = risk.find(id);
            return it == risk.end() ? 0.0 : it->second;
        };
        for (auto& kv : edges) {
            Edge& e = kv.second;
            double br = e.txCount > 0 ? static_cast<double>(e.blockedCount) / e.txCount : 0.0;
            e.weight = std::clamp(0.4 * rget(e.from) + 0.6 * rget(e.to) + 0.2 * br, 0.0, 1.0);
        }
        g.edges = std::move(edges);
    }

    struct ReviewItem {
        std::string from, to, fromName, toName;
        double weight = 0.0;
        int txCount = 0, blockedCount = 0;
        double sumAmount = 0.0;
        int hits = 0;
        double score = 0.0;
        bool isHit = false;
    };

    struct MCResult {
        double pHat = 0.0;     // procenjena verovatnoca hita
        double d = 0.0;        // CLT granica greske (p=0.5, 95%)
        double ciLow = 0.0, ciHigh = 0.0;
        double targetD = 0.0;  // ciljana granica
        long n = 0;            // broj iteracija
        long nReq = 0;         // potreban n za targetD
        bool enough = false;   // n >= nReq ?
        std::vector<ReviewItem> items;
    };

    // ---- Monte Carlo procena p(hit) + CLT ----
    static MCResult monteCarlo(const Graph& g, int iterations = 20000,
                               double targetD = 0.015, int topK = 50,
                               unsigned seed = 0) {
        MCResult R;
        R.n = iterations;
        R.targetD = targetD;
        R.d = Z95 * std::sqrt(0.25 / std::max(1, iterations));         // p=0.5 worst case
        R.nReq = static_cast<long>(std::ceil(Z95 * Z95 * 0.25 / (targetD * targetD)));
        R.enough = iterations >= R.nReq;

        std::vector<const Edge*> e;
        for (const auto& kv : g.edges) e.push_back(&kv.second);
        const int n = static_cast<int>(e.size());
        if (n == 0) return R;

        // unapred izracunaj hit-status i tezine (memoizovan sanctFrac)
        std::unordered_map<std::string, double> memo;
        std::unordered_set<std::string> vis;
        auto hitNode = [&](const std::string& id) {
            if (g.isSanctioned(id)) return true;
            return sanctFrac(g, id, memo, vis) > OWNERSHIP_HIT;
        };
        // prefix-sum kumulativ tezina za O(log n) roulette-wheel izbor
        std::vector<double> cum(n);
        std::vector<char> hit(n);
        double total = 0.0;
        for (int i = 0; i < n; ++i) {
            total += std::max(1e-9, e[i]->weight);
            cum[i] = total;
            hit[i] = hitNode(e[i]->to) || hitNode(e[i]->from);
        }

        std::vector<long long> hits(n, 0);
        long long totalHits = 0;
        std::mt19937 rng(seed ? seed : std::random_device{}());
        std::uniform_real_distribution<double> pick(0.0, total);
        for (int it = 0; it < iterations; ++it) {
            double r = pick(rng);
            int cur = static_cast<int>(std::lower_bound(cum.begin(), cum.end(), r) - cum.begin());
            if (cur >= n) cur = n - 1;
            if (hit[cur]) { ++hits[cur]; ++totalHits; }
        }

        R.pHat = static_cast<double>(totalHits) / iterations;
        R.ciLow = std::clamp(R.pHat - R.d, 0.0, 1.0);
        R.ciHigh = std::clamp(R.pHat + R.d, 0.0, 1.0);

        long long maxHit = 1;
        for (int i = 0; i < n; ++i) maxHit = std::max(maxHit, hits[i]);
        for (int i = 0; i < n; ++i) {
            if (hits[i] == 0) continue;
            const Edge& ed = *e[i];
            R.items.push_back(ReviewItem{
                ed.from, ed.to, g.nameOf(ed.from), g.nameOf(ed.to),
                ed.weight, ed.txCount, ed.blockedCount, ed.sumAmount,
                static_cast<int>(hits[i]),
                static_cast<double>(hits[i]) / static_cast<double>(maxHit),
                static_cast<bool>(hit[i])});
        }
        std::sort(R.items.begin(), R.items.end(),
                  [](const ReviewItem& a, const ReviewItem& b) { return a.hits > b.hits; });
        if (static_cast<int>(R.items.size()) > topK) R.items.resize(topK);
        return R;
    }
};
