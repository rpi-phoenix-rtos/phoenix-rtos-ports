# Self-validating jq feature check: emits "ALL-OK" or the failing cases.
# Run: jq -n -f selfcheck.jq   (psh-safe: no filter on the command line)
[
  {n:"add",       got:([1,2,3]|add),                              want:6},
  {n:"map",       got:([1,2,3]|map(.*2)),                         want:[2,4,6]},
  {n:"select",    got:([1,2,3,4]|map(select(.>2))),               want:[3,4]},
  {n:"group_by",  got:([{k:"a"},{k:"b"},{k:"a"}]|group_by(.k)|map({k:.[0].k,c:length})), want:[{k:"a",c:2},{k:"b",c:1}]},
  {n:"reduce",    got:([1,2,3,4,5]|reduce .[] as $x (0;.+$x)),     want:15},
  {n:"foreach",   got:([1,2,3]|[foreach .[] as $x (0;.+$x)]),      want:[1,3,6]},
  {n:"recurse",   got:({a:1,b:{c:2,d:[3,4]}}|[..|numbers]),        want:[1,2,3,4]},
  {n:"entries",   got:({x:1,y:2}|to_entries|from_entries),        want:{x:1,y:2}},
  {n:"sqrt",      got:([4,9,16]|map(sqrt|floor)),                  want:[2,3,4]},
  {n:"pow",       got:(pow(2;10)),                                 want:1024},
  {n:"sort",      got:([3,1,2]|sort),                              want:[1,2,3]},
  {n:"sort_by",   got:([{v:3},{v:1}]|sort_by(.v)|map(.v)),         want:[1,3]},
  {n:"unique",    got:([1,1,2,3,3]|unique),                        want:[1,2,3]},
  {n:"split",     got:("a,b,c"|split(",")),                        want:["a","b","c"]},
  {n:"join",      got:(["a","b","c"]|join("-")),                   want:"a-b-c"},
  {n:"upcase",    got:("hi"|ascii_upcase),                         want:"HI"},
  {n:"tonumber",  got:("42"|tonumber),                             want:42},
  {n:"tostring",  got:(42|tostring),                               want:"42"},
  {n:"keys",      got:({b:1,a:2}|keys),                            want:["a","b"]},
  {n:"has",       got:({a:1}|has("a")),                            want:true},
  {n:"contains",  got:({a:1,b:2}|contains({a:1})),                 want:true},
  {n:"flatten",   got:([[1,[2]],[3]]|flatten),                     want:[1,2,3]},
  {n:"range",     got:([range(0;5;2)]),                            want:[0,2,4]},
  {n:"utf8len",   got:("héllo"|length),                           want:5},
  {n:"minmax",    got:([3,1,2]|[min,max]),                         want:[1,3]},
  {n:"paths",     got:({a:{b:1}}|[paths]),                         want:[["a"],["a","b"]]},
  {n:"getpath",   got:({a:{b:9}}|getpath(["a","b"])),              want:9},
  {n:"ascii",     got:(65|[.]|implode),                            want:"A"},
  {n:"floor_neg", got:(-1.5|floor),                                want:-2},
  {n:"add_str",   got:(["ab","cd"]|add),                           want:"abcd"}
]
| map(select(.got != .want))
| if length==0 then "ALL-OK" else {FAILED:.} end
