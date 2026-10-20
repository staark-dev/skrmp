"""Headless Chromium smoke test for the uploaded SKR Client V3.3 UI.
Run from project folder after installing Playwright Python + Chromium.
"""
import base64, http.server, functools, socketserver, threading, pathlib
from playwright.sync_api import sync_playwright

root = pathlib.Path(__file__).resolve().parents[1] / 'client' / 'lobby'
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self,*args): pass

handler = functools.partial(Quiet, directory=str(root))
httpd = socketserver.TCPServer(('127.0.0.1',0),handler)
threading.Thread(target=httpd.serve_forever,daemon=True).start()
serverurl = f'http://127.0.0.1:{httpd.server_address[1]}/lobby.html'
# valid 1x1 PNG, only for deterministic smoke UI test
png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=')

with sync_playwright() as p:
    browser = p.chromium.launch(headless=True, executable_path='/usr/bin/chromium', args=['--no-sandbox'])
    context = browser.new_context(viewport={'width':1366,'height':768})
    context.route('https://example.org/*', lambda route:route.fulfill(status=200, body=png, content_type='image/png'))
    context.add_init_script('''
      window.__native = { handlers: {}, emitted: [] };
      window.cef = {
        emit: function (name, payload) { window.__native.emitted.push({name:name,payload:payload}); },
        on: function (name, callback) { (window.__native.handlers[name] || (window.__native.handlers[name]=[])).push(callback); }
      };
    ''')
    page = context.new_page()
    page.goto(serverurl)
    page.wait_for_selector('.home-page:not([hidden])')
    fixture = [
        {'id':'omp:8.8.8.8:7777','name':'Example Racing','address':'8.8.8.8:7777','mode':'Racing','language':'English','players':30,'maxPlayers':100,'version':'1.5.8', 'map':'San Andreas', 'website':'https://example.org/', 'discord':'https://discord.gg/example', 'bannerDark':'https://example.org/banner.png', 'logo':'https://example.org/logo.png'},
        {'id':'omp:1.1.1.1:7777','name':'Another Freeroam','address':'1.1.1.1:7777','mode':'Freeroam','language':'English','players':3,'maxPlayers':55,'version':'1.4.0'},
    ]
    page.evaluate('data => (window.__native.handlers["skr:lobby:servers"] || []).forEach(fn => fn(JSON.stringify(data)))',fixture)
    assert page.locator('.inline-server-row').count() == 2
    assert page.locator('.inline-server-row').first.locator('img').count() == 1
    page.get_by_role('button',name='Servers',exact=True).click()
    page.locator('.server-details-link',has_text='Example Racing').click()
    assert page.locator('#detailsName').inner_text() == 'Example Racing'
    assert page.locator('#detailsBanner img').count() == 1
    facts = page.locator('#detailsFacts').inner_text()
    assert 'San Andreas' in facts and '1.5.8' in facts
    assert page.locator('#detailsLinks a').count() == 2
    page.locator('#detailsLinks a').first.click()
    assert page.locator('#externalLinkModal').is_visible()
    page.locator('#externalLinkCancel').click()
    page.locator('#detailsBack').click()
    page.locator('#nickname').fill('TestPlayer')
    page.locator('#nickname').dispatch_event('input')
    page.locator('.inline-server-row').first.locator('.inline-connect').click()
    emitted = page.evaluate('window.__native.emitted.filter(x => x.name === "skr:lobby:connect")')
    assert emitted and '8.8.8.8:7777' in emitted[-1]['payload']
    recent = page.evaluate('JSON.parse(localStorage.getItem("skr.launcher.recent.v3"))')
    assert recent[0]['address'] == '8.8.8.8:7777' and recent[0]['status'] == 'requested'
    page.reload()
    page.wait_for_selector('.home-page:not([hidden])')
    assert page.locator('#lastServerBadge').inner_text() == 'RECENT'
    assert 'Example Racing' in page.locator('#lastServerContent').inner_text()
    page.evaluate('data => (window.__native.handlers["skr:lobby:servers"] || []).forEach(fn => fn(JSON.stringify(data)))',fixture)
    page.locator('#nickname').fill('TestPlayer')
    page.locator('#nickname').dispatch_event('input')
    assert page.locator('#lastServerConnect').is_enabled()
    # Repeat launch then test native failure cleanup and successful-connection promotion.
    page.locator('#lastServerConnect').click()
    page.evaluate('() => (window.__native.handlers["skr:lobby:connect:connected"] || []).forEach(fn => fn())')
    assert page.evaluate('JSON.parse(localStorage.getItem("skr.launcher.recent.v3"))[0].status') == 'connected'
    context.close()
    browser.close()
httpd.shutdown()
print('V3.3 browser smoke: 16 integration checks passed')
