import json
import os
import unittest
import urllib.request

from gygax import Extension, ExtensionError


def build():
    ext = Extension("ops", "1.2.3", token="s3cret")

    @ext.tool(description="Upper-case the input")
    def shout(text):
        return text.upper()

    @ext.tool(json_input=True)
    def add(args):
        """Add two numbers"""
        return {"sum": args["a"] + args["b"]}

    @ext.tool()
    def boom(text):
        raise RuntimeError("exploded")

    return ext


AUTH = {"Authorization": "Bearer s3cret"}


class ExtensionTests(unittest.TestCase):
    def test_describe_lists_tools_and_docstring_descriptions(self):
        status, body = build().handle("GET", "/gygax/describe", AUTH, b"")
        self.assertEqual(status, 200)
        self.assertEqual(body["name"], "ops")
        tools = {t["name"]: t["description"] for t in body["tools"]}
        self.assertEqual(tools, {"shout": "Upper-case the input", "add": "Add two numbers", "boom": ""})

    def test_calls_return_output_or_structured_errors(self):
        ext = build()
        self.assertEqual(ext.handle("POST", "/gygax/call/shout", AUTH, b'{"input":"hi"}'), (200, {"output": "HI"}))
        status, body = ext.handle("POST", "/gygax/call/add", AUTH, b'{"input":"{\\"a\\":1,\\"b\\":2}"}')
        self.assertEqual((status, json.loads(body["output"])), (200, {"sum": 3}))
        self.assertEqual(ext.handle("POST", "/gygax/call/boom", AUTH, b'{"input":""}')[0], 500)
        self.assertEqual(ext.handle("POST", "/gygax/call/nope", AUTH, b'{"input":""}')[0], 404)
        self.assertEqual(ext.handle("POST", "/gygax/call/add", AUTH, b'{"input":"not json"}')[0], 400)
        self.assertEqual(ext.handle("POST", "/gygax/call/shout", AUTH, b"[1]")[0], 400)
        self.assertEqual(ext.handle("POST", "/gygax/call/shout", AUTH, b'{"input":5}')[0], 400)

    def test_requires_the_token(self):
        ext = build()
        self.assertEqual(ext.handle("GET", "/gygax/describe", {}, b"")[0], 401)
        self.assertEqual(ext.handle("GET", "/gygax/describe", {"Authorization": "Bearer wrong"}, b"")[0], 401)
        self.assertEqual(ext.handle("GET", "/other", AUTH, b"")[0], 404)

    def test_rejects_bad_definitions_and_open_listeners(self):
        with self.assertRaises(ValueError):
            Extension("Bad Name")
        ext = Extension("x")
        with self.assertRaises(ValueError):
            ext.tool("bad.name")(lambda s: s)
        ext.tool("ok")(lambda s: s)
        with self.assertRaises(ValueError):
            ext.tool("ok")(lambda s: s)
        with self.assertRaises(ValueError):
            ext.serve(host="0.0.0.0")
        with self.assertRaises(ExtensionError):
            ext.call("missing", "")

    def test_serves_over_http(self):
        with build().serve() as running:
            req = urllib.request.Request(running.url + "/gygax/describe", headers=AUTH)
            with urllib.request.urlopen(req, timeout=5) as resp:
                self.assertEqual(json.load(resp)["name"], "ops")


@unittest.skipUnless(os.environ.get("GYGAX_BIN"), "GYGAX_BIN not set")
class ExtensionEndToEnd(unittest.TestCase):
    def test_service_registers_and_calls_python_tools_and_native_plugin(self):
        from gygax import LocalService

        plugins = [os.environ["GYGAX_PLUGIN"]] if os.environ.get("GYGAX_PLUGIN") else []
        with build().serve() as running:
            with LocalService(token="t", extensions=[running.url + ";token=s3cret"], plugins=plugins) as svc:
                names = {t["name"] for t in svc.client.tools()}
                self.assertIn("ext.ops.shout", names)
                self.assertEqual(svc.client.invoke_tool("ext.ops.shout", "quiet"), "QUIET")
                self.assertEqual(json.loads(svc.client.invoke_tool("ext.ops.add", json.dumps({"a": 2, "b": 5})))["sum"], 7)
                if plugins:
                    self.assertEqual(svc.client.invoke_tool("plugin.geo.echo", "ping"), "ping")


if __name__ == "__main__":
    unittest.main()
