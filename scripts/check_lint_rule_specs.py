"""Validate rule TOML and fixture contracts using the checked-in schema.

The schema uses only the JSON Schema keywords implemented below, so validation
has no third-party Python dependency. Unknown keywords fail closed.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "eta/tools/linter/rules/schema.json"
TYPES = {"object": dict, "array": list, "string": str, "boolean": bool, "integer": int, "number": (int, float)}
KEYWORDS = {"$schema", "title", "type", "additionalProperties", "required", "properties", "pattern",
            "minLength", "enum", "uniqueItems", "items", "minItems", "minimum", "maximum"}


def validate(value, schema, path="rule"):
    if set(schema) - KEYWORDS:
        raise ValueError(f"{path}: unsupported schema keywords")
    kind = schema.get("type")
    if kind and (not isinstance(value, TYPES[kind]) or (kind in ("integer", "number") and isinstance(value, bool))):
        raise ValueError(f"{path}: expected {kind}")
    if "enum" in schema and value not in schema["enum"]:
        raise ValueError(f"{path}: value outside enum")
    if isinstance(value, dict):
        missing = set(schema.get("required", [])) - value.keys()
        if missing:
            raise ValueError(f"{path}: missing {sorted(missing)}")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, item in value.items():
            if key in properties:
                validate(item, properties[key], f"{path}.{key}")
            elif additional is False:
                raise ValueError(f"{path}: unknown field {key}")
            elif isinstance(additional, dict):
                validate(item, additional, f"{path}.{key}")
    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0) or ("pattern" in schema and not re.fullmatch(schema["pattern"], value)):
            raise ValueError(f"{path}: invalid string")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            raise ValueError(f"{path}: too few items")
        if schema.get("uniqueItems") and len({json.dumps(item, sort_keys=True) for item in value}) != len(value):
            raise ValueError(f"{path}: duplicate items")
        for index, item in enumerate(value):
            validate(item, schema.get("items", {}), f"{path}[{index}]")
    if type(value) in (int, float):
        if value < schema.get("minimum", value) or value > schema.get("maximum", value):
            raise ValueError(f"{path}: value outside bounds")


def validate_spec(spec, fixtures, schema=None):
    validate(spec, schema or json.loads(SCHEMA.read_text(encoding="utf-8")))
    if (spec["owner"] == "compiler") != (spec["status"] == "delegated"):
        raise ValueError("delegated rules must have compiler ownership")
    enabled = spec["status"] == "enabled"
    if spec["presets"] and not enabled:
        raise ValueError("only enabled rules may belong to presets")
    if enabled:
        for name in ("positive-fixtures", "negative-fixtures", "dependency-fixtures", "unknown-fixtures"):
            if not spec.get(name):
                raise ValueError(f"enabled rule requires {name}")
        if set(spec["prerequisites"]) != set(spec.get("verified-prerequisites", [])):
            raise ValueError("enabled rule has unverified prerequisites")
        if spec["fix"] != "none" and not spec.get("fix-fixtures"):
            raise ValueError("fixable rule requires fix fixtures")
    for name, option in spec["options"].items():
        constraints = {key: value for key, value in option.items() if key != "default"}
        validate(option["default"], constraints, f"options.{name}.default")
        if "minimum" in option and "maximum" in option and option["minimum"] > option["maximum"]:
            raise ValueError(f"options.{name}: inverted bounds")
        if option["type"] not in ("integer", "number") and ({"minimum", "maximum"} & option.keys()):
            raise ValueError(f"options.{name}: numeric bounds on non-numeric option")
        for item in option.get("enum", []):
            validate(item, {"type": option["type"]}, f"options.{name}.enum")
    for field in sorted(key for key in spec if key.endswith("-fixtures")):
        for fixture in spec[field]:
            source = fixtures / f"{fixture}.eta"
            expected = fixtures / f"{fixture}.json"
            if not source.is_file() or not expected.is_file():
                raise ValueError(f"missing fixture {fixture}")
            data = json.loads(expected.read_text(encoding="utf-8"))
            if not isinstance(data.get("diagnostics"), list):
                raise ValueError(f"{fixture}: diagnostics must be an array")
            size = len(source.read_bytes())
            for diagnostic in data["diagnostics"]:
                if diagnostic.get("code") != spec["code"] or not isinstance(diagnostic.get("span"), list) or len(diagnostic["span"]) != 2:
                    raise ValueError(f"{fixture}: requires rule code and exact byte span")
                begin, end = diagnostic["span"]
                if type(begin) is not int or type(end) is not int or not 0 <= begin <= end <= size:
                    raise ValueError(f"{fixture}: invalid span")
            if field == "positive-fixtures" and not data["diagnostics"]:
                raise ValueError(f"{fixture}: positive fixture requires a finding")
            if field in ("negative-fixtures", "unknown-fixtures") and data["diagnostics"]:
                raise ValueError(f"{fixture}: negative/unknown fixture must not report findings")
            if field in ("dependency-fixtures", "unknown-fixtures") and not data.get("coverage"):
                raise ValueError(f"{fixture}: requires analysis coverage")
            if field == "fix-fixtures" and not isinstance(data.get("fixed-source"), str):
                raise ValueError(f"{fixture}: requires expected fixed source")


def inventory(rules, fixtures):
    specs = []
    codes = set()
    for path in sorted(rules.glob("*.toml")):
        spec = tomllib.loads(path.read_text(encoding="utf-8"))
        validate_spec(spec, fixtures)
        if spec["code"] in codes or path.stem != spec["code"]:
            raise ValueError(f"{path.name}: duplicate code or filename mismatch")
        codes.add(spec["code"])
        specs.append(spec)
    return {"rules": specs, "presets": {name: sorted(spec["code"] for spec in specs if name in spec["presets"])
                                         for name in ("recommended", "strict", "all")}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rules", type=pathlib.Path, default=SCHEMA.parent)
    parser.add_argument("--fixtures", type=pathlib.Path, default=ROOT / "eta/qa/test/fixtures/linter")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    result = inventory(args.rules, args.fixtures)
    if args.output:
        args.output.write_text(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Validated {len(result['rules'])} rule specifications")


if __name__ == "__main__":
    main()
