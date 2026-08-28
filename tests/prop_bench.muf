( Property engine benchmark. Run PREEMPT so the interpreter never   )
( timeslices the measurement. Each section notifies                 )
( BENCH:<name>:<milliseconds>. Compare runs between engine builds.  )
var i
var t0
var cnt
var obj
var first
var arr
: ms ( f -- i ) systime_precise swap - 1000.0 * int ;
: mark ( -- f ) systime_precise ;
: report ( f s -- ) swap ms intostr strcat me @ swap notify ;
: main
  preempt

  ( set 50000 props in one directory )
  mark t0 !
  1 i !
  begin i @ 50000 <= while
    me @ "bench/p" i @ intostr strcat "v" i @ intostr strcat setprop
    i @ 1 + i !
  repeat
  t0 @ "BENCH:set50000:" report

  ( read them all back, hits )
  mark t0 !
  1 i !
  begin i @ 50000 <= while
    me @ "bench/p" i @ intostr strcat getpropstr pop
    i @ 1 + i !
  repeat
  t0 @ "BENCH:get50000hit:" report

  ( 50000 misses, the whole-db-scan shape )
  mark t0 !
  1 i !
  begin i @ 50000 <= while
    me @ "nosuchdir/q" i @ intostr strcat getpropstr pop
    i @ 1 + i !
  repeat
  t0 @ "BENCH:get50000miss:" report

  ( enumerate the 50000-entry directory with nextprop )
  mark t0 !
  0 cnt !
  me @ "bench/" nextprop
  begin dup while
    cnt @ 1 + cnt !
    me @ swap nextprop
  repeat pop
  t0 @ "BENCH:walk50000:" report
  cnt @ intostr "BENCH:walkcount:" swap strcat me @ swap notify

  ( deep path traffic )
  mark t0 !
  1 i !
  begin i @ 20000 <= while
    me @ "deep/a/b/c/d/e" "x" setprop
    me @ "deep/a/b/c/d/e" getpropstr pop
    i @ 1 + i !
  repeat
  t0 @ "BENCH:deep20000:" report

  ( remove all 50000 )
  mark t0 !
  1 i !
  begin i @ 50000 <= while
    me @ "bench/p" i @ intostr strcat remove_prop
    i @ 1 + i !
  repeat
  t0 @ "BENCH:rm50000:" report

  ( --- wide scans: many objects probed for one path --- )
  ( create 10000 things; every 10th carries the prop )
  mark t0 !
  1 i !
  begin i @ 10000 <= while
    #0 "so" i @ intostr strcat newobject obj !
    i @ 1 = if obj @ first ! then
    i @ 10 % not if obj @ "scan/findme" "yes" setprop then
    i @ 1 + i !
  repeat
  t0 @ "BENCH:mk10000:" report

  ( manual scan of the first 1000 )
  mark t0 !
  0 cnt !
  0 i !
  begin i @ 1000 < while
    first @ int i @ + dbref "scan/findme" getpropstr if
      cnt @ 1 + cnt !
    then
    i @ 1 + i !
  repeat
  t0 @ "BENCH:scan1000:" report
  cnt @ intostr "BENCH:scan1000hits:" swap strcat me @ swap notify

  ( manual scan of all 10000 )
  mark t0 !
  0 cnt !
  0 i !
  begin i @ 10000 < while
    first @ int i @ + dbref "scan/findme" getpropstr if
      cnt @ 1 + cnt !
    then
    i @ 1 + i !
  repeat
  t0 @ "BENCH:scan10000:" report
  cnt @ intostr "BENCH:scan10000hits:" swap strcat me @ swap notify

  ( the propsearch primitive shape: array_filter_prop over 10000 )
  0 array_make arr !
  0 i !
  begin i @ 10000 < while
    first @ int i @ + dbref arr @ array_appenditem arr !
    i @ 1 + i !
  repeat
  mark t0 !
  arr @ "scan/findme" "*" array_filter_prop array_count cnt !
  t0 @ "BENCH:filter10000:" report
  cnt @ intostr "BENCH:filter10000hits:" swap strcat me @ swap notify

  "BENCH:done:1" "" swap strcat me @ swap notify
;
