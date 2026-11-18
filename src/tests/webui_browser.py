"""Exercise the controller, full configuration and shared mobile pages in headless Chromium against a loopback fixture.
Run inside pitrove-ui-test with --network host; the C++ fixture listens on 19030.
"""
import base64
import json
import os
import subprocess
import tempfile
import time
import urllib.request
import websocket

base = 'http://127.0.0.1:19030'
key = 'chores-fixture-key'

def api(path, fields=None):
    import urllib.parse
    data = urllib.parse.urlencode(fields).encode() if fields is not None else None
    req = urllib.request.Request(base + path, data=data, headers={'Authorization': 'Bearer ' + key})
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)

with tempfile.TemporaryDirectory(prefix='pitrove-chrome-') as profile:
    chrome = subprocess.Popen(['chromium', '--headless', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--no-first-run', '--no-default-browser-check', '--remote-allow-origins=*', '--remote-debugging-port=9223', '--user-data-dir='+profile, 'about:blank'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                tabs = json.load(urllib.request.urlopen('http://127.0.0.1:9223/json/list', timeout=1))
                target = next(t for t in tabs if t['type'] == 'page')
                ws = websocket.create_connection(target['webSocketDebuggerUrl'], timeout=20, suppress_origin=True)
                break
            except Exception:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.2)
        serial = 0
        exceptions = []
        def call(method, params=None):
            global serial
            serial += 1
            ws.send(json.dumps({'id': serial, 'method': method, 'params': params or {}}))
            while True:
                data = json.loads(ws.recv())
                if data.get('method') == 'Runtime.exceptionThrown':
                    exceptions.append(data['params']['exceptionDetails'].get('text'))
                if data.get('id') == serial:
                    assert 'error' not in data, data
                    return data.get('result', {})
        def js(expression):
            result = call('Runtime.evaluate', {'expression': expression, 'awaitPromise': True, 'returnByValue': True})
            assert 'exceptionDetails' not in result, result
            return result.get('result', {}).get('value')
        def wait(expression, seconds=12):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if js(expression):
                    return
                time.sleep(.15)
            raise AssertionError('Browser condition timed out: '+expression)
        def fill(id, value):
            js('document.getElementById('+json.dumps(id)+').value='+json.dumps(value))
        def click(id):
            js('document.getElementById('+json.dumps(id)+').click()')
        def submit(id):
            js('document.getElementById('+json.dumps(id)+').requestSubmit()')
        call('Runtime.enable')
        call('Page.enable')
        call('Emulation.setDeviceMetricsOverride', {'width':1280,'height':1000,'deviceScaleFactor':1,'mobile':False})
        call('Page.navigate', {'url':base+'/'})
        wait("document.getElementById('unlock') && !document.getElementById('unlock').hidden")
        fill('api-key',key);submit('unlock')
        wait("document.getElementById('unlock').hidden && document.getElementById('media-title').textContent.includes('family')")
        assert js("document.getElementById('lan-address').value") == base+'/'
        assert not js("!!document.querySelector('.heading-actions a[href=\"/config\"]')"), 'Duplicate full configuration button remains'
        assert js("document.querySelector('.nav-link[data-page=settings]').getAttribute('href')") == '/config'
        click('btn-pause');wait("document.getElementById('txt-pause').textContent==='Resume'")
        assert api('/api/status')['paused'], 'Authenticated playback command failed'
        click('btn-next');wait("document.getElementById('media-progress').textContent.startsWith('2 /')")
        fill('appearance','light');js("document.getElementById('appearance').dispatchEvent(new Event('change'))")
        assert js("document.documentElement.dataset.theme")=='light'
        # Phone, tablet and desktop layouts must fit; navigation remains visible on phones.
        def viewport(width,height=900):
            call('Emulation.setDeviceMetricsOverride', {'width':width,'height':height,'deviceScaleFactor':1,'mobile':width<761})
            time.sleep(.1)
            assert js('document.documentElement.scrollWidth<=window.innerWidth'), 'Horizontal overflow at '+str(width)
            assert js("[...document.querySelectorAll('.nav-link')].every(n=>{const r=n.getBoundingClientRect();return r.width>=44&&r.height>=44;})"), 'Navigation touch targets too small'
        for width in (320,390,768,1280):viewport(width)
        js("location.hash='#logs'")
        wait("document.getElementById('log-console').textContent.includes('Playback started')")
        fill('log-search','warn');js("document.getElementById('log-search').dispatchEvent(new Event('input'))")
        assert 'INFO' not in js("document.getElementById('log-console').textContent")
        assert 'WARN' in js("document.getElementById('log-console').textContent")
        assert not js("!!document.querySelector('#log-console img')")
        click('live-logs');assert not js("document.getElementById('live-logs').checked")
        call('Page.navigate',{'url':base+'/config'})
        wait("document.querySelectorAll('.config-field').length>=180")
        assert js("document.documentElement.dataset.theme")=='light','Appearance did not persist across pages'
        data=api('/api/config');assert len(data['fields'])==184
        assert len({f['id'] for f in data['fields']})==184
        assert all(not f['value'] for f in data['fields'] if f['secret']), 'API exposed configured credentials'
        for width in (320,390,768,1280):viewport(width)
        fill('config-search','preload');js("document.getElementById('config-search').dispatchEvent(new Event('input'))")
        assert js("[...document.querySelectorAll('.config-field')].filter(n=>!n.hidden).length") == 2
        fill('option-slideshow.preload_capacity','7');js("document.getElementById('option-slideshow.preload_capacity').dispatchEvent(new Event('input'))")
        assert '1 unsaved change' in js("document.getElementById('pending-count').textContent")
        fill('config-search','');js("document.getElementById('config-search').dispatchEvent(new Event('input'))")
        # A clear review, save, and reread round trip uses the actual model/persistence handler.
        submit('config-form');wait("document.getElementById('config-review').open")
        assert '7' in js("document.getElementById('review-list').textContent")
        click('save-config');wait("!document.getElementById('config-review').open && document.getElementById('pending-count').textContent==='All changes saved'")
        assert next(f for f in api('/api/config')['fields'] if f['id']=='slideshow.preload_capacity')['value']==7
        assert not js("document.getElementById('restart-notice').hidden"),'Restart guidance missing'
        # Literal punctuation, commas and quotes in lists must persist as entries, not HTML.
        fill('option-scan.ignore_folders','Folder, with comma\n<private & safe>\nA "quoted" folder')
        js("document.getElementById('option-scan.ignore_folders').dispatchEvent(new Event('input'))")
        submit('config-form');click('save-config');wait("document.getElementById('pending-count').textContent==='All changes saved'")
        saved=next(f for f in api('/api/config')['fields'] if f['id']=='scan.ignore_folders')['value']
        assert saved==['Folder, with comma','<private & safe>','A "quoted" folder'],saved
        # Credentials can be changed without exposing or resending existing values.
        fill('option-mqtt.pass','new-test-secret');js("document.getElementById('option-mqtt.pass').dispatchEvent(new Event('input'))")
        submit('config-form');assert 'new-test-secret' not in js("document.getElementById('review-list').textContent")
        click('save-config');wait("document.getElementById('pending-count').textContent==='All changes saved'")
        field=next(f for f in api('/api/config')['fields'] if f['id']=='mqtt.pass')
        assert field['configured'] and field['value']==''
        # A concurrent settings change is rejected; the pending browser edit survives.
        fill('option-slideshow.transition_delay','88');js("document.getElementById('option-slideshow.transition_delay').dispatchEvent(new Event('input'))")
        current=api('/api/config');api('/api/config',{'revision':current['revision'],'slideshow.shuffle':'0'})
        submit('config-form');click('save-config');wait("!document.getElementById('config-error').hidden")
        assert 'changed on the frame' in js("document.getElementById('config-error').textContent")
        assert js("document.getElementById('option-slideshow.transition_delay').value")=='88'
        js('window.confirm=()=>true');click('reload-config');wait("document.getElementById('pending-count').textContent==='All changes saved'")
        # Changing the access key updates this browser after saving with the old key.
        fill('option-remote.api_key','temporary-browser-key');js("document.getElementById('option-remote.api_key').dispatchEvent(new Event('input'))")
        submit('config-form');click('save-config');wait("document.getElementById('pending-count').textContent==='All changes saved'")
        assert js("localStorage.getItem('api_key')")=='temporary-browser-key'
        key='temporary-browser-key'
        assert api('/api/config')['revision']
        fill('option-remote.api_key','chores-fixture-key');js("document.getElementById('option-remote.api_key').dispatchEvent(new Event('input'))")
        submit('config-form');click('save-config');wait("document.getElementById('pending-count').textContent==='All changes saved'")
        key='chores-fixture-key'
        # Pin setup is hashed in storage and remains configured after the editor reload.
        fill('option-dashboard.dashboard_pin','2468');js("document.getElementById('option-dashboard.dashboard_pin').dispatchEvent(new Event('input'))")
        submit('config-form');click('save-config');wait("document.getElementById('pending-count').textContent==='All changes saved'")
        pin=next(f for f in api('/api/config')['fields'] if f['id']=='dashboard.dashboard_pin')
        assert pin['configured'] and not pin['value']
        viewport(390,844)
        fill('config-search','chores');js("document.getElementById('config-search').dispatchEvent(new Event('input'))")
        if os.path.isdir('/artifacts'):
            shot=call('Page.captureScreenshot',{'format':'png','captureBeyondViewport':True})
            with open('/artifacts/config-mobile.png','wb') as output:output.write(base64.b64decode(shot['data']))
        call('Page.navigate',{'url':base+'/chores'})
        wait("document.getElementById('panel') && !document.getElementById('panel').disabled")
        for width in (320,390,768,1280):viewport(width)
        assert not js("!!document.querySelector('#people img')")
        call('Page.navigate',{'url':base+'/'})
        wait("document.getElementById('media-title') && document.getElementById('media-title').textContent.includes('family')")
        viewport(1280,1000)
        if os.path.isdir('/artifacts'):
            shot=call('Page.captureScreenshot',{'format':'png','captureBeyondViewport':True})
            with open('/artifacts/frame-desktop.png','wb') as output:output.write(base64.b64decode(shot['data']))
        viewport(390,844)
        assert js("(()=>{const r=document.getElementById('btn-pause').getBoundingClientRect();const n=document.querySelector('.app-nav').getBoundingClientRect();return r.top>=0&&r.bottom<n.top;})()"), 'Phone playback controls hidden beneath navigation'
        if os.path.isdir('/artifacts'):
            shot=call('Page.captureScreenshot',{'format':'png','captureBeyondViewport':True})
            with open('/artifacts/frame-mobile.png','wb') as output:output.write(base64.b64decode(shot['data']))
        assert not exceptions,exceptions
        print('Chromium: authenticated playback, shared themes, 184 config options, search/review/save, lists, secrets/PIN, conflicts, access-key rotation, safe logs and 320/390/768/1280px layouts passed')
        ws.close()
    finally:
        chrome.terminate();chrome.wait(timeout=10)
