let session = null; // { token, username, role }
let companies = [];
const $ = (id) => document.getElementById(id);

async function api(path, method = "GET", body = null) {
  const res = await fetch(path, {
    method,
    headers: { "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : null,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || "HTTP " + res.status);
  return data;
}

const pct = (x) => (x * 100).toFixed(0) + "%";
const eur = (x) => "€" + Number(x).toLocaleString("de-DE");
const riskBar = (w, width) =>
  `<span class="bar" style="width:${width || 90}px"><span style="width:${(w * 100).toFixed(0)}%"></span></span>`;

// ---------- LOGIN ----------
$("login-btn").onclick = doLogin;
$("login-pass").addEventListener("keydown", (e) => { if (e.key === "Enter") doLogin(); });

async function doLogin() {
  $("login-err").textContent = "";
  try {
    session = await api("/api/login", "POST", {
      username: $("login-user").value.trim(),
      password: $("login-pass").value,
    });
    onLogin();
  } catch (e) {
    $("login-err").textContent = e.message;
  }
}

$("logout").onclick = () => {
  session = null;
  $("app").classList.add("hidden");
  $("login-screen").classList.remove("hidden");
  $("login-pass").value = "";
};

async function onLogin() {
  $("who").textContent = `${session.username} • ${session.role}`;
  $("login-screen").classList.add("hidden");
  $("app").classList.remove("hidden");
  // user vidi samo "Nova transakcija"; reviewer vidi sve
  const isReviewer = session.role === "reviewer";
  document.querySelectorAll(".reviewer-only").forEach((b) => b.classList.toggle("hidden", !isReviewer));
  await loadCompanies();
  showView("transaction");
}

// ---------- NAV ----------
document.querySelectorAll("#nav button").forEach((b) => {
  b.onclick = () => showView(b.dataset.view);
});

function showView(view) {
  ["transaction", "lookup", "graph", "review"].forEach((v) =>
    $("view-" + v).classList.toggle("hidden", v !== view)
  );
  document.querySelectorAll("#nav button").forEach((b) =>
    b.classList.toggle("active", b.dataset.view === view)
  );
  if (view === "review") loadReview();
  if (view === "graph") loadGraph($("gr-select").value);
}

// ---------- COMPANIES ----------
async function loadCompanies() {
  companies = await api("/api/companies");
  companies.sort((a, b) => a.name.localeCompare(b.name));
  const opts = companies
    .map((c) => `<option value="${c.id}">${c.name} (${c.kind})</option>`)
    .join("");
  $("tx-from").innerHTML = opts;
  $("tx-to").innerHTML = opts;
  $("lk-select").innerHTML = opts;
  // u graf se centrira SAMO na firme (osobe su krajnji vlasnici)
  $("gr-select").innerHTML = companies
    .filter((c) => c.kind === "business")
    .map((c) => `<option value="${c.id}">${c.name}</option>`)
    .join("");
  if (companies.length > 1) $("tx-to").selectedIndex = 1;
  $("lk-select").onchange = () => loadProfile($("lk-select").value);
  $("gr-select").onchange = () => loadGraph($("gr-select").value);
  $("gr-depth").onchange = () => loadGraph($("gr-select").value);
  if (companies.length && session.role === "reviewer") loadProfile(companies[0].id);
}

// ---------- TRANSACTION ----------
$("tx-btn").onclick = async () => {
  const out = $("tx-result");
  out.className = "";
  try {
    const r = await api("/api/transaction", "POST", {
      from: $("tx-from").value,
      to: $("tx-to").value,
      amount: parseFloat($("tx-amount").value) || 0,
    });
    const map = {
      approved: ["✅ ODOBRENO", "a", "approved"],
      review: ["🔍 MANUAL REVIEW", "r", "review"],
      blocked: ["⛔ BLOKIRANO", "b", "blocked"],
    };
    const [badge, bclass, cls] = map[r.status] || map.blocked;
    out.classList.add(cls);
    const reasons = (r.reasons || []).map((x) => `<li>${x}</li>`).join("");
    const factors = (r.factors || [])
      .map((f) => `
        <div class="frow">
          <span class="flabel">${f.label}</span>
          <span class="fbar"><span style="width:${Math.min(100, (f.value / r.threshold) * 100)}%"></span></span>
          <span class="fval">+${pct(f.value)}</span>
        </div>`)
      .join("");
    out.innerHTML = `
      <div class="result-head">
        <span class="result-badge ${bclass}">${badge}</span>
        <span class="result-risk">rizik <b>${pct(r.risk)}</b> &nbsp;/&nbsp; review ≥ ${pct(r.reviewThreshold)} &nbsp;/&nbsp; blok ≥ ${pct(r.blockThreshold)}</span>
      </div>
      <div class="result-sub">${r.from} → ${r.to} • ${eur(r.amount)} • ${r.type}</div>
      <div class="why"><b>Zašto:</b><ul>${reasons}</ul></div>
      <div class="factors"><b>Doprinosi riziku:</b>${factors}</div>`;
  } catch (e) {
    out.classList.add("blocked");
    out.textContent = "Greska: " + e.message;
  }
};

// ---------- LOOKUP / PROFILE ----------
async function loadProfile(id) {
  const p = await api("/api/company/" + id);
  const owners = p.owners.length
    ? p.owners.map((o) =>
        `<div class="kv"><b>${o.name}</b> — ${pct(o.stake)}${o.sanctioned ? ' <span class="badge-sanc">SANC</span>' : ""}</div>`
      ).join("")
    : '<div class="hint">— nema upisanih vlasnika —</div>';

  const ownerOf = p.ownerOf.length
    ? p.ownerOf.map((o) => `<div class="kv"><b>${o.name}</b> — ${pct(o.stake)}</div>`).join("")
    : '<div class="hint">— ne poseduje druge firme —</div>';

  const txRows = p.transactions.length
    ? p.transactions.map((t) => `
        <tr>
          <td>${t.fromName} → ${t.toName}</td>
          <td>${t.type}</td>
          <td>${eur(t.amount)}</td>
          <td>${pct(t.risk)}</td>
          <td><span class="pill ${t.status}">${t.status}</span></td>
        </tr>`).join("")
    : '<tr><td colspan="5" class="hint">nema transakcija</td></tr>';

  $("lk-profile").innerHTML = `
    <div class="card">
      <div class="profile-head">
        <div>
          <h2 style="margin-bottom:2px">${p.name}</h2>
          <div class="kv">${p.kind} &nbsp;•&nbsp; 📍 ${p.location || "n/a"}</div>
        </div>
        ${p.sanctioned ? '<span class="badge-sanc">SANKCIONISAN</span>' : ""}
      </div>

      <div class="gauge">
        <div class="kv" style="margin-bottom:4px">Sklonost ka malicioznim transakcijama: <b>${pct(p.riskScore)}</b></div>
        ${riskBar(p.riskScore, 220)}
      </div>

      <div class="stat-row">
        <div class="stat"><div class="n">${p.txTotal}</div><div class="l">ukupno tx</div></div>
        <div class="stat"><div class="n" style="color:var(--bad)">${p.txBlocked}</div><div class="l">blokirano</div></div>
        <div class="stat"><div class="n">${pct(p.maliciousness)}</div><div class="l">bazna sklonost</div></div>
      </div>
    </div>

    <div class="card">
      <div class="grid2">
        <div><h2>Vlasnici (udeli)</h2>${owners}</div>
        <div><h2>Vlasnik u firmama</h2>${ownerOf}</div>
      </div>
    </div>

    <div class="card">
      <h2>Transakcije</h2>
      <div class="table-wrap">
        <table>
          <thead><tr><th>Grana</th><th>Tip</th><th>Iznos</th><th>Rizik</th><th>Status</th></tr></thead>
          <tbody>${txRows}</tbody>
        </table>
      </div>
    </div>`;
}

// ---------- GRAPH ----------
const riskColor = (r) => `hsl(${(1 - r) * 115}, 75%, 52%)`;

async function loadGraph(center) {
  if (!center) return;
  const W = 960, H = 600;
  const depth = parseInt($("gr-depth").value) || 2;
  const data = await api(`/api/graph?center=${center}&depth=${depth}`);
  const N = data.nodes, L = data.links;
  if (!N.length) { $("graph-canvas").innerHTML = '<p class="hint">Nema podataka.</p>'; return; }

  const idx = {};
  N.forEach((n, i) => (idx[n.id] = i));
  const maxD = Math.max(1, ...N.map((n) => n.depth));

  // hijerarhijski raspored: nivo (dubina) = horizontalni red, fokus firma gore
  const groups = {};
  N.forEach((n) => (groups[n.depth] = groups[n.depth] || []).push(n));
  const topPad = 64, botPad = 48;
  const levelH = (H - topPad - botPad) / Math.max(1, maxD);
  Object.keys(groups).forEach((d) => {
    const arr = groups[d], y = topPad + (+d) * levelH;
    arr.forEach((n, i) => { n.y = y; n.x = (W * (i + 1)) / (arr.length + 1); });
  });
  // veličina čvora ∝ udeo (vlasnistvo); fokus firma fiksna
  const rad = (n) => (n.center ? 22 : 9 + (n.stake || 0) * 20);

  let svg = `<svg viewBox="0 0 ${W} ${H}" width="100%" style="max-height:620px">`;
  svg += `<defs><marker id="arr" markerWidth="9" markerHeight="9" refX="8" refY="3" orient="auto"><path d="M0,0 L8,3 L0,6 Z" fill="#9b6dff"/></marker></defs>`;
  // oznake nivoa
  Object.keys(groups).forEach((d) => {
    const y = topPad + (+d) * levelH;
    const lbl = +d === 0 ? "Fokus firma" : +d === 1 ? "Vlasnici" : "Vlasnici · nivo " + d;
    svg += `<text x="12" y="${y + 4}" fill="#bba089" font-size="11">${lbl}</text>`;
  });
  // veze vlasnistva (vlasnik dole -> firma gore), debljina ∝ udeo
  L.forEach((l) => {
    const a = N[idx[l.source]], b = N[idx[l.target]];
    if (!a || !b) return;
    svg += `<line x1="${a.x}" y1="${a.y}" x2="${b.x}" y2="${b.y}" stroke="#9b6dff" stroke-width="${1 + l.stake * 4}" opacity="0.8" marker-end="url(#arr)"/>`;
    const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
    svg += `<text x="${mx}" y="${my}" text-anchor="middle" fill="#7a5bbf" font-size="10" paint-order="stroke" stroke="#fff" stroke-width="3">${Math.round(l.stake * 100)}%</text>`;
  });
  // cvorovi
  N.forEach((n) => {
    const r = rad(n);
    const stroke = n.sanctioned ? "#e03131" : n.hit ? "#f08c00" : "#d9c4b0";
    const sw = n.sanctioned || n.hit ? 4 : 1.5;
    const clk = n.kind === "business";
    const shape = n.kind === "person"
      ? `<rect x="${n.x - r}" y="${n.y - r}" width="${r * 2}" height="${r * 2}" rx="3" fill="${riskColor(n.riskScore)}" stroke="${stroke}" stroke-width="${sw}"/>`
      : `<circle cx="${n.x}" cy="${n.y}" r="${r}" fill="${riskColor(n.riskScore)}" stroke="${stroke}" stroke-width="${sw}"/>`;
    const sub = n.center ? "" :
      `<text x="${n.x}" y="${n.y + r + 13}" text-anchor="middle" fill="#9a8472" font-size="10">${Math.round((n.stake || 0) * 100)}%</text>`;
    svg += `<g class="gnode ${clk ? "clk" : ""}" ${clk ? `data-id="${n.id}"` : ""}>${shape}
      <text x="${n.x}" y="${n.y - r - 6}" text-anchor="middle" fill="#3a2c1e" font-size="11" font-weight="${n.center ? 700 : 500}" paint-order="stroke" stroke="#fff" stroke-width="3">${n.name}</text>${sub}</g>`;
  });
  svg += `</svg>`;
  $("graph-canvas").innerHTML = svg;
  // klik samo na firme (osobe su krajnji vlasnici — ne ulazi se u njih)
  document.querySelectorAll(".gnode.clk").forEach((el) => {
    el.onclick = () => { $("gr-select").value = el.dataset.id; loadGraph(el.dataset.id); };
  });
}

// ---------- REVIEW ----------
$("review-refresh").onclick = loadReview;

async function loadReview() {
  const data = await api("/api/review?iters=20000");
  const c = data.clt;
  $("review-meta").innerHTML = `
    <div class="clt">
      <div class="stat"><div class="n">${data.queueTotal}</div><div class="l">u redu za pregled</div></div>
      <div class="stat"><div class="n">${pct(c.pHat)}</div><div class="l">p̂ (verov. hita)</div></div>
      <div class="stat"><div class="n">±${(c.d * 100).toFixed(2)}%</div><div class="l">CLT granica d</div></div>
      <div class="stat"><div class="n">[${pct(c.ciLow)}, ${pct(c.ciHigh)}]</div><div class="l">95% interval</div></div>
      <div class="stat"><div class="n">${data.iterations.toLocaleString()}</div><div class="l">iteracija (≥ ${c.nReq})</div></div>
    </div>`;

  // red blokiranih transakcija (najnovije prvo)
  const qbody = $("queue-table").querySelector("tbody");
  qbody.innerHTML = data.queue.length
    ? data.queue.map((q) => `
        <tr>
          <td>${q.fromName} → ${q.toName}</td>
          <td>${q.type}</td>
          <td>${eur(q.amount)}</td>
          <td><span class="pill ${q.status}">${q.status === "review" ? "review" : "blocked"}</span></td>
          <td><b style="color:var(--bad)">${pct(q.risk)}</b></td>
          <td>${riskBar(q.mcScore, 80)} <span class="hint">${pct(q.mcScore)}</span></td>
          <td>
            <button class="btn-fraud" data-tx="${q.txId}" data-dec="fraud">Fraud</button>
            <button class="btn-clear" data-tx="${q.txId}" data-dec="cleared">OK</button>
          </td>
        </tr>`).join("")
    : '<tr><td colspan="7" class="hint">Nema transakcija u redu.</td></tr>';
  qbody.querySelectorAll("button").forEach((b) => {
    b.onclick = async () => {
      await api("/api/review/decision", "POST", { txId: b.dataset.tx, decision: b.dataset.dec });
      loadReview();
    };
  });

  // Monte Carlo rangiranje grana
  const tbody = $("review-table").querySelector("tbody");
  const maxHits = data.items.reduce((m, it) => Math.max(m, it.hits), 1);
  tbody.innerHTML = data.items
    .map((it, i) => `
      <tr>
        <td>${i + 1}</td>
        <td>${it.fromName} → ${it.toName}</td>
        <td>${riskBar(it.weight, 70)} <span class="hint">${pct(it.weight)}</span></td>
        <td>${riskBar(it.hits / maxHits, 70)} <span class="hint">${it.hits}</span></td>
        <td>${it.txCount}</td>
        <td>${it.blockedCount}</td>
        <td>${eur(it.sumAmount)}</td>
      </tr>`).join("");
}
