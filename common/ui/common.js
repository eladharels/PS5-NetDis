// Shared page framework: helpers, the tab bar, and the Logs and Settings
// tabs every payload has. A page calls App.init() with its own tabs.
"use strict";

const App = (() => {
  const $ = (id) => document.getElementById(id);

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function chip(cls, text) {
    return el("span", "chip " + cls, text);
  }

  function showError(msg) {
    const box = $("error");
    box.textContent = msg || "";
    box.style.display = msg ? "block" : "none";
  }

  async function call(method, url) {
    const res = await fetch(url, { method });
    const body = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(body.error || ("HTTP " + res.status));
    return body;
  }

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (e) {
      // Plain http pages have no clipboard API in most browsers.
      const ta = el("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.opacity = "0";
      document.body.append(ta);
      ta.select();
      const ok = document.execCommand("copy");
      ta.remove();
      return ok;
    }
  }

  function fmtTime(sec) {
    return new Date(sec * 1000).toTimeString().slice(0, 8);
  }

  let opts = null;
  let service = null;
  let tabIds = [];

  // ---- tabs --------------------------------------------------------------

  function showTab(id) {
    if (!tabIds.includes(id)) id = tabIds[0];
    for (const t of tabIds) {
      $("tab-" + t).hidden = t !== id;
      $("tabbtn-" + t).setAttribute("aria-selected", String(t === id));
    }
    if (location.hash !== "#" + id) history.replaceState(null, "", "#" + id);
    if (id === "logs") renderLogs(true);
    if (opts.onTab) opts.onTab(id);
  }

  function buildTabs() {
    const bar = $("tabs");
    bar.setAttribute("role", "tablist");
    const tabs = [...opts.tabs, { id: "logs", label: "Logs" },
      { id: "settings", label: "Settings" }];
    tabIds = tabs.map((t) => t.id);
    for (const t of tabs) {
      const b = el("button", "tab", t.label);
      b.type = "button";
      b.id = "tabbtn-" + t.id;
      b.setAttribute("role", "tab");
      b.setAttribute("aria-controls", "tab-" + t.id);
      if (t.id === "logs") b.append(el("span", "count", ""));
      b.addEventListener("click", () => showTab(t.id));
      bar.append(b);
    }
  }

  // ---- logs tab ----------------------------------------------------------

  const LEVELS = ["debug", "info", "warn", "error"];
  const MAX_LINES = 5000;
  let logs = [];
  let logSince = 0;
  let logPid = 0;
  let logGap = false;
  let logError = "";
  let unseenErrors = 0;

  function buildLogsTab() {
    const sec = el("section");
    sec.id = "tab-logs";
    sec.hidden = true;
    sec.setAttribute("role", "tabpanel");
    sec.innerHTML = `
      <div class="log-tools">
        <select id="loglevel" aria-label="Minimum level">
          <option value="debug">All levels</option>
          <option value="info">Info and above</option>
          <option value="warn">Warnings and errors</option>
          <option value="error">Errors only</option>
        </select>
        <input type="search" id="logsearch" placeholder="Filter lines"
               aria-label="Filter log lines">
        <label><input type="checkbox" id="follow" checked> Follow</label>
        <button id="copylogs" type="button">Copy</button>
        <a id="logdownload" href="#">Download</a>
      </div>
      <div class="log-status" id="logstatus"></div>
      <div class="logbox" id="logbox" role="log" aria-live="off"></div>`;
    document.querySelector("main").append(sec);

    const tools = sec.querySelector(".log-tools");
    for (const t of opts.logTools || []) {
      const b = el("button", "", t.label);
      b.type = "button";
      if (t.title) b.title = t.title;
      b.addEventListener("click", async () => {
        b.disabled = true;
        try {
          await t.onClick();
          await pollLogs();
        } catch (e) {
          logError = t.label + " failed: " + e.message;
          renderLogs(true);
        }
        b.disabled = false;
      });
      tools.insertBefore(b, $("logdownload"));
    }

    $("copylogs").addEventListener("click", async () => {
      const ok = await copyText(visibleLogs().map(lineText).join("\n") + "\n");
      const b = $("copylogs");
      b.textContent = ok ? "Copied" : "Copy failed";
      setTimeout(() => { b.textContent = "Copy"; }, 1500);
    });
    $("loglevel").addEventListener("change", () => renderLogs(true));
    $("logsearch").addEventListener("input", () => renderLogs(false));
    $("follow").addEventListener("change", () => renderLogs(true));
  }

  function lineText(e) {
    return fmtTime(e.time) + " " + e.level.padEnd(5) + " " + e.msg;
  }

  function visibleLogs() {
    const min = LEVELS.indexOf($("loglevel").value);
    const q = $("logsearch").value.trim().toLowerCase();
    return logs.filter((e) => LEVELS.indexOf(e.level) >= min &&
      (!q || e.msg.toLowerCase().includes(q)));
  }

  function renderLogs(force) {
    if ($("tab-logs").hidden && !force) return;
    const box = $("logbox");
    const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
    const shown = visibleLogs();
    const frag = document.createDocumentFragment();
    for (const e of shown) {
      const row = el("div", "ln " + e.level);
      row.append(el("span", "t", fmtTime(e.time) + " "), el("span", "l", e.level),
        " " + e.msg);
      frag.append(row);
    }
    if (!shown.length) frag.append(el("div", "empty", logs.length
      ? "No lines match the filter." : "No log lines yet."));
    box.replaceChildren(frag);
    if ($("follow").checked && (atBottom || force)) box.scrollTop = box.scrollHeight;

    let status = shown.length + " of " + logs.length + " lines";
    if (logGap) status += " · older lines were dropped; Download has what the console still holds";
    if (logError) status += " · " + logError;
    $("logstatus").textContent = status;
    if (!$("tab-logs").hidden) {
      unseenErrors = 0;
      document.querySelector("#tabbtn-logs .count").textContent = "";
    }
  }

  async function pollLogs() {
    try {
      let d = await call("GET", "/api/logs?since=" + logSince);
      if (logPid && d.pid !== logPid) {
        // A newer copy replaced the one this page was reading: start over.
        logs = [];
        logSince = 0;
        logGap = false;
        d = await call("GET", "/api/logs?since=0");
      }
      logPid = d.pid;
      if (d.dropped && logSince) logGap = true;
      logSince = d.next;
      logError = "";
      if (d.entries.length) {
        logs.push(...d.entries);
        if (logs.length > MAX_LINES) {
          logs.splice(0, logs.length - MAX_LINES);
          logGap = true;
        }
        if ($("tab-logs").hidden) {
          unseenErrors += d.entries.filter((e) => e.level === "error").length;
          document.querySelector("#tabbtn-logs .count").textContent =
            unseenErrors ? unseenErrors : "";
        }
      }
      renderLogs(false);
    } catch (e) {
      logError = "can't reach " + opts.name + ": " + e.message;
      renderLogs(false);
    }
  }

  // ---- settings tab ------------------------------------------------------

  function settingRow(s) {
    const row = el("div", "set-row");
    const text = el("div");
    text.append(el("h3", "", s.title));
    const note = el("div", "note", s.note || "");
    text.append(note);
    const btns = el("div", "btns");
    const b = el("button", s.danger ? "danger" : "", s.button);
    b.type = "button";
    b.addEventListener("click", async () => {
      b.disabled = true;
      try {
        await s.onClick(b);
      } catch (e) {
        showError(s.title + " failed: " + e.message);
      }
      b.disabled = false;
    });
    btns.append(b);
    row.append(text, btns);
    return { row, note, button: b };
  }

  let tileRow = null;

  function buildSettingsTab() {
    const name = opts.name;
    const sec = el("section");
    sec.id = "tab-settings";
    sec.hidden = true;
    sec.setAttribute("role", "tabpanel");

    sec.append(el("h2", "", "Home screen"));
    const tileCard = el("div", "card");
    tileRow = settingRow({
      title: name + " tile", button: "…",
      onClick: async () => {
        const on = !service.tile;
        service = await call("POST", "/api/tile?on=" + (on ? "1" : "0"));
        renderSettings();
      },
    });
    tileCard.append(tileRow.row);
    sec.append(tileCard);

    sec.append(el("h2", "", name + " on the console"));
    const card = el("div", "card");
    for (const s of opts.settings || []) card.append(settingRow(s).row);
    card.append(settingRow({
      title: "Stop " + name, button: "Stop", danger: true,
      note: (opts.stopNote ? opts.stopNote + " " : "") + "Send " + name +
        ".elf again to start it; a newer copy replaces a running one.",
      onClick: async () => {
        if (!confirm("Stop " + name + " on the console? Send " + name +
                     ".elf again to start it.")) return;
        await call("POST", "/api/quit");
        showError(name + " is stopped. Send " + name + ".elf again to start it.");
      },
    }).row);
    sec.append(card);

    sec.append(el("h2", "", "About"));
    const about = el("div", "card");
    const dl = el("dl", "info");
    dl.id = "aboutlist";
    about.append(dl);
    about.append(el("p", "hint", "Start " + name + " automatically after each " +
      "jailbreak by adding " + name + ".elf to Payload Manager's autoload list."));
    sec.append(about);

    document.querySelector("main").append(sec);
  }

  function renderSettings() {
    if (!service || !tileRow) return;
    tileRow.note.textContent = service.tile
      ? "On the home screen (Media). It opens this page in the console's browser."
      : "Not installed. Install it to open this page from the home screen.";
    tileRow.button.textContent = service.tile ? "Remove tile" : "Install tile";
    tileRow.button.className = service.tile ? "danger" : "";

    const rows = [
      ["Version", "v" + service.version],
      ["On a phone", service.lanUrl || location.origin + "/"],
      ["Build", service.build],
      ...(opts.about ? opts.about() : []),
      ["Data folder", service.dataDir],
      ["Log file", service.logFile],
    ];
    const dl = $("aboutlist");
    dl.replaceChildren();
    for (const [k, v] of rows) dl.append(el("dt", "", k), el("dd", "", v));
  }

  async function pollService() {
    try {
      service = await call("GET", "/api/service");
      $("logdownload").href = service.logRoute;
      $("logdownload").setAttribute("download", service.name + ".log");
      renderSettings();
    } catch (e) {
      // The page's own refresh reports an unreachable console.
    }
  }

  // ---- init --------------------------------------------------------------

  function init(o) {
    opts = o;
    buildTabs();
    buildLogsTab();
    buildSettingsTab();
    pollLogs();
    pollService();
    setInterval(pollLogs, 2000);
    setInterval(pollService, 10000);
    showTab(location.hash.slice(1) || tabIds[0]);
  }

  return { $, el, chip, call, showError, copyText, fmtTime, init, showTab,
           renderSettings };
})();
