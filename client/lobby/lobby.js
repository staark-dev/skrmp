// SKR Lobby — Phase 1. Ported from launcher-ui/ingame-menu/menu.js.
// Mode handling:
//   - "lobby" (pre-connect): nav = Servere / Setări / Ieșire, landing = Servere.
//   - "pause" (in-game):     nav = Continuă / Hartă / Setări / Deconectare / Ieșire.
// Phase 1 defaults to "lobby" because the native adapter has no pause coupling
// yet. Phase 2 flips to "pause" from C++ via `window.skrLobby.setMode('pause')`.
//
// CEF bridge contract (unchanged from previous Phase 1):
//   emit "skr:lobby:settings:get"            -> C++ writes settings.json next to us
//   emit "skr:lobby:settings:save" (JSON)    -> stub until bridge gains list readers
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
  var publicServers = [];
  var serverFilter = 'all';
  var favoriteServers = Object.create(null);

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
      button.disabled = !valid;
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
      ['resolution','Rezoluție','Dimensiunea imaginii jocului.','select',['Automat','1280 × 720','1366 × 768','1600 × 900','1920 × 1080','2560 × 1440']],
      ['screen','Mod de afișare','Aplicarea în joc necesită confirmare și revenire la eșec.','select',['Fullscreen','Fereastră','Fereastră fără margini']],
      ['distance','Distanță de afișare','Distanța la care sunt desenate elementele lumii.','range'],
      ['effects','Calitatea efectelor','Nivelul detaliilor efectelor vizuale.','select',['Scăzută','Medie','Ridicată','Foarte ridicată']],
      ['antialias','Anti-aliasing','Netezirea contururilor.','select',['Dezactivat','2×','4×','8×']]
    ]},
    audio:{title:'Audio',eyebrow:'SUNET ȘI AMBIANȚĂ',fields:[
      ['effects','Efecte sonore','Vehicule, pași și sunetele lumii.','range'],
      ['radio','Radio','Volumul posturilor radio din joc.','range'],
      ['volume','Audio SKR','Volumul elementelor audio ale clientului.','range']
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
    $('applyChanges').disabled = !changed;
  }
  function toast(text) {
    $('toast').textContent = text;
    $('toast').hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { $('toast').hidden = true; }, 3600);
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

  function filteredServers() {
    var query = normalized($('serverSearch').value.trim());
    return publicServers.filter(function (server) {
      if (serverFilter === 'recommended' && !server.recommended) return false;
      if (serverFilter === 'favorites' && !favoriteServers[server.id]) return false;
      if (!query) return true;
      return [server.name, server.mode, server.language].some(function (value) {
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
      var occupancy = Math.max(0, Math.min(100, Math.round((server.players / server.maxPlayers) * 100)));
      var row = document.createElement('article');
      row.className = 'inline-server-row';
      row.innerHTML =
        '<div class="inline-server-identity"><span class="server-mark">' + html(server.shortName) + '</span><span class="server-copy"><span class="server-name-line"><strong>' + html(server.name) + '</strong>' + (server.recommended ? '<i class="recommended-tag">RECOMANDAT</i>' : '') + '</span><small>' + html(server.description) + '</small></span></div>' +
        '<div class="inline-server-mode"><strong>' + html(server.mode) + '</strong><small>' + html(String(server.language).toUpperCase()) + '</small></div>' +
        '<div class="inline-population"><span><strong>' + html(server.players) + '</strong> / ' + html(server.maxPlayers) + '</span><i class="population-track"><i style="width:' + occupancy + '%"></i></i></div>' +
        '<div class="inline-ping"><i></i><span>' + html(server.ping) + ' ms</span></div>' +
        '<div class="inline-server-actions"><button class="inline-favorite" type="button" data-favorite-id="' + html(server.id) + '" aria-label="Favorite">' + (favoriteServers[server.id] ? '★' : '☆') + '</button><button class="button button--gold inline-connect" data-action="connect" data-server-id="' + html(server.id) + '"' + (nicknameIsValid(nicknameValue()) ? '' : ' disabled') + '>Conectează <span>→</span></button></div>';
      list.appendChild(row);
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
          language: String(server.language || 'RO').slice(0, 5),
          players: Math.max(0, Number(server.players) || 0),
          maxPlayers: Math.max(1, Number(server.maxPlayers) || 1),
          ping: Math.max(0, Number(server.ping) || 0),
          recommended: !!server.recommended,
          description: String(server.description || 'Server public din rețeaua SKR.'),
          order: Number(server.order) || index
        };
      }).filter(Boolean).sort(function (a, b) { return a.order - b.order; });
      $('refreshServers').classList.remove('refreshing');
      $('directoryStatus').textContent = publicServers.length
        ? 'Catalog actualizat · ' + publicServers.length + (publicServers.length === 1 ? ' server' : ' servere')
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
        holder.textContent = 'Configurare taste';
        holder.addEventListener('click', function () {
          toast('Phase 1: configurarea tastelor native va fi conectată în Phase 2.');
        });
      }
      if (input) {
        input.id = 'setting-' + f[0];
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
  function confirmAction(action) {
    previousFocus = document.activeElement;
    pendingAction = action;
    if (action === 'quit') {
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
        if (!updateNicknameState(true)) { $('nickname').focus(); return; }
        cefEmit('skr:lobby:connect', JSON.stringify({
          serverId: act.dataset.serverId || '',
          nickname: nicknameValue()
        }));
        toast('Se pregătește conexiunea pentru ' + nicknameValue() + '…');
      } else if (act.dataset.action === 'open-settings') {
        showPage('settings');
      }
    }
  });
  $('resetChanges').addEventListener('click', function () {
    state = JSON.parse(JSON.stringify(saved));
    renderFields();
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
      setProfileOpen(false);
      toast('Profil salvat pentru ' + nicknameValue() + '.');
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
    saved = JSON.parse(JSON.stringify(state));
    updateDirty();
    cefEmit('skr:lobby:settings:save', JSON.stringify(state));
    toast('Phase 1: setările se păstrează doar în memorie. Persistența reală vine în Phase 2.');
  });
  $('cancelModal').addEventListener('click', closeModal);
  $('confirmModalButton').addEventListener('click', function () {
    var action = pendingAction;
    closeModal();
    if (action === 'quit') cefEmit('skr:lobby:quit');
    else if (action === 'disconnect') { cefEmit('skr:lobby:disconnect'); toast('Phase 2: deconectarea nu este încă implementată.'); }
  });

  document.addEventListener('keydown', function (e) {
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

  cefOn('skr:lobby:settings:ready', function () {
    fetch('settings.json', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.text() : '{}'; })
      .then(hydrateFromSidecar)
      .catch(function () {});
  });
  cefOn('skr:lobby:mode', function () {
    try { setMode(String(arguments[0] || 'lobby')); } catch (e) {}
  });
  cefOn('skr:lobby:servers', function (json) { loadServers(json); });

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
