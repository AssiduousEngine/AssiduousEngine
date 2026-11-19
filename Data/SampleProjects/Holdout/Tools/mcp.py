#!/usr/bin/env python3
"""The MCP client the other scripts call in-process (`call(tool, args)`), for either engine host:

- the editor with Holdout open and its MCP server on (`Tools.Editor Data/SampleProjects/Holdout
  --mcp-port 7405`), when `HOLDOUT_MCP=http`: what the user watches build;
- otherwise a headless `Tools.Mcp` started on first use, with the project opened in it.

    mcp.py <tool> [json-args]    one call, its result printed
"""
import json, os, subprocess, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(HERE, ".."))
# The engine checkout: $GAMEENGINE_ROOT, else the one this project sits in.
ENGINE = os.environ.get("GAMEENGINE_ROOT", os.path.normpath(os.path.join(PROJECT, "../../..")))
TOOLS_MCP = os.environ.get("TOOLS_MCP", ENGINE + "/Bin/Debug/Linux64-Clang/Tools.Mcp")
PORT = int(os.environ.get("MCP_PORT", "7405"))
HTTP = os.environ.get("HOLDOUT_MCP") == "http"
TIMEOUT = float(os.environ.get("MCP_TIMEOUT", "1800"))

_process = None
_next_id = [0]


def _http(method, params):
    token_dir = os.path.expanduser(os.environ.get("MCP_TOKEN_DIR", "~/.local/share/assiduousengine"))
    token = open(os.path.join(token_dir, "mcp-token")).read().strip()
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    req = urllib.request.Request("http://127.0.0.1:%d/mcp" % PORT, data=body,
                                 headers={"Content-Type": "application/json",
                                          "Accept": "application/json, text/event-stream",
                                          "Authorization": "Bearer " + token})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        raw = resp.read().decode()
    if raw.startswith("event:") or raw.startswith("data:"):
        raw = "\n".join(l[5:] for l in raw.splitlines() if l.startswith("data:"))
    return json.loads(raw)


def _stdio(method, params, notify=False):
    global _process
    if _process is None:
        _process = subprocess.Popen([TOOLS_MCP], cwd=ENGINE, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=open(os.path.join(HERE, "generated", "mcp.log"), "w"), text=True)
        _stdio("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "holdout-tools", "version": "1"}})
        _stdio("notifications/initialized", None, notify=True)
        if os.path.exists(os.path.join(PROJECT, "Project.xml")):
            call("project_open", {"directory": PROJECT})
    message = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        message["params"] = params
    if not notify:
        _next_id[0] += 1
        message["id"] = _next_id[0]
    _process.stdin.write(json.dumps(message) + "\n")
    _process.stdin.flush()
    if notify:
        return None
    while True:
        line = _process.stdout.readline()
        if not line:
            raise SystemExit("Tools.Mcp closed (see generated/mcp.log)")
        reply = json.loads(line)
        if reply.get("id") == message["id"]:
            return reply


def call(tool, args):
    """One tool call; the result parsed as JSON (or {"text": ...}). Exits on an error."""
    os.makedirs(os.path.join(HERE, "generated"), exist_ok=True)
    rpc = _http if HTTP else _stdio
    reply = rpc("tools/call", {"name": tool, "arguments": args})
    if "error" in reply:
        raise SystemExit("%s failed: %s" % (tool, json.dumps(reply["error"])[:2000]))
    result = reply["result"]
    text = "".join(part.get("text", "") for part in result.get("content", []))
    if result.get("isError"):
        raise SystemExit("%s failed: %s" % (tool, text[:2000]))
    try:
        return json.loads(text)
    except ValueError:
        return {"text": text}


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    print(json.dumps(call(sys.argv[1], json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}), indent=1))
