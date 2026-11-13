var fails = [];
function eq(name, got, want){ if(got !== want) fails.push(name + ": got " + JSON.stringify(got) + " want " + JSON.stringify(want)); }
eq("str-basic", JSON.stringify({a: [1, "x\n\"y", true, null], b: undefined, c: function(){}}), '{"a":[1,"x\\n\\"y",true,null]}');
eq("str-indent", JSON.stringify({a: 1, b: [2]}, null, 2), '{\n  "a": 1,\n  "b": [\n    2\n  ]\n}');
eq("str-tojson", JSON.stringify({d: new Date(0)}), '{"d":"1970-01-01T00:00:00.000Z"}');
eq("str-replacer", JSON.stringify({a: 1, b: 2}, function(k, v){ return k === "b" ? undefined : v; }), '{"a":1}');
eq("str-allow", JSON.stringify({a: 1, b: 2, c: 3}, ["a", "c"]), '{"a":1,"c":3}');
var cyc = {}; cyc.self = cyc; try { JSON.stringify(cyc); fails.push("cycle"); } catch(e){ eq("cycle", e instanceof TypeError, true); }
var p = JSON.parse('{"a":[1,2.5e3,-0.5],"b":"\\u00e9\\n","c":{"d":null}}'); eq("parse", p.a[1] + p.b + p.c.d, "2500é\nnull");
eq("parse-reviver", JSON.parse('{"a":1,"b":2}', function(k, v){ return typeof v === "number" ? v * 10 : v; }).b, 20);
try { JSON.parse("{bad"); fails.push("bad-json"); } catch(e){ eq("bad-json", e instanceof SyntaxError, true); }
var big = []; for(var i = 0; i < 5000; i++) big.push({id: i, name: "item" + i}); var s = JSON.stringify(big);
eq("big-roundtrip", JSON.parse(s)[4999].name, "item4999"); eq("big-len", s.length > 100000, true);
eq("repeat", "ab".repeat(30000).length, 60000);
console.log(fails.length ? fails.join(" | ") : "ALL PASS");
