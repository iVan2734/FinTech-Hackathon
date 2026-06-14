#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>

// Vlasnicki udeo: ko poseduje koliki deo firme [0..1]
struct Owner {
    std::string ownerId;
    double stake = 0.0;
};

// Cvor u grafu = ucesnik (firma ili osoba)
struct Company {
    std::string id;
    std::string name;
    std::string kind;              // "business" | "person"
    std::string location;          // npr. "Beograd, RS"
    bool sanctioned = false;       // blokiran / na sankcionoj listi
    double maliciousness = 0.0;    // sopstvena sklonost ka malicioznim tx [0..1]
    std::vector<Owner> owners;     // vlasnicka struktura (udeli u firmi)
};

// Pojedinacna transakcija
struct Transaction {
    std::string id;
    std::string from;    // id posiljaoca
    std::string to;      // id primaoca
    double amount = 0.0;
    std::string type;    // "P2B" | "B2B"
    double risk = 0.0;   // procenjen rizik te tx [0..1]
    std::string status;  // approved | blocked | cleared | fraud
    long long ts = 0;    // vreme (ms)
};

// Agregirana grana izmedju dva ucesnika (weight koji Monte Carlo koristi).
// Tezina se inicijalizuje iz vlasnickih udela, a zatim azurira OFFLINE
// na osnovu istorije transakcija po toj grani.
struct Edge {
    std::string from;
    std::string to;
    double weight = 0.0;      // relevantnost/rizik grane [0..1]
    int txCount = 0;
    int blockedCount = 0;     // blocked + fraud
    double sumAmount = 0.0;
    long long lastUpdated = 0;
};

class Graph {
public:
    std::mutex mtx;
    std::unordered_map<std::string, Company> companies;
    std::vector<Transaction> txs;
    std::map<std::string, Edge> edges;   // kljuc = from + "->" + to

    static std::string edgeKey(const std::string& a, const std::string& b) {
        return a + "->" + b;
    }

    bool hasCompany(const std::string& id) const { return companies.count(id) > 0; }

    std::string nameOf(const std::string& id) const {
        auto it = companies.find(id);
        return it == companies.end() ? id : it->second.name;
    }

    bool isSanctioned(const std::string& id) const {
        auto it = companies.find(id);
        return it != companies.end() && it->second.sanctioned;
    }

    // Tekuca tezina grane (rezultat poslednjeg offline prolaza); ako grana
    // jos ne postoji, vraca -1 (pozivalac koristi bazu iz vlasnistva).
    double currentEdgeWeight(const std::string& from, const std::string& to) const {
        auto it = edges.find(edgeKey(from, to));
        return it == edges.end() ? -1.0 : it->second.weight;
    }
};
