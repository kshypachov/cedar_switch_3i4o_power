"""Print the steps of net_api_b.py result files in one line each, re-validating 2xx bodies.

usage: summarize.py <result.json>...
"""
import json, sys
from pathlib import Path

DOC = json.loads((Path(__file__).resolve().parents[3] / "openapi.json").read_text())
from jsonschema import Draft202012Validator
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT202012
REGISTRY = Registry().with_resource("doc", Resource.from_contents({"components": DOC["components"]}, default_specification=DRAFT202012))
SCHEMA = {"stage": "NetworkTransaction", "transaction": "NetworkTransaction", "apply": "JobAccepted"}

def errors(name, body):
    v = Draft202012Validator({"$ref": f"doc#/components/schemas/{name}"}, registry=REGISTRY,
                             format_checker=Draft202012Validator.FORMAT_CHECKER)
    return [f"{list(e.absolute_path)}: {e.message}" for e in v.iter_errors(body)]

for path in sys.argv[1:]:
    d = json.loads(Path(path).read_text())
    print(f"== {path} ({d['scenario']}, {d['when']})")
    for s in d["steps"]:
        b = s.get("body")
        err = b.get("error") if isinstance(b, dict) else None
        detail = ""
        if err and s.get("status", 200) >= 400:
            detail = f"{err['code']} " + " ".join(f"{f['path']}={f['code']}" for f in err.get("fields", []))
        elif isinstance(b, dict):
            detail = " ".join(f"{k}={b[k]}" for k in ("id", "state", "job_id", "remaining_seconds", "error") if k in b)
        extra = {k: v for k, v in s.items() if k not in ("t", "step", "body", "status", "ms", "schema_errors", "case", "host")}
        schema = ""
        if s["step"] in SCHEMA and s.get("status") in (200, 201, 202) and isinstance(b, dict):
            e = errors(SCHEMA[s["step"]], b)
            schema = f" SCHEMA {e}" if e else " schema ok"
        print(f"  +{s['t']:6.1f} {s['step']:<28} {s.get('case', ''):<32} {s.get('status', '')} {s.get('ms', '')}ms {detail}{schema} {extra if extra else ''}")
