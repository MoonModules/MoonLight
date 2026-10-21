# /// script
# dependencies = ["pytest", "pyyaml"]
# ///
"""The OpenAPI description in docs/reference/openapi.yaml and the routes HttpServerModule dispatches are the same set."""

import re
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
SPEC = yaml.safe_load((ROOT / "docs" / "reference" / "openapi.yaml").read_text())
SERVER = (ROOT / "src" / "core" / "system" / "HttpServerModule.cpp").read_text()
METHODS = ("get", "post", "patch", "delete")
STATIC = {"/"}   # the UI's index page, a static file the spec leaves out


def server_routes():
    """Per method: exact paths, prefixes and suffixes, read from the dispatch comparisons between `// Route` and the OPTIONS branch."""
    region = SERVER[SERVER.index("    // Route\n"):SERVER.index('strcmp(method, "OPTIONS")')]
    parts = re.split(r'strcmp\(method, "(\w+)"\)', region)
    routes = {}
    for method, chunk in zip(parts[1::2], parts[2::2]):
        routes[method.lower()] = {
            "exact": set(re.findall(r'strcmp\(path, "([^"]+)"\)', chunk)) - STATIC,
            "prefix": set(re.findall(r'strncmp\(path, "([^"]+)", \d+\)', chunk)),
            "suffix": set(re.findall(r'strcmp\(path \+ pathLen - \d+, "([^"]+)"\)', chunk)),
        }
    # serveModule reads its own sub-resource off the rest of the path.
    routes["get"]["suffix"] |= set(re.findall(r'strncmp\(rest, "([^"]+)"', SERVER))
    return routes


def spec_operations():
    return [(m, path) for path, item in SPEC["paths"].items() for m in METHODS if m in item]


def test_the_spec_is_openapi_3_1_and_every_operation_has_a_response():
    assert SPEC["openapi"].startswith("3.1.")
    for method, path in spec_operations():
        assert SPEC["paths"][path][method].get("responses"), f"{method.upper()} {path} has no response"


def test_every_route_the_server_dispatches_is_in_the_spec():
    ops = spec_operations()
    for method, r in server_routes().items():
        templated = [p for m, p in ops if m == method and "{" in p]
        for path in r["exact"]:
            assert (method, path) in ops, f"{method.upper()} {path} is missing from the spec"
        for prefix in r["prefix"]:
            assert any(p.startswith(prefix + "{") for p in templated), f"{method.upper()} {prefix}... is missing from the spec"
        for suffix in r["suffix"]:
            assert any(p.endswith("}" + suffix) for p in templated), f"{method.upper()} .../{suffix} is missing from the spec"


def test_every_path_in_the_spec_is_one_the_server_dispatches():
    routes = server_routes()
    for method, path in spec_operations():
        r = routes[method]
        if "{" not in path:
            assert path in r["exact"], f"the server does not dispatch {method.upper()} {path}"
            continue
        tail = path[path.rindex("}") + 1:]
        assert path[:path.index("{")] in r["prefix"], f"the server does not dispatch {method.upper()} {path}"
        assert not tail or tail in r["suffix"], f"the server does not dispatch {method.upper()} {path}"


def test_the_api_names_the_firmware_version_it_describes():
    import json
    assert SPEC["info"]["version"] == json.loads((ROOT / "library.json").read_text())["version"]
