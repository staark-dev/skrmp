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
//   emit "skr:lobby:play"       (JSON{addr}) -> stub (Phase 2 connect)
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
    $('modeLabel').textContent  = mode === 'pause' ? 'MENIU ÎN JOC' : 'ÎNAINTE DE CONECTARE';
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
    var tab = ev.target.closest ? ev.target.closest('[data-tab]') : null;
    if (tab) { currentTab = tab.dataset.tab; renderFields(); return; }
    var act = ev.target.closest ? ev.target.closest('[data-action]') : null;
    if (act) {
      if (act.dataset.action === 'connect') {
        cefEmit('skr:lobby:play', JSON.stringify({ address: act.dataset.addr || '' }));
        toast('Phase 2 (connect) va fi implementată separat.');
      } else if (act.dataset.action === 'open-settings') {
        showPage('settings');
      }
    }
  });
  $('resetChanges').addEventListener('click', function () {
    state = JSON.parse(JSON.stringify(saved));
    renderFields();
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

  // Public hook for the native adapter (Phase 2).
  window.skrLobby = { setMode: setMode };

  buildNav();
  renderFields();
  setMode('lobby');          // Phase 1 default
  requestLoad();
})();
