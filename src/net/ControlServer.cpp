#include "ControlServer.h"

#include "../core/Json.h"

#include <cstdio>

namespace acm::net {
namespace {

// The page, served whole. Inlined rather than read from disk because a control
// surface that stops working when the application is moved to another folder
// is worse than a long string literal.
//
// It is plain DOM and pointer events - no framework, no build step, and no
// network fetches beyond the socket. A tablet at a venue may have no internet
// at all, and a page that needs a CDN is a page that does not open.
constexpr const char* kPage = R"HTML(<!doctype html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>acomposter</title>
<style>
  :root { --bg:#08090B; --panel:#101317; --sunken:#0A0C0F; --border:#232A33;
          --text:#C6D0DA; --dim:#7C8894; --accent:#3FE0C0; --amber:#E9A13B; }
  * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
  html,body { margin:0; height:100%; background:var(--bg); color:var(--text);
              font:14px/1.3 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
              overscroll-behavior:none; touch-action:none; }
  #bar { height:38px; display:flex; align-items:center; gap:10px; padding:0 10px;
         background:var(--panel); border-bottom:1px solid var(--border); font-size:12px; }
  #dot { width:8px; height:8px; border-radius:4px; background:#FF5F5F; }
  #dot.on { background:var(--accent); }
  #pages { display:flex; gap:6px; margin-left:auto; }
  .page { padding:4px 10px; border:1px solid var(--border); border-radius:3px; color:var(--dim); }
  .page.sel { background:var(--accent); color:#06090B; border-color:var(--accent); }
  #grid { position:relative; width:100%; height:calc(100% - 38px); }
  .ctl { position:absolute; display:flex; flex-direction:column; align-items:center;
         justify-content:center; padding:4px; }
  .name { font-size:11px; color:var(--dim); margin-top:3px; text-align:center;
          white-space:nowrap; overflow:hidden; text-overflow:ellipsis; max-width:100%; }
  .knob { flex:1; width:100%; }
  .fader { flex:1; width:100%; background:var(--sunken); border:1px solid var(--border);
           border-radius:3px; position:relative; overflow:hidden; }
  .fill { position:absolute; left:0; right:0; bottom:0; background:var(--accent); opacity:.75; }
  .btn { flex:1; width:100%; background:var(--sunken); border:1px solid var(--border);
         border-radius:3px; display:flex; align-items:center; justify-content:center;
         color:var(--dim); }
  .btn.on { background:rgba(63,224,192,.42); border-color:var(--accent); color:var(--text); }
  .pad { flex:1; width:100%; background:var(--sunken); border:1px solid var(--border);
         border-radius:3px; position:relative; }
  .dot2 { position:absolute; width:22px; height:22px; margin:-11px 0 0 -11px;
          border-radius:11px; background:var(--accent); border:2px solid var(--text); }
  .label { font-weight:600; color:var(--text); }
  .note { flex:1; display:flex; align-items:center; justify-content:center;
          color:var(--dim); font-size:12px; text-align:center; padding:0 8px; }
</style></head><body>
<div id="bar"><div id="dot"></div><span id="status">connecting</span><div id="pages"></div></div>
<div id="grid"></div>
<script>
let layout = null, ws = null, page = 0;
const held = {};

function connect() {
  ws = new WebSocket("ws://" + location.host + "/socket");
  ws.onopen = () => { document.getElementById("dot").classList.add("on");
                      document.getElementById("status").textContent = "connected"; };
  ws.onclose = () => { document.getElementById("dot").classList.remove("on");
                       document.getElementById("status").textContent = "reconnecting";
                       setTimeout(connect, 1000); };
  ws.onmessage = e => {
    const m = JSON.parse(e.data);
    if (m.t === "layout") { layout = m; page = m.page; build(); }
    else if (m.t === "v" && layout) {
      // Not applied to a control the finger is on: the echo of our own move
      // would fight the drag it came from.
      if (held[m.id]) return;
      for (const c of layout.controls) if (c.id === m.id) { c.v = m.v; c.vy = m.vy; }
      paint();
    }
  };
}

function send(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }

function build() {
  const pages = document.getElementById("pages");
  pages.innerHTML = "";
  (layout.pages || []).forEach((n, i) => {
    const el = document.createElement("div");
    el.className = "page" + (i === page ? " sel" : "");
    el.textContent = n;
    el.onclick = () => send({ t: "page", i: i });
    pages.appendChild(el);
  });

  const grid = document.getElementById("grid");
  grid.innerHTML = "";
  for (const c of layout.controls) {
    const el = document.createElement("div");
    el.className = "ctl";
    el.style.left = (100 * c.x / layout.cols) + "%";
    el.style.top = (100 * c.y / layout.rows) + "%";
    el.style.width = (100 * c.w / layout.cols) + "%";
    el.style.height = (100 * c.h / layout.rows) + "%";
    el.dataset.id = c.id;

    if (c.k === "label") { el.innerHTML = '<div class="note label">' + esc(c.n) + "</div>"; }
    else if (c.k === "metasurface") { el.innerHTML = '<div class="note">metasurface<br>(on the laptop)</div>'; }
    else if (c.k === "button") { el.innerHTML = '<div class="btn">' + esc(c.n) + "</div>"; }
    else if (c.k === "xy") { el.innerHTML = '<div class="pad"><div class="dot2"></div></div>'
                                          + '<div class="name">' + esc(c.n) + "</div>"; }
    else if (c.k === "fader") { el.innerHTML = '<div class="fader"><div class="fill"></div></div>'
                                             + '<div class="name">' + esc(c.n) + "</div>"; }
    else { el.innerHTML = '<canvas class="knob"></canvas><div class="name">' + esc(c.n) + "</div>"; }

    if (c.k !== "label" && c.k !== "metasurface") attach(el, c);
    grid.appendChild(el);
  }
  paint();
}

function esc(s) { return (s || "").replace(/[<>&]/g, m => ({ "<":"&lt;", ">":"&gt;", "&":"&amp;" }[m])); }

function attach(el, c) {
  const target = el.firstElementChild;
  let startY = 0, startV = 0;

  const set = (v, vy) => {
    c.v = Math.max(0, Math.min(1, v));
    if (vy !== undefined) c.vy = Math.max(0, Math.min(1, vy));
    paint();
    send({ t: "v", id: c.id, v: c.v, vy: c.vy || 0 });
  };

  target.addEventListener("pointerdown", e => {
    target.setPointerCapture(e.pointerId);
    held[c.id] = true;
    if (c.k === "button") { set(c.mom ? 1 : (c.v > .5 ? 0 : 1)); }
    else if (c.k === "xy") { const r = target.getBoundingClientRect();
                             set((e.clientX - r.left) / r.width, 1 - (e.clientY - r.top) / r.height); }
    else { startY = e.clientY; startV = c.v; }
    e.preventDefault();
  });

  target.addEventListener("pointermove", e => {
    if (!held[c.id]) return;
    if (c.k === "xy") { const r = target.getBoundingClientRect();
                        set((e.clientX - r.left) / r.width, 1 - (e.clientY - r.top) / r.height); }
    else if (c.k !== "button") { set(startV + (startY - e.clientY) / 200); }
    e.preventDefault();
  });

  const release = () => {
    if (!held[c.id]) return;
    held[c.id] = false;
    // A momentary button has to go back to zero on release, or a build stays
    // running because a finger lifted.
    if (c.k === "button" && c.mom) set(0);
  };
  target.addEventListener("pointerup", release);
  target.addEventListener("pointercancel", release);
}

function paint() {
  if (!layout) return;
  for (const c of layout.controls) {
    const el = document.querySelector('.ctl[data-id="' + c.id + '"]');
    if (!el) continue;

    if (c.k === "fader") { el.querySelector(".fill").style.height = (100 * c.v) + "%"; }
    else if (c.k === "button") { el.querySelector(".btn").classList.toggle("on", c.v > .5); }
    else if (c.k === "xy") { const d = el.querySelector(".dot2");
                             d.style.left = (100 * c.v) + "%";
                             d.style.top = (100 * (1 - (c.vy || 0))) + "%"; }
    else if (c.k === "knob") {
      const cv = el.querySelector("canvas");
      const w = cv.clientWidth, h = cv.clientHeight;
      cv.width = w; cv.height = h;
      const g = cv.getContext("2d");
      const r = Math.min(w, h) / 2 - 6, cx = w / 2, cy = h / 2;
      const a0 = Math.PI * 0.75, a1 = a0 + Math.PI * 1.5;
      g.clearRect(0, 0, w, h);
      g.lineWidth = 5; g.lineCap = "round";
      g.strokeStyle = "#232A33"; g.beginPath(); g.arc(cx, cy, r, a0, a1); g.stroke();
      g.strokeStyle = "#3FE0C0"; g.beginPath();
      g.arc(cx, cy, r, a0, a0 + Math.PI * 1.5 * c.v); g.stroke();
    }
  }
}

window.addEventListener("resize", paint);
connect();
</script></body></html>)HTML";

} // namespace

void ControlServer::initialise(Engine* engine, control::Surface* surface) {
    engine_ = engine;
    surface_ = surface;

    server_.onRequest = [](const std::string& path) {
        HttpResponse response;
        if (path == "/" || path == "/index.html") {
            response.body = kPage;
        } else {
            response.status = 404;
            response.contentType = "text/plain";
            response.body = "not here";
        }
        return response;
    };
}

bool ControlServer::start(std::uint16_t port, std::string* error) {
    lastLayout_.clear();
    return server_.start(port, error);
}

void ControlServer::stop() {
    server_.stop();
    lastLayout_.clear();
}

std::vector<std::string> ControlServer::addresses() const {
    std::vector<std::string> out;
    for (const std::string& address : HttpServer::localAddresses())
        out.push_back("http://" + address + ":" + std::to_string(server_.port()));
    return out;
}

std::string ControlServer::layoutJson() const {
    JsonValue root = JsonValue::object();
    root.set("t", "layout");
    if (!surface_) return root.dump(-1);

    root.set("cols", surface_->columns());
    root.set("rows", surface_->rows());
    root.set("page", surface_->activePage());

    JsonValue pages = JsonValue::array();
    for (const control::Page& page : surface_->pages()) pages.push(page.name);
    root.set("pages", std::move(pages));

    JsonValue controls = JsonValue::array();
    if (const control::Page* page = surface_->page(surface_->activePage())) {
        for (const control::Control& control : page->controls) {
            JsonValue entry = JsonValue::object();
            entry.set("id", control.id);
            entry.set("k", control::toString(control.kind));
            entry.set("n", control.name);
            entry.set("x", control.column);
            entry.set("y", control.row);
            entry.set("w", control.width);
            entry.set("h", control.height);
            entry.set("v", control.value);
            entry.set("vy", control.valueY);
            entry.set("mom", control.momentary);
            controls.push(std::move(entry));
        }
    }
    root.set("controls", std::move(controls));

    return root.dump(-1);
}

void ControlServer::serviceFromMessageThread() {
    if (!server_.running() || !surface_ || !engine_) return;

    // -- what the tablets sent ---------------------------------------------
    // Applied here, on the message thread, through the same calls the local
    // controls make. A socket thread writing into the graph would be a second
    // writer with no ordering against the first.
    bool changed = false;

    for (const std::string& message : server_.takeReceived()) {
        std::string parseError;
        const JsonValue value = JsonValue::parse(message, &parseError);
        if (!parseError.empty() || !value.isObject()) continue;

        const std::string type = value.getString("t");

        if (type == "v") {
            const int id = value.getInt("id", 0);
            const auto x = value.getFloat("v", 0.0f);
            const auto y = value.getFloat("vy", 0.0f);

            const control::Control* control = surface_->find(id);
            if (!control) continue;

            if (control->hasSecondAxis()) surface_->setValueXY(id, x, y, engine_->graph());
            else surface_->setValue(id, x, engine_->graph());
            changed = true;
        } else if (type == "page") {
            surface_->setActivePage(value.getInt("i", 0));
            surface_->adoptAllFromGraph(engine_->graph());
            changed = true;
        }
    }

    // -- what the tablets should see ---------------------------------------
    // Only when it differs. A surface published every frame is a few kilobytes
    // sixty times a second to a device that cannot read it that fast.
    std::string layout = layoutJson();
    if (layout != lastLayout_) {
        lastLayout_ = layout;
        // The greeting and the broadcast are the same string: one for whoever
        // is already connected, one for whoever connects next.
        server_.setGreeting(layout);
        server_.broadcast(std::move(layout));
    } else {
        (void)changed;
    }
}

} // namespace acm::net
