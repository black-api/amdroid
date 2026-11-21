/* Syntax that real sites' minified scripts use and that once failed to parse */
var _pass = 0, _fail = 0;
function ok(name, cond){ if(cond) _pass++; else { _fail++; console.log('FAIL: ' + name) } }
function eq(name, a, b){ ok(name + ' (' + a + ' vs ' + b + ')', a === b) }
function done(){ console.log((_fail ? 'FAILED ' : 'passed ') + _pass + ' checks, ' + _fail + ' failures') }

/* a number written "1." followed by a member access */
eq('1..toString', 1..toString(), '1');
eq('2.5.toFixed', 2.5.toFixed(0) === '3' || 2.5.toFixed(0) === '2', true);
eq('1e3 and 1.5e2', 1e3 + 1.5e2, 1150);

/* new.target with a member after it */
function K(){ return new.target.tag }
K.tag = 'k';
eq('new.target.tag', new K() instanceof K ? 'k' : '', 'k');

/* "in" inside a function in a for(...) head */
var seen = [];
for(var f = function(o){ return 'a' in o }; seen.length < 1; seen.push(f({a: 1})));
eq('in inside a function in a for head', seen[0], true);
for(var g = [function(){ return 'x' in {x: 1} }][0]; seen.length < 2; seen.push(g()));
eq('in inside an array in a for head', seen[1], true);

/* a regex literal after the ) of if/for/while */
var s = 'on1', hits = 0;
if(s)/^on/.test(s) && hits++;
for(var k in {q: 1})/^on/.test(s) && hits++;
var w = 1; while(w--)/\d$/.test(s) && hits++;
eq('regex after ) of a statement head', hits, 3);
eq('division after ) still divides', (8)/2/2, 2);

/* braces in template text are text */
var n = 0, t = [1];
for(; n < t.length && t[n] !== `}`;) n++;
eq('template with a lone }', n, 1);
eq('templates with braces', `}` + `{` + `a}b`, '}{a}b');
eq('} in a string inside ${}', `x${1 ? '}' : `y${2}`}z`, 'x}z');
eq('object literal inside ${}', `${{a: 1}.a}`, '1');
eq('escaped backtick', `\`${5}\``, '`5`');

/* a regex literal with a quote in it, inside ${} */
eq('regex with quotes in ${}', `${'a"b\'c'.replace(/"/g, '&q;').replace(/'/g, '&a;')}`, 'a&q;b&a;c');
eq('comment with a brace in ${}', `${1 /* } */ + 1}`, '2');

done();
