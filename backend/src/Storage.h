#pragma once
#include "json.hpp"
#include "Graph.h"
#include <filesystem>
#include <fstream>
#include <string>

// Perzistencija u JSON fajlove u data/ folderu:
//   companies.json     - ucesnici (firme/osobe), vlasnistvo, sankcije
//   transactions.json  - cela istorija transakcija (ukljucujuci blokirane)
//   edges.json         - agregirane grane sa tezinama (offline rezultat)
namespace Storage {

using nlohmann::json;

inline std::string path(const std::string& dir, const std::string& file) {
    return dir + "/" + file;
}

inline bool readJson(const std::string& p, json& out) {
    std::ifstream f(p);
    if (!f.good()) return false;
    try { f >> out; } catch (...) { return false; }
    return true;
}

inline void writeJson(const std::string& p, const json& j) {
    std::filesystem::create_directories(std::filesystem::path(p).parent_path());
    std::ofstream f(p);
    f << j.dump(2);
}

// ---------- companies ----------
inline bool loadCompanies(const std::string& dir, Graph& g) {
    json j;
    if (!readJson(path(dir, "companies.json"), j)) return false;
    for (const auto& c : j) {
        Company comp;
        comp.id = c.value("id", "");
        comp.name = c.value("name", "");
        comp.kind = c.value("kind", "business");
        comp.location = c.value("location", "");
        comp.sanctioned = c.value("sanctioned", false);
        comp.maliciousness = c.value("maliciousness", 0.0);
        if (c.contains("owners"))
            for (const auto& o : c["owners"])
                comp.owners.push_back(Owner{o.value("ownerId", ""), o.value("stake", 0.0)});
        g.companies[comp.id] = std::move(comp);
    }
    return true;
}

inline void saveCompanies(const std::string& dir, const Graph& g) {
    json arr = json::array();
    for (const auto& kv : g.companies) {
        const Company& c = kv.second;
        json owners = json::array();
        for (const auto& o : c.owners)
            owners.push_back({{"ownerId", o.ownerId}, {"stake", o.stake}});
        arr.push_back({{"id", c.id}, {"name", c.name}, {"kind", c.kind},
                       {"location", c.location}, {"sanctioned", c.sanctioned},
                       {"maliciousness", c.maliciousness}, {"owners", owners}});
    }
    writeJson(path(dir, "companies.json"), arr);
}

// ---------- transactions ----------
inline bool loadTransactions(const std::string& dir, Graph& g) {
    json j;
    if (!readJson(path(dir, "transactions.json"), j)) return false;
    for (const auto& t : j) {
        Transaction tx;
        tx.id = t.value("id", "");
        tx.from = t.value("from", "");
        tx.to = t.value("to", "");
        tx.amount = t.value("amount", 0.0);
        tx.type = t.value("type", "B2B");
        tx.risk = t.value("risk", 0.0);
        tx.status = t.value("status", "approved");
        tx.ts = t.value("ts", 0LL);
        g.txs.push_back(std::move(tx));
    }
    return true;
}

inline void saveTransactions(const std::string& dir, const Graph& g) {
    json arr = json::array();
    for (const auto& t : g.txs)
        arr.push_back({{"id", t.id}, {"from", t.from}, {"to", t.to},
                       {"amount", t.amount}, {"type", t.type}, {"risk", t.risk},
                       {"status", t.status}, {"ts", t.ts}});
    writeJson(path(dir, "transactions.json"), arr);
}

// ---------- edges (offline rezultat) ----------
inline void saveEdges(const std::string& dir, const Graph& g) {
    json arr = json::array();
    for (const auto& kv : g.edges) {
        const Edge& e = kv.second;
        arr.push_back({{"from", e.from}, {"to", e.to}, {"weight", e.weight},
                       {"txCount", e.txCount}, {"blockedCount", e.blockedCount},
                       {"sumAmount", e.sumAmount}, {"lastUpdated", e.lastUpdated}});
    }
    writeJson(path(dir, "edges.json"), arr);
}

}  // namespace Storage
