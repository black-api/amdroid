var fails = [];
function eq(name, got, want){ if(got !== want) fails.push(name + ": got " + JSON.stringify(got) + " want " + JSON.stringify(want)); }
var m = /(\d+)-(\d+)/.exec("ab 12-34 cd"); eq("exec", m[0] + "|" + m[1] + "|" + m[2] + "|" + m.index, "12-34|12|34|3");
var n = /(?<y>\d{4})-(?<m>\d\d)/.exec("on 2024-06"); eq("named", n.groups.y + n.groups.m, "202406");
eq("replace-$", "John Smith".replace(/(\w+)\s(\w+)/, "$2, $1"), "Smith, John");
eq("replace-fn", "a1b2".replace(/\d/g, function(d){ return d * 2; }), "a2b4");
eq("replace-named", "2024-06".replace(/(?<y>\d+)-(?<m>\d+)/, "$<m>/$<y>"), "06/2024");
eq("replace-str", "a.b.c".replace(".", "-"), "a-b.c"); eq("replaceAll", "a.b.c".replaceAll(".", "-"), "a-b-c");
eq("match-g", "x1y22z333".match(/\d+/g).join(), "1,22,333"); eq("match-null", "abc".match(/\d/), null);
eq("split-re", "a1b22c".split(/\d+/).join(), "a,b,c"); eq("split-cap", "a1b".split(/(\d)/).join(), "a,1,b");
eq("split-limit", "a,b,c".split(",", 2).join(), "a,b"); eq("split-empty", "abc".split("").join(), "a,b,c");
eq("lookahead", /foo(?=bar)/.test("foobar") && !/foo(?!bar)/.test("foobar"), true);
eq("lookbehind", "$10 €20".match(/(?<=\$)\d+/)[0], "10");
eq("backref", /(\w)\1/.test("hello"), true); eq("lazy", "<a><b>".match(/<.+?>/)[0], "<a>");
eq("icase", /HELLO/i.test("hello"), true); eq("multiline", "a\nb".match(/^b$/m)[0], "b");
eq("dotall", /a.b/s.test("a\nb") && !/a.b/.test("a\nb"), true);
eq("class-neg", "a-b_c".replace(/[^a-z]/g, ""), "abc"); eq("class-w", "x y".replace(/\W/g, "_"), "x_y");
var re = /o/g; var c = 0; while(re.exec("foo boo")) c++; eq("lastIndex-loop", c, 4);
eq("sticky", /b/y.test("ab"), false); eq("ctor", new RegExp("a+", "g").flags + new RegExp(/x/i).source, "gx");
eq("search", "hello".search(/l+/), 2); eq("matchAll", [..."a1b2".matchAll(/[a-z](\d)/g)].map(m => m[1]).join(), "1,2");
eq("toString", String(/a\/b/gi), "/a\\/b/gi"); eq("utf8-dot", "héllo".match(/h.l/)[0], "hél");
eq("quant-brace", /^a{2,3}$/.test("aaa") && !/^a{2,3}$/.test("aaaa"), true);
eq("alt", "cat dog".replace(/cat|dog/g, "pet"), "pet pet"); eq("nested", /^((ab)+c)$/.exec("ababc")[2], "ab");
try { new RegExp("("); fails.push("bad-regex: no error"); } catch(e){ eq("syntaxerror", e instanceof SyntaxError, true); }
var big = ""; for(var i = 0; i < 2000; i++) big += "ab"; eq("big-star", big.replace(/(?:ab)*/, "").length, 0);
eq("catastrophic-safe", typeof /^(a+)+$/.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"), "boolean");
console.log(fails.length ? fails.join(" | ") : "ALL PASS");
