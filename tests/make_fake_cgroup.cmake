# Writes a fake cgroup v2 directory for test_cgroup_policy_* (SMASH_CGROUP_ROOT).
# Usage: cmake -DDIR=<dir> -P make_fake_cgroup.cmake
# memory.current is 1 MiB below a 100 MiB memory.max, i.e. ~1% headroom: the
# SMASH_CGROUP policy must classify it as CRITICAL.
if(NOT DIR)
  message(FATAL_ERROR "DIR not set")
endif()
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
file(WRITE "${DIR}/memory.max" "104857600\n")
file(WRITE "${DIR}/memory.high" "max\n")
file(WRITE "${DIR}/memory.current" "103809024\n")
file(WRITE "${DIR}/memory.events" "low 0\nhigh 0\nmax 12\noom 0\noom_kill 0\n")
file(WRITE "${DIR}/memory.pressure"
  "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n")
