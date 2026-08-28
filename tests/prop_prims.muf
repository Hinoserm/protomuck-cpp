( Property system test suite. Run via tests/run_muf_tests.py against )
( a minimal db. Each check notifies TAG:value; the driver greps for  )
( exact matches. Covers datatypes, propdir shapes, enumeration       )
( order, case-insensitive identity, removal semantics, environment   )
( search, and a large-directory walk.                                )
var acc
var cnt
var i
: t ( s s -- ) swap strcat me @ swap notify ;
: main
  ( --- datatypes --- )
  me @ "t/str" "hello" setprop
  me @ "t/str" getpropstr "PSTR:" t
  me @ "t/int" 42 setprop
  me @ "t/int" getpropval intostr "PINT:" t
  me @ "t/flt" 3.5 setprop
  me @ "t/flt" getpropfval ftostr "PFLT:" t
  me @ "t/ref" #0 setprop
  me @ "t/ref" getprop #0 dbcmp intostr "PREF:" t
  me @ "t/lok" "me" parselock setprop
  me @ "t/lok" getprop prettylock "One" instr 0 > intostr "PLOK:" t

  ( type overwrite: str then int )
  me @ "t/over" "words" setprop
  me @ "t/over" 7 setprop
  me @ "t/over" getpropval intostr "POVER:" t
  me @ "t/over" getpropstr "." strcat "POVERSTR:" t

  ( --- propdir shapes --- )
  me @ "d" "topval" setprop
  me @ "d/c" "childval" setprop
  me @ "d" getpropstr "PDIRVAL:" t
  me @ "d/c" getpropstr "PDIRCHILD:" t
  me @ "d" propdir? intostr "PDIRP:" t
  me @ "t/str" propdir? intostr "PDIRP2:" t

  ( deep nesting )
  me @ "n1/n2/n3/n4/n5" "deep" setprop
  me @ "n1/n2/n3/n4/n5" getpropstr "PDEEP:" t
  me @ "n1/n2" propdir? intostr "PDEEPDIR:" t

  ( --- enumeration order: scrambled insert, walk with nextprop --- )
  me @ "ord/b" 1 setprop
  me @ "ord/A" 1 setprop
  me @ "ord/c2" 1 setprop
  me @ "ord/C1" 1 setprop
  me @ "ord/_u" 1 setprop
  me @ "ord/2n" 1 setprop
  me @ "ord/.d" 1 setprop
  me @ "ord/~t" 1 setprop
  me @ "ord/x y" 1 setprop
  "" acc !
  me @ "ord/" nextprop
  begin dup while
    dup 4 strcut swap pop acc @ swap strcat "|" strcat acc !
    me @ swap nextprop
  repeat pop
  acc @ "PORD:" t

  ( case-insensitive identity, first spelling wins )
  me @ "ci/MiXeD" "one" setprop
  me @ "ci/mixed" getpropstr "PCASE:" t
  me @ "ci/MIXED" "two" setprop
  me @ "ci/" nextprop 3 strcut swap pop "PCASENAME:" t
  me @ "ci/mIxEd" getpropstr "PCASE2:" t

  ( --- removal semantics --- )
  me @ "rm/a/b" "x" setprop
  me @ "rm" remove_prop
  me @ "rm" propdir? intostr "PRMDIR:" t
  me @ "rm/a/b" getpropstr "gone" strcat "PRMDEEP:" t

  ( zero int removes )
  me @ "zd/keep" 1 setprop
  me @ "zd/gone" 5 setprop
  me @ "zd/gone" 0 setprop
  me @ "zd/" nextprop 3 strcut swap pop "PZERO:" t

  ( empty string removes )
  me @ "ed/keep" 1 setprop
  me @ "ed/gone" "v" setprop
  me @ "ed/gone" "" setprop
  me @ "ed/" nextprop 3 strcut swap pop "PEMPTY:" t

  ( addprop legacy form )
  me @ "ap" "apval" 0 addprop
  me @ "ap" getpropstr "PADD:" t
  me @ "apv" "" 99 addprop
  me @ "apv" getpropval intostr "PADDV:" t

  ( --- environment search --- )
  #0 "envt" "fromzero" setprop
  me @ "envt" envpropstr swap #0 dbcmp intostr strcat "PENV:" t

  ( --- large directory: 200 props, count and spot-check --- )
  1 i !
  begin i @ 200 <= while
    me @ "big/p" i @ intostr strcat "v" i @ intostr strcat setprop
    i @ 1 + i !
  repeat
  me @ "big/p137" getpropstr "PBIG137:" t
  0 cnt !
  me @ "big/" nextprop
  begin dup while
    cnt @ 1 + cnt !
    me @ swap nextprop
  repeat pop
  cnt @ intostr "PBIGN:" t

  ( parseprop runs MPI from a prop )
  me @ "mpi" "plain-mpi" setprop
  me @ "mpi" "" 0 parseprop "PMPI:" t

  "PDONE:ok" "" swap t
;
