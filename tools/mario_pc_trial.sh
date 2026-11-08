#!/bin/bash
# Runs inside the contained environment: PC trial of the Mario games, one after another, on runtime objects of their own, so the
# Tomodachi and Layton sessions' /root/pc_rt and binaries are left alone. CRO modules get the Wii U's slots (inside its flat 512 MB),
# so the generated code in games/<name>/gen_safe is the one both builds use.
# mario_pc_trial.sh <frames> <name under games/>... Env: AUTOPLAY (default 2: wander), JOBS (default 4).
# ABI=1 runs the --abi code instead, on the software renderer: if a game goes wrong on the Wii U but not here, the code generator isn't why.
FRAMES=$1
shift
export RECOMP_CRO_BASE=0x1C000000 AUTOPLAY=${AUTOPLAY:-2} JOBS=${JOBS:-4}
TRY=/root/pc_try_mario${ABI:+_abi}.sh
GEN=gen_${ABI:+abi}
GEN=${GEN/%gen_/gen_safe}
sed -e 's#^OBJ=/root/pc_rt$#OBJ=/root/pc_rt_mario#' -e 's/-P 12/-P 4/' -e '/^GAME_OBJ=/,$d' /project/tools/pc_runtime.sh > /root/pc_runtime_mario.sh
sed -e 's#/root/pc_rt/#/root/pc_rt_mario/#' -e 's#-lpthread -ldl -lm -rdynamic#$AAC_LIBS -lpthread -ldl -lm -rdynamic#' /project/tools/pc_try_game.sh > "$TRY"
[ -n "$ABI" ] && sed -i -e 's#gen_safe#gen_abi#g' -e 's#gen_abi" --extra#gen_abi" --abi --extra#' -e 's#^OBJ=/root/pc_$N#OBJ=/root/pca_$N#' -e 's#^RUN=/root/run_$N#RUN=/root/runa_$N#' -e 's#^BIN=/root/${N}_pc#BIN=/root/${N}_pca#' -e 's#shots/sheet.png#shots/sheet_abi.png#' "$TRY"
grep -q 'OBJ=/root/pc_rt_mario' /root/pc_runtime_mario.sh && grep -q '/root/pc_rt_mario/' "$TRY" || { echo "trial scripts changed: paths not found"; exit 1; }
nice -n 10 bash /root/pc_runtime_mario.sh 2>&1 | tail -6 | cut -c1-170
for N in "$@"; do
    echo "===== $N $(date +%H:%M:%S)"
    # the generator may have changed since this game's code was generated
    for f in gen.py cro.py; do [ /project/third_party/3dsrecomp/tools/recomp/$f -nt "/project/games/$N/$GEN/table.c" ] && rm -f "/project/games/$N/$GEN/table.c"; done
    # their classic soundtracks (tools/aac_build.sh)
    export AAC_LIBS=
    case $N in superstar_saga|bowsers_inside)
        clang++-18 -std=c++20 -O1 -g -w -I/root/aac_pc/include -c /project/port/aac/aac_ffmpeg.cpp -o /root/aac_pc/aac_ffmpeg.o || exit 1
        AAC_LIBS="/root/aac_pc/aac_ffmpeg.o /root/aac_pc/lib/libavcodec.a /root/aac_pc/lib/libavutil.a"
    esac
    bash "$TRY" "$N" "$FRAMES" 2>&1
    echo "===== $N done $(date +%H:%M:%S)"
done
