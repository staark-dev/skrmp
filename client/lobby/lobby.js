// SKR Lobby — Phase 1 (display + navigation only).
// Communicates with SKR.Client.dll via samp-cef events:
//   emit "skr:lobby:settings:get"    (void)              -> expect "skr:lobby:settings:value"
//   emit "skr:lobby:settings:save"   (JSON string)       -> expect "skr:lobby:settings:saved" | ":error"
//   emit "skr:lobby:quit"            (void)              -> C++ closes the GTA process
//   emit "skr:lobby:play"            (JSON {address?})   -> no-op in Phase 1 (toast)
// Browser is created by SKR.Client.dll pointing at this file.
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var qsa = function (sel) { return Array.prototype.slice.call(document.querySelectorAll(sel)); };

  var defaults = {
    nickname: 'Jucător',
    gtaPath: '',
    resolution: 'auto',
    windowed: false,
    intro: true,
    volume: 80
  };
  var settings = JSON.parse(JSON.stringify(defaults));

  // Phase 1 server list is a static placeholder (ping/players shown as "—").
  // Phase 2 will populate this from the launcher's known-servers table.
  var serverStubs = [
    { name: 'SKR Roleplay — Public',    addr: '127.0.0.1:7777' },
    { name: 'SKR Roleplay — Dev',       addr: '127.0.0.1:7778' }
  ];

  // ── Navigation ────────────────────────────────────────────────────
  function showPage(name) {
    qsa('.nav__item').forEach(function (b) {
      b.classList.toggle('is-active', b.dataset.page === name);
    });
    qsa('.page').forEach(function (p) {
      p.classList.toggle('is-active', p.dataset.page === name);
    });
  }
  qsa('.nav__item').forEach(function (b) {
    b.addEventListener('click', function () { showPage(b.dataset.page); });
  });

  // ── Toast ─────────────────────────────────────────────────────────
  var toastTimer = null;
  function toast(msg, ms) {
    var el = $('toast');
    el.textContent = msg;
    el.classList.add('is-visible');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.classList.remove('is-visible'); }, ms || 2600);
  }

  // ── Rendering ─────────────────────────────────────────────────────
  function renderNickname() {
    var n = (settings.nickname || 'Jucător').trim() || 'Jucător';
    $('homeNick').textContent = n;
    $('homeNickValue').textContent = n;
  }
  function renderSelectedServer() {
    $('homeServer').innerHTML = 'Server: <b>— nu este selectat —</b>';
  }
  function renderSettings() {
    $('setNick').value       = settings.nickname || '';
    $('setGtaPath').value    = settings.gtaPath || '(necunoscută)';
    $('setResolution').value = settings.resolution || 'auto';
    $('setWindowed').checked = !!settings.windowed;
    $('setIntro').checked    = settings.intro !== false;
    var v = typeof settings.volume === 'number' ? settings.volume : 80;
    $('setVolume').value = v;
    $('volVal').textContent = v + '%';
  }
  function renderServers() {
    var list = $('serverList');
    list.innerHTML = '';
    serverStubs.forEach(function (s) {
      var li = document.createElement('li');
      li.className = 'server';
      var info = document.createElement('div');
      var nm = document.createElement('div');
      nm.className = 'server__name';
      nm.textContent = s.name;
      var ad = document.createElement('div');
      ad.className = 'server__addr';
      ad.textContent = s.addr;
      info.appendChild(nm); info.appendChild(ad);
      var ping = document.createElement('div');
      ping.className = 'server__ping';
      ping.textContent = '—';
      var players = document.createElement('div');
      players.className = 'server__players';
      players.textContent = '— / —';
      var btn = document.createElement('button');
      btn.className = 'server__join';
      btn.textContent = 'Conectează';
      btn.addEventListener('click', function () { emitPlay(s.addr); });
      li.appendChild(info); li.appendChild(ping); li.appendChild(players); li.appendChild(btn);
      list.appendChild(li);
    });
  }

  // ── CEF bridge wrappers ──────────────────────────────────────────
  function cefEmit(name, payload) {
    if (typeof cef === 'undefined' || !cef.emit) {
      // Dev/browser preview: no CEF — log so the page is still usable standalone.
      console.log('[lobby] emit', name, payload);
      return false;
    }
    try {
      if (payload === undefined) cef.emit(name);
      else cef.emit(name, payload);
      return true;
    } catch (e) {
      console.warn('[lobby] emit failed', name, e);
      return false;
    }
  }
  function cefOn(name, handler) {
    if (typeof cef === 'undefined' || !cef.on) return;
    try { cef.on(name, handler); } catch (e) { console.warn('[lobby] on failed', name, e); }
  }

  // ── Settings load/save ───────────────────────────────────────────
  function applyLoaded(json) {
    try {
      var incoming = typeof json === 'string' ? JSON.parse(json) : (json || {});
      Object.keys(defaults).forEach(function (k) {
        if (incoming[k] !== undefined && incoming[k] !== null) settings[k] = incoming[k];
      });
    } catch (e) { /* keep defaults */ }
    renderNickname();
    renderSelectedServer();
    renderSettings();
  }
  function requestLoad() {
    // Ask C++ to refresh settings.json next to the page, then fetch it.
    cefEmit('skr:lobby:settings:get');
    fetch('settings.json', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.text() : '{}'; })
      .then(function (txt) { applyLoaded(txt); })
      .catch(function () { applyLoaded(defaults); });
  }
  function collectForm() {
    var nick = ($('setNick').value || '').trim().slice(0, 24);
    var vol  = parseInt($('setVolume').value, 10);
    if (!isFinite(vol)) vol = 80;
    vol = Math.max(0, Math.min(100, vol));
    return {
      nickname:   nick || 'Jucător',
      gtaPath:    settings.gtaPath || '',
      resolution: $('setResolution').value || 'auto',
      windowed:   !!$('setWindowed').checked,
      intro:      !!$('setIntro').checked,
      volume:     vol
    };
  }
  function saveSettings() {
    // Phase 1: persistence happens through the WPF launcher (apps/launcher-ui
    // writes launcher.ini). The in-game lobby only displays what is on disk;
    // in-place save lands in Phase 2 once the samp-cef bridge exposes list
    // readers so the C++ side can read our JSON payload.
    var out = collectForm();
    settings = out;
    renderNickname();
    renderSettings();
    cefEmit('skr:lobby:settings:save', JSON.stringify(out));
    var s = $('setStatus');
    s.classList.remove('is-error');
    s.textContent = 'Phase 1: setările se editează din launcherul Windows.';
    toast('Phase 1: salvarea in-game vine în Phase 2.');
    setTimeout(function () { s.textContent = ''; }, 3500);
  }

  // ── Play / Quit ──────────────────────────────────────────────────
  function emitPlay(addr) {
    cefEmit('skr:lobby:play', JSON.stringify({ address: addr || '' }));
    toast('Phase 2 (connect) va fi implementată separat.');
  }
  function quit() { cefEmit('skr:lobby:quit'); }

  // ── Wire up ──────────────────────────────────────────────────────
  $('playBtn').addEventListener('click', function () { emitPlay(''); });
  $('quitBtn').addEventListener('click', quit);
  $('saveBtn').addEventListener('click', saveSettings);
  $('reloadBtn').addEventListener('click', requestLoad);
  $('setVolume').addEventListener('input', function () {
    $('volVal').textContent = $('setVolume').value + '%';
  });

  document.addEventListener('keydown', function (ev) {
    if (ev.key === 'Escape') { ev.preventDefault(); quit(); }
  });

  cefOn('skr:lobby:settings:ready', function () {
    // C++ wrote a fresh settings.json; re-fetch it.
    fetch('settings.json', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.text() : '{}'; })
      .then(function (txt) { applyLoaded(txt); })
      .catch(function () {});
  });

  renderServers();
  renderSettings();
  renderNickname();
  renderSelectedServer();
  requestLoad();
})();
