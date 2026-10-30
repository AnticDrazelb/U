// The web server's one page (web_server.cpp): keys, firmware and games uploaded with
// XMLHttpRequest PUTs (each file streamed as it is read, with progress), the logs to download.
// Included inside an anonymous namespace.
constexpr std::string_view kWebPage = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Eden for PS4</title>
<style>
  :root { --bg: #2d2d2d; --card: #3a3a3a; --text: #f2f2f2; --dim: #b4b4b4; --accent: #18c8e8;
          --good: #3cc864; --bad: #e8463c; --line: #4c4c4c; }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text);
         font: 16px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
  main { max-width: 760px; margin: 0 auto; padding: 20px 16px 48px; }
  h1 { font-size: 26px; margin: 4px 0 2px; font-weight: 600; }
  .sub { color: var(--dim); margin: 0 0 18px; }
  .chips { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 18px; }
  .chip { padding: 4px 12px; border-radius: 999px; background: var(--card); font-size: 14px; }
  .chip.ok::before { content: "\2713  "; color: var(--good); font-weight: 700; }
  .chip.no::before { content: "\2717  "; color: var(--bad); font-weight: 700; }
  section { background: var(--card); border-radius: 12px; padding: 16px 18px; margin-bottom: 14px; }
  h2 { font-size: 19px; margin: 0 0 6px; font-weight: 600; }
  p { margin: 4px 0 10px; color: var(--dim); }
  label.button, button { display: inline-block; background: var(--accent); color: #062a33; border: 0;
         border-radius: 8px; padding: 9px 16px; font: inherit; font-weight: 600; cursor: pointer; margin: 4px 6px 4px 0; }
  button.quiet { background: transparent; color: var(--bad); padding: 2px 8px; margin: 0; font-weight: 500; }
  input[type=file] { display: none; }
  .bar { height: 8px; border-radius: 4px; background: var(--line); overflow: hidden; margin-top: 10px; display: none; }
  .bar > div { height: 100%; width: 0; background: var(--accent); transition: width .2s; }
  .msg { margin-top: 8px; font-size: 14px; color: var(--dim); white-space: pre-wrap; }
  .msg.err { color: var(--bad); }
  ul { list-style: none; margin: 6px 0 0; padding: 0; }
  li { display: flex; justify-content: space-between; align-items: center; padding: 6px 0;
       border-top: 1px solid var(--line); font-size: 15px; gap: 10px; }
  li span { overflow-wrap: anywhere; }
  a { color: var(--accent); }
  .size { color: var(--dim); white-space: nowrap; }
</style>
</head>
<body>
<main>
  <h1>Eden for PS4</h1>
  <p class="sub" id="version">Connecting to the PS4&hellip;</p>
  <div class="chips" id="chips"></div>

  <section>
    <h2>Keys</h2>
    <p>prod.keys (and title.keys) from the Switch your games were dumped from.</p>
    <label class="button">Choose keys<input type="file" id="keys" multiple accept=".keys"></label>
    <div class="bar" id="keys-bar"><div></div></div><div class="msg" id="keys-msg"></div>
  </section>

  <section>
    <h2>Firmware</h2>
    <p>That Switch's firmware: a folder of .nca files. It replaces the firmware on the PS4.</p>
    <label class="button">Choose the folder<input type="file" id="fw-folder" webkitdirectory directory multiple></label>
    <label class="button">or its .nca files<input type="file" id="fw-files" multiple accept=".nca"></label>
    <div class="bar" id="fw-bar"><div></div></div><div class="msg" id="fw-msg"></div>
  </section>

  <section>
    <h2>Games</h2>
    <p>.nsp, .xci or .nro files, into /data/eden-ps4 (they appear in the HOME menu).</p>
    <label class="button">Choose games<input type="file" id="games" multiple accept=".nsp,.xci,.nro,.nso"></label>
    <div class="bar" id="games-bar"><div></div></div><div class="msg" id="games-msg"></div>
    <ul id="game-list"></ul>
  </section>

  <section>
    <h2>Logs</h2>
    <p>Everything the app recorded, in one file to send when something goes wrong.</p>
    <a class="button" href="/api/logs" download style="text-decoration:none"><label class="button" style="margin:0">Download all logs</label></a>
    <a href="/api/profile" target="_blank" style="margin-left:10px">Profile summary</a>
    <ul id="log-list"></ul>
  </section>
</main>
<script>
const $ = (id) => document.getElementById(id);
const size = (n) => n >= 1e9 ? (n / 1e9).toFixed(2) + " GB" : n >= 1e6 ? (n / 1e6).toFixed(1) + " MB"
                  : n >= 1e3 ? (n / 1e3).toFixed(0) + " KB" : n + " bytes";
let busy = false;

async function refresh() {
  try {
    const s = await (await fetch("/api/status", {cache: "no-store"})).json();
    $("version").textContent = "Version " + s.version + (s.renderer ? " · " + s.renderer : "");
    const chip = (ok, text) => `<span class="chip ${ok ? "ok" : "no"}">${text}</span>`;
    $("chips").innerHTML = chip(s.keys, "prod.keys") + chip(s.title_keys, "title.keys") +
      chip(s.firmware > 0, s.firmware ? s.firmware + " firmware files" : "no firmware") +
      chip(s.games.length > 0, s.games.length === 1 ? "1 game here" : s.games.length + " games here");
    $("game-list").innerHTML = s.games.map((g) =>
      `<li><span>${esc(g.name)}</span><span class="size">${size(g.size)}
       <button class="quiet" onclick="removeGame('${encodeURIComponent(g.name)}')">Remove</button></span></li>`).join("");
    $("log-list").innerHTML = s.logs.map((l) =>
      `<li><a href="/files/${encodeURI(l.name)}" target="_blank">${esc(l.name)}</a><span class="size">${size(l.size)}</span></li>`).join("");
  } catch (e) {
    $("version").textContent = "The PS4 doesn't answer: is Eden for PS4 open?";
  }
}
function esc(t) { return t.replace(/[&<>"']/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c])); }

function put(kind, file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("PUT", "/upload/" + kind + "/" + encodeURIComponent(file.name));
    xhr.upload.onprogress = (e) => onProgress(e.loaded);
    xhr.onload = () => xhr.status === 200 ? resolve() : reject(new Error(xhr.responseText.trim() || ("HTTP " + xhr.status)));
    xhr.onerror = () => reject(new Error("the connection to the PS4 broke"));
    xhr.send(file);
  });
}

async function uploadAll(prefix, kind, files) {
  if (busy) { alert("Another upload is still running."); return false; }
  busy = true;
  const bar = $(prefix + "-bar"), fill = bar.firstElementChild, msg = $(prefix + "-msg");
  const total = files.reduce((n, f) => n + f.size, 0);
  let done = 0;
  const start = Date.now();
  bar.style.display = "block"; msg.className = "msg";
  try {
    for (const [i, f] of files.entries()) {
      await put(kind, f, (loaded) => {
        const now = done + loaded, seconds = (Date.now() - start) / 1000;
        fill.style.width = (100 * now / Math.max(total, 1)) + "%";
        msg.textContent = `${files.length > 1 ? `File ${i + 1} of ${files.length}: ` : ""}${f.name}\n` +
          `${size(now)} of ${size(total)}` + (seconds > 2 ? ` · ${size(now / seconds)}/s` : "");
      });
      done += f.size;
    }
    fill.style.width = "100%";
    msg.textContent = files.length === 1 ? `${files[0].name} is on the PS4.` : `All ${files.length} files are on the PS4.`;
    return true;
  } catch (e) {
    msg.className = "msg err"; msg.textContent = "Stopped: " + e.message;
    return false;
  } finally {
    busy = false;
    refresh();
  }
}

$("keys").onchange = (e) => {
  const files = [...e.target.files].filter((f) => f.name.toLowerCase().endsWith(".keys"));
  if (files.length) uploadAll("keys", "keys", files); else $("keys-msg").textContent = "Those aren't .keys files.";
  e.target.value = "";
};
async function firmware(e) {
  const files = [...e.target.files].filter((f) => f.name.toLowerCase().endsWith(".nca"));
  e.target.value = "";
  if (!files.length) { $("fw-msg").textContent = "No .nca files there."; return; }
  if (!confirm(`Replace the firmware on the PS4 with these ${files.length} files?`)) return;
  const r = await fetch("/api/firmware/clear", {method: "POST"});
  if (!r.ok) { $("fw-msg").textContent = "The PS4 couldn't clear its old firmware."; return; }
  uploadAll("fw", "firmware", files);
}
$("fw-folder").onchange = firmware;
$("fw-files").onchange = firmware;
$("games").onchange = (e) => {
  const files = [...e.target.files].filter((f) => /\.(nsp|xci|nro|nso)$/i.test(f.name));
  if (files.length) uploadAll("games", "games", files); else $("games-msg").textContent = "Those aren't games (.nsp, .xci, .nro).";
  e.target.value = "";
};
async function removeGame(name) {
  if (!confirm("Remove " + decodeURIComponent(name) + " from the PS4?")) return;
  await fetch("/files/" + name, {method: "DELETE"});
  refresh();
}
refresh();
setInterval(() => { if (!busy) refresh(); }, 5000);
</script>
</body>
</html>
)PAGE";
