#!/bin/zsh
# build_batch.sh <list> <parallel>: lines "backend|name|conf|overlay"; skips names already built.
S=${0:A:h}
grep -v '^#' $1 | while IFS='|' read -r b n c o; do
  [ -f $S/builds/$n/cedar_switch_3in4out_power/zephyr/zephyr.signed.bin ] && continue
  printf '%s\0' "$b|$n|$c|$o"
done | xargs -0 -n1 -P $2 $S/build_entry.sh
echo batch done
