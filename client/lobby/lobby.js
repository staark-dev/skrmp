// SKR Lobby — Phase 1. Ported from launcher-ui/ingame-menu/menu.js.
// Mode handling:
//   - "lobby" (pre-connect): nav = Servere / Setări / Ieșire, landing = Servere.
//   - "pause" (in-game):     nav = Continuă / Hartă / Setări / Deconectare / Ieșire.
// Phase 1 defaults to "lobby" because the native adapter has no pause coupling
// yet. Phase 2 flips to "pause" from C++ via `window.skrLobby.setMode('pause')`.
//
// CEF bridge contract (unchanged from previous Phase 1):
//   emit "skr:lobby:settings:get"            -> C++ writes settings.json next to us
//   emit "skr:lobby:settings:save" (flat JSON) -> local bridge persists preferences
//   emit "skr:lobby:quit"                    -> C++ closes GTA
//   emit "skr:lobby:servers:request"         -> request public server directory
//   on   "skr:lobby:servers"    (JSON array) -> render public server directory
//   emit "skr:lobby:connect"    (JSON{serverId,nickname}) -> connect through native adapter
//   emit "skr:lobby:disconnect"              -> stub (Phase 2)
//   emit "skr:lobby:resume"                  -> stub (Phase 2 pause coupling)
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };

  // ── Mode ────────────────────────────────────────────────────────
  var mode = 'lobby';   // "lobby" | "pause"
  function navItems() {
    return mode === 'pause'
      ? [['resume','Resume',false],['map','Map',false],['settings','Settings',false],['disconnect','Disconnect',true],['quit','Exit game',true]]
      : [['home','Discover',false],['servers','Servers',false]];
  }

  // ── State ───────────────────────────────────────────────────────
  // Nickname / GTA path / intro live in the WPF launcher's launcher.ini —
  // deliberately not exposed in this in-game Settings panel (see brief).
  var state = {
    graphics:  { resolution:'1920 × 1080', screen:'Fullscreen', distance:70, effects:'Ridicată', antialias:'2×' },
    audio:     { effects:80, radio:50, volume:80 },
    controls:  { sensitivity:50, invert:false },
    interface: { hudScale:100, chat:true, radar:true }
  };
  var saved = JSON.parse(JSON.stringify(state));
  var currentTab = 'graphics';
  var currentPage = '';
  var pendingAction = null;
  var previousFocus = null;
  var toastTimer;
  var pendingSettings = null;
  var settingsTimer;
  var nicknameTimer;
  var publicServers = [];
  var serverFilter = 'all';
  var favoriteServers = Object.create(null);
  var recentServers = [];
  var pendingConnectionServer = null;
  var previousRecentEntry = null;
  var detailsId = null, detailsOrigin = 'home', detailsFocus = null;
  var featuredSelectionId = null; // One public server per directory refresh, stable between page changes.
  // Launcher-only UI preferences. No change to native event contracts.
  function readLocal(key, fallback) {
    try { var data = localStorage.getItem(key); return data ? JSON.parse(data) : fallback; }
    catch(e) { return fallback; }
  }
  function writeLocal(key, value) {
    try { localStorage.setItem(key, JSON.stringify(value)); } catch(e) {}
  }
  favoriteServers = readLocal('skr.launcher.favorites.v3', Object.create(null)) || Object.create(null);
  recentServers = readLocal('skr.launcher.recent.v3', []) || [];
  if (!Array.isArray(recentServers)) recentServers = [];
  // A launch request is not proof of joining a server. Older entries lacking
  // status are retained as successful sessions from the legacy schema.
  recentServers = recentServers.filter(function (entry) {
    return entry && typeof entry.address === 'string' && /^[^:\s]+:\d{1,5}$/.test(entry.address);
  }).slice(0, 20);

  var personalServers = [];      // [{address, name}] as persisted by C++
  var pendingAddAddress = '';
  var pendingRemoveAddress = '';
  var addServerFocus = null;
  var connectPending = false;
  var MAX_PERSONAL_SERVERS = 50; // mirrors kMaxPersonalServers (settings_io.h)

  // Demo data is ONLY used when opening lobby.html outside SKRLauncher.
  // Native mode always uses the public directory provided by C++.
  var previewServers = [
    { id:'sample-freeroam', name:'Freeroam Sandbox · Example', shortName:'FR', mode:'Freeroam', language:'EN', players:0, maxPlayers:200, ping:null, recommended:true, address:'127.0.0.1:7777', description:'Sample listing for browser layout testing. Not a live server.' },
    { id:'sample-racing', name:'Racing Arena · Example', shortName:'RC', mode:'Racing', language:'EN', players:0, maxPlayers:128, ping:null, address:'127.0.0.1:7778', description:'Sample listing for browser layout testing. Not a live server.' },
    { id:'sample-roleplay', name:'Los Santos Stories · Example', shortName:'LS', mode:'Roleplay', language:'EN', players:0, maxPlayers:300, ping:null, address:'127.0.0.1:7779', description:'Sample listing for browser layout testing. Not a live server.' }
  ];

  // Directory-wide counters come from the native public directory only.
  // Personal entries and history are excluded to avoid counting the same server twice.
  // Player totals are reported only where population data is available; no estimates.
  function compactCount(value) {
    return Number(value).toLocaleString('en-US');
  }
  function updateDirectoryMetrics() {
    var playersEl = $('headerPlayersOnline');
    var serversEl = $('headerServersOnline');
    if (!playersEl || !serversEl) return;
    if (!directoryReceived) {
      playersEl.textContent = '—';
      serversEl.textContent = '—';
      playersEl.title = 'Waiting for directory data';
      return;
    }
    serversEl.textContent = compactCount(publicServers.length);
    var reported = 0;
    var sum = 0;
    var positives = 0;
    publicServers.forEach(function (server) {
      var live = server.address && liveInfo[server.address];
      var count = live && live.online && live.players != null ? live.players : server.players;
      if (count != null && Number.isFinite(Number(count)) && Number(count) >= 0) {
        reported++;
        sum += Math.floor(Number(count));
        if (count > 0) positives++;
      }
    });
    // Some directories supply a placeholder 0 for every entry before querying.
    // Treat a large all-zero directory as unknown rather than claiming 0 online.
    if (!reported || (publicServers.length > 25 && !positives)) {
      playersEl.textContent = '—';
      playersEl.title = 'Player population is not available from the directory yet';
    } else if (reported < publicServers.length) {
      playersEl.textContent = compactCount(sum) + '+';
      playersEl.title = 'Known online players from ' + reported + ' of ' + publicServers.length + ' servers';
    } else {
      playersEl.textContent = compactCount(sum);
      playersEl.title = 'Reported online players in the public server directory';
    }
  }
  var directoryReceived = false;

  function nicknameValue() { return $('nickname').value.trim(); }
  function nicknameIsValid(value) { return /^[A-Za-z0-9_]{3,24}$/.test(value); }
  function setProfileOpen(open) {
    $('profilePopover').hidden = !open;
    $('profileToggle').setAttribute('aria-expanded', String(open));
    if (open) setTimeout(function () { $('nickname').focus(); }, 0);
  }
  function updateNicknameState(showError) {
    var value = nicknameValue();
    var valid = nicknameIsValid(value);
    $('nickname').classList.toggle('invalid', !!value && !valid);
    $('nicknameValid').hidden = !valid;
    document.querySelectorAll('[data-action="connect"]').forEach(function (button) {
      button.disabled = !valid || connectPending || button.dataset.connectable !== 'true';
    });
    $('saveNickname').disabled = !valid;
    $('playerStatus').classList.toggle('valid', valid);
    $('playerName').textContent = valid ? value : 'Set nickname';
    $('playerAvatar').textContent = valid ? value.charAt(0).toUpperCase() : '?';
    if ($('featuredConnect')) updateHome();
    $('nicknameError').textContent = showError && !valid
      ? (value ? 'Invalid nickname.' : 'Enter a nickname before connecting.')
      : '';
    return valid;
  }

  var config = {
    graphics:{title:'Graphics',eyebrow:'DISPLAY & PERFORMANCE',fields:[
      ['resolution','Resolution','Applies on the next game launch.','select',['Automat','1280 × 720','1366 × 768','1600 × 900','1920 × 1080','2560 × 1440']],
      ['screen','Display mode','Display changes require confirmation in-game.','select',['Fullscreen','Fereastră','Fereastră fără margini']],
      ['distance','Draw distance','How far distant objects are rendered.','range'],
      ['effects','Effects quality','Visual effects detail level.','select',['Scăzută','Medie','Ridicată','Foarte ridicată']],
      ['antialias','Anti-aliasing','Edge smoothing.','select',['Dezactivat','2×','4×','8×']]
    ]},
    audio:{title:'Audio',eyebrow:'SOUND & AMBIENCE',fields:[
      ['effects','Sound effects','Vehicles, footsteps and world sounds.','range'],
      ['radio','Radio','In-game radio volume.','range'],
      ['volume','SKR audio','Saved preference; in-game audio support is in progress.','range']
    ]},
    controls:{title:'Controls',eyebrow:'KEYBOARD & MOUSE',fields:[
      ['sensitivity','Mouse sensitivity','Camera movement speed.','range'],
      ['invert','Invert vertical axis','Invert vertical mouse movement.','toggle'],
      ['bindings','Key bindings','Native key binding editor is not connected.','binding']
    ]},
    interface:{title:'Interface',eyebrow:'HUD & PREFERENCES',fields:[
      ['hudScale','HUD scale','Size of the HUD elements.','scale'],
      ['chat','Chatbox','Show chat messages.','toggle'],
      ['radar','Radar','Show the radar minimap.','toggle']
    ]}
  };

  // ── CEF bridge wrappers ─────────────────────────────────────────
  // Reuse the CEF event contract with a string-only WebView2 transport.
  if (window.chrome && window.chrome.webview) {
    var nativeListeners = Object.create(null);
    window.cef = {
      emit: function (name, payload) {
        window.chrome.webview.postMessage(name + '\n' + (payload === undefined ? '' : payload));
      },
      on: function (name, handler) {
        (nativeListeners[name] || (nativeListeners[name] = [])).push(handler);
      }
    };
    window.chrome.webview.addEventListener('message', function (event) {
      if (typeof event.data !== 'string') return;
      var split = event.data.indexOf('\n');
      if (split < 0) return;
      (nativeListeners[event.data.slice(0, split)] || []).forEach(function (handler) {
        handler(event.data.slice(split + 1));
      });
    });
  }
  function cefEmit(name, payload) {
    if (typeof cef === 'undefined' || !cef.emit) { console.log('[lobby] emit', name, payload); return false; }
    try { if (payload === undefined) cef.emit(name); else cef.emit(name, payload); return true; }
    catch (e) { console.warn('[lobby] emit failed', name, e); return false; }
  }
  function cefOn(name, handler) {
    if (typeof cef === 'undefined' || !cef.on) return;
    try { cef.on(name, handler); } catch (e) {}
  }

  // ── Dirty state + toast ─────────────────────────────────────────
  function dirty() { return JSON.stringify(state) !== JSON.stringify(saved); }
  function updateDirty() {
    var changed = dirty();
    $('changeStatus').textContent = changed ? 'Unsaved changes' : 'No pending changes';
    $('changeStatus').classList.toggle('dirty', changed);
    $('applyChanges').disabled = !changed || !!pendingSettings;
  }
  function toast(text) {
    $('toast').textContent = text;
    $('toast').hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { $('toast').hidden = true; }, 3600);
  }

  function showConnection(message, detail) {
    $('connectionTitle').textContent = message;
    $('connectionText').textContent = detail;
    $('connectionModal').hidden = false;
  }

  var connectionStepOrder = ['launch', 'world', 'server', 'download', 'join'];

  function formatMb(bytes) {
    return (Math.max(0, Number(bytes) || 0) / 1048576).toFixed(1);
  }
  function setConnectionStep(step, allDone) {
    var reached = connectionStepOrder.indexOf(step);
    var items = $('connectionSteps').querySelectorAll('li');
    for (var i = 0; i < items.length; i++) {
      var index = connectionStepOrder.indexOf(items[i].dataset.step);
      items[i].classList.toggle('done', allDone || index < reached);
      items[i].classList.toggle('active', !allDone && index === reached);
    }
  }
  function setConnectionServer(server) {
    $('connectionServer').textContent = server.name || server.address;
    $('connectionAddress').textContent = server.address || '';
  }

  function hideConnection() {
    $('connectionModal').hidden = true;
  }

  function normalized(value) {
    return String(value || '').toLocaleLowerCase('ro-RO');
  }

  function html(value) {
    return String(value == null ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  // ── Personal servers ────────────────────────────────────────────
  // Same rules as NormalizeServerAddress (settings_io.cpp): IPv4 or DNS name,
  // port 1..65535, no IPv6. Returns the canonical "host:port" or ''.
  function normalizeAddress(value) {
    var m = /^([^:\s]+):(\d{1,5})$/.exec(String(value || '').trim());
    if (!m) return '';
    var host = m[1].toLowerCase();
    var port = Number(m[2]);
    if (port < 1 || port > 65535 || host.length > 253) return '';
    var labels = host.split('.');
    var ok = labels.every(function (label) {
      return /^[a-z0-9-]{1,63}$/.test(label) && label.charAt(0) !== '-' && label.charAt(label.length - 1) !== '-';
    });
    if (!ok) return '';
    if (/^[0-9.]+$/.test(host)) {
      if (labels.length !== 4) return '';
      if (!labels.every(function (o) { return o.length <= 3 && Number(o) <= 255; })) return '';
    }
    return host + ':' + port;
  }

  // Live query results from the native client, keyed by "host:port".
  var liveInfo = Object.create(null);
  var pingSamples = Object.create(null);  // {address: {ping, time}} — 'i' queries only
  var pingRequestedAt = Object.create(null);
  var visiblePingAddresses = new Set();
  var visiblePingObserver = null;
  var pingRequestTimer = 0;
  var PING_RETRY_MS = 45000;
  var detailsQueryAt = Object.create(null);
  var DETAILS_QUERY_TTL = 60000; // Avoid repeated UDP queries on page navigation.

  function queryDetailsOnce(address) {
    var endpoint = normalizeAddress(address);
    if (!endpoint || typeof cef === 'undefined' || !cef.emit) return;
    var now = Date.now();
    if (detailsQueryAt[endpoint] && now - detailsQueryAt[endpoint] < DETAILS_QUERY_TTL) return;
    if (cefEmit('skr:lobby:servers:query', endpoint)) detailsQueryAt[endpoint] = now;
  }

  // Remote artwork is display-only. Never derive a URL from a game endpoint.
  // Explicit HTTPS image URLs from public directory metadata only.
  function safeImageUrl(value) {
    if (typeof value !== 'string' || value.length > 2048) return '';
    try {
      var u = new URL(value.trim());
      if (u.protocol !== 'https:' || !u.hostname || u.username || u.password || u.port) return '';
      if (u.hostname === 'localhost' || u.hostname.endsWith('.localhost') || u.hostname.endsWith('.local')) return '';
      if (/^(127\.|10\.|192\.168\.|169\.254\.|0\.)/.test(u.hostname)) return '';
      return u.href;
    } catch (e) { return ''; }
  }

  function safeWebsiteUrl(value) {
    if (typeof value !== 'string' || value.length > 2048) return '';
    var raw = value.trim();
    // Legacy `weburl` rules often contain a bare domain (e.g. mrp.mk).
    // Normalize only unmistakable domain names; never accept arbitrary schemes.
    if (/^[a-z0-9-]+(?:\.[a-z0-9-]+)+(?::[0-9]{1,5})?(?:\/[A-Za-z0-9_/?#&=.%+~:-]*)?$/i.test(raw)) raw = 'https://' + raw;
    try {
      var u = new URL(raw);
      if (!/^https?:$/.test(u.protocol) || !u.hostname || u.username || u.password) return '';
      return u.href;
    } catch (e) { return ''; }
  }

  function artworkFor(server) {
    if (!server) return { banner: '', logo: '' };
    return {
      banner: safeImageUrl(server.bannerDark) || safeImageUrl(server.bannerLight) || safeImageUrl(server.banner),
      logo: safeImageUrl(server.logo)
    };
  }

  // Safe DOM image with initials behind it. A missing/broken image never
  // replaces the fallback or shifts row layout.
  function fillServerMark(el, initials, url, imageClass) {
    el.textContent = initials || 'MP';
    var checked = safeImageUrl(url);
    if (!checked) return;
    var image = document.createElement('img');
    image.className = imageClass || 'server-artwork__img';
    image.alt = '';
    image.loading = 'lazy';
    image.decoding = 'async';
    image.referrerPolicy = 'no-referrer';
    image.addEventListener('error', function () { image.remove(); }, {once:true});
    image.src = checked;
    el.appendChild(image);
  }

  function fillWideBanner(el, url) {
    el.textContent = '';
    var checked = safeImageUrl(url);
    el.hidden = !checked;
    if (!checked) return;
    var image = document.createElement('img');
    image.alt = '';
    image.loading = 'lazy';
    image.decoding = 'async';
    image.referrerPolicy = 'no-referrer';
    image.addEventListener('error', function () { image.remove(); el.hidden = true; }, {once:true});
    image.src = checked;
    el.appendChild(image);
  }

  function personalAsServer(entry) {
    var info = liveInfo[entry.address];
    var online = !!(info && info.online);
    var name = entry.name || (online && info.name) || entry.address;
    var initials = name.replace(/[^A-Za-z0-9]/g, '').slice(0, 3);
    return {
      id: 'personal:' + entry.address,
      name: name,
      shortName: (initials || 'IP').toUpperCase(),
      mode: online ? (info.mode || '—') : '—',
      language: online ? (info.language || '') : '',
      players: online ? info.players : null,
      maxPlayers: online ? info.maxPlayers : null,
      ping: online ? info.ping : null,
      offline: !!info && !online,
      locked: online && info.password,
      skr: online ? info.skr : null,
      recommended: false,
      personal: true,
      address: entry.address,
      description: entry.address,
      map: online ? (info.map || '') : '',
      version: online ? (info.version || '') : '',
      website: online ? (info.website || '') : '',
      discord: online ? (info.discord || '') : '',
      banner: online ? (info.banner || '') : '',
      bannerDark: online ? (info.bannerDark || '') : '',
      bannerLight: online ? (info.bannerLight || '') : '',
      logo: online ? (info.logo || '') : ''
    };
  }

  function applyLiveInfo(server) {
    var info = server.address && liveInfo[server.address];
    var sample = server.address && pingSamples[server.address];
    if (!info) {
      if (sample && Date.now() - sample.time < PING_RETRY_MS) server.ping = sample.ping;
      return server;
    }
    if (!info.online) { server.offline = true; return server; }
    server.offline = false;
    server.players = info.players;
    server.maxPlayers = info.maxPlayers;
    server.ping = sample && Date.now() - sample.time < PING_RETRY_MS ? sample.ping : info.ping;
    server.locked = info.password;
    server.skr = info.skr;
    if (info.map) server.map = info.map;
    if (info.version) server.version = info.version;
    if (info.website) server.website = info.website;
    if (info.discord) server.discord = info.discord;
    if (info.banner) server.banner = info.banner;
    if (info.bannerDark) server.bannerDark = info.bannerDark;
    if (info.bannerLight) server.bannerLight = info.bannerLight;
    if (info.logo) server.logo = info.logo;
    return server;
  }

  function loadServerInfo(json) {
    try {
      var data = typeof json === 'string' ? JSON.parse(json) : json;
      (Array.isArray(data) ? data : []).forEach(function (s) {
        if (!s || typeof s.address !== 'string') return;
        liveInfo[s.address] = {
          online: !!s.online,
          password: !!s.password,
          players: Math.max(0, Number(s.players) || 0),
          maxPlayers: Math.max(0, Number(s.maxPlayers) || 0),
          ping: Math.max(0, Number(s.ping) || 0),
          name: String(s.name || '').slice(0, 64),
          mode: String(s.mode || '').slice(0, 40),
          language: String(s.language || '').slice(0, 20),
          map: String(s.map || s.mapname || '').slice(0, 80),
          version: String(s.version || '').slice(0, 64),
          website: String(s.website || s.weburl || '').slice(0, 2048),
          discord: String(s.discord || '').slice(0, 2048),
          banner: safeImageUrl(s.banner),
          bannerDark: safeImageUrl(s.bannerDark),
          bannerLight: safeImageUrl(s.bannerLight),
          logo: safeImageUrl(s.logo),
          allowedClients: String(s.allowedClients || '').slice(0, 255),
          artwork: String(s.artwork || '').slice(0, 255),
          rulesKnown: s.rulesKnown === true,
          skr: s.skr === true ? true : s.skr === false ? false : null
        };
      });
    } catch (e) {}
    renderServerBrowser();
  }

  function loadPersonalServers(json) {
    try {
      var data = typeof json === 'string' ? JSON.parse(json) : json;
      personalServers = (Array.isArray(data) ? data : []).map(function (entry) {
        if (!entry || typeof entry !== 'object') return null;
        var address = normalizeAddress(entry.address);
        if (!address) return null;
        return { address: address, name: String(entry.name || '').trim().slice(0, 64) };
      }).filter(Boolean);
    } catch (e) {
      personalServers = [];
    }
    if (pendingAddAddress && personalServers.some(function (s) { return s.address === pendingAddAddress; })) {
      toast('Server added: ' + pendingAddAddress);
      pendingAddAddress = '';
      closeAddServer();
    }
    if (pendingRemoveAddress && !personalServers.some(function (s) { return s.address === pendingRemoveAddress; })) {
      delete favoriteServers['personal:' + pendingRemoveAddress];
      toast('Server removed: ' + pendingRemoveAddress);
      pendingRemoveAddress = '';
    }
    renderServerBrowser();
  }

  function setAddServerError(text) {
    $('addServerError').textContent = text || '';
    $('addServerAddress').classList.toggle('invalid', !!text);
  }
  function updateAddServerState() {
    var raw = $('addServerAddress').value.trim();
    var valid = !!normalizeAddress(raw);
    $('confirmAddServer').disabled = !valid || !!pendingAddAddress;
    $('addServerAddress').classList.toggle('invalid', !!raw && !valid);
    if (!raw || valid) $('addServerError').textContent = '';
    return valid;
  }
  function openAddServer() {
    addServerFocus = document.activeElement;
    pendingAddAddress = '';
    $('addServerAddress').value = '';
    $('addServerName').value = '';
    setAddServerError('');
    updateAddServerState();
    $('addServerModal').hidden = false;
    $('addServerAddress').focus();
  }
  function closeAddServer() {
    if ($('addServerModal').hidden) return;
    $('addServerModal').hidden = true;
    pendingAddAddress = '';
    if (addServerFocus) addServerFocus.focus();
  }
  function submitAddServer() {
    if (pendingAddAddress) return;
    var address = normalizeAddress($('addServerAddress').value);
    if (!address) {
      setAddServerError('Invalid address. Use host:port (e.g. 127.0.0.1:7777).');
      $('addServerAddress').focus();
      return;
    }
    var exists = personalServers.some(function (s) { return s.address === address; });
    if (!exists && personalServers.length >= MAX_PERSONAL_SERVERS) {
      setAddServerError('You have reached the limit of ' + MAX_PERSONAL_SERVERS + ' custom servers.');
      return;
    }
    var name = $('addServerName').value.trim().slice(0, 64);
    // address first: the C++ reader is a flat key scanner (settings_io.cpp).
    var payload = JSON.stringify({ address: address, name: name });
    if (typeof cef === 'undefined' || !cef.emit) {
      // Browser preview: no C++ side, keep the list in memory only.
      var next = personalServers.filter(function (s) { return s.address !== address; });
      next.push({ address: address, name: name });
      pendingAddAddress = address;
      loadPersonalServers(next);
      return;
    }
    pendingAddAddress = address;
    updateAddServerState();
    cefEmit('skr:lobby:servers:add', payload);
    setTimeout(function () {
      if (pendingAddAddress === address) {
        pendingAddAddress = '';
        updateAddServerState();
        setAddServerError('The client did not confirm the save. Try again.');
      }
    }, 4000);
  }
  function removePersonalServer(address) {
    if (pendingRemoveAddress) return;
    pendingRemoveAddress = address;
    if (typeof cef === 'undefined' || !cef.emit) {
      loadPersonalServers(personalServers.filter(function (s) { return s.address !== address; }));
    } else {
      cefEmit('skr:lobby:servers:remove', JSON.stringify({ address: address }));
    }
    setTimeout(function () {
      if (pendingRemoveAddress === address) {
        pendingRemoveAddress = '';
        toast('The client did not confirm removal. Try again.');
      }
    }, 4000);
  }
  function serverErrorText(code) {
    if (code === 'limit') return 'You have reached the limit of ' + MAX_PERSONAL_SERVERS + ' custom servers.';
    if (code === 'write') return 'Could not save server list to launcher.ini.';
    return 'Invalid address. Use host:port (e.g. 127.0.0.1:7777).';
  }

  function allServers() {
    var known = personalServers.map(personalAsServer).concat(publicServers.map(applyLiveInfo));
    // A recent successful server remains accessible even if no longer in the directory.
    recentServers.forEach(function (r) {
      if (!r || !normalizeAddress(r.address) || known.some(function (s) { return s.address === r.address; })) return;
      known.push({id:'history:' + r.address, name:String(r.name || r.address), shortName:'HIS', mode:'Recent',language:'',players:null,maxPlayers:null,ping:null,recommended:false,address:r.address,description:r.status === 'requested' ? 'Previously selected server' : r.status === 'launched' ? 'Previously launched server' : 'Previously connected server'});
    });
    return known;
  }

  function filteredServers() {
    var query = normalized($('serverSearch').value.trim());
    return allServers().filter(function (server) {
      if (serverFilter === 'recommended' && !server.recommended) return false;
      if (serverFilter === 'favorites' && !favoriteServers[server.id]) return false;
      if (serverFilter === 'history' && !recentServers.some(function (r) { return r.address === server.address; })) return false;
      if (!query) return true;
      return [server.name, server.mode, server.language, server.address].some(function (value) {
        return normalized(value).indexOf(query) !== -1;
      });
    });
  }

  // The directory has ~1,200 entries. Observe only rows in/near the scroll
  // viewport and ask native for at most 12 UDP pings at a time. Never bulk ping.
  function scheduleVisiblePings() {
    clearTimeout(pingRequestTimer);
    pingRequestTimer = setTimeout(function () {
      if (currentPage !== 'servers' || typeof cef === 'undefined' || !cef.emit) return;
      var now = Date.now();
      var batch = [];
      visiblePingAddresses.forEach(function (address) {
        if (batch.length >= 12 || !normalizeAddress(address)) return;
        var known = liveInfo[address];
        var fresh = pingSamples[address];
        if (known && known.online && Number.isFinite(known.ping)) return;
        if (fresh && now - fresh.time < PING_RETRY_MS) return;
        if (pingRequestedAt[address] && now - pingRequestedAt[address] < PING_RETRY_MS) return;
        batch.push(address);
      });
      if (batch.length && cefEmit('skr:lobby:servers:ping', batch.join(';'))) {
        batch.forEach(function (address) { pingRequestedAt[address] = now; });
      }
    }, 90);
  }

  function observeVisiblePings() {
    if (visiblePingObserver) visiblePingObserver.disconnect();
    visiblePingAddresses.clear();
    if (!window.IntersectionObserver) return;
    visiblePingObserver = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        var address = entry.target.dataset.serverAddress;
        if (!address) return;
        if (entry.isIntersecting) visiblePingAddresses.add(address);
        else visiblePingAddresses.delete(address);
      });
      scheduleVisiblePings();
    }, {root:$('publicServerList'),rootMargin:'100px 0px 100px 0px',threshold:0});
    $('publicServerList').querySelectorAll('.inline-server-row').forEach(function (row) {
      visiblePingObserver.observe(row);
    });
  }

  function loadVisiblePing(json) {
    var data;
    try { data = typeof json === 'string' ? JSON.parse(json) : json; } catch (e) { return; }
    (Array.isArray(data) ? data : []).forEach(function (entry) {
      if (!entry || !normalizeAddress(entry.address)) return;
      var address = normalizeAddress(entry.address);
      if (entry.online && Number.isFinite(Number(entry.ping)) && Number(entry.ping) >= 0) {
        pingSamples[address] = {ping:Math.round(Number(entry.ping)),time:Date.now()};
      }
      // Query timeouts do not establish that the game server is offline.
      var nodes = $('publicServerList').querySelectorAll('.inline-server-row');
      nodes.forEach(function (row) {
        if (row.dataset.serverAddress !== address) return;
        var indicator = row.querySelector('.inline-ping');
        if (!indicator) return;
        if (entry.online) {
          indicator.querySelector('span').textContent = pingSamples[address].ping + ' ms';
          indicator.classList.remove('is-unknown');
          indicator.title = 'UDP latency measured from this computer';
        } else if (!liveInfo[address] || !liveInfo[address].online) {
          indicator.querySelector('span').textContent = '—';
          indicator.classList.add('is-unknown');
          indicator.title = 'No UDP ping reply; availability is unknown';
        }
      });
    });
  }

  function renderServerBrowser() {
    if (currentPage === 'details') renderDetails();
    updateDirectoryMetrics();
    var servers = filteredServers();
    $('serverCount').textContent = servers.length + (servers.length === 1 ? ' SERVER' : ' SERVERS');
    var list = $('publicServerList');
    var previousScroll = list.scrollTop;
    if (visiblePingObserver) visiblePingObserver.disconnect();
    visiblePingAddresses.clear();
    list.textContent = '';
    if (!servers.length) {
      list.innerHTML = '<div class="server-list-state">No servers match this filter. Try a different search.</div>';
      return;
    }
    servers.forEach(function (server) {
      var occupancy = server.maxPlayers > 0 ? Math.max(0, Math.min(100, Math.round((server.players / server.maxPlayers) * 100))) : 0;
      var row = document.createElement('article');
      row.className = 'inline-server-row';
      row.dataset.serverAddress = server.address || '';
      row.innerHTML =
        '<div class="inline-server-identity"><span class="server-mark server-mark--banner">' + html(server.shortName) + '</span><span class="server-copy"><span class="server-name-line"><button type="button" class="server-details-link" data-details-id="' + html(server.id) + '">' + html(server.name) + '</button>' + (server.recommended ? '<i class="recommended-tag">FEATURED</i>' : '') + '</span><small>' + html(server.description) + '</small></span></div>' +
        '<div class="inline-server-mode"><strong>' + html(server.mode) + '</strong><small>' + html(String(server.language).toUpperCase()) + '</small></div>' +
        '<div class="inline-population"><span><strong>' + (server.players == null ? '—' : html(server.players)) + '</strong> / ' + (server.maxPlayers == null ? '—' : html(server.maxPlayers)) + '</span><i class="population-track"><i style="width:' + occupancy + '%"></i></i></div>' +
        '<div class="inline-ping' + (server.offline ? ' is-offline' : server.ping == null ? ' is-unknown' : '') + '"><i></i><span>' + (server.offline ? 'Offline' : server.ping == null ? '—' : html(server.ping) + ' ms') + '</span></div>' +
        '<div class="inline-server-actions"><button class="inline-favorite" type="button" data-favorite-id="' + html(server.id) + '" aria-label="Favorite">' + (favoriteServers[server.id] ? '★' : '☆') + '</button><button class="button button--gold inline-connect" data-action="connect" data-server-id="' + html(server.id) + '" data-connectable="' + (server.address ? 'true' : 'false') + '"' + (nicknameIsValid(nicknameValue()) && server.address && !connectPending ? '' : ' disabled') + '>Connect <span>↗</span></button></div>';
      list.appendChild(row);
      var artwork = artworkFor(server);
      fillServerMark(row.querySelector('.server-mark--banner'), server.shortName,
        artwork.banner || artwork.logo, 'server-artwork__img');
      if (server.skr === true || server.skr === false) {
        var skrTag = document.createElement('i');
        skrTag.className = server.skr ? 'skr-tag' : 'skr-tag skr-tag--missing';
        skrTag.textContent = server.skr ? 'SKR' : 'NO SKR';
        skrTag.title = server.skr ? 'Native SKR Client support' : 'No native SKR Client support detected';
        row.querySelector('.server-name-line').appendChild(skrTag);
      }
      if (server.personal) {
        var tag = document.createElement('i');
        tag.className = 'personal-tag';
        tag.textContent = 'CUSTOM';
        row.querySelector('.server-name-line').appendChild(tag);
        var remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'inline-remove';
        remove.dataset.removeAddress = server.address;
        remove.setAttribute('aria-label', 'Remove ' + server.name);
        remove.textContent = '×';
        row.querySelector('.inline-server-actions').prepend(remove);
      }
    });
    list.scrollTop = previousScroll;
    observeVisiblePings();
    updateHome();
  }

  // Discover only: pick ONE real public directory entry at random.
  // Never promote personal/history entries or private/local development hosts.
  function isPublicDiscoverAddress(address) {
    var normalizedAddress = normalizeAddress(address);
    if (!normalizedAddress) return false;
    var host = normalizedAddress.slice(0, normalizedAddress.lastIndexOf(':')).toLowerCase();
    if (host === 'localhost' || host.endsWith('.localhost') ||
        host.endsWith('.local') || host.endsWith('.lan') || host.endsWith('.internal')) return false;
    if (!/^[0-9.]+$/.test(host)) return true; // Valid public DNS name.
    var octets = host.split('.').map(Number);
    var a = octets[0], b = octets[1], d = octets[2];
    if (a === 0 || a === 10 || a === 127 || a >= 224 ||
        (a === 100 && b >= 64 && b <= 127) ||
        (a === 169 && b === 254) ||
        (a === 172 && b >= 16 && b <= 31) ||
        (a === 192 && (b === 168 || (b === 0 && d === 0) || (b === 0 && d === 2))) ||
        (a === 198 && (b === 18 || b === 19 || (b === 51 && d === 100))) ||
        (a === 203 && b === 0 && d === 113)) return false;
    return true;
  }

  function chooseDiscoverServer() {
    var eligible = publicServers.filter(function (server) {
      return isPublicDiscoverAddress(server.address) && !server.offline;
    });
    if (!eligible.length) { featuredSelectionId = null; return; }
    // Randomize only when the directory arrives or is refreshed, not on every redraw.
    var index = Math.floor(Math.random() * eligible.length);
    featuredSelectionId = eligible[index].id;
  }

  function loadServers(json) {
    try {
      var data = typeof json === 'string' ? JSON.parse(json) : json;
      var incoming = Array.isArray(data) ? data : (data && data.servers) || [];
      publicServers = incoming.map(function (server, index) {
        if (!server || typeof server !== 'object') return null;
        var id = String(server.id || '').trim();
        var name = String(server.name || '').trim();
        if (!id || !name) return null;
        return {
          detailsData: { description: server.description, mode: server.mode, language: server.language, players: server.players, maxPlayers: server.maxPlayers, ping: server.ping },
          version: String(server.version || '').slice(0, 64),
          map: String(server.map || server.mapname || '').slice(0, 80),
          website: String(server.website || server.weburl || '').slice(0, 2048),
          discord: String(server.discord || server.discordInvite || '').slice(0, 2048),
          banner: safeImageUrl(server.banner),
          bannerDark: safeImageUrl(server.bannerDark || server['banners.dark']),
          bannerLight: safeImageUrl(server.bannerLight || server['banners.light']),
          logo: safeImageUrl(server.logo),
          id: id,
          name: name,
          shortName: String(server.shortName || name.slice(0, 3)).slice(0, 4).toUpperCase(),
          mode: String(server.mode || 'Multiplayer'),
          language: String(server.language || '').slice(0, 12),
          players: server.players == null ? null : Math.max(0, Number(server.players) || 0),
          maxPlayers: server.maxPlayers == null ? null : Math.max(1, Number(server.maxPlayers) || 1),
          ping: server.ping == null ? null : Math.max(0, Number(server.ping) || 0),
          address: normalizeAddress(server.address),
          recommended: !!server.recommended,
          description: String(server.description || 'Public multiplayer server.'),
          order: Number(server.order) || index
        };
      }).filter(Boolean).sort(function (a, b) { return a.order - b.order; });
      directoryReceived = true;
      chooseDiscoverServer();
      $('refreshServers').classList.remove('refreshing');
      $('directoryStatus').textContent = (typeof cef === 'undefined' || !cef.emit)
        ? 'Browser demo · sample server data (not live)'
        : (publicServers.length
          ? 'Directory online · ' + publicServers.length + (publicServers.length === 1 ? ' server' : ' servers')
          : 'No public servers available');
      renderServerBrowser();
    } catch (e) {
      toast('The server directory could not be loaded.');
    }
  }

  // ── Settings panel render ───────────────────────────────────────
  function renderFields() {
    var c = config[currentTab];
    $('tabTitle').textContent = c.title;
    $('tabEyebrow').textContent = c.eyebrow;
    $('tabIndex').textContent = '0' + (Object.keys(config).indexOf(currentTab) + 1) + ' / 04';
    $('settingsFields').textContent = '';
    if ($('fpsEnginePanel')) $('fpsEnginePanel').hidden = currentTab !== 'graphics';

    document.querySelectorAll('[data-tab]').forEach(function (b) {
      var active = b.dataset.tab === currentTab;
      b.classList.toggle('active', active);
      b.setAttribute('aria-pressed', String(active));
    });

    c.fields.forEach(function (f) {
      var row = document.createElement('div'); row.className = 'setting-row';
      var text = document.createElement('div');
      var label = document.createElement('label'); label.className = 'setting-title';
      label.htmlFor = 'setting-' + f[0]; label.textContent = f[1];
      var hint = document.createElement('p'); hint.className = 'setting-hint'; hint.textContent = f[2];
      text.appendChild(label); text.appendChild(hint);
      row.appendChild(text);

      var input, holder;
      if (f[3] === 'select') {
        input = document.createElement('select'); input.className = 'setting-input';
        f[4].forEach(function (v) {
          var o = document.createElement('option'); o.value = v;
          var englishOptions = {'Automat':'Automatic','Fereastră':'Windowed','Fereastră fără margini':'Borderless window','Scăzută':'Low','Medie':'Medium','Ridicată':'High','Foarte ridicată':'Ultra','Dezactivat':'Off'};
          o.textContent = englishOptions[v] || v;
          input.appendChild(o);
        });
        input.value = state[currentTab][f[0]];
        holder = input;
      } else if (f[3] === 'range' || f[3] === 'scale') {
        holder = document.createElement('div'); holder.className = 'range-control';
        input = document.createElement('input'); input.type = 'range';
        input.min = f[3] === 'scale' ? 80 : 0;
        input.max = f[3] === 'scale' ? 125 : 100;
        input.value = state[currentTab][f[0]];
        var val = document.createElement('output'); val.htmlFor = 'setting-' + f[0];
        val.textContent = input.value + '%';
        holder.appendChild(input); holder.appendChild(val);
        input.addEventListener('input', function () { val.textContent = input.value + '%'; });
      } else if (f[3] === 'toggle') {
        holder = document.createElement('label'); holder.className = 'switch';
        holder.htmlFor = 'setting-' + f[0];
        var caption = document.createElement('span');
        caption.textContent = state[currentTab][f[0]] ? 'Enabled' : 'Disabled';
        input = document.createElement('input'); input.type = 'checkbox';
        input.checked = state[currentTab][f[0]];
        holder.appendChild(caption); holder.appendChild(input);
        input.addEventListener('change', function () {
          caption.textContent = input.checked ? 'Enabled' : 'Disabled';
        });
      } else {
        holder = document.createElement('button');
        holder.id = 'setting-' + f[0]; holder.className = 'binding';
        holder.textContent = 'Coming soon';
        holder.disabled = true;
        holder.addEventListener('click', function () {
          toast('Native key bindings are not available yet.');
        });
      }
      if (input) {
        input.id = 'setting-' + f[0];
        var supported = (currentTab === 'graphics' && (f[0] === 'resolution' || f[0] === 'screen')) || (currentTab === 'audio' && f[0] === 'volume');
        input.disabled = !supported;
        if (!supported) hint.textContent = 'Requires a future game adapter update.';
        input.addEventListener(input.type === 'range' ? 'input' : 'change', function () {
          state[currentTab][f[0]] = input.type === 'checkbox'
            ? input.checked
            : (input.type === 'range' ? Number(input.value) : input.value);
          updateDirty();
        });
      }
      row.appendChild(holder);
      $('settingsFields').appendChild(row);
    });
    updateDirty();
  }

  // ── Home / server history ───────────────────────────────────────
  function updateHome() {
    if (!$('featuredServer')) return;
    var available = allServers();
    var featured = publicServers.filter(function (server) {
      return server.id === featuredSelectionId && isPublicDiscoverAddress(server.address);
    })[0] || null;
    var recent = recentServers[0];
    $('favoritesCount').textContent = Object.keys(favoriteServers).filter(function (id) { return !!favoriteServers[id]; }).length + ' saved servers';
    $('historyCount').textContent = recentServers.length + (recentServers.length === 1 ? ' recent server' : ' recent servers');
    $('homeDirectoryStatus').textContent = $('directoryStatus').textContent;
    if (recent) {
      $('lastServerBadge').textContent = recent.status === 'requested' ? 'RECENT' : recent.status === 'launched' ? 'LAUNCHED' : 'PLAYED';
      $('lastServerContent').innerHTML = '<div class="placeholder-icon">↗</div><div><strong>' + html(recent.name) + '</strong><small>' + html(recent.address) + '</small></div>';
      var lastTime = recent.at && Number.isFinite(Number(recent.at)) ? new Date(Number(recent.at)) : null;
      $('lastServerMeta').textContent = (recent.status === 'requested' ? 'Last selected' : recent.status === 'launched' ? 'Last launch' : 'Last connected') +
        (lastTime && !isNaN(lastTime.getTime()) ? ' · ' + lastTime.toLocaleDateString() : '');
      var known = available.filter(function (s) { return s.address === recent.address; })[0];
      $('lastServerConnect').dataset.serverId = known ? known.id : '';
      $('lastServerConnect').dataset.connectable = known ? 'true' : 'false';
      $('lastServerConnect').disabled = !known || !nicknameIsValid(nicknameValue()) || connectPending;
    } else {
      $('lastServerBadge').textContent = 'NONE';
      $('lastServerContent').innerHTML = '<div class="placeholder-icon">⌁</div><div><strong>No recent servers</strong><small>Your latest server launch will show here.</small></div>';
      $('lastServerMeta').textContent = 'Ready for multiplayer';
      $('lastServerConnect').dataset.serverId = '';
      $('lastServerConnect').dataset.connectable = 'false';
      $('lastServerConnect').disabled = true;
    }
    if (featured) {
      var identity = $('featuredServer').querySelector('.featured-server__identity');
      var featuredArt = artworkFor(featured);
      fillServerMark(identity.querySelector('.featured-server__logo'), featured.shortName || 'MP', featuredArt.logo, 'server-artwork__img');
      fillWideBanner($('featuredServerBanner'), featuredArt.banner);
      identity.querySelector('h3').textContent = featured.name;
      identity.querySelector('p').textContent = featured.mode + (featured.language ? ' · ' + featured.language.toUpperCase() : '');
      $('featuredDescription').textContent = featured.description || 'Explore this community on San Andreas.';
      $('featuredMeta').innerHTML = '<span>' + (featured.players == null ? 'Players unavailable' : html(featured.players) + ' / ' + html(featured.maxPlayers) + ' players') + '</span><span>' + (featured.ping == null ? 'Ping unavailable' : html(featured.ping) + ' ms') + '</span>';
      $('featuredConnect').dataset.serverId = featured.id;
      $('featuredConnect').dataset.connectable = 'true';
      $('featuredConnect').disabled = !nicknameIsValid(nicknameValue()) || connectPending;
      $('featuredDetails').textContent = 'Server details →';
    } else {
      var title = $('featuredServer').querySelector('.featured-server__identity h3');
      title.textContent = 'Explore San Andreas multiplayer.';
      $('featuredServer').querySelector('.featured-server__identity p').textContent = 'Public open.mp directory';
      fillServerMark($('featuredServer').querySelector('.featured-server__logo'), 'MP', '');
      fillWideBanner($('featuredServerBanner'), '');
      $('featuredDescription').textContent = directoryReceived
        ? 'No public server with a valid address is currently available.'
        : 'Waiting for the public server directory…';
      $('featuredMeta').textContent = 'Waiting for public servers';
      $('featuredConnect').dataset.connectable = 'false';
      $('featuredConnect').dataset.serverId = '';
      $('featuredConnect').disabled = true;
    }
  }
  function saveRecentServer(server, status) {
    if (!server || !normalizeAddress(server.address)) return;
    var address = normalizeAddress(server.address);
    recentServers = recentServers.filter(function (r) { return r.address !== address; });
    recentServers.unshift({name:String(server.name || address).slice(0, 64), address:address,
      status:status, at:Date.now()});
    recentServers = recentServers.slice(0, 20);
    writeLocal('skr.launcher.recent.v3', recentServers);
    renderServerBrowser();
  }
  function forgetFailedLaunch() {
    if (!pendingConnectionServer) return;
    var address = pendingConnectionServer.address;
    recentServers = recentServers.filter(function (r) {
      return r.address !== address || (r.status !== 'requested' && r.status !== 'launched');
    });
    // If this was a reconnect attempt to a previously played server,
    // restore the previous successful history entry on an explicit failure.
    if (previousRecentEntry && previousRecentEntry.address === address &&
        !recentServers.some(function (r) { return r.address === address; })) {
      recentServers.unshift(previousRecentEntry);
    }
    recentServers = recentServers.slice(0,20);
    writeLocal('skr.launcher.recent.v3', recentServers);
    pendingConnectionServer = null;
    previousRecentEntry = null;
    renderServerBrowser();
  }
  function rememberNativeLaunch() {
    if (!pendingConnectionServer || !pendingConnectionServer.address) return;
    var current = recentServers.filter(function (r) { return r.address === pendingConnectionServer.address; })[0];
    if (current && current.status === 'requested') saveRecentServer(pendingConnectionServer, 'launched');
  }
  function rememberSuccessfulConnection() {
    if (!pendingConnectionServer || !pendingConnectionServer.address) return;
    saveRecentServer(pendingConnectionServer, 'connected');
    pendingConnectionServer = null;
    previousRecentEntry = null;
  }
  function setServerFilter(filter) {
    serverFilter = filter;
    document.querySelectorAll('[data-server-filter]').forEach(function (b) {
      b.classList.toggle('active', b.dataset.serverFilter === filter);
    });
    renderServerBrowser();
  }
  function openExitToolbar() {
    if (mode === 'pause') { confirmAction('quit'); return; }
    setProfileOpen(false);
    $('exitDimmer').hidden = false;
    $('exitToolbar').hidden = false;
    $('exitCancelButton').focus();
  }
  function closeExitToolbar() {
    $('exitDimmer').hidden = true;
    $('exitToolbar').hidden = true;
    $('headerQuit').focus();
  }

  // ── Navigation / modals ─────────────────────────────────────────
  function showPage(name) {
    currentPage = name;
    document.body.classList.toggle('view-servers', name === 'servers');
    document.querySelectorAll('.page').forEach(function (p) {
      p.hidden = p.dataset.page !== name;
    });
    if (name === 'servers') scheduleVisiblePings();
    document.querySelectorAll('.nav-button').forEach(function (b) {
      var active = b.dataset.page === name;
      b.classList.toggle('active', active);
      if (active) b.setAttribute('aria-current', 'page');
      else b.removeAttribute('aria-current');
    });
  }

  function closeModal() {
    $('confirmModal').hidden = true;
    pendingAction = null;
    if (previousFocus) previousFocus.focus();
  }
  function startConnect(selected) {
    connectPending = true;
    updateNicknameState(false);
    setConnectionServer(selected);
    setConnectionStep('launch');
    pendingConnectionServer = selected;
    previousRecentEntry = recentServers.filter(function (r) { return r.address === selected.address; })[0] || null;
    showConnection('Starting game…', 'Preparing San Andreas for ' + nicknameValue() + '.');
    if (!cefEmit('skr:lobby:connect', JSON.stringify({
      address: selected.address,
      nickname: nicknameValue()
    }))) {
      connectPending = false;
      updateNicknameState(false);
      hideConnection();
      pendingConnectionServer = null;
      previousRecentEntry = null;
      toast('Native connection is only available in SKR Client.');
    } else {
      // Native postMessage is not a successful-connect acknowledgement.
      // Save the selected server immediately so the card survives the GTA
      // handoff; a native launch/connection event upgrades its status later.
      saveRecentServer(selected, 'requested');
      if (window.chrome && window.chrome.webview) {
        // In-game CEF uses the existing connection modal; no launch cover there.
        showLaunchCover(selected);
      }
    }
  }

  function confirmAction(action) {
    previousFocus = document.activeElement;
    pendingAction = action;
    if (action && action.type === 'connect') {
      $('modalTitle').textContent = 'Limited SKR compatibility';
      $('modalText').textContent  = (action.server.name || action.server.address) +
        ' has no detected SKR Client support. Some in-game features may be unavailable. Connect anyway?';
    } else if (action === 'quit') {
      $('modalTitle').textContent = 'Exit game?';
      $('modalText').textContent  = 'Your current game session will close.';
    } else {
      $('modalTitle').textContent = 'Disconnect?';
      $('modalText').textContent  = 'You will return to the server browser. (Native disconnect is not connected yet.)';
    }
    $('confirmModal').hidden = false;
    $('cancelModal').focus();
  }

  function buildNav() {
    var nav = $('navigation');
    nav.textContent = '';
    navItems().forEach(function (item) {
      var b = document.createElement('button');
      b.className = 'nav-button' + (item[2] ? ' nav-button--danger' : '');
      b.dataset.page = item[0];
      b.textContent  = item[1];
      b.addEventListener('click', function () {
        if (item[0] === 'quit' || item[0] === 'disconnect') confirmAction(item[0]);
        else if (item[0] === 'resume') cefEmit('skr:lobby:resume');  // Phase 2 stub
        else showPage(item[0]);
      });
      nav.appendChild(b);
    });
    $('modeLabel').textContent  = mode === 'pause' ? 'IN GAME' : 'READY';
    $('escapeLabel').textContent = mode === 'pause' ? 'Exit game' : 'Exit to desktop';
    $('headerQuit').title = $('headerQuit').getAttribute('aria-label') || 'Exit';
    $('headerQuit').setAttribute('aria-label', mode === 'pause' ? 'Exit game' : 'Exit to desktop');
  }

  function setMode(next) {
    mode = next === 'pause' ? 'pause' : 'lobby';
    document.body.classList.toggle('transparent', mode === 'pause');
    document.body.classList.toggle('is-pause-mode', mode === 'pause');
    buildNav();
    showPage(mode === 'pause' ? 'settings' : 'home');
  }

  // ── Settings read (sidecar) ─────────────────────────────────────
  function hydrateFromSidecar(json) {
    try {
      var incoming = typeof json === 'string' ? JSON.parse(json) : (json || {});
      if (typeof incoming.nickname === 'string') {
        $('nickname').value = incoming.nickname;
        updateNicknameState(false);
      }
      if (dirty() || pendingSettings) return;
      // Map launcher.ini fields that still make sense here.
      if (typeof incoming.resolution === 'string' && incoming.resolution) {
        // launcher.ini stores "1920x1080"; UI uses "1920 × 1080".
        var pretty = incoming.resolution.replace('x', ' × ');
        if (pretty === 'auto') state.graphics.resolution = 'Automat';
        else state.graphics.resolution = pretty;
      }
      if (typeof incoming.windowed === 'boolean') {
        state.graphics.screen = incoming.windowed ? 'Fereastră' : 'Fullscreen';
      }
      if (typeof incoming.volume === 'number') {
        state.audio.volume = Math.max(0, Math.min(100, incoming.volume));
      }
      saved = JSON.parse(JSON.stringify(state));
      renderFields();
    } catch (e) { /* keep defaults */ }
  }
  function requestLoad() {
    cefEmit('skr:lobby:settings:get');
    fetch('settings.json', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.text() : '{}'; })
      .then(hydrateFromSidecar)
      .catch(function () {});
  }

  // ── Wire up ─────────────────────────────────────────────────────
  document.addEventListener('click', function (ev) {
    var remove = ev.target.closest ? ev.target.closest('[data-remove-address]') : null;
    if (remove) { removePersonalServer(remove.dataset.removeAddress); return; }
    var details = ev.target.closest ? ev.target.closest('[data-details-id]') : null;
    if (details) { openDetails(details.dataset.detailsId); return; }
    var favorite = ev.target.closest ? ev.target.closest('[data-favorite-id]') : null;
    if (favorite) {
      var favoriteId = favorite.dataset.favoriteId;
      favoriteServers[favoriteId] = !favoriteServers[favoriteId];
      writeLocal('skr.launcher.favorites.v3', favoriteServers);
      renderServerBrowser();
      return;
    }
    var tab = ev.target.closest ? ev.target.closest('[data-tab]') : null;
    if (tab) { currentTab = tab.dataset.tab; renderFields(); return; }
    var act = ev.target.closest ? ev.target.closest('[data-action]') : null;
    if (act) {
      if (act.dataset.action === 'connect') {
        if (connectPending) return;
        if (!updateNicknameState(true)) { setProfileOpen(true); $('nickname').focus(); return; }
        var selected = allServers().filter(function (server) { return server.id === act.dataset.serverId; })[0];
        if (!selected || !selected.address) {
          toast('This server has no connection address.');
          return;
        }
        // Rules queried and no "skr_client" rule: ask before connecting.
        if (selected.skr === false) confirmAction({ type: 'connect', server: selected });
        else startConnect(selected);
      } else if (act.dataset.action === 'open-settings') {
        showPage('settings');
      }
    }
  });
  $('resetChanges').addEventListener('click', function () {
    state = JSON.parse(JSON.stringify(saved));
    renderFields();
  });
  $('addServerButton').addEventListener('click', openAddServer);
  $('cancelAddServer').addEventListener('click', closeAddServer);
  $('addServerAddress').addEventListener('input', updateAddServerState);
  $('addServerForm').addEventListener('submit', function (e) {
    e.preventDefault();
    submitAddServer();
  });
  $('nickname').addEventListener('input', function () { updateNicknameState(false); });
  $('nickname').addEventListener('blur', function () { updateNicknameState(true); });
  $('nickname').addEventListener('keydown', function (e) {
    if (e.key === 'Enter') {
      if (updateNicknameState(true)) setProfileOpen(false);
    }
  });
  $('profileToggle').addEventListener('click', function () {
    setProfileOpen($('profilePopover').hidden);
  });
  $('saveNickname').addEventListener('click', function () {
    if (updateNicknameState(true)) {
      cefEmit('skr:lobby:nickname:save', JSON.stringify({ nickname: nicknameValue() }));
      clearTimeout(nicknameTimer);
      nicknameTimer = setTimeout(function () { toast('Profile save was not confirmed by the client.'); }, 4000);
    }
  });
  document.addEventListener('click', function (e) {
    if (!$('profilePopover').hidden && !e.target.closest('.player-profile')) setProfileOpen(false);
  });
  $('serverSearch').addEventListener('input', renderServerBrowser);
  document.body.appendChild($('exitToolbar'));
  $('headerSettings').addEventListener('click', function () { showPage('settings'); });
  $('headerQuit').addEventListener('click', openExitToolbar);
  $('exitCancelButton').addEventListener('click', closeExitToolbar);
  $('exitDesktopButton').addEventListener('click', function () {
    if (!cefEmit('skr:lobby:quit')) { closeExitToolbar(); toast('Exit to desktop is available in SKRLauncher.'); }
  });
  $('brandLink').addEventListener('click', function (e) { e.preventDefault(); if(mode !== 'pause') showPage('home'); });
  $('homeBrowse').addEventListener('click', function () { showPage('servers'); setServerFilter('all'); });
  $('homeViewAll').addEventListener('click', function () { showPage('servers'); setServerFilter('all'); });
  $('featuredDetails').addEventListener('click', function () { if (featuredSelectionId) openDetails(featuredSelectionId); else showPage('servers'); });
  $('homeFavorites').addEventListener('click', function () { showPage('servers'); setServerFilter('favorites'); });
  $('homeHistory').addEventListener('click', function () { showPage('servers'); setServerFilter('history'); });
  $('homeAddServer').addEventListener('click', openAddServer);
  $('homeRefresh').addEventListener('click', function () { $('refreshServers').click(); });

  $('refreshServers').addEventListener('click', function () {
    $('refreshServers').classList.add('refreshing');
    $('directoryStatus').textContent = 'Refreshing directory…';
    if (typeof cef === 'undefined' || !cef.emit) {
      setTimeout(function () {
        loadServers(previewServers);
        $('refreshServers').classList.remove('refreshing');
      }, 450);
    } else {
      cefEmit('skr:lobby:servers:request');
      setTimeout(function () { $('refreshServers').classList.remove('refreshing'); }, 900);
    }
  });
  document.querySelectorAll('[data-server-filter]').forEach(function (button) {
    button.addEventListener('click', function () {
      setServerFilter(button.dataset.serverFilter);
    });
  });
  // FPS Engine settings are intentionally separate from legacy lobby settings.
  // A native acknowledgement is required before claiming persistence.
  var fpsSaving = false;
  function fpsSetStatus(message) { if ($('fpsSettingsStatus')) $('fpsSettingsStatus').textContent = message; }
  function fpsApplyNative(json) {
    try {
      var value = JSON.parse(json);
      if (typeof value.showCounter !== 'boolean' || ![0,30,60,90,120,144,165,240].includes(Number(value.cap))) return;
      $('fpsCounterEnabled').checked = value.showCounter;
      $('fpsCapSetting').value = String(value.cap);
      fpsSaving = false;
      $('saveFpsSettings').disabled = false;
      fpsSetStatus('Saved preferences — next GTA launch.');
    } catch (e) { fpsSetStatus('Could not read native FPS preferences.'); }
  }
  cefOn('skr:fps:ready', fpsApplyNative);
  cefOn('skr:fps:saved', fpsApplyNative);
  cefOn('skr:fps:error', function (message) {
    fpsSaving = false;
    $('saveFpsSettings').disabled = false;
    fpsSetStatus(message || 'FPS preferences could not be saved.');
  });
  $('fpsCounterEnabled').addEventListener('change', function () { fpsSetStatus('Unsaved changes'); });
  $('fpsCapSetting').addEventListener('change', function () { fpsSetStatus('Unsaved changes'); });
  $('saveFpsSettings').addEventListener('click', function () {
    if (fpsSaving) return;
    fpsSaving = true;
    this.disabled = true;
    var payload = 'show=' + ($('fpsCounterEnabled').checked ? '1' : '0') + ';cap=' + $('fpsCapSetting').value;
    if (!cefEmit('skr:fps:save', payload)) {
      fpsSaving = false; this.disabled = false;
      fpsSetStatus('Saving requires the native SKR Launcher.');
    }
  });
  $('applyChanges').addEventListener('click', function () {
    if (pendingSettings) return;
    pendingSettings = JSON.parse(JSON.stringify(state));
    updateDirty();
    var payload = {
      resolution: state.graphics.resolution === 'Automat' ? 'auto' : state.graphics.resolution.replace(' × ', 'x'),
      windowed: state.graphics.screen === 'Fereastră',
      volume: state.audio.volume
    };
    if (!cefEmit('skr:lobby:settings:save', JSON.stringify(payload))) {
      pendingSettings = null;
      updateDirty();
      toast('Saving requires the native SKR Client bridge.');
      return;
    }
    settingsTimer = setTimeout(function () {
      pendingSettings = null;
      updateDirty();
      toast('The client did not confirm saving. Changes remain unapplied.');
    }, 4000);
  });
  $('cancelModal').addEventListener('click', closeModal);
  $('confirmModalButton').addEventListener('click', function () {
    var action = pendingAction;
    closeModal();
    if (action && action.type === 'connect') startConnect(action.server);
    else if (action === 'quit') cefEmit('skr:lobby:quit');
    else if (action === 'disconnect') { cefEmit('skr:lobby:disconnect'); toast('Native disconnect is not implemented yet.'); }
  });

  document.addEventListener('keydown', function (e) {
    if (!$('externalLinkModal').hidden) { handleExternalKeys(e); return; }
    if (!$('addServerModal').hidden) {
      if (e.key === 'Escape') { e.preventDefault(); closeAddServer(); return; }
      if (e.key === 'Tab') {
        var fields = Array.prototype.filter.call($('addServerForm').querySelectorAll('input, button'), function (el) { return !el.disabled; });
        var first = fields[0], last = fields[fields.length - 1];
        if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
        else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
      }
      return;
    }
    if (!$('confirmModal').hidden && e.key === 'Tab') {
      var a = $('cancelModal'), b = $('confirmModalButton');
      if (e.shiftKey && document.activeElement === a) { e.preventDefault(); b.focus(); }
      else if (!e.shiftKey && document.activeElement === b) { e.preventDefault(); a.focus(); }
    }
    if (e.key === 'Escape') {
      e.preventDefault();
      if (!$('exitToolbar').hidden) { closeExitToolbar(); return; }
      if (!$('confirmModal').hidden) { closeModal(); return; }
      if (!$('profilePopover').hidden) { setProfileOpen(false); return; }
      if (mode === 'pause') confirmAction('quit');
      else openExitToolbar();
    }
  });

  cefOn('skr:lobby:settings:ready', function (json) {
    if (typeof json === 'string') { hydrateFromSidecar(json); return; }
    fetch('settings.json', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.text() : '{}'; })
      .then(hydrateFromSidecar)
      .catch(function () {});
  });
  cefOn('skr:lobby:settings:saved', function () {
    clearTimeout(settingsTimer);
    if (!pendingSettings) return;
    saved = pendingSettings;
    pendingSettings = null;
    updateDirty();
    toast('Preferences saved. Display changes apply on next launch.');
  });
  cefOn('skr:lobby:settings:error', function () {
    clearTimeout(settingsTimer);
    pendingSettings = null;
    updateDirty();
    toast('Settings could not be saved. Try again.');
  });
  cefOn('skr:lobby:nickname:saved', function (name) {
    clearTimeout(nicknameTimer);
    setProfileOpen(false);
    toast('Profile saved for ' + String(name) + '.');
  });
  cefOn('skr:lobby:nickname:error', function () {
    clearTimeout(nicknameTimer);
    toast('Unable to save nickname.');
  });
  cefOn('skr:lobby:connect:error', function (code) {
    var timedOut = String(code || '') === 'timeout';
    if (!timedOut && launchCover && !launchCover.hidden) launchCover.hidden = true;
    if (!timedOut) forgetFailedLaunch();
    connectPending = timedOut;
    updateNicknameState(false);
    if (timedOut) {
      showConnection('Connection is taking longer…', 'Still waiting for the server to respond.');
    } else {
      hideConnection();
      toast('Connection failed. Check samp.dll and the SKR client logs.');
    }
  });
  cefOn('skr:lobby:connect:started', function () {
    rememberNativeLaunch();
    connectPending = true;
    updateNicknameState(false);
    setConnectionStep('world');
    showConnection('Loading game world…', 'San Andreas is preparing your session.');
    updateLaunchCoverStatus('Starting San Andreas and loading the world…', 'LOADING WORLD');
  });
  cefOn('skr:lobby:connect:phase', function (phase) {
    var phases = {
      server:   ['server', 'Connecting to server…', 'Waiting for server response.'],
      restart:  ['server', 'Server restarting…', 'Reconnecting automatically.'],
      waitjoin: ['join', 'Preparing your session…', 'Almost there.'],
      join:     ['join', 'Joining server…', 'Loading your character.']
    };
    var p = phases[String(phase || '')];
    if (!p) return;
    setConnectionStep(p[0]);
    showConnection(p[1], p[2]);
    updateLaunchCoverStatus(p[1] + ' ' + p[2], 'CONNECTING TO SERVER');
  });
  cefOn('skr:lobby:connect:download', function (json) {
    var d;
    try { d = typeof json === 'string' ? JSON.parse(json) : json; } catch (e) { return; }
    if (!d) return;
    var total = Number(d.total) || 0;
    var received = Math.min(total, Number(d.received) || 0);
    var percent = total > 0 ? Math.round(received / total * 100) : 100;
    if (d.state === 'done') {
      $('connectionDownload').textContent = ' · ' + formatMb(total) + ' MB';
      setConnectionStep('join');
      showConnection('Joining server…', 'Server resources are ready.');
      updateLaunchCoverStatus('Server resources are ready. Joining session…', 'JOINING SERVER');
      return;
    }
    $('connectionDownload').textContent = ' · ' + formatMb(received) + ' / ' + formatMb(total) + ' MB (' + percent + '%)';
    setConnectionStep('download');
    showConnection('Downloading resources…', (Number(d.files) || 0) + ' files from server.');
    updateLaunchCoverStatus('Downloading game resources · ' + percent + '%', 'DOWNLOADING RESOURCES');
  });
  cefOn('skr:lobby:connect:connected', function () {
    connectPending = false;
    setConnectionStep('join', true);
    showConnection('Connected', 'Welcome to the server.');
    updateLaunchCoverStatus('Connected. Entering San Andreas…', 'SESSION READY');
    rememberSuccessfulConnection();
  });
  cefOn('skr:lobby:mode', function () {
    try { setMode(String(arguments[0] || 'lobby')); } catch (e) {}
  });
  cefOn('skr:lobby:servers', function (json) { loadServers(json); });
  cefOn('skr:lobby:servers:personal', loadPersonalServers);
  cefOn('skr:lobby:servers:info', loadServerInfo);
  cefOn('skr:lobby:servers:ping', loadVisiblePing);
  cefOn('skr:lobby:servers:error', function (code) {
    pendingAddAddress = '';
    pendingRemoveAddress = '';
    updateAddServerState();
    if (!$('addServerModal').hidden) setAddServerError(serverErrorText(code));
    else toast(serverErrorText(code));
  });

  // WebView2 launcher-to-GTA splash; native D3D9 handoff remains unchanged.
  // Animated activity is deliberately indeterminate: we don't fake loading percent.
  var launchCover = document.createElement('div');
  launchCover.id = 'skrLaunchCover'; launchCover.hidden = true;
  launchCover.setAttribute('role', 'status');
  launchCover.setAttribute('aria-live', 'polite');
  launchCover.innerHTML =
    '<div class="launch-scene" aria-hidden="true"><span class="launch-scene__grid"></span><span class="launch-scene__skyline"></span><span class="launch-scene__glow"></span></div>' +
    '<div class="launch-cover__header"><div class="launch-cover__brand">SKR<span>.</span><small>CLIENT / MULTIPLAYER</small></div><div class="launch-cover__ready"><i></i> SESSION LAUNCH</div></div>' +
    '<div class="launch-cover__body"><div class="launch-cover__eyebrow"><span></span> SAN ANDREAS MULTIPLAYER</div>' +
      '<h1>YOUR WORLD.<br><em>IS LOADING.</em></h1><p class="launch-cover__subtitle" id="skrLaunchStatus">Preparing your session…</p>' +
      '<div class="launch-cover__server"><div class="launch-cover__server-icon">↗</div><div><small>CONNECTING TO</small><strong id="skrLaunchServer">Multiplayer server</strong><span id="skrLaunchAddress">Starting San Andreas</span></div></div>' +
      '<div class="launch-cover__progress"><div class="launch-cover__track"><span></span></div><div class="launch-cover__progress-labels"><span id="skrLaunchPhase">LAUNCHING GAME</span><span>SKR CLIENT</span></div></div>' +
    '</div><div class="launch-cover__footer"><span>SKR CLIENT <i>·</i> SAN ANDREAS MULTIPLAYER</span><button id="skrLaunchCancel" type="button">Cancel launch <span aria-hidden="true">×</span></button></div>';
  document.body.appendChild(launchCover);
  function updateLaunchCoverStatus(message, phase) {
    if (!launchCover || launchCover.hidden) return;
    $('skrLaunchStatus').textContent = message;
    $('skrLaunchPhase').textContent = phase;
  }
  function showLaunchCover(server) {
    var s = server || pendingConnectionServer;
    if (s) {
      $('skrLaunchServer').textContent = s.name || s.address || 'Multiplayer server';
      $('skrLaunchAddress').textContent = s.address || '';
    }
    launchCover.classList.remove('launch-cover--failed');
    $('skrLaunchStatus').textContent = 'Preparing your session…';
    $('skrLaunchPhase').textContent = 'LAUNCHING GAME';
    $('skrLaunchCancel').firstChild.textContent = 'Cancel launch ';
    launchCover.hidden = false;
  }
  $('skrLaunchCancel').addEventListener('click', function () {
    updateLaunchCoverStatus('Cancelling launch…', 'CANCELLING');
    cefEmit('skr:launch:cancel');
  });
  cefOn('skr:launch:loading', function () {
    rememberNativeLaunch();
    showLaunchCover();
    connectPending = true;
    updateNicknameState(false);
  });
  cefOn('skr:launch:failed', function (message) {
    forgetFailedLaunch();
    launchCover.hidden = false;
    launchCover.classList.add('launch-cover--failed');
    $('skrLaunchStatus').textContent = String(message || 'Unable to launch San Andreas.');
    $('skrLaunchPhase').textContent = 'LAUNCH FAILED';
    $('skrLaunchCancel').firstChild.textContent = 'Back to launcher ';
  });
  cefOn('skr:launch:idle', function (message) {
    launchCover.hidden = true;
    launchCover.classList.remove('launch-cover--failed');
    connectPending = false; hideConnection(); updateNicknameState(false);
    if (message) toast(String(message));
  });
  // Details resolve current data by ID on each update, never a stale copied server.
  function detailServer() { return allServers().filter(function (s) { return s.id === detailsId; })[0]; }
  function openDetails(id) {
    if (!allServers().some(function (s) { return s.id === id; })) return;
    if (currentPage !== 'details') { detailsOrigin = currentPage === 'servers' ? 'servers' : 'home'; detailsFocus = document.activeElement; }
    detailsId = id; showPage('details'); renderDetails(); $('detailsName').focus();
    var selected = detailServer(); if (selected) queryDetailsOnce(selected.address);
  }
  function renderDetails() {
    var s = detailServer();
    $('detailsFacts').textContent = ''; $('detailsCopyStatus').textContent = '';
    $('detailsName').textContent = s ? s.name : 'Server no longer available';
    var art = artworkFor(s);
    fillServerMark($('detailsInitials'), s ? s.shortName || s.name.slice(0, 3).toUpperCase() : '—', art.logo, 'server-artwork__img');
    fillWideBanner($('detailsBanner'), art.banner);
    var info = s && liveInfo[s.address];
    var raw = s && s.detailsData || {};
    var description = raw.description ? String(raw.description) : '';
    $('detailsDescription').textContent = description; $('detailsDescription').hidden = !description;
    $('detailsStatus').textContent = !s ? 'Return to the directory to select another server.' : info && !info.online ? 'No response to the latest query; server status is unconfirmed.' : info && info.online ? 'Responded to the latest server query.' : detailsQueryAt[s.address] ? 'Checking live server information…' : 'Catalog information; live availability has not been confirmed.';
    function fact(label, value) {
      if (value == null || value === '') return;
      var group = document.createElement('div'), term = document.createElement('dt'), definition = document.createElement('dd');
      term.textContent = label; definition.textContent = String(value); group.append(term, definition); $('detailsFacts').appendChild(group);
    }
    if (s) {
      var live = info && info.online ? info : null;
      if (!(info && !info.online)) {
        var players = live ? live.players : raw.players, capacity = live ? live.maxPlayers : raw.maxPlayers;
        fact('Players', players == null ? null : players + (capacity == null ? '' : ' / ' + capacity));
        var ping = live ? live.ping : (s.ping == null ? raw.ping : s.ping); fact('Latency', ping == null ? null : ping + ' ms');
      }
      fact('Gamemode', info && info.online && info.mode ? info.mode : raw.mode);
      fact('Language', info && info.online && info.language ? info.language : raw.language);
      fact('Map', (info && info.online && info.map) || s.map);
      fact('Version', (info && info.online && info.version) || s.version);
      fact('Supported clients', info && info.online && info.allowedClients);
      fact('Runtime', info && info.rulesKnown ? (info.skrProtocol === '2' ? 'SKR Enhanced' : info.skrProtocol ? 'SKR (incompatible version)' : 'Standard') : 'Unconfirmed');
      fact('Address', s.address);
    }
    var links = $('detailsLinks');
    links.textContent = '';
    if (s) {
      [['Website', (info && info.online && info.website) || s.website], ['Discord', (info && info.online && info.discord) || s.discord]].forEach(function (entry) {
        var url = safeWebsiteUrl(entry[1]);
        if (!url) return;
        var a = document.createElement('a');
        a.href = url;
        a.target = '_blank';
        a.rel = 'noopener noreferrer';
        a.className = 'server-details__external';
        a.textContent = entry[0] + ' ↗';
        a.title = url;
        links.appendChild(a);
      });
    }
    links.hidden = !links.childElementCount;
    // "Resources" describes announced capabilities, not downloadable files.
    var resources = $('detailsResources'), resourceStatus = $('detailsResourcesStatus');
    resources.textContent = '';
    function resource(label, value) {
      var row = document.createElement('div'); row.className = 'server-details__resource';
      var title = document.createElement('strong'), detail = document.createElement('span');
      title.textContent = label; detail.textContent = value;
      row.append(title, detail); resources.appendChild(row);
    }
    if (s && info && info.online && info.artwork) {
      var artworkValue = info.artwork.trim().toLowerCase();
      var enabled = ['1', 'on', 'yes', 'true', 'enabled'].includes(artworkValue);
      var disabled = ['0', 'off', 'no', 'false', 'disabled'].includes(artworkValue);
      if (enabled || disabled) resource('Custom Models (Artwork)', enabled ? 'Enabled' : 'Disabled');
    }
    if (s && info && info.online && info.allowedClients) resource('Supported client builds', info.allowedClients);
    resourceStatus.textContent = !s ? 'Unavailable' : resources.childElementCount ? 'Server-advertised' :
      info && info.rulesKnown ? 'No resource details advertised' :
      detailsQueryAt[s.address] && !info ? 'Checking live query…' : 'Not available';

    $('detailsConnect').dataset.serverId = s ? s.id : '';
    $('detailsConnect').dataset.connectable = s && s.address ? 'true' : 'false';
    $('detailsConnect').disabled = !s || !s.address || connectPending || !nicknameIsValid(nicknameValue());
    $('detailsFavorite').dataset.favoriteId = s ? s.id : '';
    $('detailsFavorite').disabled = !s;
    var favorite = !!(s && favoriteServers[s.id]);
    $('detailsFavorite').textContent = favorite ? '★ Favorited' : '☆ Favorite';
    $('detailsFavorite').setAttribute('aria-pressed', String(favorite));
    $('detailsCopy').disabled = !s || !s.address;
  }
  $('detailsBack').addEventListener('click', function () {
    showPage(detailsOrigin);
    if (detailsFocus && detailsFocus.isConnected && detailsFocus.getClientRects().length) detailsFocus.focus();
    else if (detailsOrigin === 'servers') $('serverSearch').focus(); else $('featuredDetails').focus();
  });
  $('detailsCopy').addEventListener('click', async function () {
    var s = detailServer(); if (!s || !s.address) return;
    try {
      if (!navigator.clipboard || !navigator.clipboard.writeText) throw new Error('unavailable');
      await navigator.clipboard.writeText(s.address);
      $('detailsCopyStatus').textContent = 'Server address copied.';
    } catch (e) { $('detailsCopyStatus').textContent = 'Clipboard unavailable. Select and copy the address above.'; }
  });
  // Cards remain separate from their Connect/Favorite actions.
  $('featuredServer').addEventListener('click', function (e) {
    if (e.target.closest('button, a, input')) return;
    if (featuredSelectionId) openDetails(featuredSelectionId);
  });
  $('publicServerList').addEventListener('click', function (e) {
    if (e.target.closest('button, a, input')) return;
    var row = e.target.closest('.inline-server-row');
    var connect = row && row.querySelector('[data-action="connect"]');
    if (connect) openDetails(connect.dataset.serverId);
  });

  var externalDestination = null, externalFocus = null;
  function closeExternal() {
    $('externalLinkModal').hidden = true; externalDestination = null;
    if (externalFocus && externalFocus.isConnected) externalFocus.focus();
  }
  function confirmExternal(anchor) {
    if (!$('externalLinkModal').hidden) return;
    var url;
    try {
      url = new URL(anchor.getAttribute('href'), location.href);
      if (!/^https?:$/.test(url.protocol) || !url.hostname || url.username || url.password) throw new Error('invalid');
    } catch (e) { toast('This link is not a valid HTTP or HTTPS website.'); return; }
    externalDestination = url.href; externalFocus = document.activeElement;
    $('externalLinkHost').textContent = url.hostname;
    $('externalLinkUrl').textContent = url.href;
    // WebView2 keeps navigation blocked; only the confirmed URL is sent to
    // the launcher, which validates it again before asking Windows to open it.
    var unsupportedHost = typeof cef !== 'undefined' && location.hostname !== 'skr-launcher.invalid';
    $('externalLinkVisit').disabled = unsupportedHost;
    $('externalLinkLimit').textContent = unsupportedHost ? 'Opening external websites is unavailable in this in-game interface.' : 'Opens in your default browser.';
    $('externalLinkModal').hidden = false; $('externalLinkCancel').focus();
  }
  function interceptExternal(e) {
    var anchor = e.target.closest && e.target.closest('a[href]');
    if (!anchor || anchor.getAttribute('href').charAt(0) === '#') return;
    e.preventDefault(); e.stopPropagation(); confirmExternal(anchor);
  }
  document.addEventListener('click', interceptExternal, true);
  document.addEventListener('auxclick', function (e) { if (e.button === 1) interceptExternal(e); }, true);
  $('externalLinkCancel').addEventListener('click', closeExternal);
  $('externalLinkVisit').addEventListener('click', function () {
    if (!externalDestination || this.disabled) return;
    if (location.hostname === 'skr-launcher.invalid') {
      this.disabled = true;
      if (!cefEmit('skr:external:open', externalDestination)) {
        this.disabled = false;
        $('externalLinkLimit').textContent = 'Could not contact the launcher.';
      }
      return;
    }
    // Browser preview uses the existing target=_blank behavior, after confirmation.
    window.open(externalDestination, '_blank', 'noopener,noreferrer'); closeExternal();
  });
  cefOn('skr:external:opened', closeExternal);
  cefOn('skr:external:error', function (message) {
    $('externalLinkVisit').disabled = false;
    $('externalLinkLimit').textContent = String(message || 'Could not open the browser.');
  });
  function handleExternalKeys(e) {
    if (e.key === 'Escape') { e.preventDefault(); closeExternal(); return; }
    if (e.key !== 'Tab') return;
    var controls = Array.from($('externalLinkModal').querySelectorAll('button:not(:disabled)'));
    var index = controls.indexOf(document.activeElement);
    e.preventDefault(); controls[(index + (e.shiftKey ? controls.length - 1 : 1)) % controls.length].focus();
  }
  // Public hook for the native adapter (Phase 2).
  window.skrLobby = { setMode: setMode };

  // ── Loading splash ──────────────────────────────────────────
  function setSplashProgress(pct, text) {
    var fill = $('splashFill');
    var status = $('splashStatus');
    if (fill) fill.style.width = Math.min(100, Math.max(0, pct)) + '%';
    if (status && text) status.textContent = text;
  }

  function revealLobby() {
    if (!document.body.classList.contains('loading')) return;
    setSplashProgress(100, 'Ready.');
    setTimeout(function () {
      document.body.classList.remove('loading');
      var splash = $('splash');
      if (splash) {
        splash.classList.add('hidden');
        setTimeout(function () { splash.remove(); }, 500);
      }
    }, 200);
  }

  function preloadAssets() {
    var images = ['assets/background.png', 'assets/logo.png'];
    var loaded = 0;
    var total = images.length;

    setSplashProgress(10, 'Loading resources…');

    if (!total) { revealLobby(); return; }

    function tick() {
      loaded++;
      var pct = 10 + Math.round((loaded / total) * 80);
      setSplashProgress(pct, loaded < total ? 'Loading resources… (' + loaded + '/' + total + ')' : 'Preparing interface…');
      if (loaded >= total) setTimeout(revealLobby, 150);
    }

    images.forEach(function (src) {
      var img = new Image();
      img.onload = tick;
      img.onerror = tick;
      img.src = src;
    });

    // Fallback: reveal after 5s even if assets fail silently
    setTimeout(revealLobby, 5000);
  }

  buildNav();
  renderFields();
  setMode('lobby');          // Phase 1 default
  if (typeof cef === 'undefined' || !cef.emit) loadServers(previewServers);
  else cefEmit('skr:lobby:servers:request');
  updateNicknameState(false);
  requestLoad();
  if (!cefEmit('skr:fps:get')) fpsSetStatus('FPS preferences available in SKRLauncher.');
  preloadAssets();
})();
