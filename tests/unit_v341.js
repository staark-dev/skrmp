// V3.4.1 regression: viewport-bound ping, ping failures stay unknown, compact resources.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.join(__dirname, '..');
const js = fs.readFileSync(path.join(root, 'client/lobby/lobby.js'),'utf8');
const native = fs.readFileSync(path.join(root, 'apps/launcher/lobby_window.cpp'),'utf8');
const query = fs.readFileSync(path.join(root, 'client/lobby/server_query.cpp'),'utf8');
const css = fs.readFileSync(path.join(root, 'client/lobby/lobby.css'),'utf8');
const html = fs.readFileSync(path.join(root, 'client/lobby/lobby.html'),'utf8');
const snippet = (left,right) => {
  let a=js.indexOf(left),b=js.indexOf(right,a+left.length);
  assert(a>=0 && b>a,`section missing: ${left}`);
  return js.slice(a,b);
};
let passed=0;
function check(v,reason) { assert(v,reason);passed++; }
const requests=[];
const rows=[];
const indicator=(address) => {
  const node={textContent:'—'};
  const classes = new Set(['is-unknown']);
  const ic={querySelector:(selector) => selector === 'span'?node:null,
    classList:{remove:(name)=>classes.delete(name),add:(name)=>classes.add(name)},title:''};
  const row={dataset:{serverAddress:address},querySelector:(selector)=>selector==='.inline-ping'?ic:null};
  rows.push(row);
  return {node,ic,classes};
};
const a=indicator('example.org:7777');
const b=indicator('noresponse.org:8888');
const el={querySelectorAll:()=>rows};
const time={now:()=>1000000};
const context={
  Date:time,
  pingRequestTimer:0,
  pingSamples:Object.create(null),
  pingRequestedAt:Object.create(null),
  liveInfo:Object.create(null),
  visiblePingAddresses:new Set(['example.org:7777','noresponse.org:8888']),
  PING_RETRY_MS:45000,
  currentPage:'servers',
  cef:{emit(){}},
  $:(id)=>{assert.equal(id,'publicServerList');return el;},
  normalizeAddress:(address)=>/^[-.a-z0-9]+:[0-9]{1,5}$/.test(address)?address:'',
  cefEmit:(name,payload)=>{requests.push([name,payload]);return true;},
  clearTimeout:()=>{},
  setTimeout:(fn)=>{fn();return 1;},
  window:{IntersectionObserver:false},
  console,
};
vm.createContext(context);
vm.runInContext(snippet('  function scheduleVisiblePings()', '  function renderServerBrowser()'),context);
context.scheduleVisiblePings();
check(requests.length===1,'one bounded request for visible servers');
check(requests[0][0]==='skr:lobby:servers:ping','dedicated visible ping bridge');
check(requests[0][1]==='example.org:7777;noresponse.org:8888','only visible addresses requested');
context.scheduleVisiblePings();
check(requests.length===1,'duplicate ping requests suppressed');
context.loadVisiblePing(JSON.stringify([{address:'example.org:7777',online:true,ping:37}]));
check(a.node.textContent==='37 ms','online ping displayed');
check(context.pingSamples['example.org:7777'].ping===37,'latency cache updated');
check(!a.classes.has('is-unknown'),'successful ping no longer grey unknown');
context.loadVisiblePing(JSON.stringify([{address:'noresponse.org:8888',online:false,ping:0}]));
check(b.node.textContent==='—' && b.classes.has('is-unknown'),'timeout remains unknown, not offline');
check(!context.pingSamples['noresponse.org:8888'],'failed ping never converted to fake 0ms');
context.currentPage='home';
context.visiblePingAddresses.add('another.org:9999');
context.scheduleVisiblePings();
check(requests.length===1,'home page does not bulk ping');
check(native.includes('std::vector<std::string> queuedVisiblePings') && native.includes('next.size() < 12'), 'native bounded queue');
check(native.includes('queuedVisiblePings = std::move(next)'), 'scroll replaces stale queue');
check(native.includes('QueryServer(address, 250, false)'), 'list pings skip rules packet');
check(query.includes('if (info.online && includeRules)'), 'optional rules query in UDP implementation');
check(native.includes('state->Emit("skr:lobby:servers:ping"'), 'native ping response event');
check(css.includes('.server-details__resources {grid-column:3;grid-row:1/span 4;align-self:start}'), 'resources constrained to right rail');
check(css.includes('max-width:100%') && css.includes('min-height:0'), 'compact cards without forced height');
check(html.includes('class="server-details__identity"'), 'details identity has explicit column');
check(js.includes('list.scrollTop = previousScroll;'), 'list scroll preserved after live updates');
console.log(`V3.4.1 unit tests: ${passed} checks, 0 failures`);
