"""Settings smoke check only: never mounts, backs up or restores game saves."""
import http.cookiejar
import json
import urllib.parse
import urllib.request

BASE = 'http://192.168.0.193:8082'
client = urllib.request.build_opener(urllib.request.ProxyHandler({}),
    urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

def call(path, values=None):
    body = None if values is None else urllib.parse.urlencode(values).encode()
    request = urllib.request.Request(BASE+path, body, {'X-PSCloud-Request':'1'})
    with client.open(request, timeout=40) as response:
        return json.load(response)

def values(prefs):
    return {key:'1' if prefs[key] else '0' for key in ('auto_upload','activity_refresh')}

client.open(BASE+'/', timeout=10).close()
state = call('/api/state')
assert state['version'] == '0.9.0', state['version']
original = call('/api/preferences')
try:
    changed = values(original)
    changed['auto_upload'] = '0'
    changed['activity_refresh'] = '0' if original['activity_refresh'] else '1'
    call('/api/preferences', changed)
    current = call('/api/preferences')
    assert current['auto_upload'] is False
    assert current['activity_refresh'] != original['activity_refresh']
    assert not current['game_close_available'] and not current['sharing_available']
    print('PASS: preferences save/read; unavailable features stay disabled')
finally:
    call('/api/preferences', values(original))
assert values(call('/api/preferences')) == values(original)
print('PASS: original preferences restored')
if state['configured']:
    call('/api/connect', {'url':state['url'],'username':state['username'],'password':''})
    assert call('/api/state')['connected']
    print('PASS: saved cloud connection verified without exposing password')
assert 'Preferences saved' in call('/api/log')['log']
print('PASS: settings actions visible in recent activity; no game save operations run')
