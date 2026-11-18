"""Exercise the actual chores page in headless Chromium against a loopback fixture.
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
        call('Emulation.setDeviceMetricsOverride', {'width': 1200, 'height': 1100, 'deviceScaleFactor': 1, 'mobile': False})
        call('Page.navigate', {'url': base+'/chores'})
        wait("document.getElementById('unlock') && !document.getElementById('unlock').hidden")
        fill('api-key', key)
        submit('unlock')
        wait("!document.getElementById('unlock').hidden === false && !document.getElementById('panel').disabled")
        dangerous_name = '<img src=x onerror=alert(1)> Zoë'
        fill('person-name', dangerous_name)
        submit('person-form')
        wait("document.querySelectorAll('.person').length===1")
        assert not js("!!document.querySelector('#people img')"), 'Person name became HTML'
        assert dangerous_name in js("document.getElementById('people').textContent")
        fill('chore-title', 'Feed <cat> & tidy up')
        submit('chore-form')
        wait("document.querySelectorAll('.task').length===1")
        data = api('/api/chores')
        assert data['chores'][0]['all_day'] and data['chores'][0]['repeat'] == 'daily'
        assert js("document.querySelector('.person-actions button:last-child').disabled"), 'Assigned person could be deleted'
        js("document.querySelector('.task input[type=checkbox]').click()")
        wait("!!document.querySelector('.task.done')")
        assert api('/api/chores')['chores'][0]['completed']
        # Editing the same chore preserves its identity and switches the schedule intuitively.
        js("document.querySelector('.task-actions button').click()")
        fill('chore-repeat', 'once')
        js("document.getElementById('chore-repeat').dispatchEvent(new Event('change'))")
        assert not js("document.getElementById('date-field').hidden")
        fill('chore-date', '2026-11-18')
        click('all-day')
        assert not js("document.getElementById('time-field').hidden")
        fill('chore-time', '17:30')
        submit('chore-form')
        wait("document.querySelectorAll('.task').length===0")
        fill('view', 'upcoming')
        js("document.getElementById('view').dispatchEvent(new Event('change'))")
        wait("document.querySelectorAll('.task').length===1")
        task = api('/api/chores')['chores'][0]
        assert task['repeat'] == 'once' and task['date'] == '2026-11-18' and task['time'] == '17:30' and not task['completed']
        click('panel')
        wait("!document.getElementById('panel').disabled && document.getElementById('panel').checked")
        assert api('/api/settings')['left_panel_enabled']
        assert js("document.getElementById('panel-clock').checked"), 'Clock should default on'
        assert not js("!!document.querySelector('.board-status a[href=\"/config\"]')"), 'Duplicate settings link remains'
        click('panel-clock');wait("!document.getElementById('panel-clock').disabled && !document.getElementById('panel-clock').checked")
        assert not api('/api/settings')['left_panel_clock_enabled']
        assert api('/api/settings')['left_panel_enabled'], 'Clock toggle hid the panel'
        call('Page.reload');wait("document.getElementById('panel-clock') && !document.getElementById('panel-clock').disabled")
        assert not js("document.getElementById('panel-clock').checked"), 'Clock preference lost on reload'
        click('panel-clock');wait("!document.getElementById('panel-clock').disabled && document.getElementById('panel-clock').checked")
        assert api('/api/settings')['left_panel_clock_enabled']

        # Full reload gets persisted people/chores and the active panel state.
        call('Page.reload')
        wait("document.querySelectorAll('.person').length===1 && !document.getElementById('panel').disabled")
        fill('view', 'all')
        js("document.getElementById('view').dispatchEvent(new Event('change'))")
        wait("document.querySelectorAll('.task').length===1")
        assert js("document.getElementById('panel').checked")
        assert not js("!!document.querySelector('#people img')")
        # Mobile layout must have no horizontal overflow, even with long names/titles.
        call('Emulation.setDeviceMetricsOverride', {'width': 390, 'height': 844, 'deviceScaleFactor': 1, 'mobile': True})
        assert js('document.documentElement.scrollWidth <= window.innerWidth'), 'Mobile page overflowed'
        assert not exceptions, exceptions
        if os.path.isdir('/artifacts'):
            shot = call('Page.captureScreenshot', {'format': 'png', 'captureBeyondViewport': True})
            with open('/artifacts/chores-mobile.png','wb') as output:
                output.write(base64.b64decode(shot['data']))
        js("window.confirm=()=>true; document.querySelector('.task-actions button:last-child').click()")
        wait("document.querySelectorAll('.task').length===0")
        wait("!document.querySelector('.person-actions button:last-child').disabled")
        js("document.querySelector('.person-actions button:last-child').click()")
        wait("document.querySelectorAll('.person').length===0")
        assert not api('/api/chores')['people'] and not api('/api/chores')['chores']
        print('Chromium: auth, people/chore create/edit/delete, safe text, daily completion, dated time, panel/clock toggles, reload and mobile layout passed')
        ws.close()
    finally:
        chrome.terminate()
        chrome.wait(timeout=10)
