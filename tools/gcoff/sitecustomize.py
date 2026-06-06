# Injected via PYTHONPATH to disable Python's cyclic GC for the whole process
# tree (parent driver + forked ProcessPoolExecutor workers). Hypothesis: the
# default gen-2 GC sweep walks large cold object graphs (HLO/BIR IR), touching
# smash-compressed pages and forcing decompress faults (churn). Disabling it
# should cut churn and wall time. gc.disable() only stops automatic collection;
# refcounting still frees everything acyclic, so memory impact is limited to
# genuine cycles (which neuron-cc's IR may have, so measure RSS too).
import gc
gc.disable()
