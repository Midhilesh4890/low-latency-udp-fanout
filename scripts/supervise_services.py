#!/usr/bin/env python3
"""Supervise configured local services, including TLS relays, as one restart group.

Each restart gets a private runtime directory. Durable DB paths must be outside
{runtime}. Commands are argument arrays; no shell expansion is performed.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import tempfile
import time
import uuid

def run(config, state_path, readiness_timeout):
    services=config["services"]
    if not 1 <= len(services) <= 64:
        raise ValueError("services must contain 1..64 entries")
    names=[s["name"] for s in services]
    if len(set(names)) != len(names):
        raise ValueError("service names must be unique")
    epoch=uuid.uuid4().hex
    children=[]
    selector=selectors.DefaultSelector()
    buffers={}
    ready=set()
    with tempfile.TemporaryDirectory(prefix="pulse_services_") as runtime:
        def drain(timeout):
            for key,_ in selector.select(timeout):
                p,name,needle=key.data
                chunk=os.read(key.fd,4096)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                text=buffers.get(name,b"")+chunk
                if needle and needle.encode() in text:
                    ready.add(name)
                # Keep partial lines bounded even for misbehaving children.
                lines=text.split(b"\n")
                for line in lines[:-1]:
                    print(name+": "+line.decode(errors="replace"),flush=True)
                buffers[name]=lines[-1][-65536:]
        try:
            for service in services:
                argv=service["argv"]
                if not isinstance(argv,list) or not argv or not all(isinstance(x,str) for x in argv):
                    raise ValueError("argv must be a nonempty string array")
                command=[arg.replace("{runtime}",runtime).replace("{epoch}",epoch) for arg in argv]
                p=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                children.append((service["name"],p))
                os.set_blocking(p.stdout.fileno(),False)
                selector.register(p.stdout,selectors.EVENT_READ,(p,service["name"],service.get("ready")))
                if service.get("ready"):
                    deadline=time.monotonic()+readiness_timeout
                    while service["name"] not in ready:
                        drain(.05)
                        if any(child.poll() not in (None,0) for _,child in children):
                            raise RuntimeError("service failed during startup")
                        if p.poll() is not None or time.monotonic()>=deadline:
                            raise TimeoutError("service readiness failed: "+service["name"])
            completion=config.get("completion")
            if completion and completion not in names:
                raise ValueError("completion must name a configured service")
            for name,child in children:
                if child.poll() is not None:
                    if child.returncode==0 and name==completion:
                        return
                    raise RuntimeError("service exited during startup: "+name)
            state={"epoch":epoch,"services":[{"name":name,"pid":p.pid,"start_ticks":Path(f"/proc/{p.pid}/stat").read_text().rsplit(")",1)[1].split()[19]} for name,p in children],
                   "endpoints":json.loads(json.dumps(config.get("endpoints",{})).replace("{runtime}",runtime))}
            if state_path:
                temporary=state_path.with_name(state_path.name+"."+epoch)
                temporary.write_text(json.dumps(state))
                os.replace(temporary,state_path)
            completion=config.get("completion")
            if completion and completion not in names:
                raise ValueError("completion must name a configured service")
            while True:
                drain(.05)
                for name,p in children:
                    if p.poll() is None:
                        continue
                    if p.returncode != 0:
                        raise RuntimeError("service failed: "+name)
                    if name==completion:
                        return
                    if not completion:
                        raise RuntimeError("service exited: "+name)
        finally:
            for _,p in children:
                if p.poll() is None:
                    p.terminate()
            deadline=time.monotonic()+5
            while any(p.poll() is None for _,p in children) and time.monotonic()<deadline:
                drain(.05)
            for _,p in children:
                if p.poll() is None:
                    p.kill()
                p.wait()
                p.stdout.close()
            selector.close()
            if state_path and state_path.exists():
                try:
                    if json.loads(state_path.read_text()).get("epoch")==epoch:
                        state_path.unlink()
                except (ValueError,OSError):
                    pass

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("config",type=Path)
    p.add_argument("--state-file",type=Path)
    p.add_argument("--restarts",type=int,default=3)
    p.add_argument("--readiness-timeout",type=float,default=10)
    args=p.parse_args()
    if args.restarts<0 or args.readiness_timeout<=0:
        p.error("invalid restart/readiness bounds")
    def stop(signum,frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM,stop)
    try:
        config=json.loads(args.config.read_text())
        for attempt in range(args.restarts+1):
            try:
                run(config,args.state_file,args.readiness_timeout)
                return
            except (OSError,RuntimeError,TimeoutError) as error:
                if attempt==args.restarts:
                    raise
                print(f"restarting service group: {error}",flush=True)
                time.sleep(min(attempt+1,5))
    except KeyboardInterrupt:
        p.exit(130,"service group stopped\n")
    except (OSError,ValueError,RuntimeError,KeyError) as error:
        p.exit(1,f"supervision failed: {error}\n")

if __name__=="__main__":
    main()
