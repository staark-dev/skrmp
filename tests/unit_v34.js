// V3.4 regression tests: website normalization, live rules, and on-demand native query contract.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.join(__dirname, '..');
const src = fs.readFileSync(path.join(root,'client/lobby/lobby.js'), 'utf8');
function between(a,b) {
  const i=src.indexOf(a),j=src.indexOf(b,i+a.length);
  assert(i>=0 && j>i,`Missing source section ${a}`);
  return src.slice(i,j);
}
const sent=[];
const clock={now:()=>1000000};
const sandbox={URL,Date:clock,liveInfo:Object.create(null),detailsQueryAt:Object.create(null),
  renderServerBrowser(){this.count=(this.count||0)+1;},
  cef:{emit(){ }},
  cefEmit(event,payload){sent.push([event,payload]);return true;},
  console};
vm.createContext(sandbox);
vm.runInContext(between('  function normalizeAddress(', '  // Live query results'), sandbox);
vm.runInContext(between('  var liveInfo = Object.create(null);', '  // Remote artwork'), sandbox);
vm.runInContext(between('  // Remote artwork','  function personalAsServer('),sandbox);
vm.runInContext(between('  function applyLiveInfo(', '  function loadPersonalServers('), sandbox);
let count=0;
function check(test,label){assert(test,label);count++;}
check(sandbox.safeWebsiteUrl('mrp.mk')==='https://mrp.mk/', 'bare rule weburl normalizes to https');
check(sandbox.safeWebsiteUrl('https://open.mp/')==='https://open.mp/', 'https external link preserved');
check(sandbox.safeWebsiteUrl('javascript:alert(1)')==='', 'unsafe scheme rejected');
check(sandbox.safeImageUrl('data:image/svg+xml,x')==='', 'data image URI rejected');
sandbox.queryDetailsOnce('EXAMPLE.ORG:7777');
check(sent.length===1 && sent[0][0]==='skr:lobby:servers:query' && sent[0][1]==='example.org:7777','query bridge validates and normalizes address');
sandbox.queryDetailsOnce('example.org:7777');
check(sent.length===1,'one native query per address during TTL');
sandbox.queryDetailsOnce('example.org:99999');
check(sent.length===1,'invalid address not sent');
sandbox.loadServerInfo(JSON.stringify([{address:'example.org:7777',online:true,players:24,maxPlayers:100,ping:41,name:'Sample',mode:'Freeroam',language:'EN',map:'Los Santos',website:'mrp.mk',version:'omp 1.5',allowedClients:'0.3.7, 0.3.DL',artwork:'Yes',rulesKnown:true,logo:'https://example.org/logo.png',bannerDark:'https://example.org/banner.png',skr:false}]));
const info=sandbox.liveInfo['example.org:7777'];
check(info&&info.map==='Los Santos'&&info.website==='mrp.mk','live rules parsed');
check(info.allowedClients==='0.3.7, 0.3.DL'&&info.artwork==='Yes','resource capabilities carried');
check(info.bannerDark==='https://example.org/banner.png','safe image metadata accepted');
const server={address:'example.org:7777',offline:true};
sandbox.applyLiveInfo(server);
check(server.offline===false&&server.logo==='https://example.org/logo.png','live response clears stale offline and enriches artwork');
const nativeQuery=fs.readFileSync(path.join(root,'client/lobby/server_query.cpp'),'utf8');
for (const name of ['mapname','weburl','allowed_clients','artwork','version']) {
  check(nativeQuery.includes(`name == "${name}"`),`native query handles ${name}`);
}
for (const name of ['allowedClients','artwork','map','website','bannerDark','rulesKnown']) {
  check(nativeQuery.includes(`\\"${name}\\"`),`native serializer exposes ${name}`);
}
const windowCpp=fs.readFileSync(path.join(root,'apps/launcher/lobby_window.cpp'),'utf8');
check(windowCpp.includes('skr:lobby:servers:query') && windowCpp.includes('std::async'), 'launcher implements bounded asynchronous details query');
const html=fs.readFileSync(path.join(root,'client/lobby/lobby.html'),'utf8');
check(html.includes('id="detailsResources"')&&html.includes('id="detailsResourcesStatus"'),'resources DOM present');
console.log(`V3.4 unit tests: ${count} checks, 0 failures`);
