// Dependency-free tests for key V3.3 paths. Run: node tests/unit_v33.js
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const src = fs.readFileSync(path.join(__dirname, '../client/lobby/lobby.js'), 'utf8');
function sourceBetween(a, b) {
  let start = src.indexOf(a), end = src.indexOf(b, start + a.length);
  assert(start >= 0 && end > start, 'Missing source interval ' + a);
  return src.slice(start, end);
}
const store = Object.create(null);
const sandbox = {
  URL,
  recentServers: [],
  pendingConnectionServer: null,
  previousRecentEntry: null,
  renderCount: 0,
  renderServerBrowser() { this.renderCount++; },
  writeLocal(key, value) { store[key] = JSON.parse(JSON.stringify(value)); },
  Date,
};
vm.createContext(sandbox);
vm.runInContext(sourceBetween('  function normalizeAddress(', '  // Live query results'), sandbox);
vm.runInContext(sourceBetween('  // Remote artwork', '  function personalAsServer('), sandbox);
// DOM helpers above aren't called in this test; only URL filtering functions are.
vm.runInContext(sourceBetween('  function saveRecentServer(', '  function setServerFilter('), sandbox);
let count = 0;
function check(expr, desc) { assert(expr, desc); count++; }
check(sandbox.normalizeAddress('EXAMPLE.ORG:7777') === 'example.org:7777', 'normalizes endpoint');
check(sandbox.normalizeAddress('example.org:99999') === '', 'rejects invalid port');
check(sandbox.safeImageUrl('https://example.org/banner.png') === 'https://example.org/banner.png', 'accept HTTPS banner');
check(sandbox.safeImageUrl('http://example.org/banner.png') === '', 'reject HTTP image');
check(sandbox.safeImageUrl('javascript:alert(1)') === '', 'reject script URI');
check(sandbox.safeImageUrl('https://user:secret@example.org/p.png') === '', 'reject credentials in image URL');
check(sandbox.safeImageUrl('https://localhost/banner.png') === '', 'reject localhost image');
check(sandbox.safeWebsiteUrl('https://example.org/') === 'https://example.org/', 'accept HTTPS site');
check(sandbox.safeWebsiteUrl('javascript:alert(1)') === '', 'reject invalid site');
const s = {address:'8.8.8.8:7777',name:'Example Racing'};
sandbox.saveRecentServer(s, 'requested');
check(store['skr.launcher.recent.v3'][0].status === 'requested', 'persist selected server without claiming connection');
check(store['skr.launcher.recent.v3'][0].at > 0, 'store time');
sandbox.saveRecentServer(s, 'connected');
check(store['skr.launcher.recent.v3'].length === 1 && store['skr.launcher.recent.v3'][0].status === 'connected', 'upgrade without duplicate');
sandbox.previousRecentEntry = JSON.parse(JSON.stringify(store['skr.launcher.recent.v3'][0]));
sandbox.pendingConnectionServer = s;
sandbox.saveRecentServer(s, 'requested');
sandbox.forgetFailedLaunch();
check(store['skr.launcher.recent.v3'].length === 1 && store['skr.launcher.recent.v3'][0].status === 'connected', 'failed reconnect restores original');
sandbox.pendingConnectionServer = {address:'1.1.1.1:7777',name:'Other'};
sandbox.saveRecentServer(sandbox.pendingConnectionServer, 'requested');
sandbox.forgetFailedLaunch();
check(store['skr.launcher.recent.v3'].length === 1 && store['skr.launcher.recent.v3'][0].address === s.address, 'failed new server not saved as recent');
check(sandbox.artworkFor({bannerDark:'https://example.org/a.jpg', logo:'https://example.org/logo.png'}).banner === 'https://example.org/a.jpg', 'prefers dark banner');
check(sandbox.artworkFor({bannerDark:'http://example.org/a.jpg',bannerLight:'https://example.org/b.jpg'}).banner === 'https://example.org/b.jpg', 'fallback when invalid dark banner');
console.log(`V3.3 unit tests: ${count} checks, 0 failures`);
