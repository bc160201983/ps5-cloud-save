"""Read-only timing check: never backs up, uploads, restores or edits settings."""
import argparse
import concurrent.futures
import http.cookiejar
import json
import re
import socket
import statistics
import time
import urllib.parse
import urllib.request


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',required=True)
    parser.add_argument('--port',type=int,default=8082)
    parser.add_argument('--user')
    parser.add_argument('--title')
    args=parser.parse_args()
    if not re.fullmatch(r'[0-9.]+',args.host):parser.error('Use the console IPv4 address')
    if bool(args.user)!=bool(args.title):parser.error('Supply both user and title, or neither')
    if args.user and (not re.fullmatch(r'[0-9a-f]{1,16}',args.user) or not re.fullmatch(r'PPSA[0-9]{5}',args.title)):
        parser.error('Invalid test selection')
    base=f'http://{args.host}:{args.port}'
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def get(path,timeout=15):
        start=time.monotonic()
        with opener.open(base+path,timeout=timeout) as response:
            data=response.read()
        return data,round(time.monotonic()-start,3)

    _,elapsed=get('/')
    state=json.loads(get('/api/state')[0])
    print('Version:',state['version'],'page seconds:',elapsed,flush=True)
    if state['version'] not in ('0.11.1','0.11.2'):raise RuntimeError('Unexpected payload version')
    print('Cloud configured:',state['configured'],flush=True)
    timings=[get('/api/health')[1] for _ in range(3)]
    print('Health median/max seconds:',round(statistics.median(timings),3),max(timings),flush=True)
    idle=[socket.create_connection((args.host,args.port),timeout=5) for _ in range(2)]
    try:
        data,elapsed=get('/api/health')
        print('Health with two idle sockets:',elapsed,json.loads(data),flush=True)
    finally:
        for connection in idle:connection.close()
    if args.user:
        query=urllib.parse.urlencode({'user':args.user,'title':args.title,'slot':'WholeGame'})
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            listing=pool.submit(get,'/api/backups?'+query,90)
            # Test concurrent lightweight requests, not a concurrent save action.
            time.sleep(0.2)
            data,elapsed=get('/api/health')
            print('Health during cloud listing:',elapsed,json.loads(data),flush=True)
            _,elapsed=get('/')
            print('Page during cloud listing seconds:',elapsed,flush=True)
            try:
                data,elapsed=listing.result(timeout=95)
                result=json.loads(data)
                print('Cloud list seconds / versions:',elapsed,len(result['backups']),flush=True)
            except Exception as error:
                print('Cloud list did not pass:',type(error).__name__,flush=True)
                raise
    print('Read-only checks complete; no save or cloud writes.',flush=True)


if __name__=='__main__':main()
