const fs = require('fs');
const path = require('path');
const root = path.resolve(__dirname, '..');
const read = (p) => fs.readFileSync(path.join(root,p), 'utf8');
const tests = [
  ['native fps engine is linked',()=> read('CMakeLists.txt').includes('client/render-d3d9/fps_engine.cpp')],
  ['fps preference source linked into both executables',()=> (read('CMakeLists.txt').match(/shared\/performance\/fps_settings.cpp/g)||[]).length === 2],
  ['present hook measures completed frames',()=> read('client/render-d3d9/d3d9_hook.cpp').includes('fps::Presented(result')],
  ['fps overlay is gated by in-game state',()=> read('client/render-d3d9/d3d9_hook.cpp').includes('!introActive && IsPlayerInGame()')],
  ['reset releases stateblock',()=> read('client/render-d3d9/d3d9_hook.cpp').includes('fps::BeforeReset()')],
  ['native D3D state is restored',()=> read('client/render-d3d9/fps_engine.cpp').includes('g_savedState->Apply()')],
  ['fps cap disabled by default',()=> read('shared/performance/fps_settings.h').includes('unsigned cap = 0')],
  ['preferences validated',()=> read('shared/performance/fps_settings.cpp').includes('IsValidCap(value.cap)')],
  ['preferences saved to LocalAppData',()=> read('shared/performance/fps_settings.cpp').includes('FOLDERID_LocalAppData')],
  ['save bridge present',()=> read('apps/launcher/lobby_window.cpp').includes('skr:fps:save')],
  ['load bridge present',()=> read('apps/launcher/lobby_window.cpp').includes('skr:fps:get')],
  ['FPS UI elements present',()=> ['fpsEnginePanel','fpsCounterEnabled','fpsCapSetting','saveFpsSettings'].every(x=>read('client/lobby/lobby.html').includes('id="'+x+'"'))],
  ['FPS save JS wired',()=> read('client/lobby/lobby.js').includes("cefEmit('skr:fps:save', payload)")],
  ['FPS panel graphics-only',()=> read('client/lobby/lobby.js').includes("currentTab !== 'graphics'")],
  ['no memory overwrite of GTA limiter',()=> !/VirtualProtect|WriteProcessMemory|PatchVtable/.test(read('client/render-d3d9/fps_engine.cpp'))],
];
let failed=0;
tests.forEach(([label,test])=>{const ok=test();console.log((ok?'[OK] ':'[FAIL] ')+label);if(!ok)failed++});
console.log(`V3.5 unit checks: ${tests.length} checks, ${failed} failures`);
if(failed)process.exitCode=1;
