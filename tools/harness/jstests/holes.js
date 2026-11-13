var fails = [];
function eq(name, got, want){ if(got !== want) fails.push(name + ": got " + JSON.stringify(got) + " want " + JSON.stringify(want)); }
var c = 0; Array(3).concat("a", "b").map(function(x){ c++; return x; }); eq("map-skips-holes", c, 2);
var r = Array(2).map(function(){ return 1; }); eq("map-keeps-holes", r.length + ":" + (0 in r), "2:false");
c = 0; [1, , 3].forEach(function(){ c++; }); eq("literal-hole", c, 2);
var a = []; a[3] = "x"; c = 0; a.forEach(function(){ c++; }); eq("sparse-write", c + ":" + a.length, "1:4");
eq("hole-read", Array(2)[0], undefined); eq("join-holes", Array(3).join("-"), "--");
var obj = {k: 5}; eq("thisArg", [1].map(function(){ return this.k; }, obj)[0], 5);
eq("reduce-holes", [, 2, , 3].reduce(function(s, v){ return s + v; }), 5);
console.log(fails.length ? fails.join(" | ") : "ALL PASS");
