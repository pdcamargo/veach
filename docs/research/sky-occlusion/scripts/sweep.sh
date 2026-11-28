#!/bin/zsh
# sweep.sh: GPU ms with and without the sky occlusion, Bistro at noon and sunset, 1080p and 1440p, two rounds.
D=/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/6c5aac47-c6f0-4e9a-9705-ac1bf3f844c8/scratchpad/sky-occlusion
T=/Users/patrickdiascamargo/dev/cpp/veach/.claude/worktrees/agent-a056c205dd68cbbf3
for r in 1 2; do
for sc in bistro_noon bistro_sunset; do
for sz in 1920x1080 2560x1440; do
    echo "== $sc $sz after round $r"
    python3 $D/time.py $T lab scripts/$sc.luau $sz
    echo "== $sc $sz before round $r"
    python3 $D/time.py $T lab scripts/$sc.luau $sz VEACH_DISABLE_SKY_OCCLUSION=1
done
done
done
