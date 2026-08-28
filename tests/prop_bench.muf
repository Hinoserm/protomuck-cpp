( Property engine benchmark. Run PREEMPT so the interpreter never   )
( timeslices the measurement. Each section notifies                 )
( BENCH:<name>:<milliseconds>. Compare runs between engine builds.  )
var i
var t0
var cnt
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

  "BENCH:done:1" "" swap strcat me @ swap notify
;
