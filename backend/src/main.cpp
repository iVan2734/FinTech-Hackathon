#include "httplib.h"
#include "json.hpp"
#include "Graph.h"
#include "RiskEngine.h"
#include "Storage.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using json = nlohmann::json;

static Graph g;
static std::atomic<long long> txCounter{0};
static std::string dataDir = "data";

// Prag iznad kog se transakcija blokira i salje na manual review
static constexpr double BLOCK_THRESHOLD = RiskEngine::BLOCK_THRESHOLD;

// ---- Offline obrada (azuriranje tezina grana + perzistencija) ----
static std::mutex jobMtx;
static std::condition_variable jobCv;
static bool jobDirty = false;

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static void triggerOffline() {
    { std::lock_guard<std::mutex> lk(jobMtx); jobDirty = true; }
    jobCv.notify_one();
}

// Worker: ceka signal, pa OFFLINE ponovo izracuna tezine grana i sacuva fajlove.
// Tako se sinhroni put transakcije ne usporava tezim racunom.
static void offlineWorker() {
    for (;;) {
        std::unique_lock<std::mutex> lk(jobMtx);
        jobCv.wait(lk, [] { return jobDirty; });
        jobDirty = false;
        lk.unlock();

        std::lock_guard<std::mutex> glk(g.mtx);
        RiskEngine::recomputeEdges(g);
        Storage::saveTransactions(dataDir, g);
        Storage::saveEdges(dataDir, g);
    }
}

// Pocetni podaci ako data/ fajlovi ne postoje.
static void seed() {
    auto C = [](const std::string& id, const std::string& name, const std::string& kind,
                const std::string& loc, bool sanc, double mal, std::vector<Owner> owners) {
        Company c; c.id = id; c.name = name; c.kind = kind; c.location = loc;
        c.sanctioned = sanc; c.maliciousness = mal; c.owners = std::move(owners);
        g.companies[id] = std::move(c);
    };

    // Osobe
    C("p1", "Marko Markovic", "person", "Beograd, RS", false, 0.05, {});
    C("p2", "Jovana Jovanovic", "person", "Novi Sad, RS", false, 0.10, {});
    C("p3", "Hans Mueller", "person", "Wien, AT", false, 0.10, {});
    C("p4", "Ivan Petrov", "person", "Moscow, RU", true,  0.40, {});  // sankcionisan
    C("p5", "Bruce Wayne", "person", "Gotham, US", false, 0.02, {});

    // Firme (sa vlasnickim udelima)
    C("c1", "Acme Corp", "business", "Beograd, RS", false, 0.10,
      {{"p1", 0.6}, {"p2", 0.4}});
    C("c2", "Globex d.o.o.", "business", "Wien, AT", false, 0.20,
      {{"c1", 0.3}, {"p3", 0.7}});
    C("c3", "Initech", "business", "Limassol, CY", false, 0.45,
      {{"p4", 0.8}, {"p2", 0.2}});                  // 80% sankcionisani vlasnik
    C("c4", "Umbrella LLC", "business", "Valletta, MT", false, 0.55,
      {{"c3", 0.5}, {"p4", 0.5}});                  // lanac ka sankcijama
    C("c5", "Wayne Enterprises", "business", "Gotham, US", false, 0.03,
      {{"p5", 1.0}});

    struct S { std::string from, to, type; double amount; };
    std::vector<S> seeds = {
        {"p1", "c1", "P2B", 500.0},   {"p2", "c2", "P2B", 12000.0},
        {"c1", "c2", "B2B", 9000.0},  {"c2", "c3", "B2B", 45000.0},
        {"c3", "c4", "B2B", 47000.0}, {"c4", "c5", "B2B", 30000.0},
        {"c5", "c1", "B2B", 8000.0},  {"p1", "c4", "P2B", 9500.0},
        {"c2", "c4", "B2B", 60000.0}, {"c1", "c3", "B2B", 1500.0},
    };
    for (auto& s : seeds) {
        Transaction t;
        t.id = "tx" + std::to_string(++txCounter);
        t.from = s.from; t.to = s.to; t.type = s.type; t.amount = s.amount;
        t.ts = nowMs();
        t.risk = RiskEngine::computeRisk(g, t);
        t.status = RiskEngine::decide(t.risk);
        g.txs.push_back(t);
    }
}

static void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

// Najveci numericki sufiks u postojecim txN id-evima (za nastavak brojaca)
static long long maxTxId() {
    long long mx = 0;
    for (const auto& t : g.txs)
        if (t.id.rfind("tx", 0) == 0) {
            try { mx = std::max(mx, std::stoll(t.id.substr(2))); } catch (...) {}
        }
    return mx;
}

static json companySummary(const Company& c) {
    return {{"id", c.id}, {"name", c.name}, {"kind", c.kind},
            {"location", c.location}, {"sanctioned", c.sanctioned}};
}

int main(int argc, char** argv) {
    const std::string frontendDir = (argc > 1) ? argv[1] : "frontend";
    if (argc > 2) dataDir = argv[2];

    // Ucitaj iz fajlova; ako nema -> seed pa snimi.
    {
        std::lock_guard<std::mutex> glk(g.mtx);
        bool loaded = Storage::loadCompanies(dataDir, g);
        Storage::loadTransactions(dataDir, g);
        if (!loaded || g.companies.empty()) {
            seed();
            Storage::saveCompanies(dataDir, g);
        }
        txCounter = maxTxId();
        RiskEngine::recomputeEdges(g);   // inicijalne tezine grana
        Storage::saveTransactions(dataDir, g);
        Storage::saveEdges(dataDir, g);
    }

    std::thread(offlineWorker).detach();

    httplib::Server svr;
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    });
    svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    // --- Login (demo nalozi) ---
    svr.Post("/api/login", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return sendJson(res, {{"error", "bad json"}}, 400);
        std::string user = body.value("username", "");
        std::string pass = body.value("password", "");
        std::string role;
        if (user == "reviewer" && pass == "reviewer") role = "reviewer";
        else if (user == "user" && pass == "user") role = "user";
        else return sendJson(res, {{"error", "pogresni kredencijali"}}, 401);
        sendJson(res, {{"token", "demo-" + user}, {"username", user}, {"role", role}});
    });

    // --- Lista ucesnika ---
    svr.Get("/api/companies", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g.mtx);
        json arr = json::array();
        for (auto& kv : g.companies) arr.push_back(companySummary(kv.second));
        sendJson(res, arr);
    });

    // --- Sankcionisani ucesnici ---
    svr.Get("/api/sanctioned", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g.mtx);
        json arr = json::array();
        for (auto& kv : g.companies)
            if (kv.second.sanctioned) arr.push_back(companySummary(kv.second));
        sendJson(res, arr);
    });

    // --- Profil firme/osobe: lokacija, rizik, vlasnistvo, transakcije ---
    svr.Get(R"(/api/company/([\w-]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1].str();
        std::lock_guard<std::mutex> lk(g.mtx);
        auto it = g.companies.find(id);
        if (it == g.companies.end()) return sendJson(res, {{"error", "nepoznat ucesnik"}}, 404);
        const Company& c = it->second;

        json owners = json::array();
        for (const auto& o : c.owners)
            owners.push_back({{"ownerId", o.ownerId}, {"name", g.nameOf(o.ownerId)},
                              {"stake", o.stake}, {"sanctioned", g.isSanctioned(o.ownerId)}});

        // Firme u kojima je ovaj ucesnik vlasnik
        json ownerOf = json::array();
        for (const auto& kv : g.companies)
            for (const auto& o : kv.second.owners)
                if (o.ownerId == id)
                    ownerOf.push_back({{"id", kv.first}, {"name", kv.second.name}, {"stake", o.stake}});

        // Transakcije ovog ucesnika (ulazne i izlazne) + brojaci
        json txs = json::array();
        int blocked = 0, total = 0;
        for (const auto& t : g.txs) {
            if (t.from != id && t.to != id) continue;
            ++total;
            if (t.status == "blocked" || t.status == "fraud") ++blocked;
            txs.push_back({{"id", t.id}, {"from", t.from}, {"fromName", g.nameOf(t.from)},
                           {"to", t.to}, {"toName", g.nameOf(t.to)}, {"amount", t.amount},
                           {"type", t.type}, {"risk", t.risk}, {"status", t.status}});
        }

        sendJson(res, {
            {"id", c.id}, {"name", c.name}, {"kind", c.kind}, {"location", c.location},
            {"sanctioned", c.sanctioned}, {"maliciousness", c.maliciousness},
            {"riskScore", RiskEngine::baseRisk(g, id)},
            {"sanctFrac", RiskEngine::sanctFrac(g, id)},
            {"sanctionedControlled", RiskEngine::isSanctionedControlled(g, id)},
            {"owners", owners}, {"ownerOf", ownerOf},
            {"txTotal", total}, {"txBlocked", blocked}, {"transactions", txs}});
    });

    // --- Upit nad transakcijama: ?status=blocked&company=c3 ---
    svr.Get("/api/transactions", [](const httplib::Request& req, httplib::Response& res) {
        std::string fStatus = req.has_param("status") ? req.get_param_value("status") : "";
        std::string fComp = req.has_param("company") ? req.get_param_value("company") : "";
        std::lock_guard<std::mutex> lk(g.mtx);
        json arr = json::array();
        for (const auto& t : g.txs) {
            if (!fStatus.empty() && t.status != fStatus) continue;
            if (!fComp.empty() && t.from != fComp && t.to != fComp) continue;
            arr.push_back({{"id", t.id}, {"from", t.from}, {"fromName", g.nameOf(t.from)},
                           {"to", t.to}, {"toName", g.nameOf(t.to)}, {"amount", t.amount},
                           {"type", t.type}, {"risk", t.risk}, {"status", t.status}, {"ts", t.ts}});
        }
        sendJson(res, arr);
    });

    // --- Vlasnicka struktura firme (SAMO vlasnistvo, uzlazno) za vizuelizaciju ---
    // Koren = fokus firma; deca = njeni vlasnici; pa vlasnici vlasnika ... Osobe su listovi.
    svr.Get("/api/graph", [](const httplib::Request& req, httplib::Response& res) {
        std::string center = req.has_param("center") ? req.get_param_value("center") : "";
        int depth = req.has_param("depth") ? std::stoi(req.get_param_value("depth")) : 3;

        std::lock_guard<std::mutex> lk(g.mtx);
        if (center.empty()) {
            for (const auto& kv : g.companies)
                if (kv.second.kind == "business") { center = kv.first; break; }
            if (center.empty()) return sendJson(res, {{"nodes", json::array()}, {"links", json::array()}});
        }
        if (!g.hasCompany(center)) return sendJson(res, {{"error", "nepoznat cvor"}}, 404);

        std::unordered_map<std::string, int> depthOf;
        std::unordered_map<std::string, double> stakeOf;        // udeo cvora u firmi koja ga je uvela
        std::set<std::pair<std::string, std::string>> ownLinks;  // (vlasnik -> firma)

        std::vector<std::pair<std::string, int>> q = {{center, 0}};
        depthOf[center] = 0;
        stakeOf[center] = 1.0;
        for (size_t qi = 0; qi < q.size(); ++qi) {
            auto [id, d] = q[qi];
            if (d >= depth) continue;
            const Company& c = g.companies.at(id);
            for (const auto& o : c.owners) {                     // samo vlasnici (uzlazno)
                if (!g.hasCompany(o.ownerId)) continue;
                ownLinks.insert({o.ownerId, id});
                if (!depthOf.count(o.ownerId)) {
                    depthOf[o.ownerId] = d + 1;
                    stakeOf[o.ownerId] = o.stake;
                    q.push_back({o.ownerId, d + 1});
                }
            }
        }

        json jnodes = json::array();
        for (const auto& kv : depthOf) {
            const std::string& id = kv.first;
            const Company& c = g.companies.at(id);
            jnodes.push_back({{"id", id}, {"name", c.name}, {"kind", c.kind},
                              {"sanctioned", c.sanctioned},
                              {"riskScore", RiskEngine::baseRisk(g, id)},
                              {"sanctFrac", RiskEngine::sanctFrac(g, id)},
                              {"hit", RiskEngine::isSanctionedControlled(g, id)},
                              {"depth", kv.second}, {"stake", stakeOf[id]},
                              {"center", id == center}});
        }
        json jlinks = json::array();
        for (const auto& l : ownLinks) {
            if (!depthOf.count(l.first) || !depthOf.count(l.second)) continue;
            double stake = 0.0;
            for (const auto& o : g.companies.at(l.second).owners)
                if (o.ownerId == l.first) stake = o.stake;
            jlinks.push_back({{"source", l.first}, {"target", l.second},
                              {"kind", "ownership"}, {"stake", stake}});
        }
        sendJson(res, {{"center", center}, {"nodes", jnodes}, {"links", jlinks}});
    });

    // --- Pokretanje transakcije ---
    svr.Post("/api/transaction", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return sendJson(res, {{"error", "bad json"}}, 400);

        Transaction t;
        t.from = body.value("from", "");
        t.to = body.value("to", "");
        t.amount = body.value("amount", 0.0);

        std::lock_guard<std::mutex> lk(g.mtx);
        if (!g.hasCompany(t.from) || !g.hasCompany(t.to))
            return sendJson(res, {{"error", "nepoznat ucesnik"}}, 400);
        if (t.from == t.to)
            return sendJson(res, {{"error", "posiljalac i primalac su isti"}}, 400);

        // Tip zakljucuje sistem iz posiljaoca: osoba -> P2B, firma -> B2B
        t.type = (g.companies.at(t.from).kind == "person") ? "P2B" : "B2B";

        auto rb = RiskEngine::explain(g, t);
        t.risk = rb.risk;
        t.id = "tx" + std::to_string(++txCounter);
        t.ts = nowMs();
        t.status = rb.status;
        g.txs.push_back(t);

        // Brzo snimi istoriju; tezine grana se azuriraju OFFLINE.
        Storage::saveTransactions(dataDir, g);
        triggerOffline();

        json factors = json::array();
        for (auto& f : rb.factors) factors.push_back({{"label", f.label}, {"value", f.value}});
        json reasons = json::array();
        for (auto& n : rb.notes) reasons.push_back(n);

        std::string msg = t.status == "approved" ? "Transakcija ODOBRENA."
                        : t.status == "review"   ? "Transakcija ZADRZANA — ide na manual review."
                                                 : "Transakcija BLOKIRANA.";
        sendJson(res, {
            {"id", t.id}, {"risk", t.risk}, {"status", t.status},
            {"blocked", t.status == "blocked"}, {"review", t.status == "review"},
            {"threshold", RiskEngine::REVIEW_THRESHOLD},
            {"reviewThreshold", RiskEngine::REVIEW_THRESHOLD},
            {"blockThreshold", RiskEngine::BLOCK_THRESHOLD},
            {"from", g.nameOf(t.from)}, {"to", g.nameOf(t.to)},
            {"amount", t.amount}, {"type", t.type},
            {"factors", factors}, {"reasons", reasons}, {"message", msg}});
    });

    // --- Manual review red (Monte Carlo + CLT) ---
    svr.Get("/api/review", [](const httplib::Request& req, httplib::Response& res) {
        int iters = 20000;
        if (req.has_param("iters")) iters = std::max(100, std::stoi(req.get_param_value("iters")));
        double targetD = 0.015;
        if (req.has_param("d")) targetD = std::stod(req.get_param_value("d"));
        std::lock_guard<std::mutex> lk(g.mtx);
        auto R = RiskEngine::monteCarlo(g, iters, targetD);
        json arr = json::array();
        for (auto& it : R.items) {
            arr.push_back({{"from", it.from}, {"fromName", it.fromName},
                           {"to", it.to}, {"toName", it.toName}, {"weight", it.weight},
                           {"txCount", it.txCount}, {"blockedCount", it.blockedCount},
                           {"sumAmount", it.sumAmount}, {"hits", it.hits},
                           {"score", it.score}, {"isHit", it.isHit}});
        }

        // Red za pregled: review + blocked transakcije (najnovije prvo)
        std::vector<const Transaction*> pending;
        for (const auto& t : g.txs)
            if (t.status == "review" || t.status == "blocked") pending.push_back(&t);
        std::sort(pending.begin(), pending.end(),
                  [](const Transaction* a, const Transaction* b) { return a->ts > b->ts; });
        json queue = json::array();
        int lim = 0;
        for (const auto* t : pending) {
            if (lim++ >= 100) break;
            // relevantnost = tezina grane (verovatnoca kojom je Monte Carlo uzorkuje)
            auto ge = g.edges.find(Graph::edgeKey(t->from, t->to));
            double w = ge != g.edges.end() ? ge->second.weight : 0.0;
            queue.push_back({{"txId", t->id}, {"from", t->from}, {"fromName", g.nameOf(t->from)},
                             {"to", t->to}, {"toName", g.nameOf(t->to)}, {"amount", t->amount},
                             {"type", t->type}, {"risk", t->risk}, {"status", t->status}, {"mcScore", w}});
        }

        sendJson(res, {
            {"iterations", R.n},
            {"clt", {{"pHat", R.pHat}, {"d", R.d}, {"ciLow", R.ciLow}, {"ciHigh", R.ciHigh},
                     {"targetD", R.targetD}, {"nReq", R.nReq}, {"enough", R.enough}}},
            {"queueTotal", (int)pending.size()},
            {"queue", queue},
            {"items", arr}});
    });

    // --- Odluka reviewera ---
    svr.Post("/api/review/decision", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return sendJson(res, {{"error", "bad json"}}, 400);
        std::string txId = body.value("txId", "");
        std::string decision = body.value("decision", "");
        if (decision != "cleared" && decision != "fraud")
            return sendJson(res, {{"error", "decision mora biti cleared|fraud"}}, 400);

        std::lock_guard<std::mutex> lk(g.mtx);
        for (auto& t : g.txs) {
            if (t.id == txId) {
                t.status = decision;
                if (decision == "fraud") t.risk = 1.0; else t.risk = std::min(t.risk, 0.2);
                Storage::saveTransactions(dataDir, g);
                triggerOffline();   // azuriraj tezine grana offline
                return sendJson(res, {{"ok", true}, {"txId", txId}, {"status", t.status}});
            }
        }
        sendJson(res, {{"error", "transakcija nije pronadjena"}}, 404);
    });

    svr.set_mount_point("/", frontendDir);

    int port = 8080;
    std::cout << "FinTech API + frontend: http://localhost:" << port << "\n";
    std::cout << "Frontend: " << frontendDir << "  |  Data: " << dataDir << "\n";
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "Ne mogu da otvorim port " << port << " (zauzet?)\n";
        return 1;
    }
    return 0;
}
