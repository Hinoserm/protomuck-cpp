( Array prim test suite. One check per prim. Driver greps TAG:value. )
: t ( s s -- ) swap strcat me @ swap notify ;
: j ( a s -- ) swap "-" array_join swap t ;
: main
  "x" "y" 2 array_make "AMAKE:" j
  "k1" "v1" "k2" "v2" 2 array_make_dict "k2" array_getitem "AGETI:" t
  { "a" "b" }list array_explode array_make_dict 1 array_getitem "AEXPL:" t
  { "a" "b" }list array_vals array_make "AVALS:" j
  { "k" "v" }dict array_keys array_make "AKEYS:" j
  { "a" "b" "c" }list array_count intostr "ACOUNT:" t
  { "x" "y" }list array_first pop intostr "AFIRST:" t
  { "x" "y" }list array_last pop intostr "ALAST:" t
  { "x" "y" }list 0 array_next pop intostr "ANEXT:" t
  { "x" "y" }list 1 array_prev pop intostr "APREV:" t
  "z" { "x" "y" }list 1 array_setitem "ASETI:" j
  "z" { "x" "y" }list 1 array_insertitem "AINSI:" j
  "z" { "x" }list array_appenditem "AAPPI:" j
  { "a" "b" "c" "d" }list 1 2 array_getrange "AGETR:" j
  { "a" "b" "c" }list 1 { "X" }list array_setrange "ASETR:" j
  { "a" "b" }list 1 { "X" }list array_insertrange "AINSR:" j
  { "a" "b" "c" }list 1 array_delitem "ADELI:" j
  { "a" "b" "c" "d" }list 1 2 array_delrange "ADELR:" j
  { "a" "b" }list { "b" "c" }list 2 array_nunion "ANUNI:" j
  { "a" "b" }list { "b" "c" }list 2 array_nintersect "ANINT:" j
  { "a" "b" "c" }list { "c" "e" "f" }list 2 array_ndiff "ANDIF:" j
  { "a" "b" }list { "b" "c" }list array_union "AUNIO:" j
  { "a" "b" }list { "b" "c" }list array_intersect "AINTE:" j
  { "a" "b" "c" }list { "c" "e" "f" }list array_diff "ADIFF:" j
  { "a" "b" }list array_reverse "AREVE:" j
  { "c" "a" "b" }list sorttype_case_ascend array_sort "ASORT:" j
  { { "k" "b" }dict { "k" "a" }dict }list sorttype_case_ascend "k"
    array_sort_indexed 0 array_getitem "k" array_getitem "ASRTI:" t
  me @ "tst/" { "p1" "v1" }dict array_put_propvals
  me @ "tst/" array_get_propvals "p1" array_getitem "APROPV:" t
  me @ "tst2/a/b" "x" setprop
  me @ "tst2/" array_get_propdirs "APROPD:" j
  me @ "lst" { "l1" "l2" }list array_put_proplist
  me @ "lst" array_get_proplist array_count intostr "APROPL:" t
  ( NOTE: expected 2, but ARRAY_PUT_PROPLIST silently writes nothing; )
  ( verified identical on pre-conversion binary 25c3b42. See TODO.    )
  me @ "rl" { #1 #0 }list array_put_reflist
  me @ "rl" array_get_reflist "AREFL:" j
  { "a" "b" "a" }list "a" array_findval "AFIND:" j
  { "a" "b" "a" }list "a" array_excludeval "AEXCL:" j
  "a b" " " explode_array "AEXPA:" j
  { "a" "b" "c" }list 1 array_cut "-" array_join swap "-" array_join
    "|" strcat swap strcat "ACUT:" t
  { "a" }list { "a" }list array_compare intostr "ACOMP:" t
  { "q" 5 }list array_interpret "AINTR:" t
  { #1 }list "P" array_filter_flags array_count intostr "AFFLG:" t
  { "foo" { 2 { "bar" "qux" }dict }dict }dict
    { "foo" 2 "bar" }list array_nested_get "ANGET:" t
  { }dict "q" swap { "a" "b" }list array_nested_set
    "a" array_getitem "b" array_getitem "ANSET:" t
  { "foo" { 2 "x" 3 "y" }dict }dict { "foo" 2 }list array_nested_del
    "foo" array_getitem array_count intostr "ANDEL:" t
  { 1 2 3 }list array_sum intostr "ASUM:" t
  "Greetings" 3 array_string_fragment "AFRAG:" j
  me @ "tst/*" properties_array array_count intostr "APRAR:" t
  { me @ }list "tst/p1" "*" array_filter_prop array_count intostr "AFPRP:" t
  { me @ }list filter_prop_exists "tst/p1" 0 array_filter_smart
    array_count intostr "AFSMT:" t
  { "foo" 1 "bar" 2 }dict "^f" reg_none array_regmatchkey
    array_count intostr "ARGMK:" t
  { "abc" "xyz" }list "b" reg_none array_regmatchval
    array_count intostr "ARGMV:" t
  { "abc" }list "b" "X" reg_none array_regsub 0 array_getitem "ARGSB:" t
  { me @ }list "tst/p1" "v" reg_none array_regfilter_prop
    array_count intostr "ARGFP:" t
  me @ descr_array array_count intostr "ADESC:" t
  online_array array_count intostr "AONLN:" t
  { "ANOTF:ok" }list { me @ }list array_notify
  { "AANSI:ok" }list { me @ }list array_ansi_notify
  { "ANHTM:ran" }list { me @ }list array_notify_html
  "" "ADONE:ok" swap t
;
