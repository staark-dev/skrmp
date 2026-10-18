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
      ? [['resume','Continuă',false],['map','Hartă',false],['settings','Setări',false],['disconnect','Deconectare',true],['quit','Ieșire',true]]
      : [['servers','Servere',false],['settings','Setări',false],['quit','Ieșire',true]];
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
  var personalServers = [];      // [{address, name}] as persisted by C++
  var pendingAddAddress = '';
  var pendingRemoveAddress = '';
  var addServerFocus = null;
  var connectPending = false;
  var MAX_PERSONAL_SERVERS = 50; // mirrors kMaxPersonalServers (settings_io.h)

  var previewServers = [
    { id:'skr-main', name:'SKR Roleplay', shortName:'SKR', mode:'Roleplay', language:'RO', players:142, maxPlayers:500, ping:24, recommended:true, address:'127.0.0.1:7777', description:'Construiește-ți povestea într-un San Andreas refăcut pentru roleplay.' },
    { id:'vinewood-stories', name:'Vinewood Stories', shortName:'VS', mode:'RPG', language:'RO', players:386, maxPlayers:1000, ping:31, description:'Progres, facțiuni și economie într-o comunitate activă.' },
    { id:'ls-central', name:'Los Santos Central', shortName:'LS', mode:'Roleplay', language:'RO', players:87, maxPlayers:300, ping:42, description:'Roleplay modern cu sisteme construite pentru comunitate.' },
    { id:'sa-freeroam', name:'San Andreas Freeroam', shortName:'SA', mode:'Freeroam', language:'EN', players:54, maxPlayers:200, ping:58, description:'Explorează San Andreas fără limite.' },
    { id:'skr-dev', name:'SKR Development', shortName:'DEV', mode:'Development', language:'RO', players:12, maxPlayers:100, ping:18, address:'127.0.0.1:7778', description:'Server de testare pentru următoarele funcții SKR.' }
  ];

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
    $('playerName').textContent = valid ? value : 'Setează nickname';
    $('playerAvatar').textContent = valid ? value.charAt(0).toUpperCase() : '?';
    $('nicknameError').textContent = showError && !valid
      ? (value ? 'Nickname invalid.' : 'Introdu un nickname înainte de conectare.')
      : '';
    return valid;
  }

  var config = {
    graphics:{title:'Grafică',eyebrow:'AFIȘARE ȘI PERFORMANȚĂ',fields:[
      ['resolution','Rezoluție','Se aplică la următoarea pornire a jocului.','select',['Automat','1280 × 720','1366 × 768','1600 × 900','1920 × 1080','2560 × 1440']],
      ['screen','Mod de afișare','Aplicarea în joc necesită confirmare și revenire la eșec.','select',['Fullscreen','Fereastră','Fereastră fără margini']],
      ['distance','Distanță de afișare','Distanța la care sunt desenate elementele lumii.','range'],
      ['effects','Calitatea efectelor','Nivelul detaliilor efectelor vizuale.','select',['Scăzută','Medie','Ridicată','Foarte ridicată']],
      ['antialias','Anti-aliasing','Netezirea contururilor.','select',['Dezactivat','2×','4×','8×']]
    ]},
    audio:{title:'Audio',eyebrow:'SUNET ȘI AMBIANȚĂ',fields:[
      ['effects','Efecte sonore','Vehicule, pași și sunetele lumii.','range'],
      ['radio','Radio','Volumul posturilor radio din joc.','range'],
      ['volume','Audio SKR','Preferință salvată; aplicarea audio în joc este în lucru.','range']
    ]},
    controls:{title:'Controale',eyebrow:'TASTATURĂ ȘI MOUSE',fields:[
      ['sensitivity','Sensibilitate mouse','Viteza de mișcare a camerei.','range'],
      ['invert','Inversează axa verticală','Inversează mișcarea verticală a mouse-ului.','toggle'],
      ['bindings','Configurare taste','Va deschide configurarea nativă prin adaptorul clientului.','binding']
    ]},
    interface:{title:'Interfață SKR',eyebrow:'HUD ȘI PREFERINȚE',fields:[
      ['hudScale','Scară HUD','Dimensiunea elementelor HUD.','scale'],
      ['chat','Chatbox','Afișează mesajele de chat.','toggle'],
      ['radar','Radar','Afișează minimapul.','toggle']
    ]}
  };

  // ── CEF bridge wrappers ─────────────────────────────────────────
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
    $('changeStatus').textContent = changed ? 'Modificări neaplicate' : 'Nicio modificare';
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
      description: entry.address
    };
  }

  function applyLiveInfo(server) {
    var info = server.address && liveInfo[server.address];
    if (!info) return server;
    if (!info.online) { server.offline = true; return server; }
    server.players = info.players;
    server.maxPlayers = info.maxPlayers;
    server.ping = info.ping;
    server.locked = info.password;
    server.skr = info.skr;
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
      toast('Server adăugat: ' + pendingAddAddress);
      pendingAddAddress = '';
      closeAddServer();
    }
    if (pendingRemoveAddress && !personalServers.some(function (s) { return s.address === pendingRemoveAddress; })) {
      delete favoriteServers['personal:' + pendingRemoveAddress];
      toast('Server eliminat: ' + pendingRemoveAddress);
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
      setAddServerError('Adresă invalidă. Folosește formatul host:port (ex. 127.0.0.1:7777).');
      $('addServerAddress').focus();
      return;
    }
    var exists = personalServers.some(function (s) { return s.address === address; });
    if (!exists && personalServers.length >= MAX_PERSONAL_SERVERS) {
      setAddServerError('Ai atins limita de ' + MAX_PERSONAL_SERVERS + ' servere personale.');
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
        setAddServerError('Clientul nu a confirmat salvarea. Încearcă din nou.');
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
        toast('Clientul nu a confirmat eliminarea. Încearcă din nou.');
      }
    }, 4000);
  }
  function serverErrorText(code) {
    if (code === 'limit') return 'Ai atins limita de ' + MAX_PERSONAL_SERVERS + ' servere personale.';
    if (code === 'write') return 'Lista de servere nu a putut fi salvată în launcher.ini.';
    return 'Adresă invalidă. Folosește formatul host:port (ex. 127.0.0.1:7777).';
  }

  function allServers() {
    return personalServers.map(personalAsServer).concat(publicServers.map(applyLiveInfo));
  }

  function filteredServers() {
    var query = normalized($('serverSearch').value.trim());
    return allServers().filter(function (server) {
      if (serverFilter === 'recommended' && !server.recommended) return false;
      if (serverFilter === 'favorites' && !favoriteServers[server.id]) return false;
      if (!query) return true;
      return [server.name, server.mode, server.language, server.address].some(function (value) {
        return normalized(value).indexOf(query) !== -1;
      });
    });
  }

  function renderServerBrowser() {
    var servers = filteredServers();
    $('serverCount').textContent = servers.length + (servers.length === 1 ? ' SERVER' : ' SERVERE');
    var list = $('publicServerList');
    list.textContent = '';
    if (!servers.length) {
      list.innerHTML = '<div class="server-list-state">Niciun server nu corespunde filtrului.</div>';
      return;
    }
    servers.forEach(function (server) {
      var occupancy = server.maxPlayers > 0 ? Math.max(0, Math.min(100, Math.round((server.players / server.maxPlayers) * 100))) : 0;
      var row = document.createElement('article');
      row.className = 'inline-server-row';
      row.innerHTML =
        '<div class="inline-server-identity"><span class="server-mark">' + html(server.shortName) + '</span><span class="server-copy"><span class="server-name-line"><strong>' + html(server.name) + '</strong>' + (server.recommended ? '<i class="recommended-tag">RECOMANDAT</i>' : '') + '</span><small>' + html(server.description) + '</small></span></div>' +
        '<div class="inline-server-mode"><strong>' + html(server.mode) + '</strong><small>' + html(String(server.language).toUpperCase()) + '</small></div>' +
        '<div class="inline-population"><span><strong>' + (server.players == null ? '—' : html(server.players)) + '</strong> / ' + (server.maxPlayers == null ? '—' : html(server.maxPlayers)) + '</span><i class="population-track"><i style="width:' + occupancy + '%"></i></i></div>' +
        '<div class="inline-ping' + (server.offline ? ' is-offline' : '') + '"><i></i><span>' + (server.offline ? 'Offline' : server.ping == null ? '—' : html(server.ping) + ' ms') + '</span></div>' +
        '<div class="inline-server-actions"><button class="inline-favorite" type="button" data-favorite-id="' + html(server.id) + '" aria-label="Favorite">' + (favoriteServers[server.id] ? '★' : '☆') + '</button><button class="button button--gold inline-connect" data-action="connect" data-server-id="' + html(server.id) + '" data-connectable="' + (server.address ? 'true' : 'false') + '"' + (nicknameIsValid(nicknameValue()) && server.address && !connectPending ? '' : ' disabled') + '>Conectează <span>→</span></button></div>';
      list.appendChild(row);
      if (server.skr === true || server.skr === false) {
        var skrTag = document.createElement('i');
        skrTag.className = server.skr ? 'skr-tag' : 'skr-tag skr-tag--missing';
        skrTag.textContent = server.skr ? 'SKR' : 'FĂRĂ SKR';
        skrTag.title = server.skr ? 'Suport nativ pentru SKR Client' : 'Serverul nu are suport nativ pentru SKR Client';
        row.querySelector('.server-name-line').appendChild(skrTag);
      }
      if (server.personal) {
        var tag = document.createElement('i');
        tag.className = 'personal-tag';
        tag.textContent = 'PERSONAL';
        row.querySelector('.server-name-line').appendChild(tag);
        var remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'inline-remove';
        remove.dataset.removeAddress = server.address;
        remove.setAttribute('aria-label', 'Elimină ' + server.name);
        remove.textContent = '×';
        row.querySelector('.inline-server-actions').prepend(remove);
      }
    });
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
          id: id,
          name: name,
          shortName: String(server.shortName || name.slice(0, 3)).slice(0, 4).toUpperCase(),
          mode: String(server.mode || 'Multiplayer'),
          language: String(server.language || '').slice(0, 12),
          players: Math.max(0, Number(server.players) || 0),
          maxPlayers: Math.max(1, Number(server.maxPlayers) || 1),
          ping: server.ping == null ? null : Math.max(0, Number(server.ping) || 0),
          address: normalizeAddress(server.address),
          recommended: !!server.recommended,
          description: String(server.description || 'Server public din rețeaua SKR.'),
          order: Number(server.order) || index
        };
      }).filter(Boolean).sort(function (a, b) { return a.order - b.order; });
      $('refreshServers').classList.remove('refreshing');
      $('directoryStatus').textContent = publicServers.length
        ? 'Catalog open.mp · ' + publicServers.length + (publicServers.length === 1 ? ' server' : ' servere')
        : 'Catalogul nu conține servere';
      renderServerBrowser();
    } catch (e) {
      toast('Lista publică nu a putut fi încărcată.');
    }
  }

  // ── Settings panel render ───────────────────────────────────────
  function renderFields() {
    var c = config[currentTab];
    $('tabTitle').textContent = c.title;
    $('tabEyebrow').textContent = c.eyebrow;
    $('tabIndex').textContent = '0' + (Object.keys(config).indexOf(currentTab) + 1) + ' / 04';
    $('settingsFields').textContent = '';

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
          var o = document.createElement('option'); o.value = v; o.textContent = v;
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
        caption.textContent = state[currentTab][f[0]] ? 'Activat' : 'Dezactivat';
        input = document.createElement('input'); input.type = 'checkbox';
        input.checked = state[currentTab][f[0]];
        holder.appendChild(caption); holder.appendChild(input);
        input.addEventListener('change', function () {
          caption.textContent = input.checked ? 'Activat' : 'Dezactivat';
        });
      } else {
        holder = document.createElement('button');
        holder.id = 'setting-' + f[0]; holder.className = 'binding';
        holder.textContent = 'În lucru';
        holder.disabled = true;
        holder.addEventListener('click', function () {
          toast('Phase 1: configurarea tastelor native va fi conectată în Phase 2.');
        });
      }
      if (input) {
        input.id = 'setting-' + f[0];
        var supported = (currentTab === 'graphics' && (f[0] === 'resolution' || f[0] === 'screen')) || (currentTab === 'audio' && f[0] === 'volume');
        input.disabled = !supported;
        if (!supported) hint.textContent = 'Disponibil după integrarea adaptorului de joc.';
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

  // ── Navigation / modals ─────────────────────────────────────────
  function showPage(name) {
    currentPage = name;
    document.querySelectorAll('.page').forEach(function (p) {
      p.hidden = p.dataset.page !== name;
    });
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
    showConnection('Se pornește sesiunea…', 'Pregătim jocul pentru ' + nicknameValue() + '.');
    if (!cefEmit('skr:lobby:connect', JSON.stringify({
      address: selected.address,
      nickname: nicknameValue()
    }))) {
      connectPending = false;
      updateNicknameState(false);
      hideConnection();
      toast('Clientul nativ nu este disponibil pentru conectare.');
    }
  }

  function confirmAction(action) {
    previousFocus = document.activeElement;
    pendingAction = action;
    if (action && action.type === 'connect') {
      $('modalTitle').textContent = 'Server fără suport nativ SKR';
      $('modalText').textContent  = (action.server.name || action.server.address) +
        ' nu are suport nativ pentru SKR Client. Lobby-ul, intro-ul și resursele SKR pot lipsi. Continui conectarea?';
    } else if (action === 'quit') {
      $('modalTitle').textContent = 'Ieși din joc?';
      $('modalText').textContent  = 'Jocul se va închide complet. Confirmă pentru a opri procesul.';
    } else {
      $('modalTitle').textContent = 'Te deconectezi?';
      $('modalText').textContent  = 'Vei reveni la lista de servere. (Phase 1: deconectarea reală vine în Phase 2.)';
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
    $('modeLabel').textContent  = mode === 'pause' ? 'ÎN JOC' : 'OFFLINE';
    $('escapeLabel').textContent = mode === 'pause' ? 'Continuă jocul' : 'Ieși din joc';
  }

  function setMode(next) {
    mode = next === 'pause' ? 'pause' : 'lobby';
    document.body.classList.toggle('transparent', mode === 'pause');
    buildNav();
    showPage(mode === 'pause' ? 'settings' : 'servers');
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
    var favorite = ev.target.closest ? ev.target.closest('[data-favorite-id]') : null;
    if (favorite) {
      var favoriteId = favorite.dataset.favoriteId;
      favoriteServers[favoriteId] = !favoriteServers[favoriteId];
      renderServerBrowser();
      return;
    }
    var tab = ev.target.closest ? ev.target.closest('[data-tab]') : null;
    if (tab) { currentTab = tab.dataset.tab; renderFields(); return; }
    var act = ev.target.closest ? ev.target.closest('[data-action]') : null;
    if (act) {
      if (act.dataset.action === 'connect') {
        if (connectPending) return;
        if (!updateNicknameState(true)) { $('nickname').focus(); return; }
        var selected = allServers().filter(function (server) { return server.id === act.dataset.serverId; })[0];
        if (!selected || !selected.address) {
          toast('Serverul nu are o adresă de conectare configurată.');
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
      nicknameTimer = setTimeout(function () { toast('Salvarea profilului nu a fost confirmată de client.'); }, 4000);
    }
  });
  document.addEventListener('click', function (e) {
    if (!$('profilePopover').hidden && !e.target.closest('.player-profile')) setProfileOpen(false);
  });
  $('serverSearch').addEventListener('input', renderServerBrowser);
  $('refreshServers').addEventListener('click', function () {
    $('refreshServers').classList.add('refreshing');
    $('directoryStatus').textContent = 'Se actualizează catalogul…';
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
      serverFilter = button.dataset.serverFilter;
      document.querySelectorAll('[data-server-filter]').forEach(function (item) {
        item.classList.toggle('active', item === button);
      });
      renderServerBrowser();
    });
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
      toast('Salvarea necesită clientul GTA și bridge-ul local CEF.');
      return;
    }
    settingsTimer = setTimeout(function () {
      pendingSettings = null;
      updateDirty();
      toast('Salvarea nu a fost confirmată. Modificările rămân neaplicate.');
    }, 4000);
  });
  $('cancelModal').addEventListener('click', closeModal);
  $('confirmModalButton').addEventListener('click', function () {
    var action = pendingAction;
    closeModal();
    if (action && action.type === 'connect') startConnect(action.server);
    else if (action === 'quit') cefEmit('skr:lobby:quit');
    else if (action === 'disconnect') { cefEmit('skr:lobby:disconnect'); toast('Phase 2: deconectarea nu este încă implementată.'); }
  });

  document.addEventListener('keydown', function (e) {
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
      if (!$('confirmModal').hidden) { closeModal(); return; }
      // Phase 1: Esc quits in both modes (pause isn't driven yet).
      confirmAction('quit');
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
    toast('Preferințe salvate. Afișarea se aplică la următoarea pornire.');
  });
  cefOn('skr:lobby:settings:error', function () {
    clearTimeout(settingsTimer);
    pendingSettings = null;
    updateDirty();
    toast('Setările nu au fost salvate. Încearcă din nou.');
  });
  cefOn('skr:lobby:nickname:saved', function (name) {
    clearTimeout(nicknameTimer);
    setProfileOpen(false);
    toast('Profil salvat pentru ' + String(name) + '.');
  });
  cefOn('skr:lobby:nickname:error', function () {
    clearTimeout(nicknameTimer);
    toast('Nickname-ul nu a putut fi salvat.');
  });
  cefOn('skr:lobby:connect:error', function (code) {
    var timedOut = String(code || '') === 'timeout';
    connectPending = timedOut;
    updateNicknameState(false);
    if (timedOut) {
      showConnection('Conectarea durează mai mult…', 'Serverul nu a răspuns încă. Încercăm în continuare.');
    } else {
      hideConnection();
      toast('Conectarea a eșuat. Verifică versiunea samp.dll și jurnalul SKR.');
    }
  });
  cefOn('skr:lobby:connect:started', function () {
    connectPending = true;
    updateNicknameState(false);
    setConnectionStep('world');
    showConnection('Se încarcă lumea…', 'San Andreas se pregătește pentru sesiune.');
  });
  cefOn('skr:lobby:connect:phase', function (phase) {
    var phases = {
      server:   ['server', 'Conectare la server…', 'Așteptăm răspunsul serverului.'],
      restart:  ['server', 'Serverul repornește…', 'Reconectarea pornește automat.'],
      waitjoin: ['join', 'Serverul pregătește sesiunea…', 'Mai durează câteva momente.'],
      join:     ['join', 'Intrare în joc…', 'Conectat. Se încarcă personajul.']
    };
    var p = phases[String(phase || '')];
    if (!p) return;
    setConnectionStep(p[0]);
    showConnection(p[1], p[2]);
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
      showConnection('Intrare în joc…', 'Resursele serverului sunt pregătite.');
      return;
    }
    $('connectionDownload').textContent = ' · ' + formatMb(received) + ' / ' + formatMb(total) + ' MB (' + percent + '%)';
    setConnectionStep('download');
    showConnection('Descărcare resurse…', (Number(d.files) || 0) + ' fișier(e) de la server.');
  });
  cefOn('skr:lobby:connect:connected', function () {
    connectPending = false;
    setConnectionStep('join', true);
    showConnection('Conectat', 'Bun venit pe server.');
  });
  cefOn('skr:lobby:mode', function () {
    try { setMode(String(arguments[0] || 'lobby')); } catch (e) {}
  });
  cefOn('skr:lobby:servers', function (json) { loadServers(json); });
  cefOn('skr:lobby:servers:personal', loadPersonalServers);
  cefOn('skr:lobby:servers:info', loadServerInfo);
  cefOn('skr:lobby:servers:error', function (code) {
    pendingAddAddress = '';
    pendingRemoveAddress = '';
    updateAddServerState();
    if (!$('addServerModal').hidden) setAddServerError(serverErrorText(code));
    else toast(serverErrorText(code));
  });

  // Public hook for the native adapter (Phase 2).
  window.skrLobby = { setMode: setMode };

  buildNav();
  renderFields();
  setMode('lobby');          // Phase 1 default
  if (typeof cef === 'undefined' || !cef.emit) loadServers(previewServers);
  else cefEmit('skr:lobby:servers:request');
  updateNicknameState(false);
  requestLoad();
})();
