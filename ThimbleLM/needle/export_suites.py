# Turns Cactus's frozen environment suites (cactus/environments/*.py) into JSON without
# running any of their code: the files are parsed with `ast`, and each @cactus.tool function
# becomes a schema the way cactus/agent/tools.py build_schema() writes one (same keys, same
# order), so the prompt matches what the official package sends to the engine.
#   python3 -I export_suites.py CACTUS_SRC_DIR suites.json
import ast, json, os, re, sys

JSON_TYPES = {"str": "string", "int": "integer", "float": "number", "bool": "boolean",
              "list": "array", "dict": "object"}
FIELD_KEYS = (("description", "description"), ("enum", "enum"), ("ge", "minimum"), ("le", "maximum"),
              ("gt", "exclusiveMinimum"), ("lt", "exclusiveMaximum"), ("multiple_of", "multipleOf"),
              ("min_length", "minLength"), ("max_length", "maxLength"), ("pattern", "pattern"),
              ("format", "format"), ("min_items", "minItems"), ("max_items", "maxItems"),
              ("unique_items", "uniqueItems"))


def name_of(node):
    if isinstance(node, ast.Name): return node.id
    if isinstance(node, ast.Attribute): return node.attr
    return None


class Env:
    def __init__(self, tree):
        self.aliases = {}
        for st in tree.body:
            if isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name):
                self.aliases[st.targets[0].id] = st.value

    def resolve(self, node):
        while isinstance(node, ast.Name) and node.id in self.aliases:
            node = self.aliases[node.id]
        return node

    def split(self, node):
        """(origin name, args) of a subscript annotation, or (name, None)."""
        node = self.resolve(node)
        if isinstance(node, ast.Subscript):
            sl = node.slice
            args = list(sl.elts) if isinstance(sl, ast.Tuple) else [sl]
            return name_of(node.value), args, node
        if isinstance(node, ast.Constant) and node.value is None:
            return "None", None, node
        return name_of(node), None, node

    def field(self, node):
        """the cactus.Field(...) keyword values in an annotation, or None"""
        origin, args, node = self.split(node)
        if origin == "Annotated":
            for meta in args[1:]:
                if isinstance(meta, ast.Call) and name_of(meta.func) == "Field":
                    return meta
            return None
        if origin == "Optional":
            return self.field(args[0])
        return None

    def json_type(self, node):
        origin, args, node = self.split(node)
        if origin == "Annotated": return self.json_type(args[0])
        if origin == "Optional": return self.json_type(args[0])
        if origin == "Literal":
            vals = [ast.literal_eval(a) for a in args]
            t = {str: "string", int: "integer", float: "number", bool: "boolean"}.get(type(vals[0]), "string")
            return {"type": t, "enum": vals}
        if origin in ("list", "List"):
            return {"type": "array", "items": self.json_type(args[0]) if args else {"type": "string"}}
        if origin in ("dict", "Dict"): return {"type": "object"}
        if args is None and origin in JSON_TYPES: return {"type": JSON_TYPES[origin]}
        return {"type": "string"}

    def optional(self, node):
        origin, args, node = self.split(node)
        if origin == "Annotated": return self.optional(args[0])
        return origin == "Optional"


def parse_doc(doc):
    if not doc: return "", {}
    lines = doc.strip("\n").splitlines()
    heads = ("args:", "arguments:", "parameters:", "params:")
    desc, args, i = [], {}, 0
    while i < len(lines) and lines[i].strip().lower() not in heads:
        desc.append(lines[i].strip()); i += 1
    indent = lambda s: len(s) - len(s.lstrip())
    head = indent(lines[i]) if i < len(lines) else 0
    name, depth = None, 0
    for line in lines[i + 1:]:
        if not line.strip(): continue
        if indent(line) <= head and re.match(r"\s*[A-Za-z][\w ]*:\s*$", line): break
        m = re.match(r"\s+(\w+)\s*(?:\([^)]*\))?\s*:\s*(.+)", line)
        if m and (name is None or indent(line) <= depth):
            name, depth = m.group(1), indent(line); args[name] = m.group(2).strip()
        elif name and indent(line) > depth:
            args[name] += " " + line.strip()
    return " ".join(w for w in desc if w).strip(), args


def schema(env, fn):
    desc, arg_docs = parse_doc(ast.get_docstring(fn, clean=False))
    props, required = {}, []
    a = fn.args
    defaults = [None] * (len(a.args) - len(a.defaults)) + list(a.defaults)
    for arg, default in zip(a.args, defaults):
        if arg.arg in ("self", "cls"): continue
        ann = arg.annotation
        s = env.json_type(ann) if ann is not None else {"type": "string"}
        if arg.arg in arg_docs and "description" not in s: s["description"] = arg_docs[arg.arg]
        fld = env.field(ann) if ann is not None else None
        if isinstance(default, ast.Call) and name_of(default.func) == "Field": fld = default
        kw = {k.arg: ast.literal_eval(k.value) for k in fld.keywords} if fld is not None else {}
        for key, out in FIELD_KEYS:
            if kw.get(key) is not None: s[out] = list(kw[key]) if key == "enum" else kw[key]
        if "const" in kw: s["const"] = kw["const"]
        props[arg.arg] = s
        has_default = default is not None and not (isinstance(default, ast.Call) and name_of(default.func) == "Field")
        if fld is not None and (fld.args or "default" in kw): has_default = True
        if not has_default and not (ann is not None and env.optional(ann)): required.append(arg.arg)
    params = {"type": "object", "properties": props}
    if required: params["required"] = required
    out = {"name": fn.name}
    if desc: out["description"] = desc
    out["parameters"] = params
    for dec in fn.decorator_list:           # @cactus.tool(triggers=[...])
        if isinstance(dec, ast.Call):
            for k in dec.keywords:
                if k.arg == "triggers": out["triggers"] = ast.literal_eval(k.value)
    return out


def load_env(path):
    tree = ast.parse(open(path, encoding="utf-8").read())
    env = Env(tree)
    fns = {st.name: st for st in tree.body if isinstance(st, ast.FunctionDef)}
    tools = [schema(env, fns[name_of(e)]) for e in env.aliases["TOOLS"].elts]
    return {"tools": tools, "system": ast.literal_eval(env.aliases["SYSTEM"]),
            "cases": ast.literal_eval(env.aliases["TEST_CASES"])}


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    out = {e: load_env(os.path.join(src, "cactus/environments", e + ".py"))
           for e in ("media_player", "smart_home", "productivity", "wearable", "kitchen_appliance", "data_capture")}
    json.dump(out, open(dst, "w"), indent=1)
    print({k: len(v["cases"]) for k, v in out.items()})
