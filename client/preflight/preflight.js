(function(){
  'use strict';
  var view=document.getElementById('view');
  var toast=document.getElementById('toast');
  var snapshot=null;
  var selectedIndex=-1;
  var current='verify';
  var bridge=window.chrome&&window.chrome.webview;
  function send(type,fields){
    if(!bridge)return false;
    var message={v:1,type:type};
    Object.keys(fields||{}).forEach(function(key){message[key]=fields[key]});
    bridge.postMessage(message);return true;
  }
  function node(tag,className,text){var el=document.createElement(tag);if(className)el.className=className;if(text!=null)el.textContent=text;return el}
  function logo(){var img=node('img','logo');img.src='assets/logo.png';img.alt='SKR Enhanced';return img}
  function pill(status,text){return node('span','status-pill '+(status||'pending'),text||status||'Pending')}
  function conciseCandidateStatus(candidate){
    var status=String(candidate&&candidate.status||'pending');
    var summary=String(candidate&&candidate.summary||'').toLowerCase();
    var version=String(candidate&&candidate.version||'');
    if(candidate&&(candidate.approved||candidate.compatibilityApproved)||status==='verified')return 'Compatible';
    if(status==='pending')return 'Verification pending';
    if(summary.indexOf('explicitly blocked')>=0||summary.indexOf('blocked identity')>=0)return 'Executable blocked';
    if(version.indexOf('Hoodlum')>=0)return 'Hoodlum · Not certified';
    return 'Executable blocked';
  }
  function hexValue(value){var number=Number(value);return Number.isFinite(number)?'0x'+Math.max(0,number).toString(16).toUpperCase().padStart(8,'0'):'Unavailable'}
  function byteValue(value){var number=Number(value);return Number.isFinite(number)?number.toLocaleString('en-US')+' bytes':'Unavailable'}
  function identityRow(label,value,options){options=options||{};var row=node('div','identity-row'+(options.wide?' identity-row--wide':''));row.appendChild(node('dt','',label));var data=node('dd',options.mono?'mono':'',value||'Unavailable');if(options.title)data.title=options.title;row.appendChild(data);return row}
  function showToast(text){toast.textContent=String(text||'The native host rejected that request.');toast.hidden=false;setTimeout(function(){toast.hidden=true},3400)}
  function resize(expanded){send('preflight.window.resize',{expanded:!!expanded})}
  function clear(){view.textContent='';view.focus({preventScroll:true})}
  function renderVerify(){
    current='verify';resize(false);clear();
    var screen=node('section','screen verify');
    var brand=node('div','logo-stage');brand.appendChild(logo());screen.appendChild(brand);
    var bottom=node('div','verify-bottom');
    var labels=node('div','stage-label');labels.append(node('strong','', 'Starting native verification…'),node('span','', '0 / 5'));
    var bar=node('div','bar');bar.appendChild(node('i'));
    bottom.append(labels,bar,node('p','substatus','Waiting for the native verifier…'));
    var meta=node('div','verify-meta');meta.append(node('span','','SKR ENHANCED CLIENT'),node('span','','SECURE PREFLIGHT'));bottom.appendChild(meta);
    screen.appendChild(bottom);view.appendChild(screen);
  }
  function updateProgress(payload){
    if(current!=='verify')renderVerify();
    var label=view.querySelector('.stage-label strong'),count=view.querySelector('.stage-label span'),fill=view.querySelector('.bar i'),sub=view.querySelector('.substatus');
    if(label)label.textContent=payload.label||'Verifying game environment…';
    if(count)count.textContent=Math.min(payload.index||0,payload.total||5)+' / '+(payload.total||5);
    if(fill)fill.style.setProperty('--progress',Math.max(4,Math.min(100,((payload.index||0)/(payload.total||5))*100))+'%');
    if(sub)sub.textContent=payload.summary||stageCopy(payload.stage);
  }
  function stageCopy(stage){var copy={'locating-gta':'Discovering installations…','verifying-executable':'Validating game executable…','scanning-samp':'Inspecting SA-MP components…','checking-dependencies':'Checking runtime dependencies…','validating-runtime':'Validating SKR Enhanced components…','complete':'Verification complete.'};return copy[stage]||'Verifying content…'}
  function selectedCandidate(){if(!snapshot||!Array.isArray(snapshot.candidates))return null;return snapshot.candidates.filter(function(c){return c.index===snapshot.selectedIndex})[0]||null}
  function renderFailure(){
    current='failure';resize(false);clear();var blocker=snapshot&&snapshot.blocker||{};var candidate=selectedCandidate();
    var screen=node('section','screen result');var brand=node('div','result-brand');brand.appendChild(logo());screen.appendChild(brand);
    screen.append(node('div','result-icon','!'),node('h1','','Game verification needs attention'),node('p','',blocker.summary||"We couldn't verify the selected GTA San Andreas installation."));
    var path=node('div','selected-path');path.appendChild(node('span','selected-path__drive','SA'));var pathText=node('div');pathText.append(node('strong','',candidate?candidate.name:'No installation selected'),node('code','',candidate?candidate.path:'Choose a GTA San Andreas folder'));path.append(pathText,pill(blocker.status||'action-required','Not approved'));screen.appendChild(path);
    var actions=node('div','result-actions');actions.append(button('Choose installation','button primary','choose'),button('Retry verification','button secondary','retry'),button('View details','button secondary','details'));screen.appendChild(actions);
    var foot=node('div','result-footer');var checks=snapshot&&snapshot.checks||{};foot.append(node('span','',(checks.dependencies&&checks.dependencies.status==='verified'?'Dependencies verified':'Dependencies need attention')+' · '+(checks.runtime&&checks.runtime.status==='verified'?'Runtime verified':'Runtime needs attention')),node('span','',((snapshot&&snapshot.asiLoaders||[]).length?'1 warning':'No warnings')+' · 1 blocker'));screen.appendChild(foot);view.appendChild(screen);
  }
  function button(text,className,action){var b=node('button',className,text);b.type='button';b.dataset.action=action;return b}
  function renderSelection(){
    current='select';resize(true);clear();var screen=node('section','screen panel');
    var heading=node('div','panel-heading');var copy=node('div');copy.append(node('span','micro','GAME INSTALLATION'),node('h1','','Choose San Andreas'),node('p','','Every selection is verified by the native engine before SKR Enhanced can continue.'));heading.append(copy,button('×','close-panel','back'));screen.appendChild(heading);
    var list=node('div','install-list');var candidates=snapshot&&snapshot.candidates||[];
    if(!candidates.length)list.appendChild(node('div','empty-installations','No GTA San Andreas installations were detected. Browse to another folder.'));
    candidates.forEach(function(candidate){var row=button('','install-row'+(candidate.index===selectedIndex?' selected':''),'candidate');row.dataset.index=String(candidate.index);row.title=candidate.summary||'';var radio=node('span','radio');var info=node('span','install-row__info');var aliases=Array.isArray(candidate.aliases)?candidate.aliases.length:0;var name=node('strong','',candidate.name||'GTA San Andreas');name.title=candidate.name||'GTA San Andreas';var path=node('code','',candidate.path||'');path.title=candidate.path||'';info.append(name,path);if(aliases>1)info.append(node('small','install-row__aliases',String(aliases)+' linked paths'));var meta=node('span','install-row__meta');var status=pill(candidate.status,conciseCandidateStatus(candidate));status.title=candidate.summary||'';meta.append(status,node('small','',candidate.version||'Unknown'));row.append(radio,info,meta);list.appendChild(row)});screen.appendChild(list);
    var note=node('p','selection-note');note.append(node('span','','●'),document.createTextNode(' Selection never grants approval; the native verifier runs again before success.'));screen.appendChild(note);
    var actions=node('div','panel-actions');actions.append(button('Browse another folder','button secondary','browse'),node('span','panel-actions__state',selectedIndex>=0?'Installation selected':'Choose an installation'),button('Retry','button secondary','retry'),button('Verify selection','button primary','verify-selection'));actions.lastChild.disabled=selectedIndex<0;screen.appendChild(actions);view.appendChild(screen);
  }
  function renderDiagnostics(){
    current='diagnostics';resize(true);clear();var blocker=snapshot&&snapshot.blocker||{};var checks=snapshot&&snapshot.checks||{};var screen=node('section','screen panel');
    var heading=node('div','panel-heading');var copy=node('div');copy.append(node('span','micro','VERIFICATION DETAILS'),node('h1','','Recovery & diagnostics'),node('p','','Native results only. SKR will not change GTA files.'));heading.append(copy,button('×','close-panel','back'));screen.appendChild(heading);
    var summary=node('div','diag-summary');summary.append(node('span','diag-summary__icon','!'));var summaryText=node('div');summaryText.append(node('h2','',blocker.summary||'Verification requirement failed'),node('p','',blocker.remediation||'Choose another installation and retry.'));summary.append(summaryText,node('code','diag-code',blocker.code||'PREFLIGHT_FAILED'));screen.appendChild(summary);
    var req=node('div','requirement');var reqText=node('div');reqText.append(node('small','','FAILED REQUIREMENT'),node('strong','',blocker.detail||blocker.summary||'Native compatibility approval'));req.append(reqText,pill(blocker.status||'action-required','Blocking'));screen.appendChild(req);
    var grid=node('div','diag-grid');grid.append(diagItem('System dependencies',checks.dependencies),diagItem('Bundled SKR runtime',checks.runtime));screen.appendChild(grid);
    var loaders=snapshot&&snapshot.asiLoaders||[];if(loaders.length){var warning=node('div','warning-strip');warning.append(node('strong','','ASI loader detected · Warning'),node('p','',loaders.join(', ')+' — informational unless the native engine reports a blocking conflict.'));screen.appendChild(warning)}
    var identity=snapshot&&snapshot.executableIdentity||{};var identityPanel=node('section','identity-panel');var identityHead=node('div','identity-panel__heading');identityHead.append(node('strong','','Executable fingerprint'),node('span','mono',blocker.code||'UNKNOWN'));identityPanel.appendChild(identityHead);var identityGrid=node('dl','identity-grid');var selected=selectedCandidate()||{};identityGrid.append(identityRow('Executable family',identity.variant||selected.version||'Unknown'),identityRow('PE timestamp',hexValue(identity.timestamp),{mono:true}),identityRow('Image size',hexValue(identity.imageSize),{mono:true}),identityRow('File size',byteValue(identity.fileSize),{mono:true}),identityRow('SHA-256',identity.sha256||'Unavailable',{mono:true,wide:true,title:identity.sha256||''}),identityRow('Structural verification',identity.structuralMatch?'Verified':'Not verified'),identityRow('Exact fingerprint',identity.exactFingerprintVerified?'Verified':'Not verified'),identityRow('Compatibility approval',identity.compatibilityApproved?'Approved':'Blocked'),identityRow('Executable path',selected.exePath||selected.path||'Unavailable',{mono:true,wide:true,title:selected.exePath||selected.path||''}));identityPanel.appendChild(identityGrid);screen.appendChild(identityPanel);
    var nativeDetails=node('details','native-details');var nativeSummary=node('summary','','Full native diagnostic');var nativePre=node('pre','');nativePre.textContent=(blocker.detail||blocker.summary||'No native detail available.')+'\n\nRecovery: '+(blocker.remediation||'Choose another installation and retry.')+'\nSA-MP: '+((checks.samp||{}).summary||'Unavailable')+' ['+((checks.samp||{}).code||'UNKNOWN')+']';nativeDetails.append(nativeSummary,nativePre);screen.appendChild(nativeDetails);
    var actions=node('div','panel-actions');actions.append(button('Open diagnostic log','button link','log'),button('Retry verification','button secondary','retry'),button('Change installation','button primary','choose'));screen.appendChild(actions);view.appendChild(screen);
  }
  function diagItem(title,check){check=check||{};var item=node('article','diag-item');var top=node('div','diag-item__top');top.append(node('strong','',title),pill(check.status,check.status==='verified'?'Verified':'Attention'));item.append(top,node('p','',check.summary||'No native result available.'));return item}
  function renderSuccess(){
    current='success';resize(false);clear();var screen=node('section','screen success');var brand=node('div','success-logo');brand.appendChild(logo());screen.append(brand,node('div','success-check','✓'),node('h1','',"You're ready to play."),node('p','','SKR Enhanced verification complete.'));screen.appendChild(button('Continue to lobby','button primary','continue'));screen.appendChild(node('small','','Approved by the native compatibility engine'));view.appendChild(screen)
  }
  function renderFatal(){clear();var screen=node('section','fatal');var box=node('div');box.append(node('h1','','Native preflight unavailable'),node('p','','The production interface must be hosted by SKR Launcher. No mock verifier is enabled.'));screen.appendChild(box);view.appendChild(screen)}
  function acceptResult(payload){if(!payload||typeof payload.approved!=='boolean'||!payload.checks||!Array.isArray(payload.candidates))return;snapshot=payload;selectedIndex=typeof payload.selectedIndex==='number'?payload.selectedIndex:-1;if(payload.approved)renderSuccess();else renderFailure()}
  document.addEventListener('click',function(event){var target=event.target.closest('[data-action]');if(!target)return;var action=target.dataset.action;if(action==='choose'){selectedIndex=snapshot&&snapshot.selectedIndex>=0?snapshot.selectedIndex:-1;renderSelection()}else if(action==='back')renderFailure();else if(action==='details')renderDiagnostics();else if(action==='candidate'){selectedIndex=Number(target.dataset.index);renderSelection()}else if(action==='verify-selection'){send('preflight.installation.select',{index:selectedIndex});renderVerify()}else if(action==='browse')send('preflight.installation.browse');else if(action==='retry'){send('preflight.retry');renderVerify()}else if(action==='log')send('preflight.diagnostics.open-log');else if(action==='continue'){target.disabled=true;target.textContent='Opening lobby…';send('preflight.continue')}});
  document.getElementById('minimize').addEventListener('click',function(event){event.stopPropagation();send('preflight.window.minimize')});
  document.getElementById('closeWindow').addEventListener('click',function(event){event.stopPropagation();send('preflight.window.close')});
  document.getElementById('dragRegion').addEventListener('pointerdown',function(event){if(!event.target.closest('button'))send('preflight.window.drag')});
  if(bridge){bridge.addEventListener('message',function(event){var message=event.data;if(!message||message.v!==1||typeof message.type!=='string')return;if(message.type==='preflight.progress')updateProgress(message.payload||{});else if(message.type==='preflight.result')acceptResult(message.payload);else if(message.type==='preflight.verification.started')renderVerify();else if(message.type==='preflight.host.error')showToast(message.payload&&message.payload.message)});renderVerify();send('preflight.ready')}else renderFatal();
})();
