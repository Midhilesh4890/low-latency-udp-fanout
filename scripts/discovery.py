#!/usr/bin/env python3
"""Serve or fetch supervisor discovery state using mutually authenticated TLS."""
import argparse
import http.server
import json
from pathlib import Path
import ssl
import urllib.request

class Server(http.server.HTTPServer):
    def get_request(self):
        connection,address=super().get_request()
        connection.settimeout(3)
        try:
            stream=self.tls.wrap_socket(connection,server_side=True)
            names={name.lower() for kind,name in stream.getpeercert().get("subjectAltName",()) if kind=="DNS"}
            if not names.intersection(self.allowed):
                stream.close()
                raise OSError("unauthorized discovery identity")
            return stream,address
        except BaseException:
            connection.close()
            raise

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path!="/v1/services":
            self.send_error(404)
            return
        try:
            with self.server.state.open("rb") as file:
                data=file.read(1024*1024+1)
            if len(data)>1024*1024:
                raise ValueError("discovery state exceeds limit")
            state=json.loads(data)
            if not isinstance(state,dict) or "epoch" not in state:
                raise ValueError("invalid discovery state")
            for service in state.get("services",[]):
                status=Path(f"/proc/{int(service['pid'])}/stat").read_text().rsplit(")",1)[1].split()
                if status[0]=="Z" or status[19]!=service["start_ticks"]:
                    raise ValueError("stale discovery registration")
        except (OSError,ValueError,KeyError,IndexError):
            self.send_error(503,"service group unavailable")
            return
        self.send_response(200)
        self.send_header("Content-Type","application/json")
        self.send_header("Cache-Control","no-store")
        self.send_header("Content-Length",str(len(data)))
        self.end_headers()
        self.wfile.write(data)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode",choices=("serve","fetch"))
    p.add_argument("--ca",required=True)
    p.add_argument("--cert",required=True)
    p.add_argument("--key",required=True)
    p.add_argument("--host",default="127.0.0.1")
    p.add_argument("--port",type=int,default=9444)
    p.add_argument("--state-file",type=Path)
    p.add_argument("--client-name",action="append",default=[])
    p.add_argument("--url",help="HTTPS URL with a certificate-matching hostname")
    args=p.parse_args()
    server=args.mode=="serve"
    ctx=ssl.create_default_context(ssl.Purpose.CLIENT_AUTH if server else ssl.Purpose.SERVER_AUTH,cafile=args.ca)
    ctx.minimum_version=ssl.TLSVersion.TLSv1_3
    ctx.verify_mode=ssl.CERT_REQUIRED
    ctx.load_cert_chain(args.cert,args.key)
    if server:
        if not args.state_file or not args.client_name:
            p.error("serve requires state-file and at least one client-name")
        with Server((args.host,args.port),Handler) as service:
            service.tls=ctx
            service.allowed={name.lower() for name in args.client_name}
            service.state=args.state_file
            print("LISTENING",flush=True)
            service.serve_forever(poll_interval=.2)
    else:
        if not args.url or not args.url.startswith("https://"):
            p.error("fetch requires an HTTPS URL")
        # Disallow redirects: service identities must not silently change.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self,*unused,**kwargs):
                return None
        opener=urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx),NoRedirect())
        with opener.open(args.url,timeout=5) as response:
            data=response.read(1024*1024+1)
        if len(data)>1024*1024:
            p.error("discovery response exceeds limit")
        print(json.dumps(json.loads(data),indent=2))

if __name__=="__main__":
    main()
